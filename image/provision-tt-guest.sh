#!/bin/bash -eux
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# Guest-side provisioning for a Tenstorrent-capable SimBricks image. Runs as
# root inside the VM; driven by image/build-tt-image.sh.
#
# Two things happen here:
#
#  1. Install the distro `linux-generic` kernel. The SimBricks base image ships
#     a trimmed, no-initrd 5.15.93 kernel built for gem5, and tt-kmd will not
#     link against it -- modpost fails on hwmon_device_register_with_info and
#     dma_buf_export because that config has neither CONFIG_HWMON nor
#     CONFIG_DMA_SHARED_BUFFER. The distro kernel enables both.
#     Consequence: the distro kernel is modular, so it needs an initrd; set
#     QemuSim.initrd to the extracted boot/initrd.
#
#  2. Build and install tt-kmd against that kernel.

set -eux
export DEBIAN_FRONTEND=noninteractive

TT_KMD_REPO="${TT_KMD_REPO:-https://github.com/tenstorrent/tt-kmd.git}"
TT_KMD_REF="${TT_KMD_REF:-main}"

apt-get update
apt-get install -y --no-install-recommends linux-generic build-essential git pciutils kmod

# Pick the generic kernel explicitly. `sort -V | tail -1` would choose the base
# image's custom 5.15.93 over 5.15.0-N-generic, which is the kernel we cannot
# build against.
KVER=$(ls -d /lib/modules/*-generic 2>/dev/null | sed 's|.*/||' | sort -V | tail -1)
if [ -z "$KVER" ]; then
    echo "ERROR: no *-generic kernel found under /lib/modules" >&2
    exit 1
fi
KDIR="/lib/modules/$KVER/build"
[ -d "$KDIR" ] || { echo "ERROR: no build tree at $KDIR" >&2; exit 1; }
echo "building tt-kmd against $KVER"

rm -rf /tmp/tt-kmd
git clone "$TT_KMD_REPO" /tmp/tt-kmd
git -C /tmp/tt-kmd checkout "$TT_KMD_REF"
git -C /tmp/tt-kmd rev-parse HEAD > /etc/tt-kmd.gitrev

make -C /tmp/tt-kmd modules KDIR="$KDIR"

install -D -m 644 /tmp/tt-kmd/tenstorrent.ko \
    "/lib/modules/$KVER/kernel/drivers/misc/tenstorrent.ko"
depmod -a "$KVER"

echo tenstorrent > /etc/modules-load.d/simbricks-tenstorrent.conf
install -D -m 644 /tmp/tt-kmd/udev-50-tenstorrent.rules \
    /etc/udev/rules.d/50-tenstorrent.rules

# Record the kernel the image expects to boot, so the host-side extraction and
# the experiment script agree on which vmlinuz/initrd to use.
echo "$KVER" > /etc/simbricks-kernel-version

update-initramfs -c -k "$KVER" || update-initramfs -u -k "$KVER"
update-grub || true

modinfo "/lib/modules/$KVER/kernel/drivers/misc/tenstorrent.ko" | head -12
rm -rf /tmp/tt-kmd

# Keep the image small-ish; build deps are no longer needed.
apt-get -y autoremove --purge
apt-get -y clean
rm -rf /var/lib/apt/lists/*
