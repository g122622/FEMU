#include "l2p_cache.h"

#define FEMU_L2P_STATS_LOG_PERIOD_NS      (5ULL * 1000 * 1000 * 1000)

static inline uint64_t l2p_get_ptid(struct ssd *ssd, uint64_t lpn)
{
    return lpn / ssd->l2p_l1.ents_per_page;
}

static inline uint32_t l2p_get_ptoff(struct ssd *ssd, uint64_t lpn)
{
    return lpn % ssd->l2p_l1.ents_per_page;
}

static inline void l2p_lru_remove(FemuL2pCacheMeta *meta, int32_t *head,
                                  int32_t *tail, int32_t idx)
{
    int32_t prev = meta[idx].prev;
    int32_t next = meta[idx].next;

    if (prev >= 0) {
        meta[prev].next = next;
    } else {
        *head = next;
    }

    if (next >= 0) {
        meta[next].prev = prev;
    } else {
        *tail = prev;
    }

    meta[idx].prev = -1;
    meta[idx].next = -1;
}

static inline void l2p_lru_push_front(FemuL2pCacheMeta *meta, int32_t *head,
                                      int32_t *tail, int32_t idx)
{
    meta[idx].prev = -1;
    meta[idx].next = *head;
    if (*head >= 0) {
        meta[*head].prev = idx;
    } else {
        *tail = idx;
    }
    *head = idx;
}

static inline void l2p_lru_touch(FemuL2pCacheMeta *meta, int32_t *head,
                                 int32_t *tail, int32_t idx)
{
    if (*head == idx) {
        return;
    }

    l2p_lru_remove(meta, head, tail, idx);
    l2p_lru_push_front(meta, head, tail, idx);
}

typedef struct L2pCacheAlgoOps {
    int32_t (*pick_victim)(FemuL2pCacheMeta *meta, uint32_t nr_slots,
                           uint32_t *used_slots, int32_t *head,
                           int32_t *tail);
    void (*touch)(FemuL2pCacheMeta *meta, int32_t *head, int32_t *tail,
                  int32_t idx);
} L2pCacheAlgoOps;

static int32_t l2p_lru_pick_victim(FemuL2pCacheMeta *meta, uint32_t nr_slots,
                                   uint32_t *used_slots, int32_t *head,
                                   int32_t *tail)
{
    (void)meta;
    (void)head;
    if (*used_slots < nr_slots) {
        return (*used_slots)++;
    }

    ftl_assert(*tail >= 0);
    return *tail;
}

static const L2pCacheAlgoOps l2p_algo_lru_ops = {
    .pick_victim = l2p_lru_pick_victim,
    .touch = l2p_lru_touch,
};

static const L2pCacheAlgoOps *l2p_get_algo_ops(uint32_t algo)
{
    switch (algo) {
    case FEMU_L2P_CACHE_ALGO_LRU:
        return &l2p_algo_lru_ops;
    default:
        ftl_err("unsupported L2P cache algo=%u, fallback to LRU\n", algo);
        return &l2p_algo_lru_ops;
    }
}

static inline struct ppa *l1_slot_base(struct ssd *ssd, int32_t slot)
{
    return &ssd->l2p_l1.slots[(uint64_t)slot * ssd->l2p_l1.ents_per_page];
}

static void l3_copy_pt_page_from_maptbl(struct ssd *ssd, uint64_t ptid,
                                        struct ppa *dst)
{
    uint64_t first_lpn = ptid * ssd->l2p_l1.ents_per_page;

    for (uint32_t i = 0; i < ssd->l2p_l1.ents_per_page; i++) {
        uint64_t lpn = first_lpn + i;

        dst[i].ppa = (lpn < ssd->sp.tt_pgs) ? ssd->maptbl[lpn].ppa : UNMAPPED_PPA;
    }
}

