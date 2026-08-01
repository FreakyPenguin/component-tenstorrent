#!/bin/bash
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# Guest-side provisioning for a Tenstorrent image on the SimBricks *custom*
# kernel. Runs as root inside the VM; driven by image/build-tt-image.sh.
#
# This replaces provision-tt-guest.sh, which installed the distro
# `linux-generic` kernel because tt-kmd would not build against the stock
# SimBricks kernel. That was the wrong trade: SimBricks ships a trimmed,
# no-initrd kernel specifically to keep boot short, and boot length is the
# dominant cost of a synchronized run -- every second of guest time obliges the
# device model to simulate a second, at roughly 88 s of wall time each. The
# distro kernel is modular, needs an initrd, and takes ~12 s of guest time to
# reach the payload where the custom kernel takes ~1.
#
# So instead of changing kernels we changed two config options
# (image/kernel-tt.config) and rebuilt: image/build-tt-kernel.sh produces the
# debs installed here, into /tmp/ttimg/kernel-out.

set -eux
export DEBIAN_FRONTEND=noninteractive

HERE="$(cd "$(dirname "$0")" && pwd)"
KOUT="${KOUT:-$HERE/kernel-out}"
TT_KMD_REPO="${TT_KMD_REPO:-https://github.com/tenstorrent/tt-kmd.git}"
TT_KMD_REF="${TT_KMD_REF:-main}"

[ -f "$KOUT/kernelrelease" ] || {
    echo "ERROR: no kernel build at $KOUT -- run image/build-tt-kernel.sh first" >&2
    exit 1
}
KVER="$(tr -d '\r\n' < "$KOUT/kernelrelease")"
echo "installing kernel $KVER"

apt-get update
apt-get install -y --no-install-recommends build-essential git pciutils kmod

# dpkg -i rather than `apt-get install ./*.deb`: this kernel carries a
# -tt localversion so it does not collide with the stock 5.15.93 already in the
# image, and dpkg installs it unconditionally without apt's version reasoning.
dpkg -i "$KOUT"/linux-image-*.deb "$KOUT"/linux-headers-*.deb

KDIR="/lib/modules/$KVER/build"
[ -d "$KDIR" ] || { echo "ERROR: no build tree at $KDIR" >&2; exit 1; }

rm -rf /tmp/tt-kmd
git clone "$TT_KMD_REPO" /tmp/tt-kmd
git -C /tmp/tt-kmd checkout "$TT_KMD_REF"
git -C /tmp/tt-kmd rev-parse HEAD > /etc/tt-kmd.gitrev

# The whole point of the config change: this is what failed against the stock
# kernel, with undefined hwmon_device_register_with_info / dma_buf_export /
# dma_buf_move_notify at the modpost stage.
make -C /tmp/tt-kmd modules KDIR="$KDIR"

install -D -m 644 /tmp/tt-kmd/tenstorrent.ko \
    "/lib/modules/$KVER/kernel/drivers/misc/tenstorrent.ko"
depmod -a "$KVER"

echo tenstorrent > /etc/modules-load.d/simbricks-tenstorrent.conf
install -D -m 644 /tmp/tt-kmd/udev-50-tenstorrent.rules \
    /etc/udev/rules.d/50-tenstorrent.rules

# Which kernel the host-side extraction and the experiment should use.
echo "$KVER" > /etc/simbricks-kernel-version
# This kernel needs no initrd -- everything required to mount root is built in.
# build-tt-image.sh keys off this to skip extracting one, and the experiment
# scripts then leave QemuSim.initrd unset.
echo "no" > /etc/simbricks-needs-initrd

modinfo "/lib/modules/$KVER/kernel/drivers/misc/tenstorrent.ko" | head -8
rm -rf /tmp/tt-kmd

apt-get -y clean
rm -rf /var/lib/apt/lists/*
