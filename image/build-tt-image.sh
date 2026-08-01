#!/bin/bash
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# Build a Tenstorrent-capable SimBricks guest image from an existing base image.
#
# Boots the base image in QEMU with user-mode networking, provisions it over SSH
# (see provision-tt-guest.sh), shuts it down, then extracts the boot artifacts
# the orchestration needs. Output layout matches what DistroDiskImage expects:
#
#   <out>/images/<name>/<name>          the disk image (qcow2)
#   <out>/images/<name>/boot/vmlinuz    kernel  -> QemuSim -kernel
#   <out>/images/<name>/boot/initrd     initramfs -> QemuSim.initrd
#
# This deliberately does not use image-builder/packer: packer's SSH step times
# out against this base image here, and driving QEMU directly keeps the failure
# modes visible. The provisioning itself is the same idea -- run a script in the
# guest as root.
#
# Usage:
#   image/build-tt-image.sh [-b BASE_IMAGE] [-o OUT_DIR] [-n NAME]
#                           [-s PROVISION_SCRIPT] [-B]
#
# -s names the guest-side script to run (default provision-tt-guest.sh). The
#    whole image/ directory is copied into the guest, so a script may use files
#    next to it.
# -B layers on the base image with a qcow2 backing file instead of copying it.
#    Only the delta is stored, which matters when stacking a multi-GiB stage
#    such as tt-metal on top of an existing image. The result then depends on
#    the base image staying put; drop -B for a standalone image.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE_IMAGE="${BASE_IMAGE:-/global_input/images/base/base}"
OUT_DIR="${OUT_DIR:-/global_input}"
NAME="${NAME:-tenstorrent}"
PROVISION_SCRIPT="${PROVISION_SCRIPT:-provision-tt-guest.sh}"
USE_BACKING="${USE_BACKING:-0}"
SSH_USER="${SSH_USER:-ubuntu}"
SSH_PASS="${SSH_PASS:-ubuntu}"
MEM="${MEM:-4096}"
CPUS="${CPUS:-$(nproc)}"
# Match the CPU the experiments run on (QemuSim uses -cpu Skylake-Server). The
# default qemu64 model has no AVX at all, and importing ttnn on it dies with
# SIGILL -- so provisioning would happily build an image that cannot run.
# QEMU masks any feature TCG cannot emulate, so this stays correct without KVM.
CPU_MODEL="${CPU_MODEL:-Skylake-Server}"
ACCEL="${ACCEL:-kvm:tcg}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
PROVISION_TIMEOUT="${PROVISION_TIMEOUT:-3600}"

while getopts "b:o:n:s:B" opt; do
  case "$opt" in
    b) BASE_IMAGE="$OPTARG" ;;
    o) OUT_DIR="$OPTARG" ;;
    n) NAME="$OPTARG" ;;
    s) PROVISION_SCRIPT="$OPTARG" ;;
    B) USE_BACKING=1 ;;
    *) echo "usage: $0 [-b BASE_IMAGE] [-o OUT_DIR] [-n NAME] [-s SCRIPT] [-B]" >&2; exit 1 ;;
  esac
done
[ -f "$HERE/$PROVISION_SCRIPT" ] || { echo "no provision script $HERE/$PROVISION_SCRIPT" >&2; exit 1; }

for tool in qemu-system-x86_64 qemu-img sshpass virt-copy-out virt-cat; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
[ -f "$BASE_IMAGE" ] || { echo "no base image at $BASE_IMAGE" >&2; exit 1; }

DEST="$OUT_DIR/images/$NAME"
WORK="$(mktemp -d)"
QEMU_PID=""

