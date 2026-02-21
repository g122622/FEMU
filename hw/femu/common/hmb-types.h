#ifndef __FEMU_HMB_TYPES_H
#define __FEMU_HMB_TYPES_H

#include <stdint.h>
#include "qemu/compiler.h"

typedef struct QEMU_PACKED NvmeHmbDescriptor {
    uint64_t    addr;
    uint32_t    size;
    uint32_t    rsvd;
} NvmeHmbDescriptor;

typedef struct QEMU_PACKED NvmeHmbAttrs {
    uint32_t    hsize;
    uint32_t    hmdlal;
    uint32_t    hmdlau;
    uint32_t    hmdlec;
} NvmeHmbAttrs;

#endif
