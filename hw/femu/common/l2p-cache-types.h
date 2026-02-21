#ifndef __FEMU_L2P_CACHE_TYPES_H
#define __FEMU_L2P_CACHE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct _GHashTable GHashTable;

typedef struct FemuL2pCacheMeta {
    uint64_t tag;
    int32_t prev;
    int32_t next;
    uint8_t valid;
} FemuL2pCacheMeta;

typedef struct FemuL2pL2Cache {
    bool initialized;
    uint32_t algo;
    uint32_t page_size;
    uint32_t ents_per_page;
    uint32_t nr_slots;
    uint32_t used_slots;
    int32_t lru_head;
    int32_t lru_tail;

    FemuL2pCacheMeta *meta;
    GHashTable *tag2slot;

    uint64_t hmb_total_bytes;
    uint32_t hmb_seg_count;
    uint64_t *hmb_seg_addrs;
    uint64_t *hmb_seg_sizes;

    uint64_t hits;
    uint64_t misses;
    uint64_t evicts;
} FemuL2pL2Cache;

#endif
