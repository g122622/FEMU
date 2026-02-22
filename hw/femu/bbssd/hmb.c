#include "hmb.h"
#include "write_buffer.h"

uint64_t femu_hmb_total_bytes(FemuCtrl *n)
{
    return n->hmb_size_bytes;
}

bool femu_hmb_rw(FemuCtrl *n, uint64_t off, void *buf, uint32_t len,
                 bool is_write)
{
    uint8_t *p = buf;
    uint64_t cur = 0;
    uint32_t left = len;

    if (!n->hmb_enabled || !n->hmb_descs || !n->hmb_desc_count) {
        return false;
    }

    if (off + len > n->hmb_size_bytes) {
        return false;
    }

    for (uint32_t i = 0; i < n->hmb_desc_count && left; i++) {
        uint64_t seg_addr = n->hmb_descs[i].addr;
        uint64_t seg_sz = (uint64_t)n->hmb_descs[i].size * n->page_size;

        if (off >= cur + seg_sz) {
            cur += seg_sz;
            continue;
        }

        uint64_t in_seg = off > cur ? off - cur : 0;
        uint64_t seg_avail = seg_sz - in_seg;
        uint32_t xfer = MIN((uint64_t)left, seg_avail);
        uint64_t gpa = seg_addr + in_seg;

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

void femu_hmb_ctrl_reset(FemuCtrl *n)
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

    femu_wb_ctrl_reset(n);
}

uint16_t femu_hmb_get_feature(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe)
{
    NvmeHmbAttrs attrs = {0};
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint8_t select = NVME_GETFEAT_SELECT(dw10);
    uint8_t ehm = 0;

    switch (select) {
    case NVME_GETFEAT_SELECT_CURRENT:
        ehm = n->hmb_enabled ? 1 : 0;
        attrs.hsize = cpu_to_le32(n->hmb_hsize);
        attrs.hmdlal = cpu_to_le32((uint32_t)(n->hmb_desc_addr & 0xffffffffULL));
        attrs.hmdlau = cpu_to_le32((uint32_t)(n->hmb_desc_addr >> 32));
        attrs.hmdlec = cpu_to_le32(n->hmb_desc_count);
        break;
    case NVME_GETFEAT_SELECT_DEFAULT:
    case NVME_GETFEAT_SELECT_SAVED:
    case NVME_GETFEAT_SELECT_CAP:
        ehm = 0;
        break;
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    cqe->n.result = cpu_to_le32(NVME_HMB_ATTRS(0, ehm));

    if (prp1 || prp2) {
        return dma_read_prp(n, (uint8_t *)&attrs, sizeof(attrs), prp1, prp2);
    }

    return NVME_SUCCESS;
}

uint16_t femu_hmb_set_feature(FemuCtrl *n, NvmeCmd *cmd, NvmeCqe *cqe)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t dw12 = le32_to_cpu(cmd->cdw12);
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint32_t dw14 = le32_to_cpu(cmd->cdw14);
    uint32_t dw15 = le32_to_cpu(cmd->cdw15);
    uint8_t save = NVME_SETFEAT_SAVE(dw10);
    uint8_t ehm = NVME_HMB_EHM(dw11);
    uint8_t mr = NVME_HMB_MR(dw11);
    uint32_t hsize = dw12;
    uint32_t hmdlec = dw15;
    uint64_t hmdl_addr = ((uint64_t)dw14 << 32) | (dw13 & ~0xfU);
    NvmeHmbDescriptor *descs = NULL;
    uint64_t hmb_bytes = 0;
    uint16_t st = NVME_SUCCESS;
    uint64_t magic = cpu_to_le64(FEMU_HMB_TEST_MAGIC);
    uint64_t verify_magic = 0;

    if (save) {
        femu_log("HMB Set Features Save=1 is not supported\n");
        return NVME_FID_NOT_SAVEABLE | NVME_DNR;
    }

    if (dw13 & 0xf) {
        femu_log("HMB Set Features failed: HMDLLA not 16-byte aligned\n");
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (ehm || mr) {
        uint64_t sum_pages = 0;

        if (!hsize || !hmdlec || !hmdl_addr) {
            femu_log("HMB Set Features failed: invalid HSIZE/HMDLEC/HMDL tuple\n");
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        if (ehm && hsize < le32_to_cpu(n->id_ctrl.hmmin)) {
            femu_log("HMB Set Features failed: HSIZE(%u) < HMMIN(%u)\n",
                     hsize, le32_to_cpu(n->id_ctrl.hmmin));
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        descs = g_malloc0(sizeof(*descs) * hmdlec);
        nvme_addr_read(n, hmdl_addr, (void *)descs, sizeof(*descs) * hmdlec);

        for (uint32_t i = 0; i < hmdlec; i++) {
            uint64_t badd = le64_to_cpu(descs[i].addr);
            uint32_t bsize = le32_to_cpu(descs[i].size);
            uint32_t rsvd = le32_to_cpu(descs[i].rsvd);

            if (rsvd) {
                femu_log("HMB Set Features failed: descriptor[%u] reserved != 0\n", i);
                st = NVME_INVALID_FIELD | NVME_DNR;
                goto out_hmb_set;
            }
            if (!bsize) {
                femu_log("HMB Set Features failed: descriptor[%u] BSIZE is zero\n", i);
                st = NVME_INVALID_FIELD | NVME_DNR;
                goto out_hmb_set;
            }
            if (badd & (n->page_size - 1)) {
                femu_log("HMB Set Features failed: descriptor[%u] BADD is not CC.MPS aligned\n", i);
                st = NVME_INVALID_FIELD | NVME_DNR;
                goto out_hmb_set;
            }

            sum_pages += bsize;
            descs[i].addr = badd;
            descs[i].size = bsize;
            descs[i].rsvd = rsvd;
        }

        if (sum_pages != hsize) {
            femu_log("HMB Set Features failed: descriptor BSIZE sum mismatch\n");
            st = NVME_INVALID_FIELD | NVME_DNR;
            goto out_hmb_set;
        }

        hmb_bytes = sum_pages * n->page_size;
    }

    if (mr) {
        if (!n->hmb_prev_valid || !descs) {
            femu_log("HMB Set Features failed: MR=1 without previous known configuration\n");
            st = NVME_INVALID_FIELD | NVME_DNR;
            goto out_hmb_set;
        }

        if (hsize != n->hmb_prev_hsize || hmdl_addr != n->hmb_prev_desc_addr ||
            hmdlec != n->hmb_prev_desc_count) {
            femu_log("HMB Set Features failed: MR=1 config mismatch\n");
            st = NVME_INVALID_FIELD | NVME_DNR;
            goto out_hmb_set;
        }

        if (memcmp(descs, n->hmb_prev_descs, sizeof(*descs) * hmdlec)) {
            femu_log("HMB Set Features failed: MR=1 descriptor content mismatch\n");
            st = NVME_INVALID_FIELD | NVME_DNR;
            goto out_hmb_set;
        }
    }

    if (ehm) {
        g_free(n->hmb_descs);
        n->hmb_descs = descs;
        descs = NULL;

        n->hmb_enabled = true;
        n->hmb_hsize = hsize;
        n->hmb_size_bytes = hmb_bytes;
        n->hmb_desc_addr = hmdl_addr;
        n->hmb_desc_count = hmdlec;

        g_free(n->hmb_prev_descs);
        n->hmb_prev_descs = g_malloc0(sizeof(*n->hmb_prev_descs) * hmdlec);
        memcpy(n->hmb_prev_descs, n->hmb_descs,
               sizeof(*n->hmb_prev_descs) * hmdlec);
        n->hmb_prev_valid = true;
        n->hmb_prev_hsize = hsize;
        n->hmb_prev_size_bytes = hmb_bytes;
        n->hmb_prev_desc_addr = hmdl_addr;
        n->hmb_prev_desc_count = hmdlec;

        nvme_addr_write(n, n->hmb_descs[0].addr, (void *)&magic, sizeof(magic));
        nvme_addr_read(n, n->hmb_descs[0].addr, (void *)&verify_magic,
                       sizeof(verify_magic));
        femu_log("HMB enabled: hsize=%u pages, hmdlec=%u, bytes=%" PRIu64 "\n",
                 hsize, hmdlec, hmb_bytes);

        if (!femu_wb_init_layout(n)) {
            femu_log("WB init degraded: keep HMB for L2P cache only\n");
        }

        femu_log("HMB magic verify at 0x%" PRIx64 " = 0x%" PRIx64 "\n",
                 n->hmb_descs[0].addr, le64_to_cpu(verify_magic));
        femu_debug("HMB test magic written to guest addr=0x%" PRIx64 "\n",
                   n->hmb_descs[0].addr);
    } else {
        n->hmb_enabled = false;
        n->hmb_hsize = 0;
        n->hmb_size_bytes = 0;
        n->hmb_desc_addr = 0;
        n->hmb_desc_count = 0;
        g_free(n->hmb_descs);
        n->hmb_descs = NULL;
        femu_wb_ctrl_reset(n);
        femu_log("HMB disabled\n");
    }

    cqe->n.result = cpu_to_le32(NVME_HMB_ATTRS(mr, n->hmb_enabled ? 1 : 0));

out_hmb_set:
    g_free(descs);
    return st;
}
