#!/bin/bash
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# Build the SimBricks custom kernel with the options tt-kmd needs.
#
# SimBricks ships a trimmed, no-initrd 5.15.93 kernel precisely to keep boot
# short, which matters enormously in synchronized runs where every second of
# guest time costs ~90 s of wall time. The stock config just happens to leave
# CONFIG_HWMON and CONFIG_DMA_SHARED_BUFFER off, so tt-kmd will not link against
# it -- the reason the earlier images fell back to the distro `linux-generic`
# kernel, which is modular, needs an initrd, and boots roughly ten times slower.
# Adding two options is much cheaper than paying that on every run.
#
# This mirrors image-builder/kernel/build-kernel.sh -- same source version, same
# gem5 timer patch, same base config -- and merges image/kernel-tt.config over
# the top rather than forking the pinned config.
#
# Produces in $OUT:
#   vmlinux             uncompressed ELF
#   linux-image-*.deb   kernel + modules
#   linux-headers-*.deb build tree, for building tt-kmd in the guest
#
# Usage: image/build-tt-kernel.sh [-o OUT_DIR]

set -euo pipefail

VER=5.15.93
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Where image-builder's pinned config and gem5 timer patch live.
IB_KERNEL="${IB_KERNEL:-$HERE/../../image-builder/kernel}"
# Default inside image/, because build-tt-image.sh copies that whole directory
# into the guest -- provision-tt-kernel.sh then installs from there.
OUT="${OUT:-$HERE/kernel-out}"
JOBS="${JOBS:-$(nproc)}"

while getopts "o:" opt; do
  case "$opt" in
    o) OUT="$OPTARG" ;;
    *) echo "usage: $0 [-o OUT_DIR]" >&2; exit 1 ;;
  esac
done

[ -f "$IB_KERNEL/config-5.15.93" ] || {
  echo "no base config at $IB_KERNEL/config-5.15.93 (set IB_KERNEL)" >&2; exit 1; }
[ -f "$IB_KERNEL/linux-5.15.93-timers-gem5.patch" ] || {
  echo "no gem5 timer patch at $IB_KERNEL" >&2; exit 1; }
for tool in curl patch make gcc bc flex bison dpkg-buildpackage; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

OUT="$(mkdir -p "$OUT" && cd "$OUT" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "==> fetching linux-$VER"
curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/linux-$VER.tar.xz" \
    | tar -xJ -C "$work"
cd "$work/linux-$VER"

# gem5 miscalibrates the LAPIC timer / TSC; this adds lapic_timer_period= and
# tsc_override_freq=. Kept because the image is meant to boot under gem5 too.
echo "==> applying gem5 timer patch"
patch -p1 < "$IB_KERNEL/linux-5.15.93-timers-gem5.patch"

echo "==> merging config"
cp "$IB_KERNEL/config-5.15.93" .config
./scripts/kconfig/merge_config.sh -m .config "$HERE/kernel-tt.config"
make olddefconfig

# Assert rather than trust: CONFIG_DMA_SHARED_BUFFER is reached only by `select`
# from SYNC_FILE, so a Kconfig change upstream would silently drop it and the
# failure would not surface until modpost, inside a guest, twenty minutes later.
missing=""
for opt in CONFIG_HWMON CONFIG_DMA_SHARED_BUFFER CONFIG_DMABUF_MOVE_NOTIFY; do
  grep -q "^$opt=y" .config || missing="$missing $opt"
done
if [ -n "$missing" ]; then
  echo "ERROR: config did not take:$missing" >&2
  exit 1
fi
echo "    tt-kmd options present; version $(make -s kernelrelease)"

echo "==> building (-j$JOBS)"
make -j"$JOBS" bindeb-pkg

cp vmlinux "$OUT/"
mv "$work"/linux-image-*.deb "$work"/linux-headers-*.deb "$OUT/"
make -s kernelrelease > "$OUT/kernelrelease"

echo
echo "kernel $(cat "$OUT/kernelrelease") -> $OUT:"
ls -1s "$OUT"
