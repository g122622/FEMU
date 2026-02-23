#ifndef __FEMU_BBSSD_WRITE_BUFFER_H
#define __FEMU_BBSSD_WRITE_BUFFER_H

#include "../nvme.h"

bool femu_wb_init_layout(FemuCtrl *n);
void femu_wb_ctrl_reset(FemuCtrl *n);

uint16_t femu_wb_admin_kva_mapping_push(FemuCtrl *n, NvmeCmd *cmd);
uint16_t femu_wb_io_notify_copy_done(FemuCtrl *n, NvmeCmd *cmd,
									 NvmeRequest *req);
uint16_t femu_wb_io_notify_read_done(FemuCtrl *n, NvmeCmd *cmd,
									 NvmeRequest *req);

bool femu_wb_gpa_to_kva(FemuCtrl *n, uint64_t gpa, uint64_t *kva_out,
						uint64_t *max_len_out);

#endif
