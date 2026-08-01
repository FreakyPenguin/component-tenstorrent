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

"""Run a PyTorch matrix multiply on the simulated Tenstorrent accelerator.

The full stack runs in the guest -- ttnn, TT-Metalium (which JIT-compiles the
Tensix kernels), UMD, and tt-kmd -- while the chip itself is libttsim behind the
SimBricks PCIe adapter.

Needs the tt-metal guest image, built once with:

    ./image/build-tt-image.sh -b /global_input/images/tenstorrent/tenstorrent \\
        -n tt-metal -s provision-tt-metal.sh -B

which in turn needs the tt-kmd image that image/build-tt-image.sh builds from
the base image.

Run with:

    rm -rf out   # --force re-uses the run id but does not clear the directory
    simbricks-run --verbose --force --global-input-dir /global_input \\
        tests/ttsim_matmul.py

Synchronization is off by default; see the note in ttsim_smoke.py. This is a
long run either way -- tt-metal compiles kernels and initializes every Tensix
core before the first operation dispatches.
"""

from __future__ import annotations

import os

from simbricks.components.qemu.simulation import qemu as qemu_sim
from simbricks.components.tenstorrent.simulation.behavioral import ttsim as tt_bm
from simbricks.components.tenstorrent.system import ttsim as tt_sys
from simbricks.orchestration.helpers import instantiation as inst_helpers
from simbricks.orchestration.helpers import simulation as sim_helpers
from simbricks.orchestration.simulation import base as sim_base
from simbricks.orchestration.system import base as sys_base
from simbricks.orchestration.system import disk_images
from simbricks.orchestration.system import pcie as sys_pcie
from simbricks.utils import base as utils_base

TTSIM_LIB = os.environ.get(
    "TTSIM_LIB",
    os.path.abspath(
        os.path.join(
            os.path.dirname(__file__),
            "..",
            "..",
            "ttsim",
            "src",
            "_out",
            "release_wh",
            "libttsim.so",
        )
    ),
)
IMAGE = os.environ.get("TTSIM_IMAGE", "tt-metal")
GLOBAL_INPUT = os.environ.get("TTSIM_GLOBAL_INPUT", "/global_input")
INITRD = os.environ.get(
    "TTSIM_INITRD", os.path.join(GLOBAL_INPUT, "images", IMAGE, "boot", "initrd")
)

#: Square matrix dimension. 32 is one Tensix tile, the smallest shape ttnn can
#: place on the device without padding.
SIZE = int(os.environ.get("TTSIM_MATMUL_SIZE", "32"))
#: Guest RAM in MiB, on top of which the 1 GiB hugepages are reserved.
MEMORY = int(os.environ.get("TTSIM_MEMORY", "6144"))
#: Clock steps the device runs between SimBricks polls. tt-metal drives far more
#: MMIO than the earlier tests, and a request arriving mid-batch waits for the
#: batch to finish, so this trades a little device throughput for latency.
MAX_BATCH_CLOCKS = int(os.environ.get("TTSIM_MAX_BATCH_CLOCKS", "64"))


def make_instantiation():
    system = sys_base.System()

    host = tt_sys.TenstorrentMetalHost(system)
    host.name = "host"
    host.memory = MEMORY
    host.cores = 4
    host.cpu_freq = "3GHz"
    host.add_disk(disk_images.DistroDiskImage(system, name=IMAGE))
    host.add_disk(disk_images.LinuxConfigDiskImage(system, host))
    host.add_app(tt_sys.TenstorrentMatmulApp(host, size=SIZE))

    accel = tt_sys.TenstorrentAccel(system, chip="wh")
    accel.name = "ttsim"

    host_if = sys_pcie.PCIeHostInterface(host)
    host.add_if(host_if)
    sys_pcie.PCIeChannel(host=host_if, dev=accel._pci_if)

    simulation = sim_base.Simulation(name="ttsim-matmul", system=system)

    host_sim = qemu_sim.QemuSim(simulation)
    host_sim.name = "host"
    host_sim.add(host)
    # NOTE: the property is `wait_terminate`, not `wait`. Assigning `wait`
    # silently creates an unused attribute, leaving the runner with nothing to
    # wait on -- it then falls straight through to cleanup and SIGINTs QEMU
    # mid-boot, reporting success.
    host_sim.wait_terminate = True
    # Only needed for a modular kernel; the SimBricks custom kernel builds
    # everything in. See the note in ttsim_kmd.py.
    if os.path.exists(INITRD):
        host_sim.initrd = INITRD

    dev_sim = tt_bm.TTSimDevSim(simulation, lib_path=TTSIM_LIB)
    dev_sim.name = "ttsim"
    dev_sim.max_batch_clocks = MAX_BATCH_CLOCKS
    dev_sim.add(accel)

    if os.environ.get("TTSIM_SYNC", "0") == "1":
        sim_helpers.enable_sync_simulation(
            simulation, amount=500, ratio=utils_base.Time.Nanoseconds
        )
    else:
        sim_helpers.disalbe_sync_simulation(simulation)

    return inst_helpers.simple_instantiation(simulation)


instantiations = [make_instantiation()]
