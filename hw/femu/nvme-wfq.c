// SPDX-License-Identifier: GPL-2.0-only
/* TP4221 CDL v2 scheduling, timing, and persistent accounting. */

#include "nvme.h"
#include "qemu/error-report.h"
#include "trace.h"

#define CDL_STORE_HEADER_BYTES 96
#define CDL_STORE_ENTRY_BYTES  32
#define CDL_STORE_VERSION      2
#define CDL_TXN_VERSION        3
#define CDL_TXN_BYTES          96

static bool cdl_bytes_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i]) {
            return false;
        }
    }
    return true;
}

static uint64_t cdl_sat_inc(uint64_t value, bool *saturated)
{
    if (value >= UINT64_MAX - 1) {
        *saturated = true;
        return UINT64_MAX;
    }
    return value + 1;
}

static void cdl_digest(const uint8_t *buf, size_t prefix, size_t suffix_off,
                       size_t len, uint8_t digest[32])
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize digest_len = 32;

    g_checksum_update(sum, buf, prefix);
    if (suffix_off < len) {
        g_checksum_update(sum, buf + suffix_off, len - suffix_off);
    }
    g_checksum_get_digest(sum, digest, &digest_len);
    g_checksum_free(sum);
}

int64_t cdl_clock_now_ns(FemuCtrl *n)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t offset = qatomic_read(&n->cdl_test_clock_offset_ns);

    if ((offset > 0 && now > INT64_MAX - offset) ||
        (offset < 0 && now < INT64_MIN - offset)) {
        n->cdl_store_failed = true;
        n->bar.csts |= NVME_CSTS_FAILED;
        return offset > 0 ? INT64_MAX : INT64_MIN;
    }
    return now + offset;
}

void cdl_test_advance(FemuCtrl *n, NvmeCdlTestDelay stage)
{
    uint64_t delay;
    int64_t offset;

    if (stage >= CDL_TEST_DELAY_MAX) {
        return;
    }
    delay = qatomic_xchg(&n->cdl_test_delay_ns[stage], 0);
    if (!delay) {
        return;
    }
    offset = qatomic_read(&n->cdl_test_clock_offset_ns);
    if (delay > INT64_MAX || offset > INT64_MAX - (int64_t)delay) {
        n->cdl_store_failed = true;
        n->bar.csts |= NVME_CSTS_FAILED;
        return;
    }
    qatomic_set(&n->cdl_test_clock_offset_ns, offset + delay);
}

bool cdl_test_take_execution_status(FemuCtrl *n, uint16_t *status)
{
    bool valid;

    pthread_spin_lock(&n->cdl_lock);
    valid = n->cdl_test_execution_status_valid;
    if (valid) {
        *status = n->cdl_test_execution_status;
        n->cdl_test_execution_status_valid = false;
    }
    pthread_spin_unlock(&n->cdl_lock);
    return valid;
}

static bool cdl_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;

    errno = 0;
    *value = g_ascii_strtoull(text, &end, 0);
    return !errno && end && *end == '\0' && end != text;
}

static bool cdl_parse_i64(const char *text, int64_t *value)
{
    char *end = NULL;

    errno = 0;
    *value = g_ascii_strtoll(text, &end, 0);
    return !errno && end && *end == '\0' && end != text;
}

static int cdl_test_delay_name(const char *name)
{
    static const char *const names[CDL_TEST_DELAY_MAX] = {
        [CDL_TEST_DELAY_FETCH] = "fetch",
        [CDL_TEST_DELAY_VALIDATION] = "validation",
        [CDL_TEST_DELAY_QUEUE] = "queue",
        [CDL_TEST_DELAY_BACKEND] = "backend",
        [CDL_TEST_DELAY_DURABLE] = "durable",
        [CDL_TEST_DELAY_CQ] = "cq",
    };

    for (int i = 0; i < CDL_TEST_DELAY_MAX; i++) {
        if (!strcmp(name, names[i])) {
            return i;
        }
    }
    return -1;
}

static int cdl_barrier_name(const char *name)
{
    static const char *const names[] = {
        "none", "fetch", "before-admission", "after-admission",
        "check", "durable", "cq", "dispatch", "post-cq",
    };

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (!strcmp(name, names[i])) {
            return i;
        }
    }
    return -1;
}

/* No controller, scheduler, store or completion lock may be held here. */
bool cdl_test_barrier_wait(FemuCtrl *n, NvmeCdlTestBarrier stage)
{
    if (qatomic_read(&n->cdl_test_cut_stage) == stage) {
        fflush(NULL);
        _exit(86);
    }
    if (qatomic_read(&n->cdl_test_barrier) != stage) {
        return true;
    }
    qatomic_set(&n->cdl_test_barrier_reached, stage);
    while (qatomic_read(&n->cdl_test_barrier) == stage) {
        /* Delete/reset must be able to stop and retire the held owner. */
        if (!qatomic_read(&n->dataplane_started) || n->ftl_stopping) {
            return false;
        }
        g_usleep(100);
    }
    return true;
}

static NvmeCdlTestFault cdl_test_fault_name(const char *name)
{
    static const struct {
        const char *name;
        NvmeCdlTestFault point;
    } names[] = {
        { "txn-open", CDL_TEST_FAULT_TXN_OPEN },
        { "txn-write", CDL_TEST_FAULT_TXN_WRITE },
        { "txn-fsync", CDL_TEST_FAULT_TXN_FSYNC },
        { "txn-rename", CDL_TEST_FAULT_TXN_RENAME },
        { "txn-dirsync", CDL_TEST_FAULT_TXN_DIRSYNC },
        { "store-open", CDL_TEST_FAULT_STORE_OPEN },
        { "store-write", CDL_TEST_FAULT_STORE_WRITE },
        { "store-fsync", CDL_TEST_FAULT_STORE_FSYNC },
        { "store-rename", CDL_TEST_FAULT_STORE_RENAME },
        { "store-dirsync", CDL_TEST_FAULT_STORE_DIRSYNC },
        { "after-intent", CDL_TEST_FAULT_AFTER_INTENT },
        { "after-store", CDL_TEST_FAULT_AFTER_STORE },
        { "before-unlink", CDL_TEST_FAULT_BEFORE_UNLINK },
        { "after-unlink", CDL_TEST_FAULT_AFTER_UNLINK },
    };

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (!strcmp(name, names[i].name)) {
            return names[i].point;
        }
    }
    return CDL_TEST_FAULT_NONE;
}

