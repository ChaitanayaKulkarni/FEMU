// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP4221 WFQ scheduler (SCFQ — Self-Clocked Fair Queuing, Golestani 1994).
 *
 * Sits between the NVMe poller and the FTL ring. Commands on CDL-bound SQs
 * are held in per-class FIFOs and dispatched in virtual-finish-tag order.
 * v2: per-class in-flight counters bounded by weight-proportional caps replace
 * the v1 single-slot serialization gate.
 *
 * All WFQ and CDL stat state lives in FemuCtrl and is protected by cdl_lock.
 */

#include "./nvme.h"

/* ABI §7.0 cost model constants (BBSSD approximation) */
#define WFQ_FIXED_OP_NS      10000ULL         /* 10 µs per-op floor */
#define WFQ_BW_READ_BPS      4294967296ULL    /* 4 GiB/s */
#define WFQ_BW_WRITE_BPS     2147483648ULL    /* 2 GiB/s */

/* Upper bound on poller index (nr_pollers = ceil(nr_io_queues / poller_ratio)) */
#define WFQ_MAX_POLLERS      64

/* Estimate service cost for one command (nanoseconds). */
static uint64_t wfq_cost_ns(NvmeRequest *req)
{
    uint8_t lbads = req->ns->id_ns.lbaf[
        NVME_ID_NS_FLBAS_INDEX(req->ns->id_ns.flbas)].lbads;
    uint64_t bytes = (uint64_t)req->nlb << lbads;
    uint64_t bw    = req->is_write ? WFQ_BW_WRITE_BPS : WFQ_BW_READ_BPS;

    /* ceil division: (bytes * 1e9 + bw - 1) / bw */
    return WFQ_FIXED_OP_NS + (bytes * 1000000000ULL + bw - 1) / bw;
}

/* Weight scaled = (CDL_MAX_NS * WFQ_SCALE) / duration_ns.
 * Uses the descriptor matching the request direction; falls back to the other. */
static uint64_t wfq_weight(FemuCtrl *n, uint8_t cls, bool is_write)
{
    uint64_t dur = is_write ? n->cdl_write[cls].duration_ns
                            : n->cdl_read[cls].duration_ns;
    if (!dur) {
        dur = is_write ? n->cdl_read[cls].duration_ns
                       : n->cdl_write[cls].duration_ns;
    }
    if (!dur) {
        return WFQ_SCALE;
    }
    return (CDL_MAX_NS * (uint64_t)WFQ_SCALE) / dur;
}

/*
 * Recompute per-class in-flight caps from CDL descriptor weights and wfq_depth.
 * Caller must hold cdl_lock.
 *
 * Formula: max_inflight[c] = floor(wfq_depth * w[c] / total_w), then fix-up
 * each to >= 1, then clamp sum to <= wfq_depth by trimming lowest-weight classes.
 */
static void wfq_recompute_limits_locked(FemuCtrl *n)
{
    uint64_t w[CDL_MAX_CLASSES] = {0};
    uint64_t total_w = 0;

    for (int c = 1; c < CDL_MAX_CLASSES; c++) {
        w[c] = wfq_weight(n, c, false);
        if (w[c] == WFQ_SCALE &&
            n->cdl_read[c].duration_ns == 0 &&
            n->cdl_write[c].duration_ns == 0) {
            /* class not configured; weight is the fallback default — skip */
            w[c] = 0;
        }
        total_w += w[c];
    }

    if (total_w == 0) {
        /* no active classes; set all caps to 1 so any future arrival can proceed */
        for (int c = 1; c < CDL_MAX_CLASSES; c++) {
            n->wfq_max_inflight[c] = 1;
        }
        return;
    }

    uint32_t allocated = 0;
    for (int c = 1; c < CDL_MAX_CLASSES; c++) {
        if (w[c] == 0) {
            n->wfq_max_inflight[c] = 0;
            continue;
        }
        uint32_t cap = (uint32_t)((n->wfq_depth * w[c]) / total_w); /* floor */
        n->wfq_max_inflight[c] = (cap < 1) ? 1 : cap;
        allocated += n->wfq_max_inflight[c];
    }

    /* Trim from highest class index (lowest weight) downward to keep sum <= wfq_depth */
    for (int c = CDL_MAX_CLASSES - 1; c >= 1 && allocated > n->wfq_depth; c--) {
        if (w[c] == 0 || n->wfq_max_inflight[c] <= 1) {
            continue;
        }
        uint32_t drop = allocated - n->wfq_depth;
        drop = (drop < n->wfq_max_inflight[c] - 1) ? drop
                                                    : (n->wfq_max_inflight[c] - 1);
        n->wfq_max_inflight[c] -= drop;
        allocated -= drop;
    }
}

