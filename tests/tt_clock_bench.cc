/*
 * Copyright 2026 SimBricks
 * SPDX-License-Identifier: MIT
 *
 * How fast is libttsim, and does chopping it into small batches cost anything?
 *
 * The adapter calls libttsim_clock(n) in batches so it can service SimBricks
 * messages in between. If the per-call overhead were significant, a small batch
 * would throttle the device model and the fix would be to run closer to the
 * horizon SimBricks says is safe. This measures the two halves separately:
 *
 *   1. libttsim_clock throughput as a function of batch size, with no SimBricks
 *      plumbing at all -- isolates libttsim's own per-call cost.
 *   2. the same sweep with the adapter's per-iteration bookkeeping simulated by
 *      a caller-supplied delay, to show what loop overhead would have to cost
 *      before batch size mattered.
 *
 * Usage: tt_clock_bench --lib LIBTTSIM.SO [--total-clocks N]
 */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "../ttsim_bm/libttsim_abi.h"

namespace {

double MonotonicSeconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

struct LibTtsim lib;

/* Batch sizes spanning what the adapter might plausibly use: 1 is pathological,
 * 64 is what tests/ttsim_matmul.py sets, 500 is the default and also one
 * 500 ns sync interval at 1 GHz, and the large ones approach "uninterrupted". */
const uint32_t kBatches[] = {1, 8, 64, 256, 500, 4096, 65536, 1000000};

}  // namespace

int main(int argc, char *argv[]) {
  const char *lib_path = nullptr;
  uint64_t total = 20000000;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
      lib_path = argv[++i];
    } else if (strcmp(argv[i], "--total-clocks") == 0 && i + 1 < argc) {
      total = strtoull(argv[++i], nullptr, 0);
    } else {
      fprintf(stderr,
              "Usage: %s --lib LIBTTSIM.SO [--total-clocks N]\n", argv[0]);
      return 1;
    }
  }
  if (lib_path == nullptr) {
    fprintf(stderr, "%s: --lib is required\n", argv[0]);
    return 1;
  }

  if (!LibTtsimLoad(&lib, lib_path)) {
    return 1;
  }
  /* No DMA callbacks installed: this benchmark never takes a core out of reset,
   * so nothing can initiate a NoC access to the host window. */
  lib.init();

  printf("libttsim clock throughput, %" PRIu64 " clocks per measurement\n",
         total);
  printf("(device idle -- all Tensix cores in reset)\n\n");
  printf("%12s %10s %12s %14s\n", "batch", "calls", "MHz", "ns per call");

  double ref_mhz = 0.0;
  for (size_t i = 0; i < sizeof(kBatches) / sizeof(kBatches[0]); i++) {
    uint32_t batch = kBatches[i];
    uint64_t calls = total / batch;
    if (calls == 0) {
      continue;
    }

    double t0 = MonotonicSeconds();
    for (uint64_t c = 0; c < calls; c++) {
      lib.clock(batch);
    }
    double dt = MonotonicSeconds() - t0;

    double clocks = static_cast<double>(calls) * batch;
    double mhz = clocks / dt / 1e6;
    if (ref_mhz == 0.0 || mhz > ref_mhz) {
      ref_mhz = mhz;
    }
    printf("%12u %10" PRIu64 " %12.2f %14.1f\n", batch, calls, mhz,
           dt / static_cast<double>(calls) * 1e9);
  }

  printf("\nbest %.2f MHz -> %.0f s of host time per simulated second\n",
         ref_mhz, 1e9 / (ref_mhz * 1e6));

  lib.exit();
  LibTtsimUnload(&lib);
  return 0;
}
