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

"""Boot a Linux guest that binds the simulated accelerator with tt-kmd.

This is the functional gate for the Tenstorrent software stack: the real kernel
driver probing the model exercises far more of the device than enumeration does.

Needs the tt-kmd guest image, built once with:

    ./image/build-tt-image.sh

That image runs the distro kernel (the base image's trimmed 5.15.93 lacks
CONFIG_HWMON and CONFIG_DMA_SHARED_BUFFER, which tt-kmd needs), and the distro
kernel is modular, hence the explicit initrd below.

Run with:

    simbricks-run --verbose --force --global-input-dir /global_input \\
        tests/ttsim_kmd.py

Synchronization is off by default; see the note in ttsim_smoke.py.
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
IMAGE = os.environ.get("TTSIM_IMAGE", "tenstorrent")
GLOBAL_INPUT = os.environ.get("TTSIM_GLOBAL_INPUT", "/global_input")
INITRD = os.environ.get(
    "TTSIM_INITRD", os.path.join(GLOBAL_INPUT, "images", IMAGE, "boot", "initrd")
)


def make_instantiation():
    system = sys_base.System()

    host = tt_sys.TenstorrentLinuxHost(system)
    host.name = "host"
    host.memory = 4096
    host.cores = 2
    host.cpu_freq = "3GHz"
    host.add_disk(disk_images.DistroDiskImage(system, name=IMAGE))
    host.add_disk(disk_images.LinuxConfigDiskImage(system, host))
    host.add_app(tt_sys.TenstorrentKmdApp(host))

    accel = tt_sys.TenstorrentAccel(system, chip="wh")
    accel.name = "ttsim"

    host_if = sys_pcie.PCIeHostInterface(host)
    host.add_if(host_if)
    sys_pcie.PCIeChannel(host=host_if, dev=accel._pci_if)

    simulation = sim_base.Simulation(name="ttsim-kmd", system=system)

    host_sim = qemu_sim.QemuSim(simulation)
    host_sim.name = "host"
    host_sim.add(host)
    # NOTE: the property is `wait_terminate`, not `wait`. Assigning `wait`
    # silently creates an unused attribute, leaving the runner with nothing to
    # wait on -- it then falls straight through to cleanup and SIGINTs QEMU
    # mid-boot, reporting success.
    host_sim.wait_terminate = True
    # Only the distro kernel needs one -- it is modular and cannot mount root
    # without it. The SimBricks custom kernel (image/build-tt-kernel.sh) builds
    # everything in, and build-tt-image.sh then extracts no initrd at all. That
    # is not just tidiness: it is a large part of why it reaches the payload in
    # ~1 s of guest time instead of ~12, which is what a synchronized run pays
    # for.
    if os.path.exists(INITRD):
        host_sim.initrd = INITRD

    dev_sim = tt_bm.TTSimDevSim(simulation, lib_path=TTSIM_LIB)
    dev_sim.name = "ttsim"
    dev_sim.add(accel)

    if os.environ.get("TTSIM_SYNC", "0") == "1":
        sim_helpers.enable_sync_simulation(
            simulation, amount=500, ratio=utils_base.Time.Nanoseconds
        )
    else:
        sim_helpers.disalbe_sync_simulation(simulation)

    return inst_helpers.simple_instantiation(simulation)


instantiations = [make_instantiation()]