/* Public wrapper — acquires cdl_lock around the computation. */
void wfq_recompute_limits(FemuCtrl *n)
{
    pthread_spin_lock(&n->cdl_lock);
    wfq_recompute_limits_locked(n);
    pthread_spin_unlock(&n->cdl_lock);
}

/*
 * Compute the effective dispatch limit for class c.
 * If other classes have queued work, enforce the strict cap.
 * Otherwise allow work-conserving borrowing of idle slots.
 * Caller must hold cdl_lock.
 */
static uint32_t wfq_compute_limit(FemuCtrl *n, int c, bool any_nonempty_other)
{
    if (any_nonempty_other) {
        return n->wfq_max_inflight[c];
    }
    /* All other queues empty: borrow idle slots, reserving wfq_max_inflight[k]
     * for each other class k in case it wakes up. */
    uint32_t reserved = 0;
    for (int k = 1; k < CDL_MAX_CLASSES; k++) {
        if (k != c) {
            reserved += n->wfq_max_inflight[k];
        }
    }
    uint32_t borrow_cap = (n->wfq_depth > reserved)
                          ? (n->wfq_depth - reserved + n->wfq_inflight[c])
                          : n->wfq_max_inflight[c];
    return (borrow_cap > n->wfq_max_inflight[c]) ? borrow_cap
                                                  : n->wfq_max_inflight[c];
}

/*
 * Dispatch loop: fill all classes up to their caps in one pass.
 * Caller must hold cdl_lock.
 * State (V-clock, inflight counters, inflight_list) is updated AFTER a
 * successful ring enqueue — no rollback needed.
 */
static void wfq_dispatch_locked(FemuCtrl *n)
{
    bool blocked_poller[WFQ_MAX_POLLERS + 1];
    memset(blocked_poller, 0, sizeof(blocked_poller));

    /* Determine whether any class queue is non-empty (for work-conserving check) */
    bool any_nonempty[CDL_MAX_CLASSES];
    for (int c = 1; c < CDL_MAX_CLASSES; c++) {
        any_nonempty[c] = !QTAILQ_EMPTY(&n->wfq_queues[c]);
    }

    bool any_dispatched;
    do {
        any_dispatched = false;

        int      best  = -1;
        uint64_t min_f = UINT64_MAX;

        for (int c = 1; c < CDL_MAX_CLASSES; c++) {
            if (QTAILQ_EMPTY(&n->wfq_queues[c])) {
                continue;
            }
            NvmeRequest *h = QTAILQ_FIRST(&n->wfq_queues[c]);
            if (blocked_poller[h->wfq_poller_idx]) {
                continue;
            }

            /* Check if other classes (besides c) have queued work */
            bool other_nonempty = false;
            for (int k = 1; k < CDL_MAX_CLASSES; k++) {
                if (k != c && any_nonempty[k]) {
                    other_nonempty = true;
                    break;
                }
            }

            uint32_t limit = wfq_compute_limit(n, c, other_nonempty);
            if (n->wfq_inflight[c] >= limit) {
                continue;
            }
            /* strict less-than: lower class index breaks ties (ABI §7.2) */
            if (h->wfq_virt_finish < min_f) {
                min_f = h->wfq_virt_finish;
                best  = c;
            }
        }

        if (best == -1) {
            break;
        }

        NvmeRequest *req = QTAILQ_FIRST(&n->wfq_queues[best]);
        QTAILQ_REMOVE(&n->wfq_queues[best], req, wfq_entry);

        int rc = femu_ring_enqueue(n->to_ftl[req->wfq_poller_idx], (void *)&req, 1);
        if (rc != 1) {
            /* Ring full for this poller; put request back and skip this poller */
            QTAILQ_INSERT_HEAD(&n->wfq_queues[best], req, wfq_entry);
            blocked_poller[req->wfq_poller_idx] = true;
            continue;
        }

        /* Committed — update all state AFTER successful enqueue */
        req->stime = req->expire_time =
            qemu_clock_get_ns(n->test_oc12_clock ? QEMU_CLOCK_VIRTUAL
                                                  : QEMU_CLOCK_REALTIME);
        /* V-clock advances monotonically */
        if (req->wfq_virt_finish > n->wfq_v_clock) {
            n->wfq_v_clock = req->wfq_virt_finish;
        }
        n->wfq_inflight[best]++;
        QTAILQ_INSERT_TAIL(&n->wfq_inflight_list[best], req, wfq_inflight_entry);
        /* Update any_nonempty for the next iteration */
        any_nonempty[best] = !QTAILQ_EMPTY(&n->wfq_queues[best]);

        any_dispatched = true;
    } while (any_dispatched);
}

