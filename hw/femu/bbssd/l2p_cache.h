#ifndef __FEMU_BBSSD_L2P_CACHE_H
#define __FEMU_BBSSD_L2P_CACHE_H

#include "ftl.h"

void femu_l2p_init_latency(struct ssd *ssd);
void femu_l2p_log_latency_config(struct ssd *ssd);
void femu_l2p_maybe_log_stats(struct ssd *ssd);
void femu_l2p_init_l1_cache(struct ssd *ssd);
bool femu_l2p_prepare(struct ssd *ssd);

struct ppa femu_l2p_get_maptbl_ent(struct ssd *ssd, uint64_t lpn);
struct ppa femu_l2p_get_maptbl_ent_with_lat(struct ssd *ssd, uint64_t lpn,
                                            uint64_t *meta_lat);
void femu_l2p_set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa);
uint64_t femu_l2p_set_maptbl_ent_with_lat(struct ssd *ssd, uint64_t lpn,
                                          struct ppa *ppa);

void femu_l2p_ctrl_reset(FemuCtrl *n);

#endif