static inline void set_maptbl_ent_raw(struct ssd *ssd, uint64_t lpn,
                                      struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static bool l2p_l2_hmb_rw(struct ssd *ssd, uint64_t off, void *buf,
                          uint32_t len, bool is_write)
{
    FemuCtrl *n = ssd->n;
    FemuL2pL2Cache *l2 = &n->l2p_l2;
    uint8_t *p = buf;
    uint64_t cur = 0;
    uint32_t left = len;

    if (off + len > l2->hmb_total_bytes) {
        return false;
    }

    for (uint32_t i = 0; i < l2->hmb_seg_count && left; i++) {
        uint64_t seg_sz = l2->hmb_seg_sizes[i];

        if (off >= cur + seg_sz) {
            cur += seg_sz;
            continue;
        }

        uint64_t in_seg = off > cur ? off - cur : 0;
        uint64_t seg_avail = seg_sz - in_seg;
        uint32_t xfer = MIN((uint64_t)left, seg_avail);
        uint64_t gpa = l2->hmb_seg_addrs[i] + in_seg;

        if (is_write) {
            nvme_addr_write(n, gpa, p, xfer);
        } else {
            nvme_addr_read(n, gpa, p, xfer);
        }

        p += xfer;
        off += xfer;
        left -= xfer;
        cur += seg_sz;
    }

    return left == 0;
}

static inline bool l2p_l2_slot_read_page(struct ssd *ssd, int32_t slot,
                                         struct ppa *dst)
{
    uint64_t off = (uint64_t)slot * FEMU_L2P_PT_PAGE_SIZE;
    return l2p_l2_hmb_rw(ssd, off, dst, FEMU_L2P_PT_PAGE_SIZE, false);
}

static inline bool l2p_l2_slot_write_page(struct ssd *ssd, int32_t slot,
                                          struct ppa *src)
{
    uint64_t off = (uint64_t)slot * FEMU_L2P_PT_PAGE_SIZE;
    return l2p_l2_hmb_rw(ssd, off, src, FEMU_L2P_PT_PAGE_SIZE, true);
}

static int32_t l2p_find_l1_slot(struct ssd *ssd, uint64_t ptid)
{
    gpointer v = g_hash_table_lookup(ssd->l2p_l1.tag2slot, &ptid);

    return v ? GPOINTER_TO_INT(v) - 1 : -1;
}

static int32_t l2p_find_l2_slot(struct ssd *ssd, uint64_t ptid)
{
    gpointer v = g_hash_table_lookup(ssd->n->l2p_l2.tag2slot, &ptid);

    return v ? GPOINTER_TO_INT(v) - 1 : -1;
}

static void l2p_l1_install_page(struct ssd *ssd, uint64_t ptid,
                                struct ppa *src_page, uint64_t *lat,
                                bool account_lat)
{
    const L2pCacheAlgoOps *ops = l2p_get_algo_ops(ssd->l2p_l1.algo);
    int32_t victim = ops->pick_victim(ssd->l2p_l1.meta, ssd->l2p_l1.nr_slots,
                                      &ssd->l2p_l1.used_slots,
                                      &ssd->l2p_l1.lru_head,
                                      &ssd->l2p_l1.lru_tail);

    if (ssd->l2p_l1.meta[victim].valid) {
        uint64_t old_tag = ssd->l2p_l1.meta[victim].tag;
        g_hash_table_remove(ssd->l2p_l1.tag2slot, &old_tag);
        ssd->l2p_l1.evicts++;
    }

    memcpy(l1_slot_base(ssd, victim), src_page, FEMU_L2P_PT_PAGE_SIZE);

    ssd->l2p_l1.meta[victim].tag = ptid;
    ssd->l2p_l1.meta[victim].valid = 1;
    if (ssd->l2p_l1.meta[victim].prev != -1 || ssd->l2p_l1.meta[victim].next != -1 ||
        ssd->l2p_l1.lru_head == victim || ssd->l2p_l1.lru_tail == victim) {
        l2p_lru_remove(ssd->l2p_l1.meta, &ssd->l2p_l1.lru_head,
                       &ssd->l2p_l1.lru_tail, victim);
    }
    l2p_lru_push_front(ssd->l2p_l1.meta, &ssd->l2p_l1.lru_head,
                       &ssd->l2p_l1.lru_tail, victim);
    g_hash_table_insert(ssd->l2p_l1.tag2slot, &ssd->l2p_l1.meta[victim].tag,
                        GINT_TO_POINTER(victim + 1));

    if (account_lat && lat) {
        *lat += ssd->l2p_lat.l1_wr_lat;
    }
}

static void l2p_l2_install_page(struct ssd *ssd, uint64_t ptid,
                                struct ppa *src_page, uint64_t *lat,
                                bool account_lat)
{
    FemuL2pL2Cache *l2 = &ssd->n->l2p_l2;
    const L2pCacheAlgoOps *ops = l2p_get_algo_ops(l2->algo);
    int32_t victim = ops->pick_victim(l2->meta, l2->nr_slots,
                                      &l2->used_slots, &l2->lru_head,
                                      &l2->lru_tail);

    if (l2->meta[victim].valid) {
        uint64_t old_tag = l2->meta[victim].tag;
        g_hash_table_remove(l2->tag2slot, &old_tag);
        l2->evicts++;
    }

    ftl_assert(l2p_l2_slot_write_page(ssd, victim, src_page));

    l2->meta[victim].tag = ptid;
    l2->meta[victim].valid = 1;
    if (l2->meta[victim].prev != -1 || l2->meta[victim].next != -1 ||
        l2->lru_head == victim || l2->lru_tail == victim) {
        l2p_lru_remove(l2->meta, &l2->lru_head, &l2->lru_tail, victim);
    }
    l2p_lru_push_front(l2->meta, &l2->lru_head, &l2->lru_tail, victim);
    g_hash_table_insert(l2->tag2slot, &l2->meta[victim].tag,
                        GINT_TO_POINTER(victim + 1));

    if (account_lat && lat) {
        *lat += ssd->l2p_lat.l2_wr_lat;
    }
}

static struct ppa get_maptbl_ent_internal(struct ssd *ssd, uint64_t lpn,
                                          uint64_t *meta_lat,
                                          bool account_lat)
{
    uint64_t ptid;
    uint32_t ptoff;
    int32_t slot;
    struct ppa page_buf[FEMU_L2P_PT_PAGE_SIZE / sizeof(struct ppa)];

    ftl_assert(lpn < ssd->sp.tt_pgs);
    ptid = l2p_get_ptid(ssd, lpn);
    ptoff = l2p_get_ptoff(ssd, lpn);

    if (account_lat && meta_lat) {
        *meta_lat += ssd->l2p_lat.l1_rd_lat;
    }

    slot = l2p_find_l1_slot(ssd, ptid);
    if (slot >= 0) {
        l2p_get_algo_ops(ssd->l2p_l1.algo)->touch(ssd->l2p_l1.meta,
                &ssd->l2p_l1.lru_head, &ssd->l2p_l1.lru_tail, slot);
        ssd->l2p_l1.hits++;
        return l1_slot_base(ssd, slot)[ptoff];
    }

    ssd->l2p_l1.misses++;
    if (account_lat && meta_lat) {
        *meta_lat += ssd->l2p_lat.l2_rd_lat;
    }

    slot = l2p_find_l2_slot(ssd, ptid);
    if (slot >= 0) {
        FemuL2pL2Cache *l2 = &ssd->n->l2p_l2;

        ftl_assert(l2p_l2_slot_read_page(ssd, slot, page_buf));
        l2p_get_algo_ops(l2->algo)->touch(l2->meta, &l2->lru_head,
                &l2->lru_tail, slot);
        l2->hits++;
        l2p_l1_install_page(ssd, ptid, page_buf, meta_lat, account_lat);
        return page_buf[ptoff];
    }

    ssd->n->l2p_l2.misses++;
    if (account_lat && meta_lat) {
        *meta_lat += ssd->l2p_lat.l3_rd_lat;
    }
    l3_copy_pt_page_from_maptbl(ssd, ptid, page_buf);
    l2p_l2_install_page(ssd, ptid, page_buf, meta_lat, account_lat);
    l2p_l1_install_page(ssd, ptid, page_buf, meta_lat, account_lat);

    return page_buf[ptoff];
}

static uint64_t set_maptbl_ent_internal(struct ssd *ssd, uint64_t lpn,
                                        struct ppa *ppa, bool account_lat)
{
    uint64_t ptid = l2p_get_ptid(ssd, lpn);
    uint32_t ptoff = l2p_get_ptoff(ssd, lpn);
    uint64_t maxlat = account_lat ? ssd->l2p_lat.l3_wr_lat : 0;
    int32_t l1_slot = l2p_find_l1_slot(ssd, ptid);
    int32_t l2_slot = l2p_find_l2_slot(ssd, ptid);
    struct ppa page_buf[FEMU_L2P_PT_PAGE_SIZE / sizeof(struct ppa)];

    ftl_assert(lpn < ssd->sp.tt_pgs);

    if (l2_slot < 0) {
        l3_copy_pt_page_from_maptbl(ssd, ptid, page_buf);
        l2p_l2_install_page(ssd, ptid, page_buf, NULL, false);
        if (account_lat) {
            maxlat = MAX(maxlat, ssd->l2p_lat.l2_wr_lat);
        }
        l2_slot = l2p_find_l2_slot(ssd, ptid);
        ftl_assert(l2_slot >= 0);
    }

    if (l1_slot < 0) {
        ftl_assert(l2p_l2_slot_read_page(ssd, l2_slot, page_buf));
        l2p_l1_install_page(ssd, ptid, page_buf, NULL, false);
        if (account_lat) {
            maxlat = MAX(maxlat, ssd->l2p_lat.l1_wr_lat);
        }
        l1_slot = l2p_find_l1_slot(ssd, ptid);
        ftl_assert(l1_slot >= 0);
    }

    l1_slot_base(ssd, l1_slot)[ptoff] = *ppa;
    if (account_lat) {
        maxlat = MAX(maxlat, ssd->l2p_lat.l1_wr_lat);
    }

    ftl_assert(l2p_l2_slot_read_page(ssd, l2_slot, page_buf));
    page_buf[ptoff] = *ppa;
    ftl_assert(l2p_l2_slot_write_page(ssd, l2_slot, page_buf));
    if (account_lat) {
        maxlat = MAX(maxlat, ssd->l2p_lat.l2_wr_lat);
    }

    set_maptbl_ent_raw(ssd, lpn, ppa);
    l2p_get_algo_ops(ssd->l2p_l1.algo)->touch(ssd->l2p_l1.meta,
            &ssd->l2p_l1.lru_head, &ssd->l2p_l1.lru_tail, l1_slot);
    l2p_get_algo_ops(ssd->n->l2p_l2.algo)->touch(ssd->n->l2p_l2.meta,
            &ssd->n->l2p_l2.lru_head, &ssd->n->l2p_l2.lru_tail, l2_slot);

    return maxlat;
}

void femu_l2p_init_latency(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->l2p_lat.l1_rd_lat = FEMU_L2P_L1_RD_LAT_NS;
    ssd->l2p_lat.l1_wr_lat = FEMU_L2P_L1_WR_LAT_NS;
    ssd->l2p_lat.l2_rd_lat = FEMU_L2P_L2_RD_LAT_NS;
    ssd->l2p_lat.l2_wr_lat = FEMU_L2P_L2_WR_LAT_NS;
    ssd->l2p_lat.l3_rd_lat = MAX(1ULL, (uint64_t)spp->pg_rd_lat * FEMU_L2P_L3_RD_LAT_MUL);
    ssd->l2p_lat.l3_wr_lat = MAX(1ULL, (uint64_t)spp->pg_wr_lat * FEMU_L2P_L3_WR_LAT_MUL);

    ftl_log("L2P latency(ns): L1(rd=%" PRIu64 ",wr=%" PRIu64 ") L2(rd=%" PRIu64
            ",wr=%" PRIu64 ") L3(rd=%" PRIu64 ",wr=%" PRIu64 ")\n",
            ssd->l2p_lat.l1_rd_lat, ssd->l2p_lat.l1_wr_lat,
            ssd->l2p_lat.l2_rd_lat, ssd->l2p_lat.l2_wr_lat,
            ssd->l2p_lat.l3_rd_lat, ssd->l2p_lat.l3_wr_lat);
}

void femu_l2p_log_latency_config(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t l3_maptbl_bytes = (uint64_t)spp->tt_pgs * sizeof(struct ppa);

    ftl_log("Latency config(ns): NAND(rd=%d,wr=%d,erase=%d,ch_xfer=%d) "
            "L2P(L1 rd=%" PRIu64 ",wr=%" PRIu64 "; L2 rd=%" PRIu64
            ",wr=%" PRIu64 "; L3 rd=%" PRIu64 ",wr=%" PRIu64 ")\n",
            spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat, spp->ch_xfer_lat,
            ssd->l2p_lat.l1_rd_lat, ssd->l2p_lat.l1_wr_lat,
            ssd->l2p_lat.l2_rd_lat, ssd->l2p_lat.l2_wr_lat,
            ssd->l2p_lat.l3_rd_lat, ssd->l2p_lat.l3_wr_lat);

    ftl_log("L2P cache size: L1=%uKB, L2(target HMB)=%uKB, "
            "L3(maptbl)=%" PRIu64 " bytes (%" PRIu64 " KiB)\n",
            FEMU_L2P_L1_SIZE_KB, FEMU_L2P_L2_SIZE_KB,
            l3_maptbl_bytes, l3_maptbl_bytes / 1024);
}

void femu_l2p_maybe_log_stats(struct ssd *ssd)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t l1_hits = ssd->l2p_l1.hits;
    uint64_t l1_misses = ssd->l2p_l1.misses;
    uint64_t l1_total = l1_hits + l1_misses;
    uint64_t l1_dhits = l1_hits - ssd->l2p_l1_last_hits;
    uint64_t l1_dmisses = l1_misses - ssd->l2p_l1_last_misses;
    uint64_t l1_dtotal = l1_dhits + l1_dmisses;
    double l1_usage = ssd->l2p_l1.nr_slots ?
            (100.0 * (double)ssd->l2p_l1.used_slots / (double)ssd->l2p_l1.nr_slots) : 0.0;
    double l1_hit_rate = l1_total ? (100.0 * (double)l1_hits / (double)l1_total) : 0.0;
    double l1_miss_rate = l1_total ? (100.0 * (double)l1_misses / (double)l1_total) : 0.0;
    double l1_win_hit_rate = l1_dtotal ? (100.0 * (double)l1_dhits / (double)l1_dtotal) : 0.0;
    double l1_win_miss_rate = l1_dtotal ? (100.0 * (double)l1_dmisses / (double)l1_dtotal) : 0.0;

    if (ssd->l2p_stats_last_log_ns &&
        now - ssd->l2p_stats_last_log_ns < FEMU_L2P_STATS_LOG_PERIOD_NS) {
        return;
    }

    ftl_log("L2P L1 stats: usage=%.2f%% (%u/%u) hit=%" PRIu64 " miss=%" PRIu64
            " evict=%" PRIu64 " hit_rate=%.2f%% miss_rate=%.2f%% "
            "window_hit=%.2f%% window_miss=%.2f%%\n",
            l1_usage, ssd->l2p_l1.used_slots, ssd->l2p_l1.nr_slots,
            l1_hits, l1_misses, ssd->l2p_l1.evicts,
            l1_hit_rate, l1_miss_rate, l1_win_hit_rate, l1_win_miss_rate);

    if (ssd->n->l2p_l2.initialized) {
        FemuL2pL2Cache *l2 = &ssd->n->l2p_l2;
        uint64_t l2_hits = l2->hits;
        uint64_t l2_misses = l2->misses;
        uint64_t l2_total = l2_hits + l2_misses;
        uint64_t l2_dhits = l2_hits - ssd->l2p_l2_last_hits;
        uint64_t l2_dmisses = l2_misses - ssd->l2p_l2_last_misses;
        uint64_t l2_dtotal = l2_dhits + l2_dmisses;
        double l2_usage = l2->nr_slots ?
                (100.0 * (double)l2->used_slots / (double)l2->nr_slots) : 0.0;
        double l2_hit_rate = l2_total ? (100.0 * (double)l2_hits / (double)l2_total) : 0.0;
        double l2_miss_rate = l2_total ? (100.0 * (double)l2_misses / (double)l2_total) : 0.0;
        double l2_win_hit_rate = l2_dtotal ? (100.0 * (double)l2_dhits / (double)l2_dtotal) : 0.0;
        double l2_win_miss_rate = l2_dtotal ? (100.0 * (double)l2_dmisses / (double)l2_dtotal) : 0.0;

        ftl_log("L2P L2 stats: usage=%.2f%% (%u/%u) hit=%" PRIu64 " miss=%" PRIu64
                " evict=%" PRIu64 " hit_rate=%.2f%% miss_rate=%.2f%% "
                "window_hit=%.2f%% window_miss=%.2f%%\n",
                l2_usage, l2->used_slots, l2->nr_slots,
                l2_hits, l2_misses, l2->evicts,
                l2_hit_rate, l2_miss_rate, l2_win_hit_rate, l2_win_miss_rate);
    } else {
        ftl_log("L2P L2 stats: not initialized (HMB not ready)\n");
    }

    ssd->l2p_stats_last_log_ns = now;
    ssd->l2p_l1_last_hits = l1_hits;
    ssd->l2p_l1_last_misses = l1_misses;
    if (ssd->n->l2p_l2.initialized) {
        ssd->l2p_l2_last_hits = ssd->n->l2p_l2.hits;
        ssd->l2p_l2_last_misses = ssd->n->l2p_l2.misses;
    }
}

