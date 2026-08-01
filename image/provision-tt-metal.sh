#!/bin/bash
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# Guest-side provisioning for the TT-Metalium / ttnn / PyTorch stack. Runs as
# root inside the VM; driven by image/build-tt-image.sh. Layers on top of an
# image that provision-tt-guest.sh has already prepared (distro kernel + tt-kmd).
#
# Installs, in the guest:
#
#   * PyTorch (CPU build -- it is only the reference implementation and the
#     source of the input tensors; the matmul itself runs on the accelerator).
#   * ttnn, which bundles the whole TT-Metalium runtime: libtt_metal.so,
#     libtt-umd.so (the user-mode driver that talks to /dev/tenstorrent/0),
#     the SOC descriptors, and the kernel sources.
#   * The sfpi RISC-V toolchain. tt-metal JIT-compiles the Tensix kernels on
#     first use, so the compiler has to be present in the image -- there is no
#     network inside a SimBricks run. The wheel deliberately ships without it
#     (ttnn/download_sfpi.py fetches it at install time), so we do the same
#     fetch here, into the location tt-metal looks in: $TT_METAL_HOME/runtime.

set -eux
export DEBIAN_FRONTEND=noninteractive

TTNN_VERSION="${TTNN_VERSION:-0.75.0}"
TORCH_VERSION="${TORCH_VERSION:-2.5.1}"

apt-get update
apt-get install -y --no-install-recommends python3-pip python3-venv ca-certificates curl xz-utils

# --no-cache-dir throughout: the guest root filesystem is only 8 GiB and pip's
# cache would otherwise hold a second copy of every wheel.
python3 -m pip install --no-cache-dir --upgrade pip

# CPU-only torch. The default PyPI wheel drags in the bundled CUDA runtime,
# which is several GiB of libraries this guest can never use.
python3 -m pip install --no-cache-dir \
    --index-url https://download.pytorch.org/whl/cpu "torch==${TORCH_VERSION}"

# ttnn last, so its numpy<2 pin wins over anything torch pulled in.
python3 -m pip install --no-cache-dir "ttnn==${TTNN_VERSION}"

# Resolve the install location without importing ttnn: the import path pulls in
# the whole runtime and is exactly what we are still setting up.
TT_METAL_HOME=$(python3 -c 'import sysconfig, os; print(os.path.join(sysconfig.get_paths()["purelib"], "ttnn"))')
[ -d "$TT_METAL_HOME/tt_metal" ] || { echo "ERROR: no tt_metal tree under $TT_METAL_HOME" >&2; exit 1; }
echo "TT_METAL_HOME=$TT_METAL_HOME"

# Which toolchain build this ttnn expects is pinned by the wheel itself, in a
# file of shell assignments (sfpi_version, sfpi_<arch>_<dist>_txz_hash, ...).
# Reading it instead of hardcoding a version keeps the two in step when
# TTNN_VERSION changes.
# shellcheck disable=SC1091
. "$TT_METAL_HOME/tt_metal/sfpi-version"
SFPI_VERSION="${SFPI_VERSION:-$sfpi_version}"
SFPI_SHA256="${sfpi_x86_64_debian_txz_hash:-}"
SFPI_FILE="sfpi_${SFPI_VERSION}_x86_64_debian.txz"

# The toolchain tarball unpacks to a top-level sfpi/, and tt-metal resolves the
# compiler as $TT_METAL_HOME/runtime/sfpi (SFPI_BASE in tt_metal/hw/CMakeLists.txt).
mkdir -p "$TT_METAL_HOME/runtime"
# The release tag is the bare version; older releases used a v prefix, so try
# both rather than fail on a naming change.
curl -fsSL -o /tmp/sfpi.txz \
    "https://github.com/tenstorrent/sfpi/releases/download/${SFPI_VERSION}/${SFPI_FILE}" \
  || curl -fsSL -o /tmp/sfpi.txz \
    "https://github.com/tenstorrent/sfpi/releases/download/v${SFPI_VERSION}/${SFPI_FILE}"

if [ -n "$SFPI_SHA256" ]; then
    echo "$SFPI_SHA256  /tmp/sfpi.txz" | sha256sum -c -
fi
tar -C "$TT_METAL_HOME/runtime" -xJf /tmp/sfpi.txz
rm -f /tmp/sfpi.txz
SFPI_GCC=$(echo "$TT_METAL_HOME"/runtime/sfpi/compiler/bin/riscv*-gcc)
[ -x "$SFPI_GCC" ] || { echo "ERROR: no sfpi gcc under $TT_METAL_HOME/runtime/sfpi" >&2; ls -R "$TT_METAL_HOME/runtime/sfpi" | head -40 >&2; exit 1; }
"$SFPI_GCC" --version | head -1

# One place that defines the environment, sourced both by the SimBricks app and
# by an interactive shell, so a manual debugging session matches the run.
cat > /etc/profile.d/tt-metal.sh <<EOF
export TT_METAL_HOME=$TT_METAL_HOME
export ARCH_NAME=wormhole_b0
# Slow dispatch: ttsim documents fast dispatch as not sufficiently tested.
export TT_METAL_SLOW_DISPATCH_MODE=1
# ttsim does not implement SFPLOADMACRO; tt-metal has a documented opt-out.
export TT_METAL_DISABLE_SFPLOADMACRO=1
export PYTHONUNBUFFERED=1
EOF
chmod 644 /etc/profile.d/tt-metal.sh

install -D -m 755 "$(dirname "$0")/tt_matmul_demo.py" /usr/local/bin/tt_matmul_demo.py

# UMD maps its host-visible scratch memory ("sysmem") out of a 1 GiB hugepage
# and finds it by scanning /proc/mounts for a hugetlbfs mount of that page size;
# /dev/hugepages-1G is the conventional location. Only the mount point is made
# here. The SimBricks payload runs as init=, so systemd never starts and a mount
# unit would not fire -- TenstorrentMetalHost mounts it explicitly instead, and
# the pages themselves are reserved on the kernel command line
# (hugepagesz=1G hugepages=N), since 1 GiB of physically contiguous memory is
# not reliably obtainable after boot.
mkdir -p /dev/hugepages-1G

# Import once here so a broken install shows up in the build log rather than
# only inside a simulation run. Not fatal: there is no device attached during
# provisioning, so an import that insists on one would fail here yet work in the
# run -- the simulation is the real test.
TT_METAL_HOME="$TT_METAL_HOME" ARCH_NAME=wormhole_b0 python3 - <<'EOF' \
    || echo "WARNING: importing ttnn during provisioning failed (see above)"
import os
import torch
import ttnn
print("torch", torch.__version__)
print("ttnn imported from", os.path.dirname(ttnn.__file__))
EOF

echo "${TTNN_VERSION}" > /etc/ttnn.version
echo "${SFPI_VERSION}" > /etc/sfpi.version

apt-get -y clean
rm -rf /var/lib/apt/lists/*
