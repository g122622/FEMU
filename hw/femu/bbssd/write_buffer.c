#include "write_buffer.h"
#include "ftl.h"
#include "hmb.h"
#include "qemu/queue.h"

typedef struct FemuWbNotifyArgs {
    uint32_t cmd_id;
    uint16_t qid;
    uint32_t seg_cnt;
} FemuWbNotifyArgs;

static void *femu_wb_flush_monitor_thread(void *opaque);

static void wb_free_cmd_track(gpointer data)
{
    FemuWbCmdTrack *t = data;

    if (!t) {
        return;
    }

    g_free(t->segs);
    g_free(t->mcp_slots);
    g_free(t);
}

static void wb_free_seg(FemuWbSeg *seg)
{
    if (!seg) {
        return;
    }

    g_free(seg);
}

static bool wb_lpn_tombstoned_locked(FemuWbLocal *l, uint64_t lpn)
{
    return l->trim_tombstones && g_hash_table_lookup(l->trim_tombstones, &lpn);
}

static bool wb_set_trim_tombstone_locked(FemuWbLocal *l, uint64_t lpn)
{
    uint64_t *key;

    if (!l->trim_tombstones) {
        l->trim_tombstones = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                   g_free, NULL);
        if (!l->trim_tombstones) {
            return false;
        }
    }

    if (g_hash_table_lookup(l->trim_tombstones, &lpn)) {
        return true;
    }

    key = g_malloc(sizeof(*key));
    if (!key) {
        return false;
    }
    *key = lpn;
    g_hash_table_insert(l->trim_tombstones, key, GINT_TO_POINTER(1));
    l->trim_tombstone_cnt++;
    return true;
}

static void wb_clear_trim_tombstone_locked(FemuWbLocal *l, uint64_t lpn)
{
    if (!l->trim_tombstones) {
        return;
    }

    g_hash_table_remove(l->trim_tombstones, &lpn);
}

static inline uint64_t wb_align_up(uint64_t v, uint64_t a)
{
    return ((v + a - 1) / a) * a;
}

static inline uint64_t wb_align_down(uint64_t v, uint64_t a)
{
    return (v / a) * a;
}

static void wb_perf_log_if_due(FemuCtrl *n)
{
    FemuWriteBuffer *wb = &n->wb;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    const uint64_t interval_ns = 1000000000ULL;
    uint64_t d_copy_calls, d_copy_ns, d_copy_segs;
    uint64_t d_copy_lock_wait_ns, d_copy_lock_hold_ns;
    uint64_t d_copy_lookup_ns, d_copy_mcp_release_ns, d_copy_loop_ns;
    uint64_t d_copy_mirror_ns, d_copy_index_ns, d_copy_reclaim_ns;
    uint64_t d_copy_remove_track_ns;
    uint64_t d_copy_trimmed_segs, d_copy_mirror_fail_segs;
    uint64_t d_copy_keep_old_busy, d_copy_insert_fail;

    if (!wb->perf_last_log_ns) {
        wb->perf_last_log_ns = now;
        wb->perf_last_copy_done_calls = wb->perf_copy_done_calls;
        wb->perf_last_copy_done_ns = wb->perf_copy_done_ns;
        wb->perf_last_copy_done_segs = wb->perf_copy_done_segs;
        wb->perf_last_copy_lock_wait_ns = wb->perf_copy_lock_wait_ns;
        wb->perf_last_copy_lock_hold_ns = wb->perf_copy_lock_hold_ns;
        wb->perf_last_copy_lookup_ns = wb->perf_copy_lookup_ns;
        wb->perf_last_copy_mcp_release_ns = wb->perf_copy_mcp_release_ns;
        wb->perf_last_copy_loop_ns = wb->perf_copy_loop_ns;
        wb->perf_last_copy_mirror_ns = wb->perf_copy_mirror_ns;
        wb->perf_last_copy_index_ns = wb->perf_copy_index_ns;
        wb->perf_last_copy_reclaim_ns = wb->perf_copy_reclaim_ns;
        wb->perf_last_copy_remove_track_ns = wb->perf_copy_remove_track_ns;
        wb->perf_last_copy_trimmed_segs = wb->perf_copy_trimmed_segs;
        wb->perf_last_copy_mirror_fail_segs = wb->perf_copy_mirror_fail_segs;
        wb->perf_last_copy_keep_old_busy = wb->perf_copy_keep_old_busy;
        wb->perf_last_copy_insert_fail = wb->perf_copy_insert_fail;
        return;
    }

    if (now - wb->perf_last_log_ns < interval_ns) {
        return;
    }

    d_copy_calls = wb->perf_copy_done_calls - wb->perf_last_copy_done_calls;
    d_copy_ns = wb->perf_copy_done_ns - wb->perf_last_copy_done_ns;
    d_copy_segs = wb->perf_copy_done_segs - wb->perf_last_copy_done_segs;
    d_copy_lock_wait_ns = wb->perf_copy_lock_wait_ns - wb->perf_last_copy_lock_wait_ns;
    d_copy_lock_hold_ns = wb->perf_copy_lock_hold_ns - wb->perf_last_copy_lock_hold_ns;
    d_copy_lookup_ns = wb->perf_copy_lookup_ns - wb->perf_last_copy_lookup_ns;
    d_copy_mcp_release_ns = wb->perf_copy_mcp_release_ns - wb->perf_last_copy_mcp_release_ns;
    d_copy_loop_ns = wb->perf_copy_loop_ns - wb->perf_last_copy_loop_ns;
    d_copy_mirror_ns = wb->perf_copy_mirror_ns - wb->perf_last_copy_mirror_ns;
    d_copy_index_ns = wb->perf_copy_index_ns - wb->perf_last_copy_index_ns;
    d_copy_reclaim_ns = wb->perf_copy_reclaim_ns - wb->perf_last_copy_reclaim_ns;
    d_copy_remove_track_ns = wb->perf_copy_remove_track_ns - wb->perf_last_copy_remove_track_ns;
    d_copy_trimmed_segs = wb->perf_copy_trimmed_segs - wb->perf_last_copy_trimmed_segs;
    d_copy_mirror_fail_segs = wb->perf_copy_mirror_fail_segs - wb->perf_last_copy_mirror_fail_segs;
    d_copy_keep_old_busy = wb->perf_copy_keep_old_busy - wb->perf_last_copy_keep_old_busy;
    d_copy_insert_fail = wb->perf_copy_insert_fail - wb->perf_last_copy_insert_fail;

    femu_log("WB copy_done perf(1s): calls=%" PRIu64 " segs=%" PRIu64
             " avg=%.2fus | lock_wait=%.2fus lock_hold=%.2fus"
             " lookup=%.2fus mcp_release=%.2fus loop=%.2fus"
             " mirror=%.2fus index=%.2fus reclaim=%.2fus rm_track=%.2fus"
             " | trimmed=%" PRIu64 " mirror_fail=%" PRIu64
             " keep_old_busy=%" PRIu64 " insert_fail=%" PRIu64 "\n",
             d_copy_calls, d_copy_segs,
             d_copy_calls ? (double)d_copy_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_lock_wait_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_lock_hold_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_lookup_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_mcp_release_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_loop_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_mirror_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_index_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_reclaim_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_calls ? (double)d_copy_remove_track_ns / (double)d_copy_calls / 1000.0 : 0.0,
             d_copy_trimmed_segs, d_copy_mirror_fail_segs,
             d_copy_keep_old_busy, d_copy_insert_fail);

    wb->perf_last_log_ns = now;
    wb->perf_last_copy_done_calls = wb->perf_copy_done_calls;
    wb->perf_last_copy_done_ns = wb->perf_copy_done_ns;
    wb->perf_last_copy_done_segs = wb->perf_copy_done_segs;
    wb->perf_last_copy_lock_wait_ns = wb->perf_copy_lock_wait_ns;
    wb->perf_last_copy_lock_hold_ns = wb->perf_copy_lock_hold_ns;
    wb->perf_last_copy_lookup_ns = wb->perf_copy_lookup_ns;
    wb->perf_last_copy_mcp_release_ns = wb->perf_copy_mcp_release_ns;
    wb->perf_last_copy_loop_ns = wb->perf_copy_loop_ns;
    wb->perf_last_copy_mirror_ns = wb->perf_copy_mirror_ns;
    wb->perf_last_copy_index_ns = wb->perf_copy_index_ns;
    wb->perf_last_copy_reclaim_ns = wb->perf_copy_reclaim_ns;
    wb->perf_last_copy_remove_track_ns = wb->perf_copy_remove_track_ns;
    wb->perf_last_copy_trimmed_segs = wb->perf_copy_trimmed_segs;
    wb->perf_last_copy_mirror_fail_segs = wb->perf_copy_mirror_fail_segs;
    wb->perf_last_copy_keep_old_busy = wb->perf_copy_keep_old_busy;
    wb->perf_last_copy_insert_fail = wb->perf_copy_insert_fail;
}

