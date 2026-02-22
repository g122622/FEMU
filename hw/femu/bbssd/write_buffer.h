#ifndef __FEMU_BBSSD_WRITE_BUFFER_H
#define __FEMU_BBSSD_WRITE_BUFFER_H

#include "../nvme.h"

bool femu_wb_init_layout(FemuCtrl *n);
void femu_wb_ctrl_reset(FemuCtrl *n);

#endif
