#include "write_buffer.h"

typedef struct FemuWbNotifyArgs {
    uint32_t cmd_id;
    uint16_t qid;
    uint32_t seg_cnt;
} FemuWbNotifyArgs;

static inline uint64_t wb_align_up(uint64_t v, uint64_t a)
{
    return ((v + a - 1) / a) * a;
}

static inline uint64_t wb_align_down(uint64_t v, uint64_t a)
{
    return (v / a) * a;
}

static void femu_wb_disable_with_reason(FemuCtrl *n, const char *reason)
{
    n->wb.layout_ready = false;
    n->wb.wb_enabled = false;
    n->wb.gate_waiting_kva_push = true;

    femu_log("WB disabled (degraded to L2P-only path): %s\n", reason);
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

    if (wb->locals) {
        for (uint32_t qid = 1; qid <= n->nr_io_queues; qid++) {
            FemuWbLocal *l = &wb->locals[qid];

            if (l->lpn_index_lock_inited) {
                qemu_mutex_destroy(&l->lpn_index_lock);
                l->lpn_index_lock_inited = false;
            }
        }
        g_free(wb->locals);
        wb->locals = NULL;
    }

    femu_wb_kva_map_reset(wb);

    memset(wb, 0, sizeof(*wb));
    wb->mcp_entries_per_q = FEMU_WB_MCP_ENTRIES_PER_Q;
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

    if (!n->hmb_enabled || !wb->layout_ready || !wb->locals) {
        femu_err("WB 0xd0 reject: HMB/WB layout not ready (hmb_enabled=%d layout_ready=%d)\n",
                 n->hmb_enabled, wb->layout_ready);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert(wb->hmb_wb_base + wb->hmb_wb_bytes <= n->hmb_size_bytes);

    if (action != 0x1) {
        femu_err("WB 0xd0 reject: unsupported action=%u\n", action);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!chunk_count || chunk_count != n->hmb_desc_count) {
        femu_err("WB 0xd0 reject: chunk_count=%u hmb_desc_count=%u\n",
                 chunk_count, n->hmb_desc_count);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (buf_size != (uint32_t)chunk_count * sizeof(FemuWbKvaPushEntry)) {
        femu_err("WB 0xd0 reject: buf_size=%u expected=%zu\n",
                 buf_size,
                 (size_t)chunk_count * sizeof(FemuWbKvaPushEntry));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (wb->kva_seq_valid) {
        if (seq_num < wb->kva_seq) {
            femu_err("WB 0xd0 reject: seq rollback new=%u old=%u\n",
                     seq_num, wb->kva_seq);
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        if (seq_num == wb->kva_seq) {
            femu_log("WB 0xd0 idempotent replay: seq=%u, keeping existing KVA map\n",
                     seq_num);
            return NVME_SUCCESS;
        }
    }

    entries = g_malloc0(buf_size);
    if (!entries) {
        femu_err("WB 0xd0 reject: no memory for %u bytes\n", buf_size);
        return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
    }

    if (dma_write_prp(n, (uint8_t *)entries, buf_size, prp1, prp2)) {
        femu_err("WB 0xd0 reject: failed to read mapping payload via PRP\n");
        g_free(entries);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    femu_wb_kva_map_reset(wb);
    if (!femu_wb_kva_map_init(wb)) {
        femu_err("WB 0xd0 reject: cannot initialize KVA map structures\n");
        g_free(entries);
        return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
    }

    for (uint16_t i = 0; i < chunk_count; i++) {
        FemuWbKvaMapEntry *ent;
        uint64_t *key;
        int desc_idx = wb_find_hmb_desc_by_base(n, entries[i].gpa);

        if (desc_idx < 0) {
            femu_err("WB 0xd0 reject: gpa=0x%" PRIx64 " not found in HMB descriptors\n",
                     entries[i].gpa);
            femu_wb_kva_map_reset(wb);
            g_free(entries);
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        if (!entries[i].kva) {
            femu_err("WB 0xd0 reject: zero KVA for gpa=0x%" PRIx64 "\n",
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
            femu_err("WB 0xd0 reject: no memory for map entry\n");
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

    femu_log("WB 0xd0 accepted: seq=%u chunks=%u map_ready=1 wb_enabled=1\n",
             seq_num, chunk_count);

    g_free(entries);
    return NVME_SUCCESS;
}

uint16_t femu_wb_io_notify_copy_done(FemuCtrl *n, NvmeCmd *cmd,
                                     NvmeRequest *req)
{
    FemuWbNotifyArgs args = {0};

    if (!wb_parse_notify_args(n, cmd, req, &args, "WB 0xd1")) {
        return NVME_SUCCESS;
    }

    if (!n->wb.wb_enabled) {
        femu_log("WB 0xd1 ignored: wb_enabled=0 qid=%u cmd_id=%u seg_cnt=%u\n",
                 args.qid, args.cmd_id, args.seg_cnt);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    n->wb.copy_done_notify_cnt++;

    femu_log("WB 0xd1 COPY_DONE: qid=%u cmd_id=%u seg_cnt=%u\n",
             args.qid, args.cmd_id, args.seg_cnt);
    req->status = NVME_SUCCESS;
    return NVME_SUCCESS;
}

uint16_t femu_wb_io_notify_read_done(FemuCtrl *n, NvmeCmd *cmd,
                                     NvmeRequest *req)
{
    FemuWbNotifyArgs args = {0};

    if (!wb_parse_notify_args(n, cmd, req, &args, "WB 0xd2")) {
        return NVME_SUCCESS;
    }

    if (!n->wb.wb_enabled) {
        femu_log("WB 0xd2 ignored: wb_enabled=0 qid=%u cmd_id=%u seg_cnt=%u\n",
                 args.qid, args.cmd_id, args.seg_cnt);
        req->status = NVME_SUCCESS;
        return NVME_SUCCESS;
    }

    n->wb.read_done_notify_cnt++;

    femu_log("WB 0xd2 READ_DONE: qid=%u cmd_id=%u seg_cnt=%u\n",
             args.qid, args.cmd_id, args.seg_cnt);
    req->status = NVME_SUCCESS;
    return NVME_SUCCESS;
}

bool femu_wb_init_layout(FemuCtrl *n)
{
    FemuWriteBuffer *wb = &n->wb;
    const uint64_t page_sz = FEMU_WB_ALIGN_BYTES;
    const uint64_t l2_bytes = (uint64_t)FEMU_L2P_L2_SIZE_KB * 1024ULL;
    uint64_t l2_end = wb_align_up(l2_bytes, page_sz);
    uint64_t wb_total_pages;
    uint64_t wb_total_bytes;
    uint64_t pages_per_q;
    uint64_t mcp_bytes_raw;
    uint64_t mcp_bytes_aligned;
    uint64_t mcp_pages;
    uint64_t cursor_pages = 0;

    femu_wb_ctrl_reset(n);

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

    mcp_bytes_raw = (uint64_t)FEMU_WB_MCP_ENTRIES_PER_Q * FEMU_WB_MCP_ENTRY_BYTES;
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
    wb->mcp_entries_per_q = FEMU_WB_MCP_ENTRIES_PER_Q;
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

    femu_log("WB layout ready: HMB=%" PRIu64 "B L2P-L2=%" PRIu64
             "B WB_base=0x%" PRIx64 " WB_bytes=%" PRIu64
             "B nr_ioq=%u mcp_entries/q=%u mcp_entry_bytes=%u\n",
             n->hmb_size_bytes, l2_bytes,
             wb->hmb_wb_base, wb->hmb_wb_bytes,
             n->nr_io_queues,
             wb->mcp_entries_per_q,
             wb->mcp_entry_bytes);
    femu_log("WB capability gate: waiting for vendor admin 0xd0 before wb_enabled=true\n");

    return true;
}
