#!/usr/bin/env python3
#
# Copyright 2026 SimBricks
# SPDX-License-Identifier: MIT
#
# A PyTorch matrix multiply executed on the simulated Tenstorrent accelerator.
#
# Runs inside the guest, on top of the full Tenstorrent stack:
#
#     torch tensor -> ttnn.from_torch -> tt-metal (JIT-compiles RISC-V kernels)
#       -> UMD -> /dev/tenstorrent/0 (tt-kmd) -> PCIe -> SimBricks -> libttsim
#
# The result is compared against torch's own CPU matmul. Because the device
# computes in bfloat16 the comparison is by Pearson correlation, which is what
# tt-metal's own tests use, rather than exact equality.

import os
import sys
import time

# Must be set before ttnn is imported: tt-metal reads the dispatch mode when the
# runtime is initialized. Slow dispatch is the mode ttsim recommends -- fast
# dispatch stands up a dispatch kernel on extra cores and is documented as not
# sufficiently tested on the simulator.
os.environ.setdefault("TT_METAL_SLOW_DISPATCH_MODE", "1")
# ttsim does not implement SFPLOADMACRO; tt-metal has a documented opt-out.
os.environ.setdefault("TT_METAL_DISABLE_SFPLOADMACRO", "1")

import torch  # noqa: E402
import ttnn  # noqa: E402

M = int(os.environ.get("TT_DEMO_M", "32"))
K = int(os.environ.get("TT_DEMO_K", "32"))
N = int(os.environ.get("TT_DEMO_N", "32"))
# Correlation, not equality: the device computes in bfloat16 (8 mantissa bits)
# and accumulates in a different order than torch does.
MIN_PCC = float(os.environ.get("TT_DEMO_MIN_PCC", "0.99"))


def pcc(actual: torch.Tensor, expected: torch.Tensor) -> float:
    """Pearson correlation over the flattened tensors."""
    a = actual.flatten().to(torch.float32)
    e = expected.flatten().to(torch.float32)
    if torch.allclose(a, e):
        return 1.0
    return torch.corrcoef(torch.stack([a, e]))[0, 1].item()


def main() -> int:
    torch.manual_seed(0)

    a = torch.randn(M, K)
    b = torch.randn(K, N)
    expected = a @ b

    print(f"TTSIM MATMUL: torch {tuple(a.shape)} @ {tuple(b.shape)}", flush=True)

    t0 = time.time()
    device = ttnn.open_device(device_id=0)
    print(f"TTSIM MATMUL: device open in {time.time() - t0:.1f}s", flush=True)

    try:
        t0 = time.time()
        ta = ttnn.from_torch(a, dtype=ttnn.bfloat16, layout=ttnn.TILE_LAYOUT, device=device)
        tb = ttnn.from_torch(b, dtype=ttnn.bfloat16, layout=ttnn.TILE_LAYOUT, device=device)
        print(f"TTSIM MATMUL: inputs uploaded in {time.time() - t0:.1f}s", flush=True)

        t0 = time.time()
        tc = ttnn.matmul(ta, tb)
        print(f"TTSIM MATMUL: matmul dispatched in {time.time() - t0:.1f}s", flush=True)

        t0 = time.time()
        actual = ttnn.to_torch(tc)
        print(f"TTSIM MATMUL: result read back in {time.time() - t0:.1f}s", flush=True)
    finally:
        ttnn.close_device(device)

    score = pcc(actual, expected)

    print("--- device result (corner) ---", flush=True)
    print(actual[:4, :4], flush=True)
    print("--- torch reference (corner) ---", flush=True)
    print(expected[:4, :4], flush=True)
    print(f"TTSIM MATMUL: pcc={score:.6f} (threshold {MIN_PCC})", flush=True)

    if score >= MIN_PCC:
        print("TTSIM MATMUL: PASS", flush=True)
        return 0
    print("TTSIM MATMUL: FAIL", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