static void femu_wb_disable_with_reason(FemuCtrl *n, const char *reason)
{
    n->wb.layout_ready = false;
    n->wb.wb_enabled = false;
    n->wb.gate_waiting_kva_push = true;

    femu_log("WB disabled (degraded to L2P-only path): %s\n", reason);
}

static void wb_stop_monitor_thread(FemuWriteBuffer *wb)
{
    if (!wb->flush_thread_started) {
        return;
    }

    wb->flush_thread_stop = true;
    qemu_thread_join(&wb->flush_thread);
    wb->flush_thread_started = false;
}

static void wb_start_monitor_thread(FemuCtrl *n)
{
    FemuWriteBuffer *wb = &n->wb;

    if (wb->flush_thread_started || !wb->layout_ready) {
        return;
    }

    wb->flush_thread_stop = false;
    qemu_thread_create(&wb->flush_thread, "femu-wb-flush-mon",
                       femu_wb_flush_monitor_thread, n,
                       QEMU_THREAD_JOINABLE);
    wb->flush_thread_started = true;
}

static void femu_wb_kva_map_reset(FemuWriteBuffer *wb)
{
    if (wb->kva_map) {
        g_hash_table_destroy(wb->kva_map);
        wb->kva_map = NULL;
    }
    if (wb->kva_ranges) {
        g_ptr_array_free(wb->kva_ranges, true);
        wb->kva_ranges = NULL;
    }

    wb->kva_map_ready = false;
}

static bool femu_wb_kva_map_init(FemuWriteBuffer *wb)
{
    if (!wb->kva_map) {
        wb->kva_map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
        if (!wb->kva_map) {
            return false;
        }
    }

    if (!wb->kva_ranges) {
        wb->kva_ranges = g_ptr_array_new_with_free_func(g_free);
        if (!wb->kva_ranges) {
            g_hash_table_destroy(wb->kva_map);
            wb->kva_map = NULL;
            return false;
        }
    }

    return true;
}

static int wb_find_hmb_desc_by_base(FemuCtrl *n, uint64_t gpa)
{
    for (uint32_t i = 0; i < n->hmb_desc_count; i++) {
        if (n->hmb_descs[i].addr == gpa) {
            return i;
        }
    }

    return -1;
}

static bool wb_hmb_off_to_gpa(FemuCtrl *n, uint64_t off, uint64_t *gpa_out)
{
    uint64_t cur = 0;

    for (uint32_t i = 0; i < n->hmb_desc_count; i++) {
        uint64_t seg_sz = (uint64_t)n->hmb_descs[i].size * n->page_size;

        if (off < cur + seg_sz) {
            *gpa_out = n->hmb_descs[i].addr + (off - cur);
            return true;
        }

        cur += seg_sz;
    }

    return false;
}

static bool wb_alloc_space_locked(FemuWbLocal *l, uint64_t len,
                                  uint64_t *rel_off)
{
    uint64_t old_tail = l->tail;

    if (len > l->wb_bytes || l->used + len > l->wb_bytes) {
        return false;
    }

    if (l->tail >= l->head) {
        uint64_t right = l->wb_bytes - l->tail;

        if (right >= len) {
            *rel_off = l->tail;
            l->tail = (l->tail + len) % l->wb_bytes;
            l->used += len;
            return true;
        }

        if (l->head > len) {
            *rel_off = 0;
            l->tail = len;
            l->used += len;
            return true;
        }
    } else {
        uint64_t hole = l->head - l->tail;

        if (hole > len) {
            *rel_off = l->tail;
            l->tail += len;
            l->used += len;
            return true;
        }
    }

    l->tail = old_tail;
    return false;
}

static bool wb_mcp_push_locked(FemuCtrl *n, FemuWbLocal *l,
                               FemuMcpEntry *entry, uint32_t *slot_out)
{
    uint64_t off;

    if (!l->mcp_free_cnt) {
        return false;
    }

    off = l->mcp_off + (uint64_t)l->mcp_tail * sizeof(FemuMcpEntry);
    if (!femu_hmb_rw(n, off, entry, sizeof(*entry), true)) {
        femu_err("WB MCP push failed: qid=%u slot=%u off=0x%" PRIx64 "\n",
                 l->qid, l->mcp_tail, off);
        return false;
    }

    if (slot_out) {
        *slot_out = l->mcp_tail;
    }
    l->mcp_tail = (l->mcp_tail + 1) % l->mcp_capacity;
    l->mcp_free_cnt--;

    assert(l->mcp_tail < l->mcp_capacity);

    return true;
}

static bool wb_sync_seg_to_backend(FemuCtrl *n, FemuWbSeg *seg)
{
    struct ssd *ssd = n->ssd;
    uint64_t data_off;
    uint8_t *dst;

    if (!ssd || !n->mbe || !n->mbe->logical_space || !seg) {
        return false;
    }

    data_off = seg->slba * ssd->sp.secsz;
    dst = (uint8_t *)n->mbe->logical_space + data_off;

    return femu_hmb_rw(n, seg->hmb_off, dst, seg->len, false);
}

static void wb_mcp_release_locked(FemuWbLocal *l, uint32_t *slots,
                                  uint32_t seg_cnt)
{
    for (uint32_t i = 0; i < seg_cnt; i++) {
        uint32_t s = slots[i];

        if (s != l->mcp_head) {
            femu_log("WB MCP release out-of-order: qid=%u expected_head=%u got=%u\n",
                     l->qid, l->mcp_head, s);
            l->mcp_head = (s + 1) % l->mcp_capacity;
        } else {
            l->mcp_head = (l->mcp_head + 1) % l->mcp_capacity;
        }

        l->mcp_free_cnt++;
        if (l->mcp_free_cnt > l->mcp_capacity) {
            l->mcp_free_cnt = l->mcp_capacity;
        }
    }
}

static void wb_flush_q_insert_sorted_locked(FemuWbLocal *l, FemuWbSeg *seg)
{
    FemuWbSeg *it;
    FemuWbSeg *tail;

    tail = QTAILQ_LAST(&l->flush_q);
    if (!tail || seg->alloc_seq >= tail->alloc_seq) {
        QTAILQ_INSERT_TAIL(&l->flush_q, seg, entry);
        return;
    }

    QTAILQ_FOREACH(it, &l->flush_q, entry) {
        if (seg->alloc_seq < it->alloc_seq) {
            QTAILQ_INSERT_BEFORE(it, seg, entry);
            return;
        }
    }

    QTAILQ_INSERT_TAIL(&l->flush_q, seg, entry);
}

