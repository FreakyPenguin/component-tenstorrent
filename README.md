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
./image/build-tt-image.sh          # -> /global_input/images/tenstorrent/
simbricks-run --verbose --force --global-input-dir /global_input tests/ttsim_kmd.py
```

It passes when the guest reports:

```
tenstorrent: Loading Tenstorrent AI driver module v2.10.1-pre
tenstorrent 0000:00:02.0: Found a Tenstorrent Wormhole device
tenstorrent 0000:00:02.0: ARC message queue not available (this is normal for old FW)
        Kernel driver in use: tenstorrent
crw------- 1 root root 241, 0 /dev/tenstorrent/0
TTSIM KMD: PASS /dev/tenstorrent/0 present
```

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
TTSIM MATMUL: device open in 30.3s
TTSIM MATMUL: inputs uploaded in 0.1s
TTSIM MATMUL: matmul dispatched in 30.5s
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

The whole thing — boot, tt-kmd load, device open, JIT-compiling the Tensix
kernels, matmul, read-back, poweroff — takes about 110 s of guest time. Set
`TTSIM_MATMUL_SIZE` for a larger problem; 128 (a 4×4 grid of tiles rather than a
single one) also passes:

| Size | Kernels built | Device open | Matmul | PCC |
|---|---|---|---|---|
| 32 | 13 | 30.3 s | 30.5 s | 0.999987 |
| 128 | 17 | 39.1 s | 51.2 s | 0.999981 |

Three warnings in that log are expected rather than symptoms:

| Warning | Why |
|---|---|
| `Failed to set initial power state: -22` (tt-kmd) | The chip model does not implement the ARC power-state transition; nothing depends on it. |
| `Board n150 expects 1 units, but harvest mask indicates 0` (UMD) | `ARC_GET_HARVESTING` reports nothing harvested, while a real n150 has one Tensix row fused off. |
| `AICLK failed to settle ... Expected 500, observed 1000` (UMD) | On close UMD asks for the idle clock; the model always reports its nominal 1000 MHz. |

**Synchronization is off by default** in all experiments. Unsynchronized is the
right mode for bringing software up, and not only for speed: when synchronized,
`QemuSim` switches to `-icount` + TCG and cannot use KVM. Set `TTSIM_SYNC=1` to
turn it on, and expect it to be much slower.

### Guest images

There are two, built in sequence by `image/build-tt-image.sh`, which boots an
image, provisions it over SSH, and writes `<name>`, `boot/vmlinuz` and
`boot/initrd` into `/global_input/images/<name>/`.

| Image | Built from | Provisioner | Adds |
|---|---|---|---|
| `tenstorrent` | `base` | `provision-tt-guest.sh` | distro kernel, tt-kmd |
| `tt-metal` | `tenstorrent` | `provision-tt-metal.sh` | ttnn, TT-Metalium, sfpi, PyTorch |

The first installs the distro `linux-generic` kernel, because **tt-kmd will not
build against the base image's kernel**: that image ships a trimmed no-initrd
5.15.93 kernel built for gem5, and `modpost` fails on
`hwmon_device_register_with_info` and `dma_buf_export` — its config has neither
`CONFIG_HWMON` nor `CONFIG_DMA_SHARED_BUFFER`. The distro kernel enables both,
but is modular, so the experiment must pass an initrd (`QemuSim.initrd`).

The second is stacked with `-B`, which makes a qcow2 overlay instead of copying
the base image, so only the ~2.5 GiB delta is stored. The result then depends on
the image below it staying put; drop `-B` for a standalone image if you have the
disk. Note that ttnn's wheel deliberately ships *without* the sfpi RISC-V
toolchain (it is fetched at install time), so the provisioner fetches it into the
image — there is no network inside a SimBricks run, and tt-metal compiles its
kernels on first use.

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
- **Performance.** Idle throughput is ~11.4 MHz of simulated device clock on one
  core (50 M clock steps in 4.4 s under `--selftest`), i.e. roughly 88 s of host
  time per simulated second. That is `libttsim`'s own ceiling, not adapter
  overhead — the adapter measures the same rate end to end. Large DMAs are split
  into 4 KiB blocking round-trips, each costing at least one link latency.
- **Idle and busy device throughput differ by ~70x.** The adapter's progress
  lines show ~13 MHz while the Tensix cores are in reset and ~0.17 MHz once they
  are running code — a clock step that has to interpret RISC-V on 80 tiles costs
  far more than one that does not. Wall-clock time for the demo is dominated by
  this, not by the SimBricks link.
- **`--max-batch-clocks` trades device throughput against MMIO latency.** A
  request arriving mid-batch is not looked at until the batch ends, so the
  default 500 steps means an MMIO round trip can take ~45 µs; batching smaller
  costs little, since the per-iteration poll overhead is a few hundred ns against
  64 steps ≈ 5.6 µs of clocking. `tests/ttsim_matmul.py` sets 64 for that reason,
  but measurement says it is not what limits this workload: the whole 32×32 run
  issues only ~27 k MMIO accesses and ~440 DMA transfers. tt-metal moves kernel
  binaries and tensors through the host sysmem window and lets the *device* DMA
  them, rather than pushing them through TLB windows with host stores.
- The adapter prints a progress line every `--progress-secs` seconds (10 by
  default) with the current clock rate and MMIO/DMA rates, and a summary at
  exit. On runs this long, "stuck or just slow?" is otherwise unanswerable.
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
