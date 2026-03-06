#ifndef __FEMU_WB_TYPES_H
#define __FEMU_WB_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "qemu/compiler.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "../lib/rb_tree.h"

typedef struct _GHashTable GHashTable;
typedef struct _GPtrArray GPtrArray;

enum FemuMcpEntryType {
    FEMU_MCP_ENTRY_WRITE_ALLOC = 0,
    FEMU_MCP_ENTRY_READ_HIT    = 1,
};

enum FemuMcpEntryFlags {
    FEMU_MCP_F_LAST_SEG        = 1u << 0,
};

enum FemuWbSegState {
    FEMU_WB_SEG_STAGED = 0,
    FEMU_WB_SEG_COPY_DONE = 1,
    FEMU_WB_SEG_FLUSHED = 2,
};

enum FemuWbCmdTrackType {
    FEMU_WB_CMD_TRACK_WRITE = 0,
    FEMU_WB_CMD_TRACK_READ = 1,
};

typedef struct QEMU_PACKED FemuMcpEntry {
    uint32_t cmd_id;
    uint32_t metadata_size;

    uint64_t hmb_vaddr;
    uint64_t hmb_off;

    uint32_t prp_off;
    uint32_t length;

    uint64_t slba;
    uint64_t lpn;

    uint32_t nlb;
    uint16_t qid;
    uint8_t type;
    uint8_t flags;

    uint8_t rsvd[8];
} FemuMcpEntry;

typedef struct QEMU_PACKED FemuWbKvaPushEntry {
    uint64_t gpa;
    uint64_t kva;
} FemuWbKvaPushEntry;

typedef struct FemuWbKvaMapEntry {
    uint64_t gpa;
    uint64_t kva;
    uint64_t size;
} FemuWbKvaMapEntry;

typedef struct FemuWbSeg {
    QTAILQ_ENTRY(FemuWbSeg) entry;

    uint64_t alloc_seq;
    uint64_t lpn;
    uint64_t slba;
    uint32_t nlb;

    uint64_t hmb_off;
    uint64_t hmb_vaddr;
    uint64_t rel_off;

    uint32_t len;
    uint32_t prp_off;
    uint32_t cmd_id;
    uint16_t qid;

    uint8_t state;
    bool trimmed;
    bool indexed;
    FemuRbNode *rbn;
} FemuWbSeg;

QTAILQ_HEAD(FemuWbSegQ, FemuWbSeg);

typedef struct FemuWbCmdTrack {
    uint32_t cmd_id;
    uint8_t type;
    uint32_t seg_cnt;
    FemuWbSeg **segs;
    uint32_t *mcp_slots;
} FemuWbCmdTrack;

typedef struct FemuWbLocal {
    uint16_t qid;

    uint64_t wb_off;
    uint64_t wb_bytes;
    uint64_t head;
    uint64_t tail;
    uint64_t used;

    uint64_t mcp_off;
    uint64_t mcp_bytes;
    uint32_t mcp_head;
    uint32_t mcp_tail;
    uint32_t mcp_capacity;
    uint32_t mcp_free_cnt;

    FemuRbTree lpn_index;
    QemuMutex lpn_index_lock;
    bool lpn_index_lock_inited;

    GHashTable *cmd_track_map;
    GHashTable *trim_tombstones;
    union FemuWbSegQ flush_q;
    uint64_t alloc_seq;

    uint64_t activity_seq;
    uint64_t monitor_seq;
    uint32_t idle_rounds;
    uint32_t idle_rounds_threshold;
    bool flush_hint;

    uint64_t idx_hits;
    uint64_t idx_misses;
    uint64_t flush_bytes;
    uint64_t mcp_full_cnt;
    uint64_t fallback_cnt;
    uint64_t trim_tombstone_cnt;
    uint64_t trim_safe_reclaim_cnt;
    uint64_t trim_skip_busy_cnt;
} FemuWbLocal;