static void wb_try_reclaim_head_locked(FemuWbLocal *l)
{
    FemuWbSeg *head_seg;

    while ((head_seg = QTAILQ_FIRST(&l->flush_q))) {
        if (head_seg->state != FEMU_WB_SEG_FLUSHED || head_seg->indexed) {
            break;
        }

        if (head_seg->rel_off != l->head) {
            break;
        }

        l->head = (l->head + head_seg->len) % l->wb_bytes;
        assert(l->used >= head_seg->len);
        l->used -= head_seg->len;

        QTAILQ_REMOVE(&l->flush_q, head_seg, entry);
        wb_free_seg(head_seg);
    }

    assert(l->used <= l->wb_bytes);
}

static FemuWbCmdTrack *wb_lookup_cmd_track_locked(FemuWbLocal *l,
                                                   uint32_t cmd_id)
{
    return l->cmd_track_map ? g_hash_table_lookup(l->cmd_track_map, &cmd_id) : NULL;
}

static bool wb_insert_cmd_track_locked(FemuWbLocal *l, FemuWbCmdTrack *track)
{
    uint32_t *key;

    if (!l->cmd_track_map) {
        l->cmd_track_map = g_hash_table_new_full(g_int_hash, g_int_equal,
                                                 g_free, wb_free_cmd_track);
        if (!l->cmd_track_map) {
            return false;
        }
    }

    if (g_hash_table_lookup(l->cmd_track_map, &track->cmd_id)) {
        femu_err("WB cmd-track conflict: qid=%u cmd_id=%u\n",
                 l->qid, track->cmd_id);
        return false;
    }

    key = g_malloc(sizeof(*key));
    if (!key) {
        return false;
    }
    *key = track->cmd_id;
    g_hash_table_insert(l->cmd_track_map, key, track);
    return true;
}

static void wb_remove_cmd_track_locked(FemuWbLocal *l, uint32_t cmd_id)
{
    if (!l->cmd_track_map) {
        return;
    }

    g_hash_table_remove(l->cmd_track_map, &cmd_id);
}

static bool wb_parse_notify_args(FemuCtrl *n, NvmeCmd *cmd,
                                 NvmeRequest *req, FemuWbNotifyArgs *args,
                                 const char *tag)
{
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint32_t cdw11 = le32_to_cpu(cmd->cdw11);
    uint32_t cdw12 = le32_to_cpu(cmd->cdw12);

    args->cmd_id = cdw10;
    args->qid = (uint16_t)(cdw11 & 0xffff);
    args->seg_cnt = cdw12;

    if (!args->qid || args->qid > n->nr_io_queues) {
        femu_err("%s: invalid qid=%u (nr_io_queues=%u)\n",
                 tag, args->qid, n->nr_io_queues);
        req->status = NVME_INVALID_FIELD | NVME_DNR;
        return false;
    }

    if (!args->seg_cnt) {
        femu_err("%s: invalid seg_cnt=0 for qid=%u cmd_id=%u\n",
                 tag, args->qid, args->cmd_id);
        req->status = NVME_INVALID_FIELD | NVME_DNR;
        return false;
    }

    if (!n->wb.layout_ready || !n->wb.locals) {
        femu_log("%s: WB layout not ready, ignore qid=%u cmd_id=%u seg_cnt=%u\n",
                 tag, args->qid, args->cmd_id, args->seg_cnt);
        req->status = NVME_SUCCESS;
        return false;
    }

    assert(n->wb.locals != NULL);
    assert(args->qid <= n->wb.nr_queues);

    FemuWbLocal *l = &n->wb.locals[args->qid];

    assert(l->qid == args->qid);
    assert(l->mcp_tail < l->mcp_capacity);
    assert(l->used <= l->wb_bytes);

    if (args->seg_cnt > l->mcp_capacity) {
        femu_err("%s: seg_cnt=%u exceeds mcp_capacity=%u on qid=%u\n",
                 tag, args->seg_cnt, l->mcp_capacity, args->qid);
        req->status = NVME_INVALID_FIELD | NVME_DNR;
        return false;
    }

    req->status = NVME_SUCCESS;
    return true;
}

void femu_wb_ctrl_reset(FemuCtrl *n)
{
    FemuWriteBuffer *wb = &n->wb;

    wb_stop_monitor_thread(wb);

    if (wb->locals) {
        for (uint32_t qid = 1; qid <= n->nr_io_queues; qid++) {
            FemuWbLocal *l = &wb->locals[qid];
            FemuWbSeg *seg, *next;

            if (l->lpn_index_lock_inited) {
                if (l->cmd_track_map) {
                    g_hash_table_destroy(l->cmd_track_map);
                    l->cmd_track_map = NULL;
                }

                if (l->trim_tombstones) {
                    g_hash_table_destroy(l->trim_tombstones);
                    l->trim_tombstones = NULL;
                }

                QTAILQ_FOREACH_SAFE(seg, &l->flush_q, entry, next) {
                    QTAILQ_REMOVE(&l->flush_q, seg, entry);
                    wb_free_seg(seg);
                }

                qemu_mutex_destroy(&l->lpn_index_lock);
                l->lpn_index_lock_inited = false;
            }
        }
        g_free(wb->locals);
        wb->locals = NULL;
    }

    femu_wb_kva_map_reset(wb);

    memset(wb, 0, sizeof(*wb));
    wb->mcp_entries_per_q = n->cfg_wb_mcp_entries_per_q;
    wb->mcp_entry_bytes = FEMU_WB_MCP_ENTRY_BYTES;
    wb->gate_waiting_kva_push = true;
}

bool femu_wb_gpa_to_kva(FemuCtrl *n, uint64_t gpa, uint64_t *kva_out,
                        uint64_t *max_len_out)
{
    FemuWriteBuffer *wb = &n->wb;
    FemuWbKvaMapEntry *ent;

    if (!wb->kva_map_ready || !wb->kva_map || !wb->kva_ranges) {
        return false;
    }

    ent = g_hash_table_lookup(wb->kva_map, &gpa);
    if (ent) {
        if (kva_out) {
            *kva_out = ent->kva;
        }
        if (max_len_out) {
            *max_len_out = ent->size;
        }
        return true;
    }

    for (guint i = 0; i < wb->kva_ranges->len; i++) {
        uint64_t delta;

        ent = g_ptr_array_index(wb->kva_ranges, i);
        if (gpa < ent->gpa || gpa >= ent->gpa + ent->size) {
            continue;
        }

        delta = gpa - ent->gpa;
        if (kva_out) {
            *kva_out = ent->kva + delta;
        }
        if (max_len_out) {
            *max_len_out = ent->size - delta;
        }
        return true;
    }

    return false;
}

bool femu_wb_should_candidate_write(FemuCtrl *n)
{
    return n->exp_enable_wb &&
           n->wb.layout_ready && n->wb.wb_enabled && !n->wb.gate_waiting_kva_push;
}

