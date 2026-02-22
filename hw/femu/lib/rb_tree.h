#ifndef __FEMU_RB_TREE_H
#define __FEMU_RB_TREE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * FEMU write-buffer index RB-tree (no internal lock).
 *
 * Concurrency model:
 * - Tree structural operations (find/insert/upsert/erase/iterate) MUST be
 *   protected by caller-provided per-queue lock.
 * - Reference counter is atomic and can be adjusted concurrently.
 *
 * Duplicate-LPN upsert risk (important):
 * - This tree uses one node per LPN. When `femu_rb_upsert()` finds existing
 *   key, it overwrites payload fields in-place (`hmb_off`, `len`, `state`).
 * - If caller upserts while old data is still being consumed, readers may
 *   observe remapped payload (old request still holds ref but node now points
 *   to newer HMB range), causing data/version confusion.
 * - Therefore caller SHOULD only upsert when old version is reclaimable, i.e.
 *   `refcnt == 0`, or enforce a stronger versioning protocol above this layer.
 *
 * Reclaim rule:
 * - Erase/recycle node only when `femu_rb_refcnt_read(node) == 0`.
 */

typedef struct FemuRbNode {
    struct FemuRbNode *parent;
    struct FemuRbNode *left;
    struct FemuRbNode *right;
    bool red;

    /* key */
    uint64_t lpn;

    /* value */
    uint64_t hmb_off;
    uint32_t len;
    uint8_t state;

    int32_t refcnt;
} FemuRbNode;

typedef struct FemuRbTree {
    FemuRbNode *root;
    uint64_t count;
} FemuRbTree;

void femu_rb_tree_init(FemuRbTree *t);
void femu_rb_node_init(FemuRbNode *n, uint64_t lpn, uint64_t hmb_off,
                       uint32_t len, uint8_t state);

FemuRbNode *femu_rb_find(FemuRbTree *t, uint64_t lpn);
FemuRbNode *femu_rb_find_ge(FemuRbTree *t, uint64_t lpn);

bool femu_rb_insert(FemuRbTree *t, FemuRbNode *n);
FemuRbNode *femu_rb_upsert(FemuRbTree *t, FemuRbNode *n, bool *inserted);

void femu_rb_remove(FemuRbTree *t, FemuRbNode *n);
FemuRbNode *femu_rb_erase_by_lpn(FemuRbTree *t, uint64_t lpn);

FemuRbNode *femu_rb_first(FemuRbTree *t);
FemuRbNode *femu_rb_next(FemuRbNode *n);

int32_t femu_rb_refcnt_read(FemuRbNode *n);

void femu_rb_refcnt_set(FemuRbNode *n, int32_t v);

int32_t femu_rb_refcnt_inc(FemuRbNode *n);

int32_t femu_rb_refcnt_dec(FemuRbNode *n);

#endif
