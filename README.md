# component-tenstorrent

SimBricks integration for [Tenstorrent's ttsim](https://github.com/tenstorrent/ttsim).

Presents `libttsim.so` — Tenstorrent's Wormhole/Blackhole chip model — as a
SimBricks PCIe device, so a simulated host can drive a simulated Tenstorrent
accelerator inside a SimBricks virtual prototype.

The chip model itself is **not** bundled or modified. The adapter `dlopen`s
whichever `libttsim.so` it is pointed at, so one binary serves every chip build
(`wh`, `bh`, `wh_x2`, …) — the same approach `ttsim-qemu` (`lib=…`) and ttsim's
own `ttsim-riscv64` (`--tt-device`) take.

## Layout

| Path | Contents |
|---|---|
| `ttsim_bm/` | The adapter: SimBricks PCIe protocol ⟷ `libttsim_*` ABI. Builds `simb_ttsim_bm`. |
| `ttsim_sys_py/` | `simbricks.components.tenstorrent.system` — the hardware description. |
| `ttsim_sim_bm_py/` | `simbricks.components.tenstorrent.simulation.behavioral` — the simulator class. |
| `conda-recipes/` | Three packages: `…-sys-py`, `…-sim-bm-py`, `…-sim-bm-bin`. |
| `image/` | Guest image provisioning: tt-kmd, then TT-Metalium / ttnn / PyTorch. |
| `tests/` | Host-side probe (no guest needed) and the QEMU experiment scripts. |
| `perf/` | Where `libttsim`'s time goes, and a prototype for the idle path. |

## Building

Needs `simbricks-lib` (headers + `libparser.a`/`libpcie.a`/`libbase.a`):

```bash
micromamba install -c https://conda.simbricks.io/latest simbricks-lib
```

Then:

```bash
# The chip model (or download it from ttsim's releases page).
make libttsim TTSIM_CHIP=wh

# The adapter and the python packages.
make ttsim-build SIMBRICKS_INC_DIR=$CONDA_PREFIX/include SIMBRICKS_LIB_DIR=$CONDA_PREFIX/lib
make ttsim-python-develop
```

## Testing

`make selftest` drives `libttsim` directly with no SimBricks plumbing — it
separates `dlopen`/ABI problems from protocol problems.

`tests/run_probe_test.sh` is the real end-to-end test and needs no guest image.
`tests/tt_host_probe` plays the role QEMU would: it connects to the adapter,
issues MMIO, and serves the device's DMA out of a local buffer. It configures a
BAR0 TLB window onto Tensix tile 0, writes and reads back its L1, then drives a
host→device DMA through the BAR2 engine and verifies both the payload and the
completion word. It runs across four link configurations, because the sync
parameters are what shake out time-advance bugs.

```bash
make -C tests SIMBRICKS_INC_DIR=$CONDA_PREFIX/include SIMBRICKS_LIB_DIR=$CONDA_PREFIX/lib
./tests/run_probe_test.sh
```

`tests/tt_clock_bench` measures `libttsim`'s clock throughput against batch size,
with no SimBricks plumbing, which is how to tell a slow chip model from an
adapter that is interrupting it too often:

```bash
./tests/tt_clock_bench --lib ../ttsim/src/_out/release_wh/libttsim.so
```

For the other half — the adapter's own loop overhead — `tt_host_probe
--idle-secs N` holds a real link open with nothing to do, so the device's
progress lines report its loop throughput at a given `--max-batch-clocks`.

`tests/ttsim_smoke.py` boots a Linux guest with the device attached and checks
enumeration and vfio-pci binding. It needs a base disk image:

```bash
simbricks-run --verbose --force --global-input-dir /global_input tests/ttsim_smoke.py
```

Running it directly (`python tests/ttsim_smoke.py`) prints the generated
commands without needing an image.

`tests/ttsim_kmd.py` is the functional gate for the Tenstorrent stack: it boots a
guest that binds the device with Tenstorrent's real kernel driver (tt-kmd) and
checks that `/dev/tenstorrent/0` appears. Build its image once first:

```bash
./image/build-tt-kernel.sh                     # -> image/kernel-out/
./image/build-tt-image.sh -b /global_input/images/base/base \
    -n tenstorrent -s provision-tt-kernel.sh -B
rm -rf out
simbricks-run --verbose --force --global-input-dir /global_input tests/ttsim_kmd.py
```

It passes when the guest reports:

```
Linux version 5.15.93-tt ...
tenstorrent: Loading Tenstorrent AI driver module v2.10.1-pre
tenstorrent 0000:00:02.0: Found a Tenstorrent Wormhole device
tenstorrent 0000:00:02.0: ARC message queue not available (this is normal for old FW)
        Kernel driver in use: tenstorrent
crw------- 1 root root 249, 0 /dev/tenstorrent/0
TTSIM KMD: PASS /dev/tenstorrent/0 present
```

This is also the experiment that runs synchronized: `TTSIM_SYNC=1` completes it
in about 103 s. See [Synchronized runs](#synchronized-runs).

`tests/ttsim_matmul.py` is the full-stack demo: a PyTorch matrix multiply that
actually executes on the simulated accelerator. In the guest, `ttnn` converts the
torch tensors, TT-Metalium JIT-compiles the Tensix kernels and pushes them over
PCIe through UMD and tt-kmd, and the result is read back and compared against
torch's own CPU matmul. It needs the tt-metal image, layered on the tt-kmd one:

```bash
./image/build-tt-image.sh -b /global_input/images/tenstorrent/tenstorrent \
    -n tt-metal -s provision-tt-metal.sh -B
rm -rf out   # --force re-uses the run id but does not clear it; see below
simbricks-run --verbose --force --global-input-dir /global_input tests/ttsim_matmul.py
```

Because the device computes in bfloat16 and accumulates in a different order than
torch does, the check is Pearson correlation against the reference — the same
criterion tt-metal's own tests use — rather than exact equality. It passes with:

```
TTSIM MATMUL: torch (32, 32) @ (32, 32)
TTSIM MATMUL: device open in 29.3s
TTSIM MATMUL: inputs uploaded in 0.1s
TTSIM MATMUL: matmul dispatched in 22.9s
TTSIM MATMUL: result read back in 0.0s
--- device result (corner) ---
tensor([[  9.8750,  -8.3125,   7.3438,  -5.7812],
        [ -6.5625,  -4.4375,  -2.7188,  -3.9219]], dtype=torch.bfloat16)
--- torch reference (corner) ---
tensor([[  9.9502,  -8.3410,   7.2702,  -5.8115],
        [ -6.5934,  -4.4204,  -2.7197,  -3.9339]])
TTSIM MATMUL: pcc=0.999987 (threshold 0.99)
TTSIM MATMUL: PASS
```

The whole run — boot, tt-kmd load, device open, JIT-compiling the Tensix kernels,
matmul, read-back, poweroff — takes about 97 s of wall time unsynchronized. Set
`TTSIM_MATMUL_SIZE` for a larger problem; 128 (a 4×4 grid of tiles rather than a
single one) also passes:

| Size | Kernels built | Device open | Matmul | PCC | Run wall time |
|---|---|---|---|---|---|
| 32 | 13 | 29.3 s | 22.9 s | 0.999987 | 97 s |
| 128 | 17 | 23.4 s | 41.2 s | 0.999981 | 104 s |

Three warnings in that log are expected rather than symptoms:

| Warning | Why |
|---|---|
| `Failed to set initial power state: -22` (tt-kmd) | The chip model does not implement the ARC power-state transition; nothing depends on it. |
| `Board n150 expects 1 units, but harvest mask indicates 0` (UMD) | `ARC_GET_HARVESTING` reports nothing harvested, while a real n150 has one Tensix row fused off. |
| `AICLK failed to settle ... Expected 500, observed 1000` (UMD) | On close UMD asks for the idle clock; the model always reports its nominal 1000 MHz. |

**Synchronization is off by default** in all experiments; set `TTSIM_SYNC=1` to
turn it on. See [Synchronized runs](#synchronized-runs) for what it costs and
where that cost comes from.

## Synchronized runs

Unsynchronized is the right mode for bringing software up: device time and guest
time float independently, so the guest runs as fast as the host allows and only
correctness is on the line. Synchronized mode couples them, which is what makes
timing meaningful — and expensive.

### Where the time goes

The device is the bottleneck, by a wide margin. Every second of *guest virtual
time* obliges `libttsim` to simulate a second of device time:

| Device state | Clock rate | Wall time per simulated second |
|---|---|---|
| Tensix cores in reset (boot, driver probe) | ~11.4 MHz | ~88 s |
| Tensix cores executing (tt-metal workloads) | ~0.17 MHz | ~5900 s |

QEMU under `-icount` costs roughly 15 wall-s per guest virtual second by
comparison, so it is not what you optimize. **The lever is reducing how much
guest virtual time the experiment needs**, which is exactly why SimBricks pairs a
trimmed no-initrd kernel with an `init=` shell script instead of booting systemd
off an initramfs. Going back to that kernel (see [Guest kernel](#guest-kernel))
cut the boot from 12.40 to 2.31 guest virtual seconds — about 15 minutes of
device wall time per synchronized run.

### `cpu_freq`, `-icount`, and a hazard

When synchronized, `QemuSim` drops KVM and adds `-icount shift=N,sleep=off`,
deriving `N` from the host's `cpu_freq` so that one instruction takes `2^N` ns.
Lower `N` means a faster modelled CPU, so the same work costs *less* guest
virtual time — which is the direction you want. Measured on a full boot of the
base image:

| `-icount shift` | Modelled CPU | Boot wall | Guest virtual time |
|---|---|---|---|
| 2 | 0.25 GHz | 35.0 s | 6.79 s |
| 0 | 1 GHz | 34.0 s | 3.36 s |
| −1 | 2 GHz | 37.1 s | 2.99 s |
| −2 | 4 GHz | 45.1 s | 2.96 s |

Wall time is nearly flat because boot mixes CPU-bound work (fixed instruction
count) with timer-driven waits (fixed virtual duration). What matters for
synchronization is the last column, and it flattens out by −1: the remaining
virtual time is waiting, not computing. `cpu_freq = "3GHz"` gives shift −2 and is
a reasonable default. Negative shifts are fine — SimBricks' QEMU accepts them,
where upstream rejects anything below 0.

**The hazard:** the conversion is only correct for `GHz`. The `MHz` branch uses a
base of 3 where it needs `log2(1000) ≈ 10`, so `cpu_freq = "500MHz"` asks for a
**64 GHz** guest rather than 0.5 GHz, and `"100MHz"` for 16 GHz. Specify
`cpu_freq` in GHz.

### What has been run

`TTSIM_SYNC=1 tests/ttsim_kmd.py` passes — boot plus driver bind, fully
synchronized, in **103 s of wall time**:

```
qemu-system-x86_64: -icount shift=-2,sleep=off: warning: time_shift=-2
[    0.594510] tenstorrent 0000:00:02.0: Found a Tenstorrent Wormhole device
TTSIM KMD: PASS /dev/tenstorrent/0 present
```

The guest reaches the driver at 0.59 s of virtual time — lower than the 2.31 s
the same image takes unsynchronized, because under `-icount` virtual time is
charged per instruction at the modelled 4 GHz rather than tracking wall time.
The device advanced 642 ms of its own time at 4–13 MHz, spending the difference
waiting for the guest at sync points.

This whole run stays in the cores-in-reset regime. A synchronized
`ttsim_matmul.py` is a different proposition: once tt-metal takes the Tensix
cores out of reset the device drops to ~0.17 MHz, so the sub-second of device
time the workload needs becomes hours of wall time. That has not been run to
completion. Treat unsynchronized as the mode for functional work and
synchronized as the mode for short, targeted timing questions.

## Guest kernel

**tt-kmd does not build against the stock SimBricks kernel**, whose config has
neither `CONFIG_HWMON` nor `CONFIG_DMA_SHARED_BUFFER`, so `modpost` fails on
`hwmon_device_register_with_info`, `dma_buf_export` and `dma_buf_move_notify`.

The obvious workaround — install the distro `linux-generic` kernel — is the wrong
trade. SimBricks ships a trimmed, no-initrd kernel *precisely* to keep boot
short, and boot length is the dominant cost of a synchronized run. Measured
unsynchronized, to the point where tt-kmd binds the device:

| Kernel | Guest time to bind tt-kmd |
|---|---|
| distro `linux-generic` + initrd | 12.40 s |
| SimBricks custom `5.15.93-tt` | **2.31 s** |

That 5.4x is what a synchronized run pays for, since the device has to simulate
every guest second at ~88 s of wall time. (The absolute figure differs under
synchronization — `-icount` charges virtual time per instruction rather than
tracking wall time, so the same boot lands at 0.59 s — but the ratio between
kernels is what matters, and only the custom-kernel case has been measured
synchronized: 103 s end to end.)

So instead of changing kernels, `image/build-tt-kernel.sh` rebuilds the SimBricks
kernel with three options added (`image/kernel-tt.config`):

| Option | For |
|---|---|
| `CONFIG_HWMON=y` | `hwmon_device_register_with_info()` in tt-kmd's `telemetry.c` |
| `CONFIG_SYNC_FILE=y` | the only user-selectable option that `select`s `CONFIG_DMA_SHARED_BUFFER`, which has no prompt of its own |
| `CONFIG_DMABUF_MOVE_NOTIFY=y` | `dma_buf_move_notify()`, which tt-kmd calls to revoke exported mappings |

It mirrors `image-builder/kernel/build-kernel.sh` — same source, same gem5 timer
patch, same base config — and merges the fragment over the top rather than
forking the pinned config. `CONFIG_LOCALVERSION="-tt"` keeps it distinguishable
from the stock 5.15.93. The build asserts `CONFIG_DMA_SHARED_BUFFER` actually
ended up `=y`, because reaching it only through `select` means an upstream
Kconfig change would otherwise go unnoticed until modpost, inside a guest,
twenty minutes later.

## Guest images

Built in sequence by `image/build-tt-image.sh`, which boots an image, provisions
it over SSH, and writes `<name>` and `boot/vmlinuz` (plus `boot/initrd`, only if
the kernel needs one) into `/global_input/images/<name>/`.

```bash
./image/build-tt-kernel.sh                     # -> image/kernel-out/
./image/build-tt-image.sh -b /global_input/images/base/base \
    -n tenstorrent -s provision-tt-kernel.sh -B
./image/build-tt-image.sh -b /global_input/images/tenstorrent/tenstorrent \
    -n tt-metal -s provision-tt-metal.sh -B
```

| Image | Built from | Provisioner | Adds |
|---|---|---|---|
| `tenstorrent` | `base` | `provision-tt-kernel.sh` | custom kernel `5.15.93-tt`, tt-kmd |
| `tt-metal` | `tenstorrent` | `provision-tt-metal.sh` | ttnn, TT-Metalium, sfpi, PyTorch |

`provision-tt-guest.sh` is the older distro-kernel provisioner, kept as a
fallback if you cannot build a kernel; images it produces need `QemuSim.initrd`,
which the experiment scripts set only when an initrd was actually extracted.

`-B` makes a qcow2 overlay instead of copying the base image, so only the delta
is stored (~600 MiB and ~2.5 GiB respectively). The result then depends on the
image below it staying put; drop `-B` for standalone images if you have the disk.
Note that ttnn's wheel deliberately ships *without* the sfpi RISC-V toolchain (it
is fetched at install time), so the provisioner fetches it into the image — there
is no network inside a SimBricks run, and tt-metal compiles its kernels on first
use.

This drives QEMU directly rather than going through `image-builder`/packer:
packer's SSH step times out against this base image in this container, while the
same credentials work fine over a plain forwarded port. `image/install-tt-kmd.sh`
is kept for use as an `image-builder` component stage (`EXTRA_SCRIPTS=`) if you
have that path working.

## How it works

### BAR translation, not config forwarding

`libttsim` keeps fixed internal BAR bases and decodes accesses by absolute
physical address; SimBricks assigns BARs host-side and delivers `(bar, offset)`.
The adapter declares the sizes in its device intro and translates
`(bar, offset) → base + offset`. Config space is only ever **read** — writing it
would move `libttsim`'s bases out from under the translation, and the ABI
documents config writes as fatal anyway.

Geometry is keyed on the device ID read back from config space, so the binary is
chip-agnostic:

| BAR | Base | Size (WH) | Size (BH) |
|---|---|---|---|
| 0 | `0x1_0000_0000` | 512 MiB | 512 MiB |
| 2 | `0x1_2000_0000` | 1 MiB | 1 MiB |
| 4 | `0x8_0000_0000` | 32 MiB | 32 GiB |

The chip advertises **no interrupts** (`interrupt_pin = 0`, no capability list,
no MSI/MSI-X), so the adapter never sends an interrupt message; guest software
polls.

### The two invariants in `ttsim_bm.cc`

**`libttsim` is not reentrant**, so it is entered from exactly one place.
`PollPcie()` never calls into it — it copies MMIO requests out of their
shared-memory slots into a queue, and `DrainDeferredMmio()` is the sole caller of
the MMIO entry points.

**`libttsim`'s DMA callbacks are synchronous** while SimBricks DMA is
request/completion asynchronous. `DmaPumpUntil()` bridges this with a nested poll
loop. This is safe because DMA callbacks are never nested — `libttsim` chunks
transfers at 4 KiB and issues them sequentially.

DMA is initiated from *both* `libttsim_clock` (a NoC access to the host window)
and from an MMIO write to the BAR2 doorbell, which runs the DMA engine inline. So
the pump must work from inside any `libttsim` call. The probe test covers exactly
this case.

A corollary that is easy to get wrong: **an outgoing message slot must never be
held across a `libttsim` call.** Slots are handed out and consumed in ring order,
so if a nested DMA sent slot N+1 while we still held an unsent slot N, the peer
would block on N — which we could not send until that DMA completed. Every path
completes its `libttsim` call into a local buffer first, then allocates the slot.

### Time

`libttsim_clock(n)` advances `n` steps and time moves only inside it. The chip
reports a 1000 MHz AICLK, so one step is one nanosecond by default
(`--clock-freq-mhz`). The adapter runs steps in batches (`--max-batch-clocks`),
clipped to the next point SimBricks needs attention.

Note that `libttsim` is a *functional*, not cycle-accurate, model — a "clock" is
a functional step, not a hardware cycle — so treat the timing as approximate and
do not present results as cycle-accurate.

## Known issues and caveats

- **Latency units are nanoseconds on the wire, picoseconds in the struct, and
  `libparser` does not convert.** The orchestration emits `latency=500`
  (nanoseconds) but `SimbricksParametersParse` stores that raw number into
  `SimbricksBaseIfParams.link_latency`, which is picoseconds. The adapter
  multiplies by 1000 to compensate. This was verified against the actual peer:
  QEMU's `simbricks-pci`, given `sync-period=500`, emits sync messages stamped
  **500000 ps** apart. Without the conversion the device syncs 1000x too often
  and the simulation crawls — a guest that boots in a couple of minutes made no
  progress at all in 40.
- **`Simulator.wait` is not the attribute you want; `wait_terminate` is.**
  `wait_terminate` is a property backed by `_wait`, so `sim.wait = True` silently
  creates an unused attribute. The runner then has nothing in `_wait_sims`, falls
  straight through its wait loop, and SIGINTs every simulator — killing QEMU
  mid-boot while reporting the run as successful. It looks exactly like a hang
  partway through the kernel log. Both experiment scripts set `wait_terminate`.
- **Performance is `libttsim`'s, not the adapter's.** Idle throughput is
  ~11.5 MHz of simulated device clock on one core, i.e. ~87 ns of host time per
  clock step and ~88 s per simulated second. The obvious suspicion — that the
  adapter throttles the model by chopping it into small batches so it can poll
  SimBricks — does not hold up. `tests/tt_clock_bench` drives `libttsim` with no
  SimBricks plumbing at all, and the whole range from a batch of 1 to a batch of
  a million spans 8%:

  | batch | `libttsim` alone | adapter, live idle peer |
  |---|---|---|
  | 1 | 10.96 MHz | 11.6 MHz |
  | 64 | 11.39 MHz | 11.4 MHz |
  | 500 | 11.56 MHz | 11.4 MHz |
  | 4096 | 11.88 MHz | 11.8 MHz |
  | 1000000 | 11.84 MHz | 13.0 MHz |

  The right-hand column is the same sweep through the real adapter against a
  live SimBricks peer (`tt_host_probe --idle-secs`), taken from `libttsim`'s own
  end-of-run total rather than sampled intervals. Calling `libttsim_clock(1)`
  with a full SimBricks poll between *every single clock step* costs about 12%
  against effectively uninterrupted. `libttsim_clock(n)` is a bare `for` loop, so
  there is no per-call setup to amortize; the model is simply slow. Large DMAs
  are split into 4 KiB blocking round-trips, each costing at least one link
  latency.

  The cost is structural: `clock_current_chip()` sweeps every Tensix tile on
  every clock, so idle throughput scales with tile count (Wormhole's 80 tiles
  give 11.08 MHz, Blackhole's 140 give 6.41). That is worth fixing — in a
  synchronized `ttsim_kmd.py` run QEMU is blocked on the device for **61%** of
  the time. [`perf/`](perf/) has the measurement, a prototype that makes an idle
  clock O(1) (bit-identical results; device-bound time drops to 1.7% and the run
  is 31% shorter), and why parallelizing the model would not help here.
- **`--profile-int S` makes the adapter dump a counter snapshot every S seconds**
  via SIGUSR1, including how long it has sat idle waiting on the peer. That
  ratio, not raw clock rate, is what decides whether the device model is worth
  optimizing: a synchronized run is only as fast as whichever side is behind.
- **Idle and busy device throughput differ by ~70x.** The adapter's progress
  lines show ~13 MHz while the Tensix cores are in reset and ~0.17 MHz once they
  are running code — a clock step that has to interpret RISC-V on 80 tiles costs
  far more than one that does not. Wall-clock time for the demo is dominated by
  this, not by the SimBricks link.
- **`--max-batch-clocks` only applies when unsynchronized, and is a minor knob.**
  It bounds how long the device runs without looking for an incoming MMIO
  request, so it trades a little throughput (see above — very little) for
  response latency. When *synchronized* it does not apply at all: SimBricks
  computes exactly how far ahead it is safe to run, and the adapter runs that
  whole window, because nothing can need its attention before the deadline.
  `tests/ttsim_matmul.py` sets 64, worth ~1 s of the 97 s run: the whole 32×32
  run issues only ~27 k MMIO accesses and ~440 DMA transfers, because tt-metal
  moves kernel binaries and tensors through the host sysmem window and lets the
  *device* DMA them rather than pushing them through TLB windows with host
  stores.
- The adapter prints a progress line every `--progress-secs` seconds (10 by
  default) with the current clock rate and MMIO/DMA rates, and a summary at
  exit. On runs this long, "stuck or just slow?" is otherwise unanswerable.
- **A guest can die mid-provision and take the reason with it.** One
  `provision-tt-metal.sh` run lost its ssh connection during `apt-get install`
  and did not reproduce. `build-tt-image.sh` now copies the guest console to
  `$SERIAL_LOG_DIR/build-<name>-serial.log` and prints its tail when a build
  fails, because an ssh session that dies says nothing about the panic or OOM
  that killed it.
- **`simbricks-run --force` does not clear the previous output directory.** It
  re-uses the same run id and then fails in `prepare()` with
  `FileExistsError: ... out/<experiment>/<n>/global_input`. Remove `out/` between
  runs.
- **Guest CPU features are not optional here.** `libtt_metal.so` needs AVX2, and
  the default `qemu64` CPU model has no AVX at all — importing `ttnn` on it dies
  with SIGILL. `QemuSim` uses `-cpu Skylake-Server`, and
  `image/build-tt-image.sh` now matches it, so provisioning cannot quietly build
  an image the experiment can't run. (The AVX-512 in `libtt_metal.so` is only
  BLAKE3's runtime-dispatched backend, selected on CPUID, so it is not a
  requirement; and QEMU masks any feature TCG cannot emulate, which makes the
  dispatch safe either way.)
- **No `/dev/kvm` in this container**, so `accel=kvm:tcg` silently lands on TCG
  and the guest runs emulated even unsynchronized. That mostly costs guest-side
  CPU time — which the matmul demo has plenty of, since tt-metal JIT-compiles
  its Tensix kernels with a RISC-V gcc at startup.
- **UMD needs a 1 GiB hugepage in the guest.** It maps its host-visible scratch
  memory ("sysmem") out of one and locates it by scanning `/proc/mounts` for a
  hugetlbfs mount of that page size; the only alternative it offers — mapping
  ordinary pages — requires an IOMMU this simulated platform does not have. So
  `TenstorrentMetalHost` reserves the pages on the kernel command line
  (`hugepagesz=1G hugepages=N`, since 1 GiB of physically contiguous memory is
  not reliably obtainable after boot) and mounts `/dev/hugepages-1G` itself. It
  cannot use a systemd mount unit: the SimBricks payload runs as `init=`, so
  systemd never starts.
- **Fatal errors.** Any `libttsim` contract violation prints a diagnostic and
  `_Exit(1)`s the whole adapter process; there is no way to catch it. The adapter
  validates BAR ranges and access sizes at its boundary so the common cases
  produce a readable message first.
- **Single chip only.** Multi-chip builds (`wh_x2`, …) expose several PCI devices
  through one library; the adapter currently presents device 0.
- **Blackhole** is implemented (device ID `0xB140`) but only Wormhole is covered
  by the tests. BH's 32 GiB BAR4 needs adequate guest MMIO space.