/*
 * wfq_defer — called by the poller via req->defer when CDL is enabled and
 * the SQ has a non-zero class.  Assigns SCFQ tags and enqueues into the
 * class FIFO; always kicks the dispatch loop.
 *
 * req->wfq_cdl_class must be set by the caller (nvme-io.c) before calling
 * wfq_defer, so the class is captured at submission time, not under lock.
 */
void wfq_defer(FemuCtrl *n, NvmeRequest *req, int poller)
{
    uint8_t  cls    = req->wfq_cdl_class;   /* set by nvme-io.c before this call */
    uint64_t cost   = wfq_cost_ns(req);
    uint64_t weight = wfq_weight(n, cls, req->is_write);
    uint64_t f_inc  = (cost * (uint64_t)WFQ_SCALE) / weight;

    pthread_spin_lock(&n->cdl_lock);

    /* SCFQ: S(c,k) = max(F(c,k-1), V_clock)  →  F(c,k) = S + f_inc */
    uint64_t s = (n->wfq_f_last[cls] > n->wfq_v_clock)
                 ? n->wfq_f_last[cls] : n->wfq_v_clock;
    uint64_t f = s + f_inc;

    req->wfq_virt_finish  = f;
    req->wfq_ingestion_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    req->wfq_poller_idx   = poller;
    req->wfq_orphaned     = false;
    n->wfq_f_last[cls]    = f;

    QTAILQ_INSERT_TAIL(&n->wfq_queues[cls], req, wfq_entry);

    wfq_dispatch_locked(n);

    pthread_spin_unlock(&n->cdl_lock);
}

/*
 * wfq_on_complete — called from nvme_process_cq_cpl() for every request
 * that has wfq_cdl_class != 0.  Records miss/completion stats, releases
 * the in-flight slot, and triggers the next dispatch.
 */
void wfq_on_complete(FemuCtrl *n, NvmeRequest *req)
{
    pthread_spin_lock(&n->cdl_lock);

    /* Read cls inside the lock: SQ delete zeroes wfq_cdl_class under cdl_lock */
    uint8_t cls = req->wfq_cdl_class;
    if (!cls) {
        /* Request was orphaned by SQ delete; counter already decremented there */
        pthread_spin_unlock(&n->cdl_lock);
        return;
    }

    int64_t  now     = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t  elapsed = now - req->wfq_ingestion_ns;
    NvmeCdlDescriptor *desc = req->is_write
                              ? &n->cdl_write[cls] : &n->cdl_read[cls];

    if (!req->wfq_orphaned) {
        n->cdl_stats[cls].completions++;
        if (desc->duration_ns > 0 && (uint64_t)elapsed > desc->duration_ns) {
            n->cdl_stats[cls].miss_count++;
            n->cdl_stats[cls].last_miss_ns = (uint64_t)now;
        }
    }

    n->wfq_inflight[cls]--;
    QTAILQ_REMOVE(&n->wfq_inflight_list[cls], req, wfq_inflight_entry);

    wfq_dispatch_locked(n);

    pthread_spin_unlock(&n->cdl_lock);
}

/* Initialise per-class FIFOs and in-flight lists — called from femu.c realize. */
void wfq_init(FemuCtrl *n)
{
    for (int c = 0; c < CDL_MAX_CLASSES; c++) {
        QTAILQ_INIT(&n->wfq_queues[c]);
        QTAILQ_INIT(&n->wfq_inflight_list[c]);
    }
    n->wfq_v_clock = 0;
    memset(n->wfq_inflight,     0, sizeof(n->wfq_inflight));
    memset(n->wfq_max_inflight, 0, sizeof(n->wfq_max_inflight));
    memset(n->wfq_f_last,       0, sizeof(n->wfq_f_last));
    /* wfq_depth is set by device property before realize; limits computed after */
}