bool femu_wb_stage_write_req(struct ssd *ssd, NvmeRequest *req,
                             uint64_t start_lpn, uint64_t end_lpn,
                             uint32_t secsz, uint32_t secs_per_pg,
                             uint8_t data_shift, uint64_t data_size)
{
    FemuCtrl *n = ssd->n;
    FemuWbLocal *l;
    FemuWbCmdTrack *track = NULL;
    uint32_t qid;
    uint32_t seg_cnt;
    uint32_t cmd_id;
    uint64_t req_start_sec = req->slba;
    uint64_t req_end_sec = req->slba + req->nlb;
    uint64_t snap_head, snap_tail, snap_used;
    uint32_t snap_mcp_head, snap_mcp_tail, snap_mcp_free;
    bool staged_ok = false;
    uint64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    n->wb.perf_stage_calls++;

    if (!femu_wb_should_candidate_write(n) || !n->wb.locals) {
        return false;
    }

    qid = req->sq ? req->sq->sqid : 0;
    if (!qid || qid > n->wb.nr_queues) {
        return false;
    }

    l = &n->wb.locals[qid];
    cmd_id = le16_to_cpu(req->cmd.cid);
    seg_cnt = (uint32_t)(end_lpn - start_lpn + 1);

    qemu_mutex_lock(&l->lpn_index_lock);

    l->activity_seq++;
    l->idle_rounds = 0;

    if (data_size > l->wb_bytes) {
        l->fallback_cnt++;
        n->wb.fallback_cnt++;
        n->wb.wb_bypass_cnt++;
        femu_log("WB Bypass: req_size=%" PRIu64 " > wb_capacity=%" PRIu64
                 ", direct write to NAND\n",
                 data_size, l->wb_bytes);
        goto out_unlock;
    }

    if (seg_cnt == 0 || l->mcp_free_cnt < seg_cnt) {
        l->mcp_full_cnt++;
        n->wb.mcp_full_cnt++;
        l->fallback_cnt++;
        n->wb.fallback_cnt++;
        femu_debug("WB fallback(write): qid=%u cmd_id=%u mcp_free=%u need=%u\n",
                 qid, cmd_id, l->mcp_free_cnt, seg_cnt);
        goto out_unlock;
    }

    track = g_malloc0(sizeof(*track));
    if (!track) {
        goto out_unlock;
    }

    track->cmd_id = cmd_id;
    track->type = FEMU_WB_CMD_TRACK_WRITE;
    track->seg_cnt = seg_cnt;
    track->segs = g_malloc0(sizeof(*track->segs) * seg_cnt);
    track->mcp_slots = g_malloc0(sizeof(*track->mcp_slots) * seg_cnt);
    if (!track->segs || !track->mcp_slots) {
        wb_free_cmd_track(track);
        track = NULL;
        goto out_unlock;
    }

    snap_head = l->head;
    snap_tail = l->tail;
    snap_used = l->used;
    snap_mcp_head = l->mcp_head;
    snap_mcp_tail = l->mcp_tail;
    snap_mcp_free = l->mcp_free_cnt;

    for (uint32_t i = 0; i < seg_cnt; i++) {
        uint64_t lpn = start_lpn + i;
        uint64_t lpn_start = lpn * secs_per_pg;
        uint64_t lpn_end = lpn_start + secs_per_pg;
        uint64_t ov_start = MAX(req_start_sec, lpn_start);
        uint64_t ov_end = MIN(req_end_sec, lpn_end);
        uint64_t ov_secs;
        uint64_t rel_off;
        uint64_t abs_off;
        uint64_t gpa;
        uint64_t kva;
        uint64_t kva_max;
        uint32_t seg_len;
        uint32_t prp_off;
        FemuWbSeg *seg;
        FemuMcpEntry me;

        if (ov_end <= ov_start) {
            continue;
        }

        ov_secs = ov_end - ov_start;
        seg_len = (uint32_t)(ov_secs * secsz);
        prp_off = (uint32_t)((ov_start - req_start_sec) * secsz);

        if (!wb_alloc_space_locked(l, seg_len, &rel_off)) {
            femu_log("WB fallback(write): qid=%u cmd_id=%u alloc failed len=%u used=%" PRIu64
                     "/%" PRIu64 "\n",
                     qid, cmd_id, seg_len, l->used, l->wb_bytes);
            l->fallback_cnt++;
            n->wb.fallback_cnt++;
            goto rollback;
        }

        abs_off = l->wb_off + rel_off;
        if (!wb_hmb_off_to_gpa(n, abs_off, &gpa) ||
            !femu_wb_gpa_to_kva(n, gpa, &kva, &kva_max) || kva_max < seg_len) {
            femu_err("WB fallback(write): qid=%u cmd_id=%u off=0x%" PRIx64
                     " cannot map GPA/KVA\n",
                     qid, cmd_id, abs_off);
            l->fallback_cnt++;
            n->wb.fallback_cnt++;
            goto rollback;
        }

        seg = g_malloc0(sizeof(*seg));
        if (!seg) {
            goto rollback;
        }

        wb_clear_trim_tombstone_locked(l, lpn);

        seg->alloc_seq = ++l->alloc_seq;
        seg->lpn = lpn;
        seg->slba = ov_start;
        seg->nlb = (uint32_t)ov_secs;
        seg->hmb_off = abs_off;
        seg->hmb_vaddr = kva;
        seg->rel_off = rel_off;
        seg->len = seg_len;
        seg->prp_off = prp_off;
        seg->cmd_id = cmd_id;
        seg->qid = qid;
        seg->state = FEMU_WB_SEG_STAGED;
        seg->trimmed = false;
        seg->indexed = false;
        seg->rbn = NULL;
        track->segs[i] = seg;

        memset(&me, 0, sizeof(me));
        me.cmd_id = cmd_id;
        me.metadata_size = sizeof(me);
        me.hmb_vaddr = seg->hmb_vaddr;
        me.hmb_off = seg->hmb_off;
        me.prp_off = seg->prp_off;
        me.length = seg->len;
        me.slba = seg->slba;
        me.lpn = seg->lpn;
        me.nlb = seg->nlb;
        me.qid = qid;
        me.type = FEMU_MCP_ENTRY_WRITE_ALLOC;
        if (i == seg_cnt - 1) {
            me.flags |= FEMU_MCP_F_LAST_SEG;
        }

        if (!wb_mcp_push_locked(n, l, &me, &track->mcp_slots[i])) {
            wb_free_seg(seg);
            track->segs[i] = NULL;
            l->fallback_cnt++;
            n->wb.fallback_cnt++;
            goto rollback;
        }
    }

    if (!wb_insert_cmd_track_locked(l, track)) {
        goto rollback;
    }

    req->wb_path = true;
    req->cqe.n.rsvd |= cpu_to_le32(FEMU_CQE_RSVD_MCP_READY);
    n->wb.idx_hits += seg_cnt;
    femu_debug("WB stage write: qid=%u cmd_id=%u seg_cnt=%u bytes=%" PRIu64
             " used=%" PRIu64 "/%" PRIu64 "\n",
             qid, cmd_id, seg_cnt, data_size, l->used, l->wb_bytes);

    staged_ok = true;
    track = NULL;
    goto out_unlock;

rollback:
    if (track) {
        for (uint32_t i = 0; i < seg_cnt; i++) {
            wb_free_seg(track->segs[i]);
            track->segs[i] = NULL;
        }
        wb_free_cmd_track(track);
        track = NULL;
    }
    l->head = snap_head;
    l->tail = snap_tail;
    l->used = snap_used;
    l->mcp_head = snap_mcp_head;
    l->mcp_tail = snap_mcp_tail;
    l->mcp_free_cnt = snap_mcp_free;
    req->wb_path = false;

out_unlock:
    qemu_mutex_unlock(&l->lpn_index_lock);
    n->wb.perf_stage_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    if (staged_ok) {
        n->wb.perf_stage_ok++;
        n->wb.perf_stage_bytes += data_size;
    }
    return staged_ok;
}

