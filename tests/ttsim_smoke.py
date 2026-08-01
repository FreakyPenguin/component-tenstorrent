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

"""Boot a Linux guest with a simulated Tenstorrent accelerator attached.

The guest enumerates the device and hands it to vfio-pci. This is the
orchestration-level counterpart to tests/run_probe_test.sh, which exercises the
adapter's MMIO and DMA paths directly without a guest.

Run with:

    simbricks-run --verbose --force \\
        --global-input-dir <dir-with-images/base/base.raw> \\
        tests/ttsim_smoke.py

Requires a base disk image; `simbricks-run` looks for it under
`<global-input-dir>/images/<name>/<name>.raw`. Set TTSIM_LIB to point at the
libttsim.so to load (defaults to a release_wh build in a sibling ttsim checkout).
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
            os.path.dirname(__file__), "..", "..", "ttsim", "src", "_out", "release_wh", "libttsim.so"
        )
    ),
)
BASE_IMAGE = os.environ.get("TTSIM_BASE_IMAGE", "base")

VENDOR = f"{tt_sys.TT_PCI_VENDOR_ID:04x}"
DEVICE = f"{tt_sys.TT_PCI_DEVICE_ID_WORMHOLE:04x}"


def make_instantiation():
    system = sys_base.System()

    host = tt_sys.TenstorrentVfioHost(system, chip="wh")
    host.name = "host"
    host.memory = 2048
    host.cores = 1
    host.cpu_freq = "3GHz"
    host.add_disk(disk_images.DistroDiskImage(system, name=BASE_IMAGE))
    host.add_disk(disk_images.LinuxConfigDiskImage(system, host))
    host.add_app(tt_sys.TenstorrentEnumerateApp(host, chip="wh"))

    accel = tt_sys.TenstorrentAccel(system, chip="wh")
    accel.name = "ttsim"

    # NOTE: do not use helpers.system.connect_host_and_device() here. It builds a
    # second PCIeDeviceInterface and calls device.add_if(), which
    # PCIeSimpleDevice deliberately rejects to protect its pre-made _pci_if.
    host_if = sys_pcie.PCIeHostInterface(host)
    host.add_if(host_if)
    sys_pcie.PCIeChannel(host=host_if, dev=accel._pci_if)

    simulation = sim_base.Simulation(name="ttsim-smoke", system=system)

    host_sim = qemu_sim.QemuSim(simulation)
    host_sim.name = "host"
    host_sim.add(host)
    # NOTE: the property is `wait_terminate`, not `wait`. Assigning `wait`
    # silently creates an unused attribute, leaving the runner with nothing to
    # wait on -- it then falls straight through to cleanup and SIGINTs QEMU
    # mid-boot, reporting success.
    host_sim.wait_terminate = True

    dev_sim = tt_bm.TTSimDevSim(simulation, lib_path=TTSIM_LIB)
    dev_sim.name = "ttsim"
    dev_sim.add(accel)

    # Synchronization is OFF by default. Unsynchronized is what you want while
    # bringing the software stack up: it is the only mode where QEMU can use KVM
    # (it falls back to icount+TCG when synchronized), which is the difference
    # between a boot that takes seconds and one that takes many minutes.
    # Timing is meaningless in this mode -- turn it on only once the stack works.
    if os.environ.get("TTSIM_SYNC", "0") == "1":
        sim_helpers.enable_sync_simulation(
            simulation, amount=500, ratio=utils_base.Time.Nanoseconds
        )
    else:
        sim_helpers.disalbe_sync_simulation(simulation)

    return inst_helpers.simple_instantiation(simulation)


instantiations = [make_instantiation()]


if __name__ == "__main__":
    # Dry run: print the commands simbricks-run would execute. Useful for
    # checking the plumbing without a disk image.
    import pathlib
    from simbricks.orchestration.instantiation import base as inst_base

    inst = instantiations[0]
    inst.env = inst_base.InstantiationEnvironment(
        workdir=pathlib.Path("/tmp/ttsim-smoke-dryrun").resolve(), global_input_dir=None
    )
    inst.assigned_fragment = inst.fragments[0]

    # Normally done by the runner. Also checks the two sides agree on who
    # listens: the device must listen and QEMU must connect.
    inst._assign_interface_socktype()
    for interface, socktype in inst._inf_socktype_assignment.items():
        sim = inst.find_sim_by_interface(interface)
        print(f"socket: {sim.full_name()} {type(interface).__name__} -> {socktype.name}")

    for simulator in inst.simulation.all_simulators():
        print(f"--- {simulator.full_name()} ---")
        try:
            print(simulator.run_cmd(inst))
        except Exception as e:
            # QEMU resolves its disk images in an async prepare() step, so its
            # command cannot be built without the actual image files. The device
            # simulator has no such dependency and is what this dry run checks.
            print(f"(unavailable without a prepared instantiation: {e})")
