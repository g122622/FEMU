# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

FEMU is a fast, accurate, scalable, and extensible NVMe SSD emulator based on QEMU/KVM. It enables full-system evaluation of storage systems and supports multiple SSD architectures for systems research.

## Build Commands

```bash
# Initial setup (from repository root)
mkdir build-femu && cd build-femu
cp ../femu-scripts/femu-copy-scripts.sh .
./femu-copy-scripts.sh .
sudo ./pkgdep.sh  # Install dependencies (Ubuntu/Debian)

# Build FEMU
./femu-compile.sh

# Clean rebuild
CLEAN=1 ./femu-compile.sh

# Verify build
./qemu-system-x86_64 -device help | grep femu
```

## Run Commands

```bash
# BlackBox SSD mode (device-managed FTL, most common for research)
./run-blackbox.sh

# Other SSD modes
./run-nossd.sh      # Ultra-low latency, no FTL
./run-whitebox.sh   # OpenChannel SSD (host-managed FTL)
./run-zns.sh        # Zoned Namespace SSD

# Debug build
../configure --enable-kvm --target-list=x86_64-softmmu --enable-debug --enable-debug-info
make -j$(nproc)

# GDB debugging
./gdb-run.sh
```

## SSD Mode Configuration (run-blackbox.sh parameters)

Key configurable parameters in run scripts:
- `secsz=512` - Sector size in bytes
- `secs_per_pg=8` - Sectors per flash page
- `pgs_per_blk=256` - Pages per block
- `blks_per_pl=256` - Blocks per plane
- `luns_per_ch=8` - LUNs per channel
- `nchs=8` - Number of channels
- `ssd_size=12288` - SSD capacity in MB
- `pg_rd_lat=40000` - Page read latency (ns)
- `pg_wr_lat=200000` - Page write latency (ns)
- `blk_er_lat=2000000` - Block erase latency (ns)
- `gc_thres_pcent=75` - GC trigger threshold

Experiment knobs (BBSSD mode):
- `exp_enable_hmb=1` - Enable Host Memory Buffer
- `exp_enable_l2p_multilevel=1` - Enable L2P multi-level cache
- `exp_enable_wb=1` - Enable Write Buffer feature
- `hmb_hmmin_mb=128` - HMB minimum size
- `l2p_l1_size_kb=256` - L2P L1 cache size

## Architecture

```
hw/femu/                          # Main FEMU implementation
├── femu.c                        # NVMe controller core, device realize, MMIO
├── nvme-admin.c                  # Admin commands (Identify, Set Features, etc.)
├── nvme-io.c                     # I/O commands (Read, Write, DSM, etc.)
├── nvme-util.c                   # Queue operations, PRP mapping utilities
├── nvme.h                        # Main header (FemuCtrl, NvmeRequest, etc.)
├── meson.build                   # Build configuration
│
├── bbssd/                        # BlackBox SSD mode (device-managed FTL)
│   ├── ftl.c                     # FTL core: gc, mapping, write pointer
│   ├── ftl.h                     # FTL structures (ssd, ppa, line_mgmt)
│   ├── bb.c                      # BBSSD extension registration
│   ├── l2p_cache.c               # Multi-level L2P cache (L1/L2/L3)
│   ├── hmb.c                     # Host Memory Buffer handling
│   └── write_buffer.c            # Write Buffer staging and flush
│
├── ocssd/                        # OpenChannel SSD mode (host-managed FTL)
│   ├── oc12.c                    # OpenChannel 1.2 implementation
│   └── oc20.c                    # OpenChannel 2.0 implementation
│
├── zns/                          # Zoned Namespace SSD mode
│   ├── zns.c                     # ZNS command handling
│   └── zftl.c                    # Zone FTL implementation
│
├── nossd/                        # NoSSD mode (minimal processing)
│   └── nop.c                     # Pass-through I/O
│
├── common/                       # Shared configuration headers
│   ├── hmb-config.h              # HMB feature configuration
│   ├── hmb-types.h               # HMB data structures
│   ├── l2p-cache-config.h        # L2P cache configuration
│   ├── l2p-cache-types.h         # L2P cache types
│   ├── wb-config.h               # Write Buffer configuration
│   └── wb-types.h                # Write Buffer types
│
├── backend/                      # Storage backends
│   └── dram.c                    # DRAM-based storage emulation
│
├── nand/                         # NAND flash model
│   └── nand.c                    # NAND timing and geometry
│
├── timing-model/                 # Performance modeling
│   └── timing.c                  # Latency emulation
│
├── lib/                          # Utility libraries
│   ├── pqueue.c                  # Priority queue
│   ├── rte_ring.c                # Lockless ring buffer
│   └── rb_tree.c                 # Red-black tree
│
└── inc/                          # External headers
    ├── rte_ring.h
    └── pqueue.h
```

## Key Data Flow (Write Path in BBSSD+HMB+WB mode)

1. **Host submits I/O**: NVMe driver writes SQE, updates doorbell
2. **Poller thread** (`nvme_poller` in `nvme-io.c`) fetches commands from SQ
3. **Command dispatch**: `nvme_io_cmd()` → `bb_io_cmd()` → `nvme_rw()`
4. **FTL processing**: Request enqueued to `to_ftl` ring, processed by `ftl_thread()`
5. **L2P preparation**: `femu_l2p_prepare()` handles address translation
6. **Write Buffer staging**: `femu_wb_stage_write_req()` for WB path, else `ssd_write()`
7. **Completion**: Request returned via `to_poller` ring, CQE posted with latency

## Key Structures

- **FemuCtrl** (`nvme.h`): Main controller state, includes all configuration and runtime state
- **struct ssd** (`ftl.h`): SSD internal state, mapping tables, L2P cache
- **NvmeRequest** (`nvme.h`): I/O request structure with tracking fields
- **NvmeSQueue/NvmeCQueue** (`nvme.h`): Submission/Completion queue handling

## SSD Mode Selection (femu_mode parameter)

| Mode | Value | Description |
|------|-------|-------------|
| FEMU_OCSSD_MODE | 0 | OpenChannel SSD |
| FEMU_BBSSD_MODE | 1 | BlackBox SSD (default research mode) |
| FEMU_NOSSD_MODE | 2 | No SSD logic, minimal latency |
| FEMU_ZNSSD_MODE | 3 | Zoned Namespace SSD |

## Extension Points

To add new SSD functionality:
1. Create new mode directory under `hw/femu/`
2. Implement `FemuExtCtrlOps` callbacks (init, io_cmd, admin_cmd, etc.)
3. Register mode in `femu.c` property parsing
4. Add source files to `meson.build`

## Coding Style

Follow QEMU coding conventions:
- 4-space indentation
- Structured C with QEMU infrastructure (QTAILQ, QemuThread, MemoryRegion)
- Error handling via `Error **` parameter pattern
