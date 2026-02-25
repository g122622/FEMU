#ifndef HMB_CONFIG_H
#define HMB_CONFIG_H

/*
 * NOTE:
 * - These macros define compile-time defaults.
 * - They may be overridden at runtime by FEMU device properties.
 */

#define FEMU_EXP_ENABLE_HMB_DEFAULT 1

#define FEMU_HMB_HMMIN_MB 64
#define FEMU_HMB_HMPRE_MB 64
#define FEMU_HMB_MB_TO_4K_UNITS(mb) (((mb) * MiB) / 4096)
#define FEMU_HMB_HMMIN_UNITS FEMU_HMB_MB_TO_4K_UNITS(FEMU_HMB_HMMIN_MB)
#define FEMU_HMB_HMPRE_UNITS FEMU_HMB_MB_TO_4K_UNITS(FEMU_HMB_HMPRE_MB)
#define FEMU_HMB_TEST_MAGIC 0x1234567890123456ULL

#endif