cleanup() {
  rc=$?
  # On failure the guest's console is the only record of why -- an ssh session
  # that dies mid-provision says nothing about the panic that killed it. Keep it
  # before the work directory goes away.
  if [ "$rc" -ne 0 ] && [ -f "$WORK/serial.log" ]; then
    keep="${SERIAL_LOG_DIR:-/tmp}/build-$NAME-serial.log"
    cp "$WORK/serial.log" "$keep" 2>/dev/null && {
      echo "--- last 25 lines of guest console (full log: $keep) ---" >&2
      tail -25 "$keep" >&2
    }
  fi
  if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# Pick a free forwarding port so concurrent builds do not collide.
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
 -o PreferredAuthentications=password -o PubkeyAuthentication=no \
 -o LogLevel=ERROR -o ConnectTimeout=10"
ssh_g()  { sshpass -p "$SSH_PASS" ssh  $SSH_OPTS -p "$PORT" "$SSH_USER@127.0.0.1" "$@"; }
scp_g()  { sshpass -p "$SSH_PASS" scp  $SSH_OPTS -P "$PORT" "$@"; }

echo "==> building '$NAME' from $BASE_IMAGE"
echo "    work=$WORK port=$PORT script=$PROVISION_SCRIPT backing=$USE_BACKING"

if [ "$USE_BACKING" = 1 ]; then
  # Store only the delta. The backing path must be absolute: the image is used
  # from a different working directory than it is built in.
  echo "==> creating overlay on $BASE_IMAGE"
  qemu-img create -f qcow2 -F "$(qemu-img info --output=json "$BASE_IMAGE" \
      | python3 -c 'import json,sys; print(json.load(sys.stdin)["format"])')" \
      -b "$(readlink -f "$BASE_IMAGE")" "$WORK/$NAME.qcow2" >/dev/null
else
  # Full copy: the result stands alone, not depending on a backing file.
  echo "==> copying base image"
  qemu-img convert -O qcow2 "$BASE_IMAGE" "$WORK/$NAME.qcow2"
fi

echo "==> booting guest"
qemu-system-x86_64 \
    -machine "q35,accel=$ACCEL" -cpu "$CPU_MODEL" \
    -m "$MEM" -smp "$CPUS" -display none \
    -drive "file=$WORK/$NAME.qcow2,if=virtio,format=qcow2" \
    -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" -device virtio-net,netdev=n0 \
    -serial "file:$WORK/serial.log" </dev/null >/dev/null 2>&1 &
QEMU_PID=$!

echo "==> waiting for ssh (up to ${BOOT_TIMEOUT}s)"
deadline=$((SECONDS + BOOT_TIMEOUT))
until ssh_g true 2>/dev/null; do
  if [ $SECONDS -ge $deadline ]; then
    echo "ERROR: guest did not become reachable over ssh" >&2
    tail -40 "$WORK/serial.log" >&2 || true
    exit 1
  fi
  kill -0 "$QEMU_PID" 2>/dev/null || { echo "ERROR: qemu exited early" >&2; exit 1; }
  sleep 5
done
echo "==> ssh up"

echo "==> provisioning with $PROVISION_SCRIPT"
# tar over ssh rather than `scp -r`: recent OpenSSH uses the SFTP protocol for
# scp and rejects a "." source path, and this copies the directory contents
# without depending on scp's varying directory-target semantics.
tar -C "$HERE" -cf - . | ssh_g "rm -rf /tmp/ttimg && mkdir -p /tmp/ttimg && tar -C /tmp/ttimg -xf -"
timeout "$PROVISION_TIMEOUT" \
  sshpass -p "$SSH_PASS" ssh $SSH_OPTS -p "$PORT" "$SSH_USER@127.0.0.1" \
  "chmod +x /tmp/ttimg/*.sh && sudo -E TT_KMD_REPO='${TT_KMD_REPO:-}' TT_KMD_REF='${TT_KMD_REF:-}' \
   TTNN_VERSION='${TTNN_VERSION:-}' TORCH_VERSION='${TORCH_VERSION:-}' \
   /tmp/ttimg/$PROVISION_SCRIPT"

echo "==> shutting down"
ssh_g "sudo shutdown -P now" >/dev/null 2>&1 || true
for _ in $(seq 1 60); do
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 2
done
if kill -0 "$QEMU_PID" 2>/dev/null; then
  echo "    guest did not power off; terminating"
  kill "$QEMU_PID" 2>/dev/null || true
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "==> extracting boot artifacts"
KVER="$(virt-cat -a "$WORK/$NAME.qcow2" /etc/simbricks-kernel-version | tr -d '\r\n')"
[ -n "$KVER" ] || { echo "ERROR: guest did not record a kernel version" >&2; exit 1; }
echo "    kernel $KVER"

mkdir -p "$DEST/boot"
virt-copy-out -a "$WORK/$NAME.qcow2" "/boot/vmlinuz-$KVER" "$DEST/boot/"
mv -f "$DEST/boot/vmlinuz-$KVER" "$DEST/boot/vmlinuz"

# The SimBricks custom kernel builds in everything needed to mount root and
# wants no initrd -- which is a large part of why it boots so much faster than
# the distro kernel, and boot length is what a synchronized run pays for. The
# provisioner says which case this is.
rm -f "$DEST/boot/initrd"
needs_initrd="$(virt-cat -a "$WORK/$NAME.qcow2" /etc/simbricks-needs-initrd 2>/dev/null \
               | tr -d '\r\n' || echo yes)"
if [ "$needs_initrd" != "no" ]; then
    virt-copy-out -a "$WORK/$NAME.qcow2" "/boot/initrd.img-$KVER" "$DEST/boot/"
    mv -f "$DEST/boot/initrd.img-$KVER" "$DEST/boot/initrd"
    echo "    initrd extracted (modular kernel)"
else
    echo "    no initrd needed"
fi

echo "==> installing image"
mv -f "$WORK/$NAME.qcow2" "$DEST/$NAME"

echo
echo "built $DEST/$NAME"
echo "  kernel:  $DEST/boot/vmlinuz  ($KVER)"
if [ -f "$DEST/boot/initrd" ]; then
    echo "  initrd:  $DEST/boot/initrd"
else
    echo "  initrd:  none (kernel needs no initrd)"
fi
echo "  tt-kmd:  $(virt-cat -a "$DEST/$NAME" /etc/tt-kmd.gitrev 2>/dev/null | tr -d '\r\n')"