void femu_l2p_init_l1_cache(struct ssd *ssd)
{
    uint32_t ents_per_page = FEMU_L2P_PT_PAGE_SIZE / sizeof(struct ppa);
    uint32_t nr_slots = (FEMU_L2P_L1_SIZE_KB * 1024) / FEMU_L2P_PT_PAGE_SIZE;

    ftl_assert(FEMU_L2P_PT_PAGE_SIZE % sizeof(struct ppa) == 0);
    ftl_assert(nr_slots > 0);

    ssd->l2p_l1.initialized = true;
    ssd->l2p_l1.algo = FEMU_L2P_CACHE_ALGO_DEFAULT;
    ssd->l2p_l1.page_size = FEMU_L2P_PT_PAGE_SIZE;
    ssd->l2p_l1.ents_per_page = ents_per_page;
    ssd->l2p_l1.nr_slots = nr_slots;
    ssd->l2p_l1.used_slots = 0;
    ssd->l2p_l1.lru_head = -1;
    ssd->l2p_l1.lru_tail = -1;
    ssd->l2p_l1.meta = g_malloc0(sizeof(*ssd->l2p_l1.meta) * nr_slots);
    ssd->l2p_l1.slots = g_malloc0((uint64_t)nr_slots * ents_per_page *
                                  sizeof(struct ppa));
    ssd->l2p_l1.tag2slot = g_hash_table_new(g_int64_hash, g_int64_equal);

    for (uint32_t i = 0; i < nr_slots; i++) {
        ssd->l2p_l1.meta[i].prev = -1;
        ssd->l2p_l1.meta[i].next = -1;
        ssd->l2p_l1.meta[i].valid = 0;
    }

    for (uint64_t i = 0; i < (uint64_t)nr_slots * ents_per_page; i++) {
        ssd->l2p_l1.slots[i].ppa = UNMAPPED_PPA;
    }

    ftl_log("L2P L1 cache ready: size=%uKB slots=%u entries/page=%u entry-bytes=%zu\n",
            FEMU_L2P_L1_SIZE_KB, nr_slots, ents_per_page, sizeof(struct ppa));
}