uint32_t femu_wb_stage_read_hits(struct ssd *ssd, NvmeRequest *req,
                                 uint64_t start_lpn, uint64_t end_lpn,
                                 uint32_t secsz, uint32_t secs_per_pg)
{
    FemuCtrl *n = ssd->n;
    uint32_t qid;
    FemuWbLocal *l;
    uint32_t cmd_id;
    uint64_t req_start_sec = req->slba;
    uint64_t req_end_sec = req->slba + req->nlb;
    uint32_t hit_cnt = 0;
    FemuWbCmdTrack *track = NULL;

    if (!femu_wb_should_candidate_write(n) || !n->wb.locals) {
        return 0;
    }

    qid = req->sq ? req->sq->sqid : 0;
    if (!qid || qid > n->wb.nr_queues) {
        return 0;
    }

    l = &n->wb.locals[qid];
    cmd_id = le16_to_cpu(req->cmd.cid);

    qemu_mutex_lock(&l->lpn_index_lock);

    l->activity_seq++;
    l->idle_rounds = 0;

    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        FemuRbNode *node = femu_rb_find(&l->lpn_index, lpn);

        if (wb_lpn_tombstoned_locked(l, lpn) ||
            !node || node->state != FEMU_WB_SEG_COPY_DONE || !node->priv) {
            continue;
        }

        hit_cnt++;
    }

    if (!hit_cnt) {
        qemu_mutex_unlock(&l->lpn_index_lock);
        return 0;
    }

    if (l->mcp_free_cnt < hit_cnt) {
        l->mcp_full_cnt++;
        n->wb.mcp_full_cnt++;
        qemu_mutex_unlock(&l->lpn_index_lock);
        femu_log("WB read-hit fallback: qid=%u cmd_id=%u hit_cnt=%u mcp_free=%u\n",
                 qid, cmd_id, hit_cnt, l->mcp_free_cnt);
        return 0;
    }

    track = g_malloc0(sizeof(*track));
    if (!track) {
        qemu_mutex_unlock(&l->lpn_index_lock);
        return 0;
    }
    track->cmd_id = cmd_id;
    track->type = FEMU_WB_CMD_TRACK_READ;
    track->seg_cnt = hit_cnt;
    track->segs = g_malloc0(sizeof(*track->segs) * hit_cnt);
    track->mcp_slots = g_malloc0(sizeof(*track->mcp_slots) * hit_cnt);
    if (!track->segs || !track->mcp_slots) {
        wb_free_cmd_track(track);
        qemu_mutex_unlock(&l->lpn_index_lock);
        return 0;
    }

    hit_cnt = 0;
    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        FemuRbNode *node = femu_rb_find(&l->lpn_index, lpn);
        FemuWbSeg *seg;
        uint64_t lpn_start = lpn * secs_per_pg;
        uint64_t lpn_end = lpn_start + secs_per_pg;
        uint64_t ov_start;
        uint64_t ov_end;
        uint64_t ov_secs;
        uint64_t delta_secs;
        FemuMcpEntry me;

        if (wb_lpn_tombstoned_locked(l, lpn) ||
            !node || node->state != FEMU_WB_SEG_COPY_DONE || !node->priv) {
            continue;
        }

        seg = node->priv;
        ov_start = MAX(req_start_sec, lpn_start);
        ov_end = MIN(req_end_sec, lpn_end);
        if (ov_end <= ov_start) {
            continue;
        }

        ov_secs = ov_end - ov_start;
        delta_secs = ov_start - seg->slba;

        femu_rb_refcnt_inc(node);
        track->segs[hit_cnt] = seg;

        memset(&me, 0, sizeof(me));
        me.cmd_id = cmd_id;
        me.metadata_size = sizeof(me);
        me.hmb_vaddr = seg->hmb_vaddr + delta_secs * secsz;
        me.hmb_off = seg->hmb_off + delta_secs * secsz;
        me.prp_off = (uint32_t)((ov_start - req_start_sec) * secsz);
        me.length = (uint32_t)(ov_secs * secsz);
        me.slba = ov_start;
        me.lpn = lpn;
        me.nlb = (uint32_t)ov_secs;
        me.qid = qid;
        me.type = FEMU_MCP_ENTRY_READ_HIT;

        if (!wb_mcp_push_locked(n, l, &me, &track->mcp_slots[hit_cnt])) {
            femu_rb_refcnt_dec(node);
            track->segs[hit_cnt] = NULL;
            for (uint32_t j = 0; j < hit_cnt; j++) {
                FemuWbSeg *s = track->segs[j];
                if (s && s->rbn) {
                    femu_rb_refcnt_dec(s->rbn);
                }
            }
            wb_free_cmd_track(track);
            qemu_mutex_unlock(&l->lpn_index_lock);
            return 0;
        }

        hit_cnt++;
    }

    if (!hit_cnt) {
        wb_free_cmd_track(track);
        qemu_mutex_unlock(&l->lpn_index_lock);
        return 0;
    }

    track->seg_cnt = hit_cnt;
    {
        uint64_t off = l->mcp_off + (uint64_t)track->mcp_slots[hit_cnt - 1] * sizeof(FemuMcpEntry);
        FemuMcpEntry last;

        if (femu_hmb_rw(n, off, &last, sizeof(last), false)) {
            last.flags |= FEMU_MCP_F_LAST_SEG;
            femu_hmb_rw(n, off, &last, sizeof(last), true);
        }
    }

    if (!wb_insert_cmd_track_locked(l, track)) {
        for (uint32_t i = 0; i < hit_cnt; i++) {
            if (track->segs[i] && track->segs[i]->rbn) {
                femu_rb_refcnt_dec(track->segs[i]->rbn);
            }
        }
        wb_free_cmd_track(track);
        qemu_mutex_unlock(&l->lpn_index_lock);
        return 0;
    }

    req->cqe.n.rsvd |= cpu_to_le32(FEMU_CQE_RSVD_MCP_READY);
    n->wb.idx_hits += hit_cnt;
    l->idx_hits += hit_cnt;
    femu_debug("WB stage read-hit: qid=%u cmd_id=%u hit_cnt=%u\n",
             qid, cmd_id, hit_cnt);

    qemu_mutex_unlock(&l->lpn_index_lock);
    return hit_cnt;
}

void femu_wb_note_queue_activity(FemuCtrl *n, uint16_t qid)
{
    FemuWbLocal *l;

    if (!n->wb.layout_ready || !n->wb.locals || !qid || qid > n->wb.nr_queues) {
        return;
    }

    l = &n->wb.locals[qid];
    qemu_mutex_lock(&l->lpn_index_lock);
    l->activity_seq++;
    l->idle_rounds = 0;
    qemu_mutex_unlock(&l->lpn_index_lock);
}

bool femu_wb_consume_flush_hint(FemuCtrl *n, uint16_t qid)
{
    FemuWbLocal *l;
    bool hint;

    if (!n->wb.layout_ready || !n->wb.locals || !qid || qid > n->wb.nr_queues) {
        return false;
    }

    l = &n->wb.locals[qid];
    qemu_mutex_lock(&l->lpn_index_lock);
    hint = l->flush_hint;
    l->flush_hint = false;
    qemu_mutex_unlock(&l->lpn_index_lock);
    return hint;
}

