#!/bin/bash -eux
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# image-builder component stage: build and install Tenstorrent's kernel-mode
# driver (tt-kmd) so the guest binds the simulated accelerator with the real
# driver rather than vfio-pci.
#
# Layer this onto a prebuilt base image:
#
#   make image NAME=tenstorrent ACCELERATOR=tcg \
#       SOURCE_IMAGE=/global_input/images/base/base SOURCE_CHECKSUM=none \
#       BASE_SCRIPTS= \
#       EXTRA_SCRIPTS=/path/to/component-tenstorrent/image/install-tt-kmd.sh

set -eux
export DEBIAN_FRONTEND=noninteractive

TT_KMD_REPO="${TT_KMD_REPO:-https://github.com/tenstorrent/tt-kmd.git}"
# Pin so image builds stay reproducible; override to track a different revision.
TT_KMD_REF="${TT_KMD_REF:-main}"

apt-get update
apt-get install -y --no-install-recommends build-essential git pciutils kmod

# Build against the kernel the image will BOOT, not the one running the build
# (during provisioning the guest is still on the cloud kernel).
KDIR=$(ls -d /lib/modules/*/build | sort -V | tail -1)
KVER=$(basename "$(dirname "$KDIR")")

git clone "$TT_KMD_REPO" /tmp/tt-kmd
git -C /tmp/tt-kmd checkout "$TT_KMD_REF"

make -C /tmp/tt-kmd modules KDIR="$KDIR"

install -D -m 644 /tmp/tt-kmd/tenstorrent.ko \
    "/lib/modules/$KVER/kernel/drivers/misc/tenstorrent.ko"
depmod -a "$KVER"

# Autoload on boot, and install the udev rule so /dev/tenstorrent/* gets sane
# permissions (the payload runs as root, but userspace tooling expects it).
echo tenstorrent > /etc/modules-load.d/simbricks-tenstorrent.conf
install -D -m 644 /tmp/tt-kmd/udev-50-tenstorrent.rules \
    /etc/udev/rules.d/50-tenstorrent.rules

# The driver ships a modprobe.d snippet; keep it if present.
if [ -f /tmp/tt-kmd/modprobe.d-tenstorrent.conf ]; then
    install -D -m 644 /tmp/tt-kmd/modprobe.d-tenstorrent.conf \
        /etc/modprobe.d/tenstorrent.conf
fi

# Record what was built so the image is self-describing.
git -C /tmp/tt-kmd rev-parse HEAD > /etc/tt-kmd.gitrev
modinfo "/lib/modules/$KVER/kernel/drivers/misc/tenstorrent.ko" | head -20

rm -rf /tmp/tt-kmd
