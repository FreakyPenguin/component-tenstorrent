# libttsim performance notes

Where `libttsim`'s time actually goes, measured rather than guessed, and what
headroom exists. Nothing here is applied to the checked-out `ttsim`; the patch is
a **prototype**, kept because the measurement is only meaningful alongside the
thing that produced it.

Reproduce the numbers with `tests/tt_clock_bench` (drives `libttsim` directly,
no SimBricks plumbing) and `tests/tt_host_probe --idle-secs N` (real link, idle
peer). All figures are Wormhole, device idle, one core of an i7-11700.

## The adapter is not the problem

The first suspicion was that the adapter throttles the model by chopping it into
small batches so it can poll SimBricks. It does not:

| batch | `libttsim` alone | adapter, live idle peer |
|---|---|---|
| 1 | 10.96 MHz | 11.6 MHz |
| 64 | 11.39 MHz | 11.4 MHz |
| 4096 | 11.88 MHz | 11.8 MHz |
| 1000000 | 11.84 MHz | 13.0 MHz |

A full SimBricks poll between *every single clock step* costs about 12% against
effectively uninterrupted. `libttsim_clock(n)` is a bare `for` loop, so there is
no per-call setup to amortize.

## Where the time goes: O(number of tiles), per clock

`clock_current_chip()` sweeps every Tensix tile on every clock. The per-tile
activity masks (`rv32_cores_active`, `inst_pipes_active`) make the *inner* work
free when a tile is idle, but the sweep itself still runs. Confirmed by the two
chips, which differ only in tile count:

| Chip | Tensix tiles | Idle clock rate |
|---|---|---|
| Wormhole | 80 | 11.08 MHz |
| Blackhole | 140 | 6.41 MHz |

11.08 / 6.41 = 1.73 against a tile ratio of 140 / 80 = 1.75. Idle cost is very
nearly linear in tile count. At ~87 ns per clock over 80 tiles that is ~1.1 ns
per tile — a handful of cycles, already tight for this loop shape. The cost is
structural, not sloppy code.

## Prototype: make an idle clock O(1)

`libttsim-idle-fastpath.patch` adds two per-chip counters — how many Tensix rv32
cores and how many tensix instruction pipes are active anywhere — maintained at
the four sites that change the per-tile masks, and skips the sweep when both are
zero. That is provably equivalent: with nothing active, every iteration of the
sweep does nothing and breaks immediately.

| | Idle clock rate |
|---|---|
| upstream | 11.70 MHz |
| with the fast path | **1084.88 MHz** (93x) |

Validated: `run_probe_test.sh` 5/5, and the full `ttsim_matmul.py` demo passes
with **pcc 0.999987 — bit-identical to upstream**, so the numerics are untouched.

It only covers the *fully* idle case. Once any core runs, the sweep is back. A
per-tile bitmask would generalize it, at the cost of maintaining 80–140 bits
across the same sites.

## Is it worth fixing? The only question that matters

A synchronized simulation runs exactly as fast as whichever side is behind at
that instant. A faster chip model is worth something **only for the time the
peer spends blocked on it** — 93x on a component that was never holding anything
up would be worth nothing.

Measured directly. The adapter times the stretches where it has run to the
SimBricks deadline and has nothing to do until QEMU advances; the complement is
time QEMU was waiting on *it*. Sampled via SIGUSR1, which SimBricks sends every
`--profile-int` seconds, so the series can be trimmed at both ends rather than
averaging in a startup window where the peer has not launched and a shutdown
window where it is already gone:

```bash
simbricks-run --verbose --force --profile-int 1 --global-input-dir /global_input \
    tests/ttsim_kmd.py     # with TTSIM_SYNC=1
```

`ttsim_kmd.py` synchronized, measured from link-up to guest poweroff:

| | upstream | with fast path |
|---|---|---|
| Guest-live window | 100.1 s | 65.1 s |
| Device idle, waiting on QEMU | 39.3 s (39.2%) | 63.9 s (98.3%) |
| **Device was the bottleneck** | **60.8 s (60.8%)** | **1.1 s (1.7%)** |
| Simulated device time reached | 665.8 ms | 667.9 ms |
| Whole run, wall | 115 s | 79 s |

So it was worth fixing: QEMU spent the majority of the run blocked on the chip
model. Afterwards it is blocked 1.7% of the time, the run is 31% shorter, and
**further libttsim optimization would buy essentially nothing for this
workload** — what remains is QEMU under `-icount` + TCG, which cannot use KVM by
construction.

Unsynchronized the picture is different and duller: the two sides are decoupled,
the guest was always the limit, and `ttsim_matmul.py` moves only 97 s → 93 s.

The same reasoning says where a further fix *would* pay: workloads whose device
time is a larger share of the total — longer synchronized runs, Blackhole (6.41
vs 11.08 MHz idle) or multi-chip builds, and anything that keeps cores out of
reset, since the fast path covers only the fully idle case.

## Compiler flags

Upstream builds release with `-O2 -march=x86-64-v3`. On a host with AVX-512:

| Flags | Idle clock rate |
|---|---|
| `-O2 -march=x86-64-v3` (upstream) | 11.55 MHz |
| `-O3 -march=x86-64-v3` | 11.76 MHz |
| `-O2 -march=native` | 12.90 MHz |
| `-O3 -march=native` | 13.53 MHz |

~17% for free, but `-march=native` gives up the binary portability upstream
deliberately keeps (their releases are byte-reproducible on a pinned toolchain),
so it is a local-build option rather than a change to propose.

## Parallelization

Harder than it looks, and it would not help the case that hurts most here.

- Tiles are **not independent within a clock**: `rv32_step()` issues NoC accesses
  that land in another tile's SRAM in the same clock, so a naive
  thread-per-tile split races on exactly the state that makes the model correct.
- `ttsim` commits to **bit-exact, deterministic** results as a product contract
  (it is the golden reference for the ISA, aimed at safety certification).
  Parallel execution would have to be staged — compute then commit, or per-clock
  message queues — to keep determinism, which is a substantial redesign rather
  than a threading annotation. Upstream is aware: their
  `unsupported_functionality.md` notes some features "can preclude multithreading
  without new communication channels".
- It does nothing for the idle phase, which the counter fix already makes free,
  and little for small workloads where only a handful of tiles are active. It
  would pay off on workloads that light up most of the 80 tiles — the opposite
  end from a 32×32 matmul.

Ranked by value for money on this integration: the idle fast path first — small,
contained, provably equivalent, and it took the device from being the bottleneck
for 61% of a synchronized run to 1.7%. Compiler flags second, and only for local
builds. Parallelization is not worth considering for this workload: after the
fast path there is almost no device-bound time left to recover, and the phase it
would target (many tiles active at once) is not one a 32×32 matmul reaches.
