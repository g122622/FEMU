#ifndef __FEMU_L2P_CACHE_CONFIG_H
#define __FEMU_L2P_CACHE_CONFIG_H

/*
 * L2P cache hierarchy configuration shared by controller and FTL.
 * Size units are KiB.
 *
 * NOTE:
 * - These macros define compile-time defaults.
 * - They may be overridden at runtime by FEMU device properties.
 */
#define FEMU_EXP_ENABLE_L2P_MULTILEVEL_DEFAULT 1
#define FEMU_L2P_L1_SIZE_KB                512
#define FEMU_L2P_L2_SIZE_KB                (16 * 1024)

/* L2P page-table cache granularity. */
#define FEMU_L2P_PT_PAGE_SIZE              4096

/*
 * Metadata latency model (ns):
 * - L1 (controller SRAM): default 0ns (configurable)
 * - L2 (HMB over PCIe): fixed cost per metadata access
 * - L3 (flash-resident table): derived from NAND latencies via divisors below
 */
#define FEMU_L2P_L1_RD_LAT_NS              10ULL
#define FEMU_L2P_L1_WR_LAT_NS              10ULL
#define FEMU_L2P_L2_RD_LAT_NS              1200ULL
#define FEMU_L2P_L2_WR_LAT_NS              1800ULL

/* L3 latency derived as NAND latency * MUL, min 1ns. */
#define FEMU_L2P_L3_RD_LAT_MUL             1
#define FEMU_L2P_L3_WR_LAT_MUL             1

enum femu_l2p_bypass_meta_mode {
    FEMU_L2P_BYPASS_META_ZERO = 0,
    FEMU_L2P_BYPASS_META_L3 = 1,
};

#define FEMU_L2P_BYPASS_META_MODE_DEFAULT  FEMU_L2P_BYPASS_META_ZERO

enum femu_l2p_cache_algo {
    FEMU_L2P_CACHE_ALGO_LRU = 0,
    FEMU_L2P_CACHE_ALGO_ARC = 1,
};

#define FEMU_L2P_CACHE_ALGO_DEFAULT        FEMU_L2P_CACHE_ALGO_LRU

#endif
