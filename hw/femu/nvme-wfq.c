// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP4221 WFQ scheduler (SCFQ — Self-Clocked Fair Queuing, Golestani 1994).
 *
 * Sits between the NVMe poller and the FTL ring. Commands on CDL-bound SQs
 * are held in per-class FIFOs and dispatched in virtual-finish-tag order.
 * v1 constraint: at most ONE command in flight at a time (ABI §7.6).
 *
 * All WFQ and CDL stat state lives in FemuCtrl and is protected by cdl_lock.
 */

#include "./nvme.h"

/* ABI §7.0 cost model constants (BBSSD approximation) */
#define WFQ_FIXED_OP_NS      10000ULL         /* 10 µs per-op floor */
#define WFQ_BW_READ_BPS      4294967296ULL    /* 4 GiB/s */
#define WFQ_BW_WRITE_BPS     2147483648ULL    /* 2 GiB/s */

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

/* Select and submit the minimum-F non-empty class. Caller holds cdl_lock. */
static void wfq_dispatch_locked(FemuCtrl *n)
{
    int best   = -1;
    uint64_t min_f = UINT64_MAX;

    for (int c = 1; c < CDL_MAX_CLASSES; c++) {
        if (!QTAILQ_EMPTY(&n->wfq_queues[c])) {
            NvmeRequest *h = QTAILQ_FIRST(&n->wfq_queues[c]);
            /* strict less-than: lower class index breaks ties (ABI §7.2) */
            if (h->wfq_virt_finish < min_f) {
                min_f = h->wfq_virt_finish;
                best  = c;
            }
        }
    }

    if (best == -1) {
        return;
    }

    NvmeRequest *req = QTAILQ_FIRST(&n->wfq_queues[best]);
    QTAILQ_REMOVE(&n->wfq_queues[best], req, wfq_entry);

    n->wfq_v_clock   = req->wfq_virt_finish;   /* advance virtual clock */
    n->wfq_in_flight = req;

    /* Reset stime/expire_time to dispatch time so FTL latency is relative
     * to when WFQ actually sends the request, not SQE submission time.
     * Must match nvme_io_clock(n) = CLOCK_REALTIME (test_oc12_clock=false). */
    req->stime = req->expire_time =
        qemu_clock_get_ns(n->test_oc12_clock ? QEMU_CLOCK_VIRTUAL
                                              : QEMU_CLOCK_REALTIME);

    int rc = femu_ring_enqueue(n->to_ftl[req->wfq_poller_idx], (void *)&req, 1);
    if (rc != 1) {
        femu_err("wfq: FTL ring enqueue failed (ring full), deferring\n");
        QTAILQ_INSERT_HEAD(&n->wfq_queues[best], req, wfq_entry);
        n->wfq_in_flight = NULL;
        return;
    }
}

/*
 * wfq_defer — called by the poller via req->defer when CDL is enabled and
 * the SQ has a non-zero class.  Assigns SCFQ tags and enqueues into the
 * class FIFO; dispatches immediately if the backend is idle.
 */
void wfq_defer(FemuCtrl *n, NvmeRequest *req, int poller)
{
    uint8_t  cls    = req->sq->cdl_class;
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
    req->wfq_cdl_class    = cls;
    req->wfq_poller_idx   = poller;
    n->wfq_f_last[cls]    = f;

    QTAILQ_INSERT_TAIL(&n->wfq_queues[cls], req, wfq_entry);

    if (!n->wfq_in_flight) {
        wfq_dispatch_locked(n);
    }

    pthread_spin_unlock(&n->cdl_lock);
}

/*
 * wfq_on_complete — called from nvme_process_cq_cpl() for every request
 * that has wfq_cdl_class != 0.  Records miss/completion stats, releases
 * the in-flight slot, and triggers the next dispatch.
 */
void wfq_on_complete(FemuCtrl *n, NvmeRequest *req)
{
    uint8_t cls = req->wfq_cdl_class;
    if (!cls) {
        return;
    }

    int64_t  now     = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t  elapsed = now - req->wfq_ingestion_ns;
    NvmeCdlDescriptor *desc = req->is_write
                              ? &n->cdl_write[cls] : &n->cdl_read[cls];

    pthread_spin_lock(&n->cdl_lock);

    n->cdl_stats[cls].completions++;
    if (desc->duration_ns > 0 && (uint64_t)elapsed > desc->duration_ns) {
        n->cdl_stats[cls].miss_count++;
        n->cdl_stats[cls].last_miss_ns = (uint64_t)now;
    }

    n->wfq_in_flight = NULL;
    wfq_dispatch_locked(n);

    pthread_spin_unlock(&n->cdl_lock);
}

/* Initialise per-class FIFOs — called from femu.c realize. */
void wfq_init(FemuCtrl *n)
{
    for (int c = 0; c < CDL_MAX_CLASSES; c++) {
        QTAILQ_INIT(&n->wfq_queues[c]);
    }
    n->wfq_v_clock   = 0;
    n->wfq_in_flight = NULL;
    memset(n->wfq_f_last, 0, sizeof(n->wfq_f_last));
}
