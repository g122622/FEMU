#ifndef __FEMU_BBSSD_HMB_H
#define __FEMU_BBSSD_HMB_H

#include "../nvme.h"

uint16_t femu_hmb_get_feature(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe);
uint16_t femu_hmb_set_feature(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe);

bool femu_hmb_rw(FemuCtrl *n, uint64_t off, void *buf, uint32_t len,
                 bool is_write);
uint64_t femu_hmb_total_bytes(FemuCtrl *n);

void femu_hmb_ctrl_reset(FemuCtrl *n);

#endif