static bool ssd_try_init_l2p_l2_cache(struct ssd *ssd)
{
    FemuCtrl *n = ssd->n;
    FemuL2pL2Cache *l2 = &n->l2p_l2;
    uint64_t hmb_total = 0;
    uint32_t wanted_slots;

    if (l2->initialized) {
        return true;
    }

    if (!n->hmb_enabled || !n->hmb_descs || !n->hmb_desc_count) {
        return false;
    }

    for (uint32_t i = 0; i < n->hmb_desc_count; i++) {
        hmb_total += (uint64_t)n->hmb_descs[i].size * n->page_size;
    }

    if (hmb_total < (uint64_t)FEMU_L2P_L2_SIZE_KB * 1024) {
        ftl_err("HMB too small for L2 cache: have=%" PRIu64 " want=%u\n",
                hmb_total, FEMU_L2P_L2_SIZE_KB * 1024);
        return false;
    }

    wanted_slots = (FEMU_L2P_L2_SIZE_KB * 1024) / FEMU_L2P_PT_PAGE_SIZE;
    ftl_assert(wanted_slots > 0);

    l2->initialized = true;
    l2->algo = FEMU_L2P_CACHE_ALGO_DEFAULT;
    l2->page_size = FEMU_L2P_PT_PAGE_SIZE;
    l2->ents_per_page = FEMU_L2P_PT_PAGE_SIZE / sizeof(struct ppa);
    l2->nr_slots = wanted_slots;
    l2->used_slots = 0;
    l2->lru_head = -1;
    l2->lru_tail = -1;
    l2->meta = g_malloc0(sizeof(*l2->meta) * l2->nr_slots);
    l2->tag2slot = g_hash_table_new(g_int64_hash, g_int64_equal);
    l2->hmb_total_bytes = 0;

    l2->hmb_seg_count = n->hmb_desc_count;
    l2->hmb_seg_addrs = g_malloc0(sizeof(*l2->hmb_seg_addrs) * l2->hmb_seg_count);
    l2->hmb_seg_sizes = g_malloc0(sizeof(*l2->hmb_seg_sizes) * l2->hmb_seg_count);

    for (uint32_t i = 0; i < l2->nr_slots; i++) {
        l2->meta[i].prev = -1;
        l2->meta[i].next = -1;
    }

    for (uint32_t i = 0; i < l2->hmb_seg_count; i++) {
        l2->hmb_seg_addrs[i] = n->hmb_descs[i].addr;
        l2->hmb_seg_sizes[i] = (uint64_t)n->hmb_descs[i].size * n->page_size;
        l2->hmb_total_bytes += l2->hmb_seg_sizes[i];
    }

    ftl_log("L2P L2 cache ready on HMB: size=%uKB slots=%u hmb_bytes=%" PRIu64
            " descs=%u\n",
            FEMU_L2P_L2_SIZE_KB, l2->nr_slots, l2->hmb_total_bytes,
            l2->hmb_seg_count);

    return true;
}

