#!/bin/bash
# Copyright 2026 SimBricks
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# End-to-end test of the ttsim SimBricks adapter without a guest.
#
# Starts simb_ttsim_bm and drives it with tt_host_probe, which plays the role
# QEMU would: it issues MMIO and serves the device's DMA out of a local buffer.
# Repeats across several link configurations, because the sync parameters are
# what shake out time-advance bugs (a link latency shorter than one clock step
# used to livelock the device's clock batching).
#
# Usage: tests/run_probe_test.sh [path/to/libttsim.so]

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"

LIB="${1:-${TTSIM_LIB:-$ROOT/../ttsim/src/_out/release_wh/libttsim.so}}"
DEV="$ROOT/ttsim_bm/simb_ttsim_bm"
PROBE="$HERE/tt_host_probe"

for f in "$DEV" "$PROBE"; do
  if [ ! -x "$f" ]; then
    echo "missing $f -- build it first (make ttsim-build; make -C tests)" >&2
    exit 1
  fi
done
if [ ! -f "$LIB" ]; then
  echo "missing libttsim.so at $LIB -- build it with 'make libttsim'" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

failures=0

run_case() {
  local name="$1" sync="$2" lat="$3" si="$4"
  shift 4
  local sock="$WORK/tt.sock" shm="$WORK/tt.shm" log="$WORK/dev.log"
  rm -f "$sock" "$shm" "$log"

  # Remaining arguments are extra device flags.
  "$DEV" "listen:$sock:$shm:sync=$sync:latency=$lat:sync_interval=$si" 0 \
      --lib "$LIB" "$@" >"$log" 2>&1 &
  local devpid=$!

  # Wait for the device to create its socket rather than sleeping blindly.
  local i
  for i in $(seq 1 100); do
    [ -S "$sock" ] && break
    sleep 0.1
  done

  timeout 120 "$PROBE" "connect:$sock:sync=$sync:latency=$lat:sync_interval=$si" \
      >"$WORK/probe.log" 2>&1
  local rc=$?

  kill "$devpid" 2>/dev/null
  wait "$devpid" 2>/dev/null

  if [ $rc -eq 0 ]; then
    echo "PASS  $name"
  else
    echo "FAIL  $name (rc=$rc)"
    sed 's/^/      probe: /' "$WORK/probe.log"
    sed 's/^/      dev:   /' "$log"
    failures=$((failures + 1))
  fi
}

# Latency and sync interval are in NANOSECONDS, matching what the orchestration
# emits and what QEMU's simbricks-pci expects.
echo "libttsim: $LIB"
run_case "synchronized, 500ns link"   true  500   500
run_case "synchronized, 10us link"    true  10000 10000
run_case "unsynchronized"             false 500   500
# A device clock slower than the sync interval: one step is 1 us but the peer
# needs attention every 500 ns, so the per-batch clock budget rounds to zero.
# Returning without advancing time there used to livelock the device.
run_case "clock step longer than sync interval" true 500 500 --clock-freq-mhz 1
# Batch larger than the sync window, so every batch gets clipped.
run_case "batch larger than sync window" true 500 500 --max-batch-clocks 100000

echo
if [ $failures -eq 0 ]; then
  echo "all probe tests passed"
  exit 0
fi
echo "$failures probe test(s) failed"
exit 1
