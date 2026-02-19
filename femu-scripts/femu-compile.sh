#!/bin/bash

set -e

NRCPUS="$(nproc)"

# Enable compiler cache when available.
if command -v ccache >/dev/null 2>&1; then
	export CC="ccache gcc"
	export CXX="ccache g++"
	export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"
	export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}"
	export CCACHE_COMPRESS=1

	ccache --max-size="$CCACHE_MAXSIZE" >/dev/null 2>&1 || true
	echo "===> ccache enabled (dir: $CCACHE_DIR, max: $CCACHE_MAXSIZE)"
else
	echo "===> ccache not found, building without compiler cache"
fi

# Optional full rebuild: CLEAN=1 ./femu-compile.sh
if [[ "${CLEAN:-0}" == "1" ]]; then
	make clean
fi

# --disable-werror --extra-cflags=-w --disable-git-update
../configure --enable-kvm --target-list=x86_64-softmmu --enable-slirp
make -j "$NRCPUS"

if command -v ccache >/dev/null 2>&1; then
	echo ""
	ccache --show-stats || true
fi

echo ""
echo "===> FEMU compilation done ..."
echo ""
exit