bool femu_l2p_prepare(struct ssd *ssd)
{
    if (ssd->l2p_cache_ready && ssd->n->l2p_l2.initialized) {
        return true;
    }

    if (ssd->l2p_cache_ready && !ssd->n->l2p_l2.initialized) {
        /* L2 state was torn down by controller reset/disable. Re-arm init. */
        ssd->l2p_cache_ready = false;
    }

    if (!ssd_try_init_l2p_l2_cache(ssd)) {
        if (!ssd->l2p_hmb_reject_logged) {
            ssd->l2p_hmb_reject_logged = true;
            ftl_err("L2P cache init failed: HMB is not enabled/ready, refusing I/O\n");
        }
        return false;
    }

    ssd->l2p_cache_ready = true;
    ftl_log("L2P multi-level cache activated (algo=%u)\n",
            FEMU_L2P_CACHE_ALGO_DEFAULT);
    return true;
}

struct ppa femu_l2p_get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    uint64_t ignored = 0;

    if (!ssd->l2p_cache_ready) {
        return ssd->maptbl[lpn];
    }

    return get_maptbl_ent_internal(ssd, lpn, &ignored, false);
}

struct ppa femu_l2p_get_maptbl_ent_with_lat(struct ssd *ssd, uint64_t lpn,
                                            uint64_t *meta_lat)
{
    ftl_assert(ssd->l2p_cache_ready);
    return get_maptbl_ent_internal(ssd, lpn, meta_lat, true);
}