uint16_t femu_wb_admin_kva_mapping_push(FemuCtrl *n, NvmeCmd *cmd)
{
    FemuWriteBuffer *wb = &n->wb;
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint32_t seq_num = le32_to_cpu(cmd->cdw11);
    uint32_t buf_size = le32_to_cpu(cmd->cdw14);
    uint8_t action = cdw10 & 0x3;
    uint16_t chunk_count = (cdw10 >> 2) & 0x3fff;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    FemuWbKvaPushEntry *entries = NULL;

    if (!n->exp_enable_wb) {
        return NVME_INVALID_OPCODE | NVME_DNR;
    }

    if (!n->hmb_enabled || !wb->layout_ready || !wb->locals) {
        femu_err("WB 0xd1 reject: HMB/WB layout not ready (hmb_enabled=%d layout_ready=%d)\n",
                 n->hmb_enabled, wb->layout_ready);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert(wb->hmb_wb_base + wb->hmb_wb_bytes <= n->hmb_size_bytes);

    if (action != 0x1) {
        femu_err("WB 0xd1 reject: unsupported action=%u\n", action);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!chunk_count || chunk_count != n->hmb_desc_count) {
        femu_err("WB 0xd1 reject: chunk_count=%u hmb_desc_count=%u\n",
                 chunk_count, n->hmb_desc_count);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (buf_size != (uint32_t)chunk_count * sizeof(FemuWbKvaPushEntry)) {
        femu_err("WB 0xd1 reject: buf_size=%u expected=%zu\n",
                 buf_size,
                 (size_t)chunk_count * sizeof(FemuWbKvaPushEntry));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (wb->kva_seq_valid) {
        if (seq_num < wb->kva_seq) {
            femu_err("WB 0xd1 reject: seq rollback new=%u old=%u\n",
                     seq_num, wb->kva_seq);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        if (seq_num == wb->kva_seq) {
            femu_log("WB 0xd1 idempotent replay: seq=%u, keeping existing KVA map\n",
                     seq_num);
            return NVME_SUCCESS;
        }
    }

    entries = g_malloc0(buf_size);
    if (!entries) {
        femu_err("WB 0xd1 reject: no memory for %u bytes\n", buf_size);
        return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
    }

    if (dma_write_prp(n, (uint8_t *)entries, buf_size, prp1, prp2)) {
        femu_err("WB 0xd1 reject: failed to read mapping payload via PRP\n");
        g_free(entries);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    femu_wb_kva_map_reset(wb);
    if (!femu_wb_kva_map_init(wb)) {
        femu_err("WB 0xd1 reject: cannot initialize KVA map structures\n");
        g_free(entries);
        return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
    }

    for (uint16_t i = 0; i < chunk_count; i++) {
        FemuWbKvaMapEntry *ent;
        uint64_t *key;
        int desc_idx = wb_find_hmb_desc_by_base(n, entries[i].gpa);

        if (desc_idx < 0) {
            femu_err("WB 0xd1 reject: gpa=0x%" PRIx64 " not found in HMB descriptors\n",
                     entries[i].gpa);
            femu_wb_kva_map_reset(wb);
            g_free(entries);
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        if (!entries[i].kva) {
            femu_err("WB 0xd1 reject: zero KVA for gpa=0x%" PRIx64 "\n",
                     entries[i].gpa);
            femu_wb_kva_map_reset(wb);
            g_free(entries);
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        ent = g_malloc0(sizeof(*ent));
        key = g_malloc0(sizeof(*key));
        if (!ent || !key) {
            g_free(ent);
            g_free(key);
            femu_err("WB 0xd1 reject: no memory for map entry\n");
            femu_wb_kva_map_reset(wb);
            g_free(entries);
            return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
        }

        ent->gpa = entries[i].gpa;
        ent->kva = entries[i].kva;
        ent->size = (uint64_t)n->hmb_descs[desc_idx].size * n->page_size;
        assert(ent->size > 0);

        *key = ent->gpa;
        g_ptr_array_add(wb->kva_ranges, ent);
        g_hash_table_insert(wb->kva_map, key, ent);
    }

    wb->kva_map_ready = true;
    wb->kva_seq_valid = true;
    wb->kva_seq = seq_num;
    wb->kva_push_cnt++;
    wb->gate_waiting_kva_push = false;
    wb->wb_enabled = true;

    femu_log("WB 0xd1 accepted: seq=%u chunks=%u map_ready=1 wb_enabled=1\n",
             seq_num, chunk_count);

    g_free(entries);
    return NVME_SUCCESS;
}

uint16_t femu_wb_io_notify_copy_done(FemuCtrl *n, NvmeCmd *cmd,
                                     NvmeRequest *req)
{
    FemuWbNotifyArgs args = {0};
    FemuWbLocal *l;
    FemuWbCmdTrack *track;
    uint64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t t_lock_req;
    uint64_t t_lock_acquired;
    uint64_t t_part;
    uint64_t local_lock_wait_ns = 0;
    uint64_t local_lock_hold_ns = 0;
    uint64_t local_lookup_ns = 0;
    uint64_t local_mcp_release_ns = 0;
    uint64_t local_loop_ns = 0;
    uint64_t local_mirror_ns = 0;
    uint64_t local_index_ns = 0;
    uint64_t local_reclaim_ns = 0;
    uint64_t local_remove_track_ns = 0;
    uint64_t local_trimmed_segs = 0;
    uint64_t local_mirror_fail_segs = 0;
    uint64_t local_keep_old_busy = 0;
    uint64_t local_insert_fail = 0;

    if (!n->exp_enable_wb) {
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        return req->status;
    }

    if (!wb_parse_notify_args(n, cmd, req, &args, "WB 0xd5")) {
        return NVME_SUCCESS;
    }

    if (!n->wb.wb_enabled) {
        femu_log("WB 0xd5 ignored: wb_enabled=0 qid=%u cmd_id=%u seg_cnt=%u\n",
                 args.qid, args.cmd_id, args.seg_cnt);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    l = &n->wb.locals[args.qid];
    t_lock_req = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    qemu_mutex_lock(&l->lpn_index_lock);
    t_lock_acquired = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    local_lock_wait_ns = t_lock_acquired - t_lock_req;

    t_lock_req = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    track = wb_lookup_cmd_track_locked(l, args.cmd_id);
    local_lookup_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_req;
    if (!track || track->type != FEMU_WB_CMD_TRACK_WRITE) {
        local_lock_hold_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_acquired;
        qemu_mutex_unlock(&l->lpn_index_lock);

        n->wb.perf_copy_lock_wait_ns += local_lock_wait_ns;
        n->wb.perf_copy_lock_hold_ns += local_lock_hold_ns;
        n->wb.perf_copy_lookup_ns += local_lookup_ns;

        femu_log("WB 0xd5 COPY_DONE: no pending write cmd_id=%u on qid=%u\n",
                 args.cmd_id, args.qid);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    if (args.seg_cnt > track->seg_cnt) {
        local_lock_hold_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_acquired;
        qemu_mutex_unlock(&l->lpn_index_lock);

        n->wb.perf_copy_lock_wait_ns += local_lock_wait_ns;
        n->wb.perf_copy_lock_hold_ns += local_lock_hold_ns;
        n->wb.perf_copy_lookup_ns += local_lookup_ns;

        req->status = NVME_INVALID_FIELD | NVME_DNR;
        return NVME_SUCCESS;
    }

    t_lock_req = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    wb_mcp_release_locked(l, track->mcp_slots, args.seg_cnt);
    local_mcp_release_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_req;

    t_lock_req = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    for (uint32_t i = 0; i < args.seg_cnt; i++) {
        FemuWbSeg *seg = track->segs[i];
        FemuRbNode *old;
        FemuRbNode *node;

        if (!seg) {
            continue;
        }

        // 为了保证trim命令正确性，
        // 0xd5 COPY_DONE 对 tombstone/trimmed 段不再镜像 backend、不再入索引，仅回收空间
        if (seg->trimmed || wb_lpn_tombstoned_locked(l, seg->lpn)) {
            local_trimmed_segs++;
            seg->state = FEMU_WB_SEG_FLUSHED;
            seg->indexed = false;
            seg->rbn = NULL;
            wb_flush_q_insert_sorted_locked(l, seg);
            continue;
        }

        t_part = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (!wb_sync_seg_to_backend(n, seg)) {
            local_mirror_fail_segs++;
            femu_err("WB 0xd5: mirror-to-backend failed qid=%u cmd_id=%u lpn=%" PRIu64
                     " off=0x%" PRIx64 " len=%u\n",
                     args.qid, args.cmd_id, seg->lpn, seg->hmb_off, seg->len);
        }
        local_mirror_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_part;

        seg->state = FEMU_WB_SEG_COPY_DONE;

        t_part = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        old = femu_rb_find(&l->lpn_index, seg->lpn);
        if (old) {
            if (femu_rb_refcnt_read(old) != 0) {
                local_keep_old_busy++;
                femu_log("WB 0xd5: keep old LPN=%" PRIu64 " due refcnt=%d\n",
                         seg->lpn, femu_rb_refcnt_read(old));
                seg->indexed = false;
            } else {
                FemuWbSeg *old_seg = old->priv;
                femu_rb_remove(&l->lpn_index, old);
                if (old_seg) {
                    old_seg->indexed = false;
                    old_seg->rbn = NULL;
                }
                g_free(old);
                old = NULL;
            }
        }

        if (!old) {
            node = g_malloc0(sizeof(*node));
            if (node) {
                femu_rb_node_init(node, seg->lpn, seg->hmb_off,
                                  seg->len, FEMU_WB_SEG_COPY_DONE);
                node->priv = seg;
                if (femu_rb_insert(&l->lpn_index, node)) {
                    seg->indexed = true;
                    seg->rbn = node;
                } else {
                    local_insert_fail++;
                    g_free(node);
                }
            } else {
                local_insert_fail++;
            }
        }
        local_index_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_part;

        wb_flush_q_insert_sorted_locked(l, seg);
    }
    local_loop_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_req;

    t_lock_req = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    wb_remove_cmd_track_locked(l, args.cmd_id);
    local_remove_track_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_req;

    t_lock_req = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    wb_try_reclaim_head_locked(l);
    local_reclaim_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_req;

    local_lock_hold_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_lock_acquired;
    qemu_mutex_unlock(&l->lpn_index_lock);

    n->wb.copy_done_notify_cnt++;
    n->wb.perf_copy_done_calls++;
    n->wb.perf_copy_done_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    n->wb.perf_copy_done_segs += args.seg_cnt;
    n->wb.perf_copy_lock_wait_ns += local_lock_wait_ns;
    n->wb.perf_copy_lock_hold_ns += local_lock_hold_ns;
    n->wb.perf_copy_lookup_ns += local_lookup_ns;
    n->wb.perf_copy_mcp_release_ns += local_mcp_release_ns;
    n->wb.perf_copy_loop_ns += local_loop_ns;
    n->wb.perf_copy_mirror_ns += local_mirror_ns;
    n->wb.perf_copy_index_ns += local_index_ns;
    n->wb.perf_copy_reclaim_ns += local_reclaim_ns;
    n->wb.perf_copy_remove_track_ns += local_remove_track_ns;
    n->wb.perf_copy_trimmed_segs += local_trimmed_segs;
    n->wb.perf_copy_mirror_fail_segs += local_mirror_fail_segs;
    n->wb.perf_copy_keep_old_busy += local_keep_old_busy;
    n->wb.perf_copy_insert_fail += local_insert_fail;

    femu_debug("WB 0xd5 COPY_DONE: qid=%u cmd_id=%u seg_cnt=%u\n",
             args.qid, args.cmd_id, args.seg_cnt);
    req->status = NVME_SUCCESS;
    return NVME_SUCCESS;
}

uint16_t femu_wb_io_notify_read_done(FemuCtrl *n, NvmeCmd *cmd,
                                     NvmeRequest *req)
{
    FemuWbNotifyArgs args = {0};
    FemuWbLocal *l;
    FemuWbCmdTrack *track;
    uint64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (!n->exp_enable_wb) {
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        return req->status;
    }

    if (!wb_parse_notify_args(n, cmd, req, &args, "WB 0xd9")) {
        return NVME_SUCCESS;
    }

    if (!n->wb.wb_enabled) {
        femu_log("WB 0xd9 ignored: wb_enabled=0 qid=%u cmd_id=%u seg_cnt=%u\n",
                 args.qid, args.cmd_id, args.seg_cnt);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    l = &n->wb.locals[args.qid];
    qemu_mutex_lock(&l->lpn_index_lock);

    track = wb_lookup_cmd_track_locked(l, args.cmd_id);
    if (!track || track->type != FEMU_WB_CMD_TRACK_READ) {
        qemu_mutex_unlock(&l->lpn_index_lock);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    if (args.seg_cnt > track->seg_cnt) {
        qemu_mutex_unlock(&l->lpn_index_lock);
        req->status = NVME_INVALID_FIELD | NVME_DNR;
        return NVME_SUCCESS;
    }

    wb_mcp_release_locked(l, track->mcp_slots, args.seg_cnt);

    for (uint32_t i = 0; i < args.seg_cnt; i++) {
        FemuWbSeg *seg = track->segs[i];
        FemuRbNode *node;
        int32_t new_ref;

        if (!seg || !seg->rbn) {
            continue;
        }

        node = seg->rbn;
        new_ref = femu_rb_refcnt_dec(node);
        if (new_ref < 0) {
            femu_rb_refcnt_set(node, 0);
            new_ref = 0;
        }

        if ((seg->state == FEMU_WB_SEG_FLUSHED || seg->trimmed) &&
            new_ref == 0 && seg->indexed) {
            femu_rb_remove(&l->lpn_index, node);
            seg->indexed = false;
            seg->rbn = NULL;
            g_free(node);
        }
    }

    wb_remove_cmd_track_locked(l, args.cmd_id);
    wb_try_reclaim_head_locked(l);
    qemu_mutex_unlock(&l->lpn_index_lock);

    n->wb.read_done_notify_cnt++;
    n->wb.perf_read_done_calls++;
    n->wb.perf_read_done_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    n->wb.perf_read_done_segs += args.seg_cnt;

    femu_debug("WB 0xd9 READ_DONE: qid=%u cmd_id=%u seg_cnt=%u\n",
             args.qid, args.cmd_id, args.seg_cnt);
    req->status = NVME_SUCCESS;
    return NVME_SUCCESS;
}

bool femu_wb_init_layout(FemuCtrl *n)
{
    FemuWriteBuffer *wb = &n->wb;
    const uint64_t page_sz = FEMU_WB_ALIGN_BYTES;
    const uint64_t l2_bytes = n->exp_enable_l2p_multilevel ?
                              (uint64_t)n->cfg_l2p_l2_size_kb * 1024ULL : 0ULL;
    uint64_t l2_end = wb_align_up(l2_bytes, page_sz);
    uint64_t wb_total_pages;
    uint64_t wb_total_bytes;
    uint64_t pages_per_q;
    uint64_t mcp_bytes_raw;
    uint64_t mcp_bytes_aligned;
    uint64_t mcp_pages;
    uint64_t cursor_pages = 0;

    femu_wb_ctrl_reset(n);

    if (!n->exp_enable_wb) {
        femu_wb_disable_with_reason(n, "WB disabled by exp_enable_wb=0");
        return false;
    }

    if (!n->hmb_enabled || !n->hmb_size_bytes) {
        femu_wb_disable_with_reason(n, "HMB is not enabled");
        return false;
    }

    if (!n->nr_io_queues) {
        femu_wb_disable_with_reason(n, "nr_io_queues is zero");
        return false;
    }

    if (n->hmb_size_bytes <= l2_end) {
        femu_wb_disable_with_reason(n, "HMB bytes are not enough after reserving L2P-L2");
        return false;
    }

    wb_total_bytes = wb_align_down(n->hmb_size_bytes - l2_end, page_sz);
    wb_total_pages = wb_total_bytes / page_sz;
    if (!wb_total_pages) {
        femu_wb_disable_with_reason(n, "WB region has zero page after alignment");
        return false;
    }

    pages_per_q = wb_total_pages / n->nr_io_queues;
    if (!pages_per_q) {
        femu_wb_disable_with_reason(n, "Per-queue WB slice is zero page");
        return false;
    }

    mcp_bytes_raw = (uint64_t)n->cfg_wb_mcp_entries_per_q * FEMU_WB_MCP_ENTRY_BYTES;
    mcp_bytes_aligned = wb_align_up(mcp_bytes_raw, page_sz);
    mcp_pages = mcp_bytes_aligned / page_sz;

    if (pages_per_q <= mcp_pages) {
        femu_wb_disable_with_reason(n,
                "Per-queue WB slice is too small to hold fixed MCP ring");
        return false;
    }

    wb->locals = g_malloc0(sizeof(*wb->locals) * (n->nr_io_queues + 1));
    wb->nr_queues = n->nr_io_queues;
    wb->hmb_wb_base = l2_end;
    wb->hmb_wb_bytes = wb_total_bytes;
    wb->mcp_entries_per_q = n->cfg_wb_mcp_entries_per_q;
    wb->mcp_entry_bytes = FEMU_WB_MCP_ENTRY_BYTES;

    for (uint32_t qid = 1; qid <= n->nr_io_queues; qid++) {
        FemuWbLocal *l = &wb->locals[qid];
        uint64_t q_pages = pages_per_q;
        uint64_t q_off;
        uint64_t q_bytes;

        if (qid == n->nr_io_queues) {
            q_pages += wb_total_pages - pages_per_q * n->nr_io_queues;
        }

        q_off = wb->hmb_wb_base + cursor_pages * page_sz;
        q_bytes = q_pages * page_sz;

        l->qid = qid;
        l->wb_off = q_off;
        l->wb_bytes = q_bytes - mcp_bytes_aligned;
        l->head = 0;
        l->tail = 0;
        l->used = 0;

        l->mcp_off = q_off + l->wb_bytes;
        l->mcp_bytes = mcp_bytes_aligned;
        l->mcp_head = 0;
        l->mcp_tail = 0;
        l->mcp_capacity = l->mcp_bytes / sizeof(FemuMcpEntry);
        l->mcp_free_cnt = l->mcp_capacity;

        femu_rb_tree_init(&l->lpn_index);
        qemu_mutex_init(&l->lpn_index_lock);
        l->lpn_index_lock_inited = true;
        QTAILQ_INIT(&l->flush_q);
        l->idle_rounds_threshold = n->cfg_wb_idle_rounds_default;
        l->flush_hint = false;
        l->activity_seq = 0;
        l->monitor_seq = 0;
        l->idle_rounds = 0;
        l->alloc_seq = 0;
        l->cmd_track_map = g_hash_table_new_full(g_int_hash, g_int_equal,
                             g_free, wb_free_cmd_track);
        l->trim_tombstones = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                               g_free, NULL);

        assert(l->mcp_tail < l->mcp_capacity);
        assert(l->used <= l->wb_bytes);

        femu_log("WB layout qid=%u: slice_off=0x%" PRIx64 " slice_bytes=%" PRIu64
                 " wb_off=0x%" PRIx64 " wb_bytes=%" PRIu64
                 " mcp_off=0x%" PRIx64 " mcp_bytes=%" PRIu64
                 " mcp_entries=%u\n",
                 qid, q_off, q_bytes,
                 l->wb_off, l->wb_bytes,
                 l->mcp_off, l->mcp_bytes,
                 l->mcp_capacity);

        cursor_pages += q_pages;
    }

    assert(cursor_pages == wb_total_pages);
    assert(wb->hmb_wb_base + wb->hmb_wb_bytes <= n->hmb_size_bytes);

    wb->layout_ready = true;
    wb->wb_enabled = false;
    wb->gate_waiting_kva_push = true;

    wb_start_monitor_thread(n);

    femu_log("WB layout ready: HMB=%" PRIu64 "B L2P-L2=%" PRIu64
             "B WB_base=0x%" PRIx64 " WB_bytes=%" PRIu64
             "B nr_ioq=%u mcp_entries/q=%u mcp_entry_bytes=%u\n",
             n->hmb_size_bytes, l2_bytes,
             wb->hmb_wb_base, wb->hmb_wb_bytes,
             n->nr_io_queues,
             wb->mcp_entries_per_q,
             wb->mcp_entry_bytes);
    femu_log("WB capability gate: waiting for vendor admin 0xd1 before wb_enabled=true\n");

    return true;
}

// TRIM 安全回收接口
bool femu_wb_trim_try_reclaim_lpn(struct ssd *ssd, uint64_t lpn)
{
    FemuCtrl *n = ssd->n;
    bool safe = true;

    if (!n->wb.layout_ready || !n->wb.wb_enabled || !n->wb.locals) {
        return true;
    }

    for (uint16_t qid = 1; qid <= n->wb.nr_queues; qid++) {
        FemuWbLocal *l = &n->wb.locals[qid];
        FemuRbNode *node;
        FemuWbSeg *seg;
        GHashTableIter iter;
        gpointer key, value;

        qemu_mutex_lock(&l->lpn_index_lock);

        node = femu_rb_find(&l->lpn_index, lpn);
        if (node && node->priv) {
            seg = node->priv;
            bool had_tomb = wb_lpn_tombstoned_locked(l, lpn);
            seg->trimmed = true;
            seg->state = FEMU_WB_SEG_FLUSHED;

            if (femu_rb_refcnt_read(node) == 0) {
                femu_rb_remove(&l->lpn_index, node);
                seg->indexed = false;
                seg->rbn = NULL;
                g_free(node);
                l->trim_safe_reclaim_cnt++;
                n->wb.trim_safe_reclaim_cnt++;
            } else {
                safe = false;
                l->trim_skip_busy_cnt++;
                n->wb.trim_skip_busy_cnt++;
            }

            if (!wb_set_trim_tombstone_locked(l, lpn)) {
                safe = false;
            } else if (!had_tomb) {
                n->wb.trim_tombstone_cnt++;
            }
        }

        if (l->cmd_track_map) {
            g_hash_table_iter_init(&iter, l->cmd_track_map);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                FemuWbCmdTrack *track = value;

                if (!track || track->type != FEMU_WB_CMD_TRACK_WRITE ||
                    !track->segs) {
                    continue;
                }

                for (uint32_t i = 0; i < track->seg_cnt; i++) {
                    if (track->segs[i] && track->segs[i]->lpn == lpn) {
                        bool had_tomb = wb_lpn_tombstoned_locked(l, lpn);
                        track->segs[i]->trimmed = true;
                        if (!wb_set_trim_tombstone_locked(l, lpn)) {
                            safe = false;
                        } else if (!had_tomb) {
                            n->wb.trim_tombstone_cnt++;
                        }
                    }
                }
            }
        }

        wb_try_reclaim_head_locked(l);
        qemu_mutex_unlock(&l->lpn_index_lock);
    }

    return safe;
}

// 写入清 tombstone 接口
void femu_wb_trim_on_lpn_write(struct ssd *ssd, uint64_t lpn)
{
    FemuCtrl *n = ssd->n;

    if (!n->wb.layout_ready || !n->wb.locals) {
        return;
    }

    for (uint16_t qid = 1; qid <= n->wb.nr_queues; qid++) {
        FemuWbLocal *l = &n->wb.locals[qid];

        qemu_mutex_lock(&l->lpn_index_lock);
        wb_clear_trim_tombstone_locked(l, lpn);
        qemu_mutex_unlock(&l->lpn_index_lock);
    }
}

static void *femu_wb_flush_monitor_thread(void *opaque)
{
    FemuCtrl *n = opaque;
    FemuWriteBuffer *wb = &n->wb;

    while (!wb->flush_thread_stop) {
        if (!wb->layout_ready || !wb->locals) {
            usleep(1000);
            continue;
        }

        for (uint32_t qid = 1; qid <= wb->nr_queues; qid++) {
            FemuWbLocal *l = &wb->locals[qid];
            bool kick = false;

            qemu_mutex_lock(&l->lpn_index_lock);

            if (l->activity_seq == l->monitor_seq) {
                l->idle_rounds++;
            } else {
                l->monitor_seq = l->activity_seq;
                l->idle_rounds = 0;
            }

            if (l->wb_bytes && l->used * 100ULL >=
                l->wb_bytes * n->cfg_wb_flush_watermark_pct) {
                // log一下
                femu_log("WB flush hint: qid=%u wb_used=%" PRIu64 " wb_total_bytes=%" PRIu64
                         " used_pct=%" PRIu64 "%% idle_rounds=%u\n",
                         qid, l->used, l->wb_bytes,
                         (uint64_t)(l->wb_bytes ? (l->used * 100ULL / l->wb_bytes) : 0),
                         l->idle_rounds);
                kick = true;
            }
            if (l->used && l->idle_rounds >= l->idle_rounds_threshold) {
                kick = true;
            }

            if (kick) {
                l->flush_hint = true;
                wb->flush_kick_cnt++;
            }

            qemu_mutex_unlock(&l->lpn_index_lock);
        }

        wb_perf_log_if_due(n);

        wb->flush_thread_rounds++;
        usleep(1000);
    }

    return NULL;
}
