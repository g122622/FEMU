#ifndef __FEMU_BBSSD_WRITE_BUFFER_H
#define __FEMU_BBSSD_WRITE_BUFFER_H

#include "../nvme.h"

struct ssd;

bool femu_wb_init_layout(FemuCtrl *n);
void femu_wb_ctrl_reset(FemuCtrl *n);

uint16_t femu_wb_admin_kva_mapping_push(FemuCtrl *n, NvmeCmd *cmd);
uint16_t femu_wb_io_notify_copy_done(FemuCtrl *n, NvmeCmd *cmd,
									 NvmeRequest *req);
uint16_t femu_wb_io_notify_read_done(FemuCtrl *n, NvmeCmd *cmd,
									 NvmeRequest *req);

bool femu_wb_gpa_to_kva(FemuCtrl *n, uint64_t gpa, uint64_t *kva_out,
						uint64_t *max_len_out);

bool femu_wb_should_candidate_write(FemuCtrl *n);
bool femu_wb_stage_write_req(struct ssd *ssd, NvmeRequest *req,
							 uint64_t start_lpn, uint64_t end_lpn,
							 uint32_t secsz, uint32_t secs_per_pg,
							 uint8_t data_shift, uint64_t data_size);
uint32_t femu_wb_stage_read_hits(struct ssd *ssd, NvmeRequest *req,
								 uint64_t start_lpn, uint64_t end_lpn,
								 uint32_t secsz, uint32_t secs_per_pg);
void femu_wb_note_queue_activity(FemuCtrl *n, uint16_t qid);
bool femu_wb_consume_flush_hint(FemuCtrl *n, uint16_t qid);

#endif
