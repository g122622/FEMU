#include "ftl.h"

//#define FEMU_DEBUG_FTL

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

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

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    uint64_t ignored = 0;
    if (!ssd->l2p_cache_ready) {
        return ssd->maptbl[lpn];
    }
    return get_maptbl_ent_internal(ssd, lpn, &ignored, false);
}

static inline struct ppa get_maptbl_ent_with_lat(struct ssd *ssd, uint64_t lpn,
                                                 uint64_t *meta_lat)
{
    ftl_assert(ssd->l2p_cache_ready);
    return get_maptbl_ent_internal(ssd, lpn, meta_lat, true);
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    if (!ssd->l2p_cache_ready) {
        set_maptbl_ent_raw(ssd, lpn, ppa);
        return;
    }
    set_maptbl_ent_internal(ssd, lpn, ppa, false);
}

static inline uint64_t set_maptbl_ent_with_lat(struct ssd *ssd, uint64_t lpn,
                                               struct ppa *ppa)
{
    ftl_assert(ssd->l2p_cache_ready);
    return set_maptbl_ent_internal(ssd, lpn, ppa, true);
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

static void ssd_init_l2p_latency(struct ssd *ssd)
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

static void ssd_log_latency_config(struct ssd *ssd)
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

static void ssd_init_l2p_l1_cache(struct ssd *ssd)
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

static bool ssd_prepare_l2p_cache(struct ssd *ssd)
{
    if (ssd->l2p_cache_ready) {
        return true;
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

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);
    ssd->n = n;

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize metadata hierarchy (L1 now, L2 on first I/O after HMB setup) */
    ssd_init_l2p_latency(ssd);
    ssd_log_latency_config(ssd);
    ssd_init_l2p_l1_cache(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    uint64_t meta_lat_sum = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent_with_lat(ssd, lpn, &meta_lat_sum);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return meta_lat_sum + maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    uint64_t meta_lat_sum = 0;
    uint64_t meta_wlat;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent_with_lat(ssd, lpn, &meta_lat_sum);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl */
        meta_wlat = set_maptbl_ent_with_lat(ssd, lpn, &ppa);
        meta_lat_sum += meta_wlat;
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return meta_lat_sum + maxlat;
}

static uint64_t ssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmRange *ranges = req->dsm_ranges;
    int nr_ranges = req->dsm_nr_ranges;
    // uint32_t attributes = req->dsm_attributes;
    
    int total_trimmed_pages = 0;
    int total_already_invalid = 0;
    int total_out_of_bounds = 0;
    
    if (!ranges || nr_ranges <= 0) {
        printf("TRIM: Invalid ranges or count\n");
        return 0;
    }
    
    // printf("TRIM: Processing %d ranges (attributes=0x%x)\n", nr_ranges, attributes);
    
    for (int range_idx = 0; range_idx < nr_ranges; range_idx++) {
        uint64_t slba = le64_to_cpu(ranges[range_idx].slba);
        uint32_t nlb = le32_to_cpu(ranges[range_idx].nlb);
        // uint32_t cattr = le32_to_cpu(ranges[range_idx].cattr);
        
        uint64_t start_lpn = slba / spp->secs_per_pg;
        uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
        uint64_t lpn;
        struct ppa ppa;
        int trimmed_pages = 0;
        int already_invalid = 0;

        // ftl_debug("TRIM Range %d: LBA %lu + %u sectors, LPN range %lu-%lu (%lu pages), cattr=0x%x\n", 
        //        range_idx, slba, nlb, start_lpn, end_lpn, end_lpn - start_lpn + 1, cattr);

        // Boundary check
        if (end_lpn >= spp->tt_pgs) {
            ftl_err("TRIM: Range %d exceeds FTL capacity - end_lpn=%lu, tt_pgs=%d\n", 
                   range_idx, end_lpn, spp->tt_pgs);
            total_out_of_bounds++;
            continue;  // Skip this range, continue with others
        }

        // Process each LPN in this range
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            
            // Skip already unmapped/invalid pages
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                already_invalid++;
                continue;
            }

            // Invalidate the existing mapped page
            mark_page_invalid(ssd, &ppa);
            
            // Clear reverse mapping
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            
            // Set mapping table entry as unmapped
            ppa.ppa = UNMAPPED_PPA;
            set_maptbl_ent(ssd, lpn, &ppa);
            
            trimmed_pages++;
        }
        
        total_trimmed_pages += trimmed_pages;
        total_already_invalid += already_invalid;
        
        // ftl_debug("TRIM Range %d: %d pages trimmed, %d already invalid\n", 
        //        range_idx, trimmed_pages, already_invalid);
    }

    // ftl_debug("TRIM: Completed - %d pages trimmed, %d already invalid, %d out of bounds across %d ranges\n", 
    //        total_trimmed_pages, total_already_invalid, total_out_of_bounds, nr_ranges);

    // Free the ranges array
    g_free(ranges);
    req->dsm_ranges = NULL;
    req->dsm_nr_ranges = 0;
    req->dsm_attributes = 0;

    return 0;  // Assume TRIM operations have no NAND latency
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            lat = 0;
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                if (!ssd_prepare_l2p_cache(ssd)) {
                    req->status = NVME_INVALID_FIELD | NVME_DNR;
                    break;
                }
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                if (!ssd_prepare_l2p_cache(ssd)) {
                    req->status = NVME_INVALID_FIELD | NVME_DNR;
                    break;
                }
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                if (req->dsm_ranges && req->dsm_nr_ranges > 0) {
                    if (!ssd_prepare_l2p_cache(ssd)) {
                        req->status = NVME_INVALID_FIELD | NVME_DNR;
                        break;
                    }
                    lat = ssd_trim(ssd, req);
                }
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}
