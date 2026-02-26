#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)

# image directory
IMGDIR=$HOME/images
# Virtual machine disk image
OSIMGF=$IMGDIR/u20s.qcow2

# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=8 # number of sectors in a flash page
pgs_per_blk=256 # number of pages per flash block
blks_per_pl=256 # number of blocks per plane
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=8 # number of chips per channel
nchs=8 # number of channels
ssd_size=12288 # in megabytes, if you change the above layout parameters, make sure you manually recalculate the ssd size and modify it here, please consider a default 25% overprovisioning ratio.

# Latency in nanoseconds
pg_rd_lat=40000 # page read latency
pg_wr_lat=200000 # page write latency
blk_er_lat=2000000 # block erase latency
ch_xfer_lat=0 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

# -----------------------------------------------------------------------
# FEMU experiment/runtime knobs (explicitly set to preserve pre-change behavior)
#
# Pre-change equivalent behavior:
# - HMB enabled
# - L2P multi-level cache enabled
# - WB feature enabled (still requires host-side 0xd1 handshake to actually enter WB path)
# - Original default sizes/latencies/watermarks
exp_enable_hmb=1
exp_enable_l2p_multilevel=1
exp_enable_l2p_l2_rw_in_hmb=0
exp_enable_wb=1
l2p_bypass_meta_mode=0

hmb_hmmin_mb=128
hmb_hmpre_mb=128

l2p_l1_size_kb=$((256 * 1))
l2p_l2_size_kb=$((2 * 1024))
l2p_pt_page_size=4096

# 各级缓存延迟
l2p_l1_rd_lat_ns=10
l2p_l1_wr_lat_ns=10
l2p_l2_rd_lat_ns=300
l2p_l2_wr_lat_ns=400
l2p_l3_rd_lat_mul=1
l2p_l3_wr_lat_mul=1

wb_mcp_entries_per_q=1024
wb_flush_watermark_pct=80
wb_idle_rounds_default=16

#-----------------------------------------------------------------------

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",exp_enable_hmb=${exp_enable_hmb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",exp_enable_l2p_multilevel=${exp_enable_l2p_multilevel}"
FEMU_OPTIONS=${FEMU_OPTIONS}",exp_enable_l2p_l2_rw_in_hmb=${exp_enable_l2p_l2_rw_in_hmb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",exp_enable_wb=${exp_enable_wb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_bypass_meta_mode=${l2p_bypass_meta_mode}"
FEMU_OPTIONS=${FEMU_OPTIONS}",hmb_hmmin_mb=${hmb_hmmin_mb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",hmb_hmpre_mb=${hmb_hmpre_mb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l1_size_kb=${l2p_l1_size_kb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l2_size_kb=${l2p_l2_size_kb}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_pt_page_size=${l2p_pt_page_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l1_rd_lat_ns=${l2p_l1_rd_lat_ns}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l1_wr_lat_ns=${l2p_l1_wr_lat_ns}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l2_rd_lat_ns=${l2p_l2_rd_lat_ns}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l2_wr_lat_ns=${l2p_l2_wr_lat_ns}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l3_rd_lat_mul=${l2p_l3_rd_lat_mul}"
FEMU_OPTIONS=${FEMU_OPTIONS}",l2p_l3_wr_lat_mul=${l2p_l3_wr_lat_mul}"
FEMU_OPTIONS=${FEMU_OPTIONS}",wb_mcp_entries_per_q=${wb_mcp_entries_per_q}"
FEMU_OPTIONS=${FEMU_OPTIONS}",wb_flush_watermark_pct=${wb_flush_watermark_pct}"
FEMU_OPTIONS=${FEMU_OPTIONS}",wb_idle_rounds_default=${wb_idle_rounds_default}"

echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

sudo ./qemu-system-x86_64 \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -m 4G \
    -fsdev local,id=wsl-share,path=/home/g122622/dev/FEMU/hw/femu,security_model=none \
    -device virtio-9p-pci,fsdev=wsl-share,mount_tag=wsl-share \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::8080-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