void cdl_test_command(FemuCtrl *n, const char *value, Error **errp)
{
    g_auto(GStrv) fields = NULL;
    uint64_t parsed[5];

    if (!n->cdl_profile) {
        error_setg(errp, "x-cdl-test requires cdl=on");
        return;
    }
    if (!strcmp(value, "reset")) {
        memset(n->cdl_test_log_xor_value, 0, sizeof(n->cdl_test_log_xor_value));
        qatomic_set(&n->cdl_test_barrier, CDL_BARRIER_NONE);
        qatomic_set(&n->cdl_test_barrier_reached, CDL_BARRIER_NONE);
        qatomic_set(&n->cdl_test_cut_stage, CDL_BARRIER_NONE);
        qatomic_set(&n->cdl_test_clock_offset_ns, 0);
        qatomic_set(&n->cdl_test_hold_completion_poller, 0);
        for (int i = 0; i < CDL_TEST_DELAY_MAX; i++) {
            qatomic_set(&n->cdl_test_delay_ns[i], 0);
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_test_stop_delta_ns = 0;
        n->cdl_test_stop_delta_valid = false;
        n->cdl_test_execution_status = 0;
        n->cdl_test_execution_status_valid = false;
        n->cdl_test_hold_dispatch = false;
        n->cdl_test_block_poller = 0;
        qatomic_set(&n->cdl_test_fault, CDL_TEST_FAULT_NONE);
        qatomic_set(&n->cdl_test_crash, false);
        memset(n->cdl_test_dispatches, 0,
               n->cdl_num_classes * sizeof(*n->cdl_test_dispatches));
        n->cdl_test_dispatch_total = 0;
        n->cdl_test_dispatch_limit = 0;
        n->cdl_test_dispatch_log_count = 0;
        pthread_spin_unlock(&n->cdl_lock);
        return;
    }
    if (g_str_has_prefix(value, "ring-size=")) {
        uint64_t size;
        bool started;
        bool busy = false;

        if (!cdl_parse_u64(strchr(value, '=') + 1, &size) ||
            size < 2 || size > FEMU_MAX_INF_REQS || (size & (size - 1)) ||
            !n->to_ftl || !n->to_poller) {
            error_setg(errp, "invalid test ring size or uninitialized rings");
            return;
        }
        started = nvme_pause_pollers(n);
        busy = n->cdl_pending || n->wfq_inflight;
        for (uint32_t p = 1; p <= n->nr_pollers; p++) {
            busy |= femu_ring_count(n->to_ftl[p]) ||
                    femu_ring_count(n->to_poller[p]) ||
                    pqueue_size(n->pq[p]) ||
                    !QTAILQ_EMPTY(&n->cpl_backlog[p]) ||
                    !QTAILQ_EMPTY(&n->ftl_backlog[p]);
        }
        if (busy) {
            nvme_resume_pollers(n, started);
            error_setg(errp, "test ring resizing requires no owned I/O");
            return;
        }
        for (uint32_t p = 1; p <= n->nr_pollers; p++) {
            struct rte_ring *backend = femu_ring_create(FEMU_RING_TYPE_MP_SC, size);
            struct rte_ring *completion = femu_ring_create(FEMU_RING_TYPE_MP_SC, size);
            assert(backend && completion);
            femu_ring_free(n->to_ftl[p]);
            femu_ring_free(n->to_poller[p]);
            n->to_ftl[p] = backend;
            n->to_poller[p] = completion;
        }
        nvme_resume_pollers(n, started);
        return;
    }
    if (g_str_has_prefix(value, "hold-completion=")) {
        uint64_t poller;

        if (!cdl_parse_u64(strchr(value, '=') + 1, &poller) ||
            poller > n->nr_pollers) {
            error_setg(errp, "invalid completion poller hold");
            return;
        }
        qatomic_set(&n->cdl_test_hold_completion_poller, poller);
        return;
    }
    if (g_str_has_prefix(value, "pci-xor=")) {
        fields = g_strsplit(strchr(value, '=') + 1, ",", 2);
        if (g_strv_length(fields) != 2 ||
            !cdl_parse_u64(fields[0], &parsed[0]) || parsed[0] >= 4 ||
            !cdl_parse_u64(fields[1], &parsed[1]) || parsed[1] > 255) {
            error_setg(errp, "invalid PCI response fixture");
            return;
        }
        n->parent_obj.config[parsed[0]] ^= parsed[1];
        return;
    }
    if (g_str_has_prefix(value, "identify-xor=")) {
        fields = g_strsplit(strchr(value, '=') + 1, ",", 2);
        if (g_strv_length(fields) != 2 ||
            !cdl_parse_u64(fields[0], &parsed[0]) || parsed[0] >= sizeof(n->id_ctrl) ||
            !cdl_parse_u64(fields[1], &parsed[1]) || parsed[1] > 255) {
            error_setg(errp, "invalid Identify response fixture");
            return;
        }
        ((uint8_t *)&n->id_ctrl)[parsed[0]] ^= parsed[1];
        return;
    }
    if (g_str_has_prefix(value, "log-xor=")) {
        fields = g_strsplit(strchr(value, '=') + 1, ",", 3);
        if (g_strv_length(fields) != 3 ||
            !cdl_parse_u64(fields[0], &parsed[0]) ||
            (parsed[0] != CDL_LOG_PAGE_C0 && parsed[0] != CDL_LOG_PAGE_C1) ||
            !cdl_parse_u64(fields[1], &parsed[1]) || parsed[1] >= CDL_LOG_HEADER_BYTES ||
            !cdl_parse_u64(fields[2], &parsed[2]) || parsed[2] > 255) {
            error_setg(errp, "invalid log response fixture");
            return;
        }
        unsigned index = parsed[0] == CDL_LOG_PAGE_C1;
        n->cdl_test_log_xor_offset[index] = parsed[1];
        n->cdl_test_log_xor_value[index] = parsed[2];
        return;
    }
    if (g_str_has_prefix(value, "barrier=") ||
        g_str_has_prefix(value, "cut-stage=")) {
        bool cut = g_str_has_prefix(value, "cut-stage=");
        int stage = cdl_barrier_name(strchr(value, '=') + 1);

        if (stage < 0) {
            error_setg(errp, "invalid x-cdl-test barrier");
            return;
        }
        if (cut) {
            qatomic_set(&n->cdl_test_cut_stage, stage);
        } else {
            qatomic_set(&n->cdl_test_barrier_reached, CDL_BARRIER_NONE);
            qatomic_set(&n->cdl_test_barrier, stage);
        }
        return;
    }
    if (g_str_has_prefix(value, "dispatch-limit=")) {
        uint64_t limit;

        if (!cdl_parse_u64(value + strlen("dispatch-limit="), &limit)) {
            error_setg(errp, "invalid x-cdl-test dispatch limit");
            return;
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_test_dispatch_limit = limit;
        pthread_spin_unlock(&n->cdl_lock);
        return;
    }
    if (g_str_has_prefix(value, "hold=")) {
        uint64_t hold;

        if (!cdl_parse_u64(value + strlen("hold="), &hold) || hold > 1) {
            error_setg(errp, "invalid x-cdl-test dispatch hold");
            return;
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_test_hold_dispatch = hold;
        pthread_spin_unlock(&n->cdl_lock);
        if (!hold) {
            cdl_dispatch(n);
        }
        return;
    }
    if (g_str_has_prefix(value, "block-poller=")) {
        uint64_t poller;

        if (!cdl_parse_u64(value + strlen("block-poller="), &poller) ||
            poller > n->nr_pollers) {
            error_setg(errp, "invalid x-cdl-test blocked poller");
            return;
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_test_block_poller = poller;
        pthread_spin_unlock(&n->cdl_lock);
        if (!poller) {
            cdl_dispatch(n);
        }
        return;
    }
    if (g_str_has_prefix(value, "execution-status=")) {
        uint64_t status;

        if (!cdl_parse_u64(value + strlen("execution-status="), &status) ||
            status > UINT16_MAX) {
            error_setg(errp, "invalid x-cdl-test execution status");
            return;
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_test_execution_status = status;
        n->cdl_test_execution_status_valid = true;
        pthread_spin_unlock(&n->cdl_lock);
        return;
    }
    if (g_str_has_prefix(value, "time-offset=")) {
        int64_t offset;

        if (!cdl_parse_i64(value + strlen("time-offset="), &offset)) {
            error_setg(errp, "invalid x-cdl-test time offset");
            return;
        }
        qatomic_set(&n->cdl_test_clock_offset_ns, offset);
        return;
    }
    if (g_str_has_prefix(value, "stop-delta=")) {
        int64_t delta;

        if (!cdl_parse_i64(value + strlen("stop-delta="), &delta)) {
            error_setg(errp, "invalid x-cdl-test stop delta");
            return;
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_test_stop_delta_ns = delta;
        n->cdl_test_stop_delta_valid = true;
        pthread_spin_unlock(&n->cdl_lock);
        return;
    }
    if (g_str_has_prefix(value, "delay=")) {
        uint64_t delay;
        int stage;

        fields = g_strsplit(value + strlen("delay="), ",", 2);
        stage = fields[0] && fields[1] ? cdl_test_delay_name(fields[0]) : -1;
        if (stage < 0 || !cdl_parse_u64(fields[1], &delay) ||
            delay > INT64_MAX) {
            error_setg(errp, "invalid x-cdl-test delay");
            return;
        }
        qatomic_set(&n->cdl_test_delay_ns[stage], delay);
        return;
    }
    if (g_str_has_prefix(value, "fail=") ||
        g_str_has_prefix(value, "crash=")) {
        bool crash = value[0] == 'c';
        const char *name = strchr(value, '=') + 1;
        NvmeCdlTestFault point = cdl_test_fault_name(name);

        if (point == CDL_TEST_FAULT_NONE) {
            error_setg(errp, "invalid x-cdl-test fault point '%s'", name);
            return;
        }
        qatomic_set(&n->cdl_test_crash, crash);
        qatomic_set(&n->cdl_test_fault, point);
        return;
    }
    if (g_str_has_prefix(value, "seed=")) {
        g_autoptr(Error) local_err = NULL;

        fields = g_strsplit(value + strlen("seed="), ",", 5);
        if (g_strv_length(fields) != 5) {
            error_setg(errp, "x-cdl-test seed needs class,timed,misses,generation,snapshot");
            return;
        }
        for (int i = 0; i < 5; i++) {
            if (!cdl_parse_u64(fields[i], &parsed[i])) {
                error_setg(errp, "invalid x-cdl-test seed value");
                return;
            }
        }
        if (!parsed[0] || parsed[0] >= n->cdl_num_classes ||
            parsed[2] > parsed[1]) {
            error_setg(errp, "invalid x-cdl-test seed class or counters");
            return;
        }
        pthread_spin_lock(&n->cdl_lock);
        n->cdl_stats[parsed[0]].timed = parsed[1];
        n->cdl_stats[parsed[0]].misses = parsed[2];
        n->cdl_stats[parsed[0]].timed_saturated = parsed[1] == UINT64_MAX;
        n->cdl_stats[parsed[0]].misses_saturated = parsed[2] == UINT64_MAX;
        n->cdl_generation = parsed[3];
        n->cdl_snapshot_seq = parsed[4];
        pthread_spin_unlock(&n->cdl_lock);
        if (!cdl_save_state(n, &local_err)) {
            error_propagate(errp, g_steal_pointer(&local_err));
        }
        return;
    }
    error_setg(errp, "unknown x-cdl-test command '%s'", value);
}

char *cdl_test_state(FemuCtrl *n)
{
    GString *out = g_string_new(NULL);

    if (!n->cdl_profile) {
        return g_string_free(out, false);
    }
    pthread_spin_lock(&n->cdl_lock);
    g_string_append_printf(out,
                           "pending=%u;inflight=%u;hold=%u;blocked=%u;total=%" PRIu64
                           ";limit=%" PRIu64 ";barrier=%u;reached=%u;counts=",
                           n->cdl_pending, n->wfq_inflight,
                           n->cdl_test_hold_dispatch,
                           n->cdl_test_block_poller,
                           n->cdl_test_dispatch_total,
                           n->cdl_test_dispatch_limit,
                           qatomic_read(&n->cdl_test_barrier),
                           qatomic_read(&n->cdl_test_barrier_reached));
    for (uint32_t c = 0; c < n->cdl_num_classes; c++) {
        if (c) {
            g_string_append_c(out, ',');
        }
        g_string_append_printf(out, "%u:%" PRIu64, c,
                               n->cdl_test_dispatches[c]);
    }
    g_string_append(out, ";rings=");
    for (uint32_t p = 1; n->to_ftl && n->to_poller && p <= n->nr_pollers; p++) {
        if (p > 1) {
            g_string_append_c(out, ',');
        }
        g_string_append_printf(out, "%u/%zu/%zu", p,
            femu_ring_count(n->to_ftl[p]), femu_ring_count(n->to_poller[p]));
    }
    g_string_append_printf(out, ";backend-ready=%u;quota=%u/%u;assoc=",
                           qatomic_read(&n->ftl_pending_count),
                           n->cdl_sqs_used, n->cdl_bound_used);
    for (uint32_t c = 1; c < n->cdl_num_classes; c++) {
        g_string_append_printf(out, "%s%u:%u", c == 1 ? "" : ",",
                               c, n->cdl_assoc[c]);
    }
    g_string_append(out, ";order=");
    for (uint32_t i = 0; i < n->cdl_test_dispatch_log_count; i++) {
        NvmeCdlTestDispatch *entry = &n->cdl_test_dispatch_log[i];

        if (i) {
            g_string_append_c(out, ',');
        }
        g_string_append_printf(out, "%u/%u/%u", entry->cls, entry->sqid,
                               entry->cid);
    }
    pthread_spin_unlock(&n->cdl_lock);
    return g_string_free(out, false);
}

static bool cdl_test_inject(FemuCtrl *n, NvmeCdlTestFault point,
                            Error **errp)
{
    if (!n || point == CDL_TEST_FAULT_NONE ||
        qatomic_cmpxchg(&n->cdl_test_fault, point,
                       CDL_TEST_FAULT_NONE) != point) {
        return false;
    }
    if (qatomic_read(&n->cdl_test_crash)) {
        fflush(NULL);
        _exit(86);
    }
    error_setg(errp, "injected CDL durable-store failure at point %u", point);
    return true;
}

static bool cdl_sync_parent(FemuCtrl *n, const char *path,
                            NvmeCdlTestFault point, Error **errp)
{
    g_autofree char *dir = g_path_get_dirname(path);
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    int saved_errno;

    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot open CDL store directory '%s'", dir);
        return false;
    }
    if (cdl_test_inject(n, point, errp)) {
        close(fd);
        return false;
    }
    if (fsync(fd) < 0) {
        saved_errno = errno;
        close(fd);
        error_setg_errno(errp, saved_errno,
                         "cannot synchronize CDL store directory '%s'", dir);
        return false;
    }
    if (close(fd) < 0) {
        error_setg_errno(errp, errno, "cannot close CDL store directory '%s'", dir);
        return false;
    }
    return true;
}

static bool cdl_atomic_write(FemuCtrl *n, const char *path,
                             const uint8_t *buf, size_t len, bool txn,
                             Error **errp)
{
    g_autofree char *tmp = g_strconcat(path, ".XXXXXX", NULL);
    NvmeCdlTestFault open_point = txn ? CDL_TEST_FAULT_TXN_OPEN :
                                               CDL_TEST_FAULT_STORE_OPEN;
    NvmeCdlTestFault write_point = txn ? CDL_TEST_FAULT_TXN_WRITE :
                                                CDL_TEST_FAULT_STORE_WRITE;
    NvmeCdlTestFault fsync_point = txn ? CDL_TEST_FAULT_TXN_FSYNC :
                                                CDL_TEST_FAULT_STORE_FSYNC;
    NvmeCdlTestFault rename_point = txn ? CDL_TEST_FAULT_TXN_RENAME :
                                                 CDL_TEST_FAULT_STORE_RENAME;
    NvmeCdlTestFault dir_point = txn ? CDL_TEST_FAULT_TXN_DIRSYNC :
                                              CDL_TEST_FAULT_STORE_DIRSYNC;
    int fd;
    int saved_errno;

    if (cdl_test_inject(n, open_point, errp)) {
        return false;
    }
    fd = g_mkstemp(tmp);
    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot create temporary CDL store '%s'", path);
        return false;
    }
    if (cdl_test_inject(n, write_point, errp)) {
        close(fd);
        unlink(tmp);
        return false;
    }
    if (qemu_write_full(fd, buf, len) != len) {
        saved_errno = errno;
        close(fd);
        unlink(tmp);
        error_setg_errno(errp, saved_errno, "cannot write CDL store '%s'", path);
        return false;
    }
    if (cdl_test_inject(n, fsync_point, errp)) {
        close(fd);
        unlink(tmp);
        return false;
    }
    if (fsync(fd) < 0) {
        saved_errno = errno;
        close(fd);
        unlink(tmp);
        error_setg_errno(errp, saved_errno, "cannot synchronize CDL store '%s'", path);
        return false;
    }
    if (close(fd) < 0) {
        saved_errno = errno;
        unlink(tmp);
        error_setg_errno(errp, saved_errno, "cannot close CDL store '%s'", path);
        return false;
    }
    if (cdl_test_inject(n, rename_point, errp)) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) < 0) {
        saved_errno = errno;
        unlink(tmp);
        error_setg_errno(errp, saved_errno, "cannot replace CDL store '%s'", path);
        return false;
    }
    return cdl_sync_parent(n, path, dir_point, errp);
}

static uint8_t *cdl_encode_store(FemuCtrl *n, size_t *len_out)
{
    size_t len = CDL_STORE_HEADER_BYTES +
                 (size_t)n->cdl_num_classes * CDL_STORE_ENTRY_BYTES;
    uint8_t *buf = g_malloc0(len);
    uint32_t flags = (n->cdl_saved_enabled_present ? 1u : 0u) |
                     (n->cdl_saved_enabled ? 2u : 0u);

    memcpy(buf, "FEMUCDL2", 8);
    stl_le_p(buf + 8, CDL_STORE_VERSION);
    stl_le_p(buf + 12, n->cdl_num_classes);
    stq_le_p(buf + 16, n->cdl_generation);
    stq_le_p(buf + 24, n->cdl_last_txid);
    memcpy(buf + 32, n->cdl_store_id, sizeof(n->cdl_store_id));
    stl_le_p(buf + 48, flags);
    stl_le_p(buf + 52, len - CDL_STORE_HEADER_BYTES);

    for (uint32_t c = 0; c < n->cdl_num_classes; c++) {
        NvmeCdlDescriptor *desc = &n->cdl_saved_desc[c];
        NvmeCdlClassStats *stats = &n->cdl_stats[c];
        uint8_t *entry = buf + CDL_STORE_HEADER_BYTES +
                         (size_t)c * CDL_STORE_ENTRY_BYTES;

        entry[0] = n->cdl_saved_desc_present[c];
        entry[1] = desc->policy;
        stl_le_p(entry + 4, desc->duration_us);
        entry[8] = (stats->timed_saturated ? 1u : 0u) |
                   (stats->misses_saturated ? 2u : 0u);
        stq_le_p(entry + 16, stats->timed);
        stq_le_p(entry + 24, stats->misses);
    }
    cdl_digest(buf, 64, CDL_STORE_HEADER_BYTES, len, buf + 64);
    *len_out = len;
    return buf;
}

/* Caller holds cdl_store_lock. */
static bool cdl_store_save_locked(FemuCtrl *n, Error **errp)
{
    g_autofree uint8_t *buf = NULL;
    size_t len;

    pthread_spin_lock(&n->cdl_lock);
    buf = cdl_encode_store(n, &len);
    pthread_spin_unlock(&n->cdl_lock);
    return cdl_atomic_write(n, n->cdl_file, buf, len, false, errp);
}

bool cdl_save_state(FemuCtrl *n, Error **errp)
{
    bool ok;

    if (!n->cdl_profile) {
        return true;
    }
    qemu_mutex_lock(&n->cdl_store_lock);
    ok = cdl_store_save_locked(n, errp);
    qemu_mutex_unlock(&n->cdl_store_lock);
    if (!ok) {
        n->cdl_store_failed = true;
        n->bar.csts |= NVME_CSTS_FAILED;
    }
    return ok;
}

static bool cdl_read_whole(const char *path, uint8_t **buf_out, size_t *len_out,
                           bool *missing, Error **errp)
{
    GByteArray *array = g_byte_array_new();
    uint8_t block[4096];
    int fd = open(path, O_RDONLY);
    ssize_t got;

    *missing = false;
    if (fd < 0) {
        if (errno == ENOENT) {
            *missing = true;
            g_byte_array_unref(array);
            return true;
        }
        error_setg_errno(errp, errno, "cannot read CDL store '%s'", path);
        g_byte_array_unref(array);
        return false;
    }
    while ((got = read(fd, block, sizeof(block))) > 0) {
        g_byte_array_append(array, block, got);
    }
    if (got < 0) {
        error_setg_errno(errp, errno, "cannot read CDL store '%s'", path);
        close(fd);
        g_byte_array_unref(array);
        return false;
    }
    close(fd);
    *len_out = array->len;
    *buf_out = g_byte_array_free(array, false);
    return true;
}

static bool cdl_decode_store(FemuCtrl *n, const uint8_t *buf, size_t len,
                             Error **errp)
{
    uint8_t digest[32];
    size_t expected = CDL_STORE_HEADER_BYTES +
                      (size_t)n->cdl_num_classes * CDL_STORE_ENTRY_BYTES;
    uint32_t flags;

    if (len != expected || memcmp(buf, "FEMUCDL2", 8) ||
        ldl_le_p(buf + 8) != CDL_STORE_VERSION ||
        ldl_le_p(buf + 12) != n->cdl_num_classes ||
        ldl_le_p(buf + 52) != len - CDL_STORE_HEADER_BYTES) {
        error_setg(errp, "invalid or incompatible CDL store '%s'", n->cdl_file);
        return false;
    }
    cdl_digest(buf, 64, CDL_STORE_HEADER_BYTES, len, digest);
    if (memcmp(digest, buf + 64, sizeof(digest))) {
        error_setg(errp, "corrupt CDL store '%s'", n->cdl_file);
        return false;
    }
    memcpy(n->cdl_store_id, buf + 32, sizeof(n->cdl_store_id));
    if (cdl_bytes_zero(n->cdl_store_id, sizeof(n->cdl_store_id))) {
        error_setg(errp, "CDL store '%s' has a zero identity", n->cdl_file);
        return false;
    }
    n->cdl_generation = ldq_le_p(buf + 16);
    n->cdl_last_txid = ldq_le_p(buf + 24);
    flags = ldl_le_p(buf + 48);
    if ((flags & ~3u) || (flags & 2u && !(flags & 1u)) ||
        !cdl_bytes_zero(buf + 56, 8)) {
        error_setg(errp, "CDL store '%s' has reserved flags", n->cdl_file);
        return false;
    }
    n->cdl_saved_enabled_present = flags & 1u;
    n->cdl_saved_enabled = flags & 2u;

    for (uint32_t c = 0; c < n->cdl_num_classes; c++) {
        const uint8_t *entry = buf + CDL_STORE_HEADER_BYTES +
                               (size_t)c * CDL_STORE_ENTRY_BYTES;
        uint8_t stats_flags = entry[8];

        if ((entry[0] & ~1u) || stats_flags & ~3u ||
            !cdl_bytes_zero(entry + 2, 2) ||
            !cdl_bytes_zero(entry + 9, 7) ||
            (!entry[0] && (entry[1] || ldl_le_p(entry + 4)))) {
            error_setg(errp, "CDL store '%s' has reserved entry data", n->cdl_file);
            return false;
        }
        n->cdl_saved_desc_present[c] = entry[0];
        n->cdl_saved_desc[c].policy = entry[1];
        n->cdl_saved_desc[c].duration_us = ldl_le_p(entry + 4);
        if (c == 0 && (entry[0] || entry[1] || ldl_le_p(entry + 4) ||
                       stats_flags || ldq_le_p(entry + 16) ||
                       ldq_le_p(entry + 24))) {
            error_setg(errp, "CDL store '%s' has nonzero bypass state", n->cdl_file);
            return false;
        }
        if (entry[0] && (entry[1] != CDL_POLICY_BEST_EFFORT ||
                         !ldl_le_p(entry + 4))) {
            error_setg(errp, "CDL store '%s' has an invalid descriptor", n->cdl_file);
            return false;
        }
        n->cdl_stats[c].timed_saturated = stats_flags & 1u;
        n->cdl_stats[c].misses_saturated = stats_flags & 2u;
        n->cdl_stats[c].timed = ldq_le_p(entry + 16);
        n->cdl_stats[c].misses = ldq_le_p(entry + 24);
        if ((n->cdl_stats[c].timed_saturated &&
             n->cdl_stats[c].timed != UINT64_MAX) ||
            (n->cdl_stats[c].misses_saturated &&
             n->cdl_stats[c].misses != UINT64_MAX) ||
            (n->cdl_stats[c].timed == UINT64_MAX &&
             !n->cdl_stats[c].timed_saturated) ||
            (n->cdl_stats[c].misses == UINT64_MAX &&
             !n->cdl_stats[c].misses_saturated) ||
            n->cdl_stats[c].misses > n->cdl_stats[c].timed) {
            error_setg(errp, "CDL store '%s' has inconsistent saturation", n->cdl_file);
            return false;
        }
    }
    return true;
}

static char *cdl_txn_path(FemuCtrl *n)
{
    return g_strconcat(n->cdl_file, ".txn", NULL);
}

static bool cdl_write_txn(FemuCtrl *n, uint64_t txid, NvmeRequest *req,
                          Error **errp)
{
    g_autofree char *path = cdl_txn_path(n);
    uint8_t buf[CDL_TXN_BYTES] = { 0 };

    memcpy(buf, "CDL2TXN\0", 8);
    stl_le_p(buf + 8, CDL_TXN_VERSION);
    buf[12] = req->wfq_cdl_class;
    buf[13] = req->cdl_miss;
    stq_le_p(buf + 16, txid);
    memcpy(buf + 24, n->cdl_store_id, sizeof(n->cdl_store_id));
    stq_le_p(buf + 40, req->cdl_attempt);
    stq_le_p(buf + 48, req->cdl_duration_ns);
    stq_le_p(buf + 56, req->cdl_elapsed_ns);
    cdl_digest(buf, 64, sizeof(buf), sizeof(buf), buf + 64);
    return cdl_atomic_write(n, path, buf, sizeof(buf), true, errp);
}

/* Caller holds cdl_store_lock. */
static bool cdl_replay_txn_locked(FemuCtrl *n, Error **errp)
{
    g_autofree char *path = cdl_txn_path(n);
    g_autofree uint8_t *buf = NULL;
    uint8_t digest[32];
    size_t len = 0;
    bool missing;
    uint64_t txid;
    uint8_t cls;
    bool miss;

    if (!cdl_read_whole(path, &buf, &len, &missing, errp) || missing) {
        return missing;
    }
    if (len != CDL_TXN_BYTES) {
        error_setg(errp, "corrupt CDL transaction '%s'", path);
        return false;
    }
    cdl_digest(buf, 64, len, len, digest);
    if (memcmp(buf, "CDL2TXN\0", 8) ||
        ldl_le_p(buf + 8) != CDL_TXN_VERSION ||
        memcmp(digest, buf + 64, sizeof(digest)) || buf[13] > 1 ||
        !cdl_bytes_zero(buf + 14, 2) ||
        memcmp(buf + 24, n->cdl_store_id, sizeof(n->cdl_store_id)) ||
        !ldq_le_p(buf + 40) || !ldq_le_p(buf + 48) ||
        ldq_le_p(buf + 56) > INT64_MAX ||
        buf[13] != (ldq_le_p(buf + 56) > ldq_le_p(buf + 48))) {
        error_setg(errp, "corrupt CDL transaction '%s'", path);
        return false;
    }
    cls = buf[12];
    miss = buf[13];
    txid = ldq_le_p(buf + 16);
    if (!cls || cls >= n->cdl_num_classes || txid == 0 ||
        (txid > n->cdl_last_txid && txid != n->cdl_last_txid + 1)) {
        error_setg(errp, "invalid CDL transaction '%s'", path);
        return false;
    }
    if (txid > n->cdl_last_txid) {
        NvmeCdlClassStats *stats = &n->cdl_stats[cls];

        stats->timed = cdl_sat_inc(stats->timed, &stats->timed_saturated);
        if (miss) {
            stats->misses = cdl_sat_inc(stats->misses,
                                        &stats->misses_saturated);
        }
        n->cdl_last_txid = txid;
        if (!cdl_store_save_locked(n, errp)) {
            return false;
        }
    }
    if (unlink(path) < 0 && errno != ENOENT) {
        error_setg_errno(errp, errno, "cannot remove CDL transaction '%s'", path);
        return false;
    }
    return cdl_sync_parent(n, path, CDL_TEST_FAULT_NONE, errp);
}

bool cdl_init(FemuCtrl *n, Error **errp)
{
    g_autofree uint8_t *buf = NULL;
    g_autoptr(Error) local_err = NULL;
    size_t len = 0;
    bool missing;
    bool restored = false;

    qemu_mutex_init(&n->cdl_store_lock);
    QTAILQ_INIT(&n->wfq_inflight_list);
    if (!n->cdl_profile) {
        return true;
    }
    if (!n->cdl_file || !*n->cdl_file) {
        error_setg(errp, "cdl=on requires cdl_file for durable state");
        return false;
    }
    if (n->vid != 0x8086 || n->did != 0x5845) {
        error_setg(errp, "cdl=on requires vid=0x8086 and did=0x5845");
        return false;
    }
    if (!(n->oncs & NVME_ONCS_SSFS)) {
        error_setg(errp, "cdl=on requires ONCS.SSFS");
        return false;
    }
    if (n->femu_mode == FEMU_OCSSD_MODE) {
        error_setg(errp, "cdl=on does not support OpenChannel command staging");
        return false;
    }
    if (n->cdl_num_classes < 1 || n->cdl_num_classes > CDL_MAX_CLASSES) {
        error_setg(errp, "cdl_classes must be between 1 and 256");
        return false;
    }
    /* The transport constraint has already bounded queues to 1..2047. */
    n->cdl_queue_caps.max_io_sqs = n->nr_io_queues;
    n->cdl_queue_caps.max_io_cqs = n->nr_io_queues;
    NvmeCdlQueueCaps *caps = &n->cdl_queue_caps;

    if (n->cdl_num_classes == 1) {
        if (caps->max_cdl_sqs || caps->sqs_per_class || caps->bound_classes) {
            error_setg(errp, "cdl_classes=1 requires all three CDL SQ limits=0");
            return false;
        }
    } else if (!caps->bound_classes ||
               caps->bound_classes >= n->cdl_num_classes ||
               !caps->sqs_per_class ||
               caps->sqs_per_class > caps->max_cdl_sqs ||
               caps->max_cdl_sqs > caps->max_io_sqs ||
               caps->max_cdl_sqs > (uint32_t)caps->bound_classes *
                                   caps->sqs_per_class) {
        error_setg(errp, "invalid CDL queue capacities: require 1 <= "
                   "cdl_bound_classes < cdl_classes, 1 <= cdl_sqs_per_class "
                   "<= cdl_max_sqs <= queues, and cdl_max_sqs <= "
                   "cdl_bound_classes * cdl_sqs_per_class");
        return false;
    }
    if (n->wfq_depth < 1 || n->wfq_depth > FEMU_MAX_INF_REQS) {
        error_setg(errp, "wfq_depth must be between 1 and %u", FEMU_MAX_INF_REQS);
        return false;
    }

    n->cdl_desc = g_new0(NvmeCdlDescriptor, n->cdl_num_classes);
    n->cdl_saved_desc = g_new0(NvmeCdlDescriptor, n->cdl_num_classes);
    n->cdl_saved_desc_present = g_new0(bool, n->cdl_num_classes);
    n->cdl_stats = g_new0(NvmeCdlClassStats, n->cdl_num_classes);
    n->cdl_assoc = g_new0(uint32_t, n->cdl_num_classes);
    n->cdl_inflight_by_class = g_new0(uint32_t, n->cdl_num_classes);
    n->cdl_dispatch_wait = g_new0(uint16_t, n->cdl_num_classes);
    n->wfq_f_last = g_new0(uint64_t, n->cdl_num_classes);
    n->cdl_rr_next = g_new0(uint16_t, n->cdl_num_classes);
    n->cdl_rr_started = g_new0(bool, n->cdl_num_classes);
    n->cdl_test_dispatches = g_new0(uint64_t, n->cdl_num_classes);
    n->cdl_test_dispatch_log = g_new0(NvmeCdlTestDispatch,
                                      CDL_TEST_DISPATCH_LOG_ENTRIES);
    wfq_init(n);

    qemu_mutex_lock(&n->cdl_store_lock);
    if (!cdl_read_whole(n->cdl_file, &buf, &len, &missing, &local_err)) {
        goto store_fault;
    }
    if (missing) {
        g_autofree char *txn_path = cdl_txn_path(n);
        g_autofree uint8_t *txn = NULL;
        size_t txn_len = 0;
        bool txn_missing;
        QemuUUID id;

        if (!cdl_read_whole(txn_path, &txn, &txn_len, &txn_missing,
                            &local_err)) {
            goto store_fault;
        }
        if (!txn_missing) {
            error_setg(&local_err,
                       "CDL store '%s' is missing while transaction '%s' exists",
                       n->cdl_file, txn_path);
            goto store_fault;
        }
        qemu_uuid_generate(&id);
        memcpy(n->cdl_store_id, id.data, sizeof(n->cdl_store_id));
        if (!cdl_store_save_locked(n, &local_err)) {
            goto store_fault;
        }
    } else if (!cdl_decode_store(n, buf, len, &local_err) ||
               !cdl_replay_txn_locked(n, &local_err)) {
        goto store_fault;
    }

    for (size_t i = 0; i < sizeof(n->cdl_store_id); i++) {
        snprintf(n->cdl_trace_id + i * 2, 3, "%02x", n->cdl_store_id[i]);
    }
    n->cdl_enabled = n->cdl_saved_enabled_present ?
                     n->cdl_saved_enabled : false;
    for (uint32_t c = 1; c < n->cdl_num_classes; c++) {
        if (n->cdl_saved_desc_present[c]) {
            n->cdl_desc[c] = n->cdl_saved_desc[c];
            restored = true;
        }
    }
    restored |= n->cdl_enabled;
    if (restored && n->cdl_generation != UINT64_MAX) {
        n->cdl_generation++;
        if (!cdl_store_save_locked(n, &local_err)) {
            goto store_fault;
        }
    }
    qemu_mutex_unlock(&n->cdl_store_lock);
    return true;

store_fault:
    n->cdl_store_failed = true;
    qemu_mutex_unlock(&n->cdl_store_lock);
    error_report_err(g_steal_pointer(&local_err));
    return true;
}

void cdl_exit(FemuCtrl *n)
{
    g_clear_pointer(&n->cdl_desc, g_free);
    g_clear_pointer(&n->cdl_saved_desc, g_free);
    g_clear_pointer(&n->cdl_saved_desc_present, g_free);
    g_clear_pointer(&n->cdl_stats, g_free);
    g_clear_pointer(&n->cdl_assoc, g_free);
    g_clear_pointer(&n->cdl_inflight_by_class, g_free);
    g_clear_pointer(&n->cdl_dispatch_wait, g_free);
    g_clear_pointer(&n->wfq_f_last, g_free);
    g_clear_pointer(&n->cdl_rr_next, g_free);
    g_clear_pointer(&n->cdl_rr_started, g_free);
    g_clear_pointer(&n->cdl_test_dispatches, g_free);
    g_clear_pointer(&n->cdl_test_dispatch_log, g_free);
    qemu_mutex_destroy(&n->cdl_store_lock);
}

void cdl_reset(FemuCtrl *n)
{
    bool changed = false;
    Error *err = NULL;

    if (!n->cdl_profile || n->cdl_store_failed) {
        return;
    }
    qemu_mutex_lock(&n->cdl_store_lock);
    pthread_spin_lock(&n->cdl_lock);
    /* nvme_clear_ctrl() retires every old queue before state restoration. */
    assert(!n->cdl_sqs_used && !n->cdl_bound_used);
    bool enable = n->cdl_saved_enabled_present ? n->cdl_saved_enabled : false;
    if (n->cdl_enabled != enable) {
        n->cdl_enabled = enable;
        changed = true;
    }
    for (uint32_t c = 1; c < n->cdl_num_classes; c++) {
        NvmeCdlDescriptor desc = n->cdl_saved_desc_present[c] ?
                                 n->cdl_saved_desc[c] :
                                 (NvmeCdlDescriptor) { 0 };
        if (memcmp(&n->cdl_desc[c], &desc, sizeof(desc))) {
            n->cdl_desc[c] = desc;
            changed = true;
        }
    }
    if (changed && n->cdl_generation != UINT64_MAX) {
        n->cdl_generation++;
    }
    pthread_spin_unlock(&n->cdl_lock);
    if (changed && !cdl_store_save_locked(n, &err)) {
        n->cdl_store_failed = true;
        n->bar.csts |= NVME_CSTS_FAILED;
        error_report_err(err);
    }
    qemu_mutex_unlock(&n->cdl_store_lock);
}

static uint64_t cdl_cost_ns(NvmeRequest *req)
{
    uint64_t bytes = 4096;

    if (req->ns && (req->cmd_opcode == NVME_CMD_READ ||
                    req->cmd_opcode == NVME_CMD_WRITE)) {
        uint8_t lbads = req->ns->id_ns.lbaf[
            NVME_ID_NS_FLBAS_INDEX(req->ns->id_ns.flbas)].lbads;
        bytes = (uint64_t)req->nlb << lbads;
    }
    return 10000 + DIV_ROUND_UP(bytes * 1000000000ULL,
                                req->is_write ? 2147483648ULL : 4294967296ULL);
}

static NvmeRequest *cdl_class_candidate(FemuCtrl *n, uint8_t cls,
                                        const bool *blocked)
{
    uint16_t start = n->cdl_rr_next[cls];

    if (!start) {
        start = 1;
    }
    for (uint32_t step = 0; step < n->nr_io_queues; step++) {
        uint16_t qid = 1 + (start - 1 + step) % n->nr_io_queues;
        NvmeSQueue *sq = n->sq[qid];

        if (!sq || !sq->is_active || sq->cdl_class != cls ||
            QTAILQ_EMPTY(&sq->cdl_pending)) {
            continue;
        }
        NvmeRequest *req = QTAILQ_FIRST(&sq->cdl_pending);
        if (!blocked || !blocked[req->wfq_poller_idx]) {
            return req;
        }
    }
    return NULL;
}

static uint16_t cdl_next_member(FemuCtrl *n, uint8_t cls, uint16_t after)
{
    for (uint32_t step = 1; step <= n->nr_io_queues; step++) {
        uint16_t qid = 1 + (after - 1 + step) % n->nr_io_queues;
        NvmeSQueue *sq = n->sq[qid];

        if (sq && sq->is_active && sq->cdl_class == cls) {
            return qid;
        }
    }
    return 0;
}

static uint64_t cdl_finish_tag(FemuCtrl *n, uint8_t cls, NvmeRequest *req)
{
    __uint128_t inc = (__uint128_t)cdl_cost_ns(req) *
                      n->cdl_desc[cls].duration_us;

    if (inc > UINT64_MAX - n->wfq_f_last[cls]) {
        return UINT64_MAX;
    }
    return n->wfq_f_last[cls] + (uint64_t)inc;
}

static void cdl_rebase_locked(FemuCtrl *n)
{
    uint64_t base = n->wfq_v_clock;

    if (base <= UINT64_MAX / 4) {
        return;
    }
    for (uint32_t c = 1; c < n->cdl_num_classes; c++) {
        n->wfq_f_last[c] = n->wfq_f_last[c] > base ?
                            n->wfq_f_last[c] - base : 0;
    }
    n->wfq_v_clock = 0;
}

static void cdl_dispatch_locked(FemuCtrl *n)
{
    g_autofree bool *blocked = NULL;

    if (!qatomic_read(&n->dataplane_started) || n->cdl_store_failed ||
        qatomic_read(&n->cdl_test_barrier) == CDL_BARRIER_AFTER_ADMISSION ||
        n->cdl_test_hold_dispatch ||
        (n->cdl_test_dispatch_limit &&
         n->cdl_test_dispatch_total >= n->cdl_test_dispatch_limit) ||
        !n->cdl_pending ||
        n->wfq_inflight >= n->wfq_depth) {
        return;
    }
    blocked = g_new0(bool, n->nr_pollers + 1);
    if (n->ftl_backlog_count) {
        for (uint32_t p = 1; p <= n->nr_pollers; p++) {
            blocked[p] = qatomic_read(&n->ftl_backlog_count[p]) != 0;
        }
    }

    while (n->wfq_inflight < n->wfq_depth) {
        NvmeRequest *best_req = NULL;
        uint64_t best_finish = UINT64_MAX;
        uint8_t best_class = 0;
        uint16_t best_wait = 0;
        bool force_progress = false;

        cdl_rebase_locked(n);

        for (uint32_t c = 1; c < n->cdl_num_classes; c++) {
            NvmeRequest *req = cdl_class_candidate(n, c, blocked);
            uint64_t finish;

            if (!req) {
                continue;
            }
            finish = cdl_finish_tag(n, c, req);
            if (n->cdl_dispatch_wait[c] >= 999) {
                if (!force_progress || n->cdl_dispatch_wait[c] > best_wait ||
                    (n->cdl_dispatch_wait[c] == best_wait && c < best_class)) {
                    force_progress = true;
                    best_wait = n->cdl_dispatch_wait[c];
                    best_req = req;
                    best_finish = finish;
                    best_class = c;
                }
            } else if (!force_progress &&
                       (!best_req || finish < best_finish ||
                        (finish == best_finish && c < best_class))) {
                best_req = req;
                best_finish = finish;
                best_class = c;
            }
        }
        if (!best_req) {
            break;
        }

        NvmeSQueue *sq = best_req->sq;
        QTAILQ_REMOVE(&sq->cdl_pending, best_req, wfq_entry);
        if (n->cdl_test_block_poller == best_req->wfq_poller_idx ||
            femu_ring_enqueue(n->to_ftl[best_req->wfq_poller_idx],
                              (void *)&best_req, 1) != 1) {
            QTAILQ_INSERT_HEAD(&sq->cdl_pending, best_req, wfq_entry);
            blocked[best_req->wfq_poller_idx] = true;
            continue;
        }
        assert(n->cdl_pending);
        n->cdl_pending--;

        best_req->wfq_virt_finish = best_finish;
        best_req->stime = best_req->expire_time =
            qemu_clock_get_ns(n->test_oc12_clock ? QEMU_CLOCK_VIRTUAL :
                                                   QEMU_CLOCK_REALTIME);
        n->wfq_f_last[best_class] = best_finish;
        n->cdl_rr_started[best_class] = true;
        n->cdl_rr_next[best_class] =
            cdl_next_member(n, best_class, sq->sqid);
        n->wfq_inflight++;
        n->cdl_inflight_by_class[best_class]++;
        n->cdl_test_dispatches[best_class]++;
        n->cdl_test_dispatch_total++;
        if (n->cdl_test_dispatch_log_count <
            CDL_TEST_DISPATCH_LOG_ENTRIES) {
            NvmeCdlTestDispatch *entry = &n->cdl_test_dispatch_log[
                n->cdl_test_dispatch_log_count++];

            entry->sqid = sq->sqid;
            entry->cid = le16_to_cpu(best_req->cmd.cid);
            entry->cls = best_class;
        }
        QTAILQ_INSERT_TAIL(&n->wfq_inflight_list, best_req,
                           wfq_inflight_entry);
        trace_femu_cdl_dispatch(n->cdl_trace_id, best_req->cdl_attempt,
                               sq->sqid,
                                le16_to_cpu(best_req->cmd.cid), best_class,
                                best_req->wfq_poller_idx,
                                cdl_clock_now_ns(n));

        n->wfq_v_clock = best_finish;
        for (uint32_t c = 1; c < n->cdl_num_classes; c++) {
            if (cdl_class_candidate(n, c, blocked)) {
                n->wfq_v_clock = MIN(n->wfq_v_clock, n->wfq_f_last[c]);
                if (c == best_class) {
                    n->cdl_dispatch_wait[c] = 0;
                } else if (n->cdl_dispatch_wait[c] != UINT16_MAX) {
                    n->cdl_dispatch_wait[c]++;
                }
            }
        }
        if (n->cdl_test_dispatch_limit &&
            n->cdl_test_dispatch_total >= n->cdl_test_dispatch_limit) {
            n->cdl_test_hold_dispatch = true;
            break;
        }
    }
}

void cdl_dispatch(FemuCtrl *n)
{
    if (!n->cdl_profile || !n->to_ftl) {
        return;
    }
    pthread_spin_lock(&n->cdl_lock);
    cdl_dispatch_locked(n);
    pthread_spin_unlock(&n->cdl_lock);
}

bool cdl_sq_reserve(FemuCtrl *n, uint8_t cls)
{
    NvmeCdlQueueCaps *caps = &n->cdl_queue_caps;
    bool allowed;

    assert(n->cdl_profile && cls && cls < n->cdl_num_classes);
    pthread_spin_lock(&n->cdl_lock);
    allowed = n->cdl_desc[cls].duration_us &&
              n->cdl_assoc[cls] < caps->sqs_per_class &&
              n->cdl_sqs_used < caps->max_cdl_sqs &&
              (n->cdl_assoc[cls] || n->cdl_bound_used < caps->bound_classes);
    if (allowed) {
        if (!n->cdl_assoc[cls]++) {
            n->cdl_bound_used++;
        }
        n->cdl_sqs_used++;
    }
    pthread_spin_unlock(&n->cdl_lock);
    return allowed;
}

static void cdl_sq_unreserve_locked(FemuCtrl *n, uint8_t cls)
{
    assert(n->cdl_assoc[cls] && n->cdl_sqs_used && n->cdl_bound_used);
    n->cdl_sqs_used--;
    if (!--n->cdl_assoc[cls]) {
        n->cdl_bound_used--;
    }
}

/* Only an unpublished initialization failure may release before retirement. */
void cdl_sq_unreserve(FemuCtrl *n, uint8_t cls)
{
    pthread_spin_lock(&n->cdl_lock);
    cdl_sq_unreserve_locked(n, cls);
    pthread_spin_unlock(&n->cdl_lock);
}

void cdl_sq_created(FemuCtrl *n, NvmeSQueue *sq)
{
    uint8_t cls = sq->cdl_class;

    if (!n->cdl_profile || !cls) {
        return;
    }
    pthread_spin_lock(&n->cdl_lock);
    assert(sq->cdl_quota_reserved && n->cdl_assoc[cls]);
    if (!n->cdl_rr_next[cls] ||
        (!n->cdl_rr_started[cls] && sq->sqid < n->cdl_rr_next[cls])) {
        n->cdl_rr_next[cls] = sq->sqid;
    }
    pthread_spin_unlock(&n->cdl_lock);
}

void cdl_sq_deleted(FemuCtrl *n, NvmeSQueue *sq)
{
    NvmeRequest *req, *next;
    uint8_t cls = sq->cdl_class;

    if (!n->cdl_profile || !cls) {
        return;
    }
    pthread_spin_lock(&n->cdl_lock);
    QTAILQ_FOREACH_SAFE(req, &sq->cdl_pending, wfq_entry, next) {
        QTAILQ_REMOVE(&sq->cdl_pending, req, wfq_entry);
        assert(n->cdl_pending);
        n->cdl_pending--;
    }
    QTAILQ_FOREACH_SAFE(req, &n->wfq_inflight_list,
                        wfq_inflight_entry, next) {
        if (req->sq == sq) {
            QTAILQ_REMOVE(&n->wfq_inflight_list, req,
                          wfq_inflight_entry);
            req->wfq_orphaned = true;
            n->wfq_inflight--;
            n->cdl_inflight_by_class[cls]--;
        }
    }
    /* Caller paused/drained all old owners before freeing the SQ. */
    assert(sq->cdl_quota_reserved);
    cdl_sq_unreserve_locked(n, cls);
    sq->cdl_quota_reserved = false;
    if (!n->cdl_assoc[cls]) {
        n->cdl_rr_started[cls] = false;
    }
    if (n->cdl_rr_next[cls] == sq->sqid) {
        n->cdl_rr_next[cls] = 0;
        for (uint32_t step = 1; step <= n->nr_io_queues; step++) {
            uint16_t qid = n->cdl_rr_started[cls] ?
                1 + (sq->sqid - 1 + step) % n->nr_io_queues : step;
            NvmeSQueue *candidate = n->sq[qid];

            if (qid != sq->sqid && candidate && candidate->is_active &&
                candidate->cdl_class == cls) {
                n->cdl_rr_next[cls] = qid;
                break;
            }
        }
    }
    cdl_dispatch_locked(n);
    pthread_spin_unlock(&n->cdl_lock);
}

void wfq_defer(FemuCtrl *n, NvmeRequest *req, int poller)
{
    uint8_t cls = req->sq->cdl_class;
    int64_t admission;

    if (!cdl_test_barrier_wait(n, CDL_BARRIER_BEFORE_ADMISSION)) {
        nvme_req_release_ranges(req);
        QTAILQ_INSERT_TAIL(&req->sq->req_list, req, entry);
        return;
    }

    pthread_spin_lock(&n->cdl_lock);
    if (!n->cdl_enabled || !n->cdl_desc[cls].duration_us) {
        req->wfq_cdl_class = 0;
        req->cdl_timed = false;
        pthread_spin_unlock(&n->cdl_lock);
        if (!cdl_test_barrier_wait(n, CDL_BARRIER_AFTER_ADMISSION)) {
            nvme_req_release_ranges(req);
            QTAILQ_INSERT_TAIL(&req->sq->req_list, req, entry);
            return;
        }
        nvme_enqueue_or_backlog(n, req, poller);
        return;
    }
    if (!cdl_class_candidate(n, cls, NULL) &&
        !n->cdl_inflight_by_class[cls]) {
        n->wfq_f_last[cls] = n->wfq_v_clock;
        n->cdl_dispatch_wait[cls] = 0;
    }
    admission = cdl_clock_now_ns(n);
    req->cdl_admitted = true;
    trace_femu_cdl_admission(n->cdl_trace_id, req->cdl_attempt,

        req->sq->sqid, le16_to_cpu(req->cmd.cid), req->cmd_opcode, cls,
        (uint64_t)n->cdl_desc[cls].duration_us * 1000,
        n->cdl_desc[cls].policy, n->cdl_enabled, admission);
    req->wfq_cdl_class = cls;
    req->cdl_duration_ns = (uint64_t)n->cdl_desc[cls].duration_us * 1000;
    req->cdl_policy = n->cdl_desc[cls].policy;
    req->cdl_start_ns = admission;
    req->wfq_poller_idx = poller;
    req->cdl_timed = true;
    req->cdl_checked = false;
    req->cdl_accounted = false;
    req->cdl_cq_delay_applied = false;
    req->wfq_orphaned = false;
    QTAILQ_INSERT_TAIL(&req->sq->cdl_pending, req, wfq_entry);
    n->cdl_pending++;
    cdl_test_advance(n, CDL_TEST_DELAY_QUEUE);
    if (qatomic_read(&n->cdl_test_barrier) != CDL_BARRIER_AFTER_ADMISSION) {
        cdl_dispatch_locked(n);
    }
    pthread_spin_unlock(&n->cdl_lock);
    if (cdl_test_barrier_wait(n, CDL_BARRIER_AFTER_ADMISSION)) {
        cdl_dispatch(n);
    }
}

static void cdl_release_slot(FemuCtrl *n, NvmeRequest *req, uint8_t cls)
{
    pthread_spin_lock(&n->cdl_lock);
    if (cls && !req->wfq_orphaned) {
        QTAILQ_REMOVE(&n->wfq_inflight_list, req, wfq_inflight_entry);
        n->wfq_inflight--;
        n->cdl_inflight_by_class[cls]--;
        req->wfq_orphaned = true;
    }
    cdl_dispatch_locked(n);
    pthread_spin_unlock(&n->cdl_lock);
}

bool cdl_check(FemuCtrl *n, NvmeRequest *req)
{
    int64_t stop;
    int64_t elapsed;
    int64_t stop_delta = 0;
    bool miss;
    bool stop_override = false;
    uint8_t cls = req->wfq_cdl_class;

    if (req->cdl_timed && n->cdl_store_failed) {
        return false;
    }
    if (!req->cdl_timed || req->cdl_accounted) {
        return true;
    }
    if (req->cdl_checked) {
        return true;
    }
    pthread_spin_lock(&n->cdl_lock);
    if (n->cdl_test_stop_delta_valid) {
        stop_delta = n->cdl_test_stop_delta_ns;
        n->cdl_test_stop_delta_valid = false;
        stop_override = true;
    }
    pthread_spin_unlock(&n->cdl_lock);
    if (stop_override) {
        __int128 candidate = (__int128)req->cdl_start_ns + stop_delta;

        if (candidate > INT64_MAX || candidate < INT64_MIN) {
            n->cdl_store_failed = true;
            n->bar.csts |= NVME_CSTS_FAILED;
            cdl_release_slot(n, req, cls);
            return false;
        }
        stop = candidate;
    } else {
        stop = cdl_clock_now_ns(n);
    }
    {
        __int128 elapsed_wide = (__int128)stop - req->cdl_start_ns;

        if (elapsed_wide > INT64_MAX || elapsed_wide < INT64_MIN) {
            n->cdl_store_failed = true;
            n->bar.csts |= NVME_CSTS_FAILED;
            cdl_release_slot(n, req, cls);
            return false;
        }
        elapsed = elapsed_wide;
    }
    if (elapsed < 0) {
        req->cdl_checked = true;
        trace_femu_cdl_completion_check(n->cdl_trace_id, req->cdl_attempt,
                               req->sq->sqid,
                                        le16_to_cpu(req->cmd.cid), cls,
                                        req->cdl_start_ns, stop,
                                        elapsed,
                                        req->cdl_duration_ns, false);
        n->cdl_store_failed = true;
        n->bar.csts |= NVME_CSTS_FAILED;
        cdl_release_slot(n, req, cls);
        return false;
    }
    miss = (uint64_t)elapsed > req->cdl_duration_ns;
    req->cdl_stop_ns = stop;
    req->cdl_elapsed_ns = elapsed;
    req->cdl_miss = miss;
    req->cdl_checked = true;
    trace_femu_cdl_completion_check(n->cdl_trace_id, req->cdl_attempt,
                                    req->sq->sqid,
                                    le16_to_cpu(req->cmd.cid), cls,
                                    req->cdl_start_ns, stop, elapsed,
                                    req->cdl_duration_ns, miss);
    /* The result is frozen before accounting, including on retries. */
    cdl_release_slot(n, req, cls);
    /* Pause/delete must still finish an accounting transaction already begun. */
    cdl_test_barrier_wait(n, CDL_BARRIER_CHECK);
    cdl_test_advance(n, CDL_TEST_DELAY_DURABLE);

    return true;
}

bool cdl_account(FemuCtrl *n, NvmeRequest *req)
{
    g_autofree char *txn_path = NULL;
    Error *err = NULL;
    uint8_t cls = req->wfq_cdl_class;
    uint64_t txid, timed, misses;
    int64_t persist_start;

    if (!cdl_check(n, req)) {
        return false;
    }
    if (!req->cdl_timed || req->cdl_accounted) {
        return true;
    }
    persist_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    qemu_mutex_lock(&n->cdl_store_lock);
    pthread_spin_lock(&n->cdl_lock);
    if (n->cdl_last_txid == UINT64_MAX) {
        pthread_spin_unlock(&n->cdl_lock);
        error_setg(&err, "CDL transaction identity exhausted");
        goto fail;
    }
    txid = n->cdl_last_txid + 1;
    pthread_spin_unlock(&n->cdl_lock);
    if (!cdl_write_txn(n, txid, req, &err)) {
        goto fail;
    }
    if (cdl_test_inject(n, CDL_TEST_FAULT_AFTER_INTENT, &err)) {
        goto fail;
    }

    pthread_spin_lock(&n->cdl_lock);
    NvmeCdlClassStats *stats = &n->cdl_stats[cls];
    stats->timed = cdl_sat_inc(stats->timed, &stats->timed_saturated);
    if (req->cdl_miss) {
        stats->misses = cdl_sat_inc(stats->misses,
                                    &stats->misses_saturated);
    }
    n->cdl_last_txid = txid;
    req->cdl_accounted = true;
    timed = stats->timed;
    misses = stats->misses;
    pthread_spin_unlock(&n->cdl_lock);
    if (!cdl_store_save_locked(n, &err)) {
        goto fail;
    }
    if (cdl_test_inject(n, CDL_TEST_FAULT_AFTER_STORE, &err)) {
        goto fail;
    }
    trace_femu_cdl_durable_commit(n->cdl_trace_id, req->cdl_attempt,
                           req->sq->sqid,
                                  le16_to_cpu(req->cmd.cid), cls, txid,
                                  timed, misses);
    txn_path = cdl_txn_path(n);
    if (cdl_test_inject(n, CDL_TEST_FAULT_BEFORE_UNLINK, &err)) {
        goto fail;
    }
    if (unlink(txn_path) < 0 && errno != ENOENT) {
        error_setg_errno(&err, errno, "cannot remove CDL transaction '%s'",
                         txn_path);
        goto fail;
    }
    if (cdl_test_inject(n, CDL_TEST_FAULT_AFTER_UNLINK, &err) ||
        !cdl_sync_parent(n, txn_path, CDL_TEST_FAULT_NONE, &err)) {
        goto fail;
    }
    qemu_mutex_unlock(&n->cdl_store_lock);
    trace_femu_cdl_persist_latency(n->cdl_trace_id, req->cdl_attempt, txid,
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - persist_start);
    return cdl_test_barrier_wait(n, CDL_BARRIER_DURABLE);

fail:
    n->cdl_store_failed = true;
    n->bar.csts |= NVME_CSTS_FAILED;
    qemu_mutex_unlock(&n->cdl_store_lock);
    if (err) {
        error_report_err(err);
    }
    return false;
}

bool wfq_on_complete(FemuCtrl *n, NvmeRequest *req)
{
    return cdl_account(n, req);
}

void wfq_init(FemuCtrl *n)
{
    QTAILQ_INIT(&n->wfq_inflight_list);
    n->cdl_pending = 0;
    n->wfq_inflight = 0;
    n->wfq_v_clock = 0;
    if (!n->cdl_profile) {
        return;
    }
    memset(n->cdl_inflight_by_class, 0,
           n->cdl_num_classes * sizeof(*n->cdl_inflight_by_class));
    memset(n->cdl_dispatch_wait, 0,
           n->cdl_num_classes * sizeof(*n->cdl_dispatch_wait));
    memset(n->wfq_f_last, 0,
           n->cdl_num_classes * sizeof(*n->wfq_f_last));
    memset(n->cdl_rr_next, 0,
           n->cdl_num_classes * sizeof(*n->cdl_rr_next));
    memset(n->cdl_rr_started, 0,
           n->cdl_num_classes * sizeof(*n->cdl_rr_started));
}
