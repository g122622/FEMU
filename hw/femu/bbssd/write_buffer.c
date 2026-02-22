#include "write_buffer.h"

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

    memset(wb, 0, sizeof(*wb));
    wb->mcp_entries_per_q = FEMU_WB_MCP_ENTRIES_PER_Q;
    wb->mcp_entry_bytes = FEMU_WB_MCP_ENTRY_BYTES;
    wb->gate_waiting_kva_push = true;
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