typedef struct FemuWriteBuffer {
    bool layout_ready;
    bool wb_enabled;
    bool gate_waiting_kva_push;

    uint64_t hmb_wb_base;
    uint64_t hmb_wb_bytes;

    uint32_t nr_queues;
    uint32_t mcp_entries_per_q;
    uint32_t mcp_entry_bytes;

    FemuWbLocal *locals;

    GHashTable *kva_map;
    GPtrArray *kva_ranges;
    bool kva_map_ready;
    bool kva_seq_valid;
    uint32_t kva_seq;

    uint64_t kva_push_cnt;
    uint64_t copy_done_notify_cnt;
    uint64_t read_done_notify_cnt;

    uint64_t idx_hits;
    uint64_t idx_misses;
    uint64_t flush_bytes;
    uint64_t mcp_full_cnt;
    uint64_t fallback_cnt;
    uint64_t trim_tombstone_cnt;
    uint64_t trim_safe_reclaim_cnt;
    uint64_t trim_skip_busy_cnt;

    QemuThread flush_thread;
    bool flush_thread_started;
    bool flush_thread_stop;
    uint64_t flush_thread_rounds;
    uint64_t flush_kick_cnt;
    uint64_t flush_done_cnt;
    uint64_t wb_bypass_cnt;

    /* Protocol-chain perf counters (controller side) */
    uint64_t perf_stage_calls;
    uint64_t perf_stage_ok;
    uint64_t perf_stage_ns;
    uint64_t perf_stage_bytes;

    uint64_t perf_copy_done_calls;
    uint64_t perf_copy_done_ns;
    uint64_t perf_copy_done_segs;
    uint64_t perf_copy_lock_wait_ns;
    uint64_t perf_copy_lock_hold_ns;
    uint64_t perf_copy_lookup_ns;
    uint64_t perf_copy_mcp_release_ns;
    uint64_t perf_copy_loop_ns;
    uint64_t perf_copy_mirror_ns;
    uint64_t perf_copy_index_ns;
    uint64_t perf_copy_reclaim_ns;
    uint64_t perf_copy_remove_track_ns;
    uint64_t perf_copy_trimmed_segs;
    uint64_t perf_copy_mirror_fail_segs;
    uint64_t perf_copy_keep_old_busy;
    uint64_t perf_copy_insert_fail;

    uint64_t perf_read_done_calls;
    uint64_t perf_read_done_ns;
    uint64_t perf_read_done_segs;

    uint64_t perf_flush_calls;
    uint64_t perf_flush_ns;
    uint64_t perf_flush_segs;
    uint64_t perf_flush_bytes;

    uint64_t perf_last_log_ns;
    uint64_t perf_last_stage_calls;
    uint64_t perf_last_stage_ok;
    uint64_t perf_last_stage_ns;
    uint64_t perf_last_stage_bytes;
    uint64_t perf_last_copy_done_calls;
    uint64_t perf_last_copy_done_ns;
    uint64_t perf_last_copy_done_segs;
    uint64_t perf_last_copy_lock_wait_ns;
    uint64_t perf_last_copy_lock_hold_ns;
    uint64_t perf_last_copy_lookup_ns;
    uint64_t perf_last_copy_mcp_release_ns;
    uint64_t perf_last_copy_loop_ns;
    uint64_t perf_last_copy_mirror_ns;
    uint64_t perf_last_copy_index_ns;
    uint64_t perf_last_copy_reclaim_ns;
    uint64_t perf_last_copy_remove_track_ns;
    uint64_t perf_last_copy_trimmed_segs;
    uint64_t perf_last_copy_mirror_fail_segs;
    uint64_t perf_last_copy_keep_old_busy;
    uint64_t perf_last_copy_insert_fail;
    uint64_t perf_last_read_done_calls;
    uint64_t perf_last_read_done_ns;
    uint64_t perf_last_read_done_segs;
    uint64_t perf_last_flush_calls;
    uint64_t perf_last_flush_ns;
    uint64_t perf_last_flush_segs;
    uint64_t perf_last_flush_bytes;
} FemuWriteBuffer;

#endif