void femu_l2p_set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    if (!ssd->l2p_cache_ready) {
        set_maptbl_ent_raw(ssd, lpn, ppa);
        return;
    }

    set_maptbl_ent_internal(ssd, lpn, ppa, false);
}

uint64_t femu_l2p_set_maptbl_ent_with_lat(struct ssd *ssd, uint64_t lpn,
                                          struct ppa *ppa)
{
    ftl_assert(ssd->l2p_cache_ready);
    return set_maptbl_ent_internal(ssd, lpn, ppa, true);
}

void femu_l2p_ctrl_reset(FemuCtrl *n)
{
    n->hmb_enabled = false;
    n->hmb_hsize = 0;
    n->hmb_size_bytes = 0;
    n->hmb_desc_addr = 0;
    n->hmb_desc_count = 0;
    g_free(n->hmb_descs);
    n->hmb_descs = NULL;

    n->hmb_prev_valid = false;
    n->hmb_prev_hsize = 0;
    n->hmb_prev_size_bytes = 0;
    n->hmb_prev_desc_addr = 0;
    n->hmb_prev_desc_count = 0;
    g_free(n->hmb_prev_descs);
    n->hmb_prev_descs = NULL;

    n->l2p_l2.initialized = false;
    n->l2p_l2.used_slots = 0;
    n->l2p_l2.lru_head = -1;
    n->l2p_l2.lru_tail = -1;
    g_free(n->l2p_l2.meta);
    n->l2p_l2.meta = NULL;
    if (n->l2p_l2.tag2slot) {
        g_hash_table_destroy(n->l2p_l2.tag2slot);
        n->l2p_l2.tag2slot = NULL;
    }
    g_free(n->l2p_l2.hmb_seg_addrs);
    n->l2p_l2.hmb_seg_addrs = NULL;
    g_free(n->l2p_l2.hmb_seg_sizes);
    n->l2p_l2.hmb_seg_sizes = NULL;
    n->l2p_l2.hmb_seg_count = 0;
    n->l2p_l2.hmb_total_bytes = 0;

    if (n->ssd) {
        n->ssd->l2p_cache_ready = false;
        n->ssd->l2p_hmb_reject_logged = false;
    }
}
