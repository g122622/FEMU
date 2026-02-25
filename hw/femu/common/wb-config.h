#ifndef __FEMU_WB_CONFIG_H
#define __FEMU_WB_CONFIG_H

/*
 * NOTE:
 * - These macros define compile-time defaults.
 * - They may be overridden at runtime by FEMU device properties.
 */

#define FEMU_EXP_ENABLE_WB_DEFAULT   1

#define FEMU_WB_ALIGN_BYTES          4096ULL
#define FEMU_WB_MCP_ENTRY_BYTES      64U      // 这个不可乱改
#define FEMU_WB_MCP_ENTRIES_PER_Q    1024U    // 每个队列固定的MCP entry数量，过大过小都不合适，需配合page size和队列数量调整
#define FEMU_WB_FLUSH_WATERMARK_PCT  80U      // 超过这个使用率就触发flush
#define FEMU_WB_IDLE_ROUNDS_DEFAULT  16U
#define FEMU_CQE_RSVD_MCP_READY      (1U << 0)

#endif