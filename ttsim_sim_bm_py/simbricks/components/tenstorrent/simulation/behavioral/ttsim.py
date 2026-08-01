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

from __future__ import annotations

import typing_extensions as tpe

from simbricks.components.tenstorrent.system import ttsim as tt_sys
from simbricks.orchestration.instantiation import base as inst_base
from simbricks.orchestration.instantiation import socket as inst_socket
from simbricks.orchestration.simulation import base as sim_base
from simbricks.orchestration.simulation import pcidev
from simbricks.orchestration.system import pcie as sys_pcie
from simbricks.utils import base as utils_base


class TTSimDevSim(pcidev.PCIDevSim):
    """Runs Tenstorrent's ttsim as a SimBricks PCIe device simulator.

    The adapter binary (`simb_ttsim_bm`) dlopens `libttsim.so` at runtime, so a
    single binary serves every chip build; `lib_path` selects which one.

    Unlike `NICSim` there is no inherited `run_cmd` to extend -- `NICSim` emits
    both a PCIe and an Ethernet parameters url, and this device has no Ethernet
    interface -- so the command is built from scratch below.
    """

    def __init__(
        self,
        simulation: sim_base.Simulation,
        lib_path: str,
        name: str = "",
    ) -> None:
        super().__init__(simulation=simulation, executable="simb_ttsim_bm", name=name)
        #: Path to libttsim.so, e.g. ttsim/src/_out/release_wh/libttsim.so.
        self.lib_path: str = lib_path
        #: Device clock. 1 step of libttsim_clock is one tick of this clock; the
        #: chip reports a 1000 MHz AICLK, so the default makes 1 step == 1 ns.
        self.clock_freq_mhz: int = 1000
        #: Clock steps executed per batch between SimBricks polls. Larger is
        #: faster but coarsens how promptly MMIO is serviced.
        self.max_batch_clocks: int = 500
        #: Override BAR4's size in bytes; defaults to the chip's own value.
        self.bar4_size: int | None = None
        self.name = f"TTSimDevSim-{self._id}"

    def resreq_cores(self) -> int:
        return 1

    def resreq_mem(self) -> int:
        # libttsim reserves its DRAM up front with MAP_PRIVATE (~12 GB of
        # address space for Wormhole), but touches it lazily, so the resident
        # set stays far below that.
        return 4096

    def toJSON(self) -> dict:
        json_obj = super().toJSON()
        json_obj["lib_path"] = self.lib_path
        json_obj["clock_freq_mhz"] = self.clock_freq_mhz
        json_obj["max_batch_clocks"] = self.max_batch_clocks
        json_obj["bar4_size"] = self.bar4_size
        return json_obj

    @classmethod
    def fromJSON(cls, simulation: sim_base.Simulation, json_obj: dict) -> tpe.Self:
        instance = super().fromJSON(simulation, json_obj)
        instance.lib_path = utils_base.get_json_attr_top(json_obj, "lib_path")
        instance.clock_freq_mhz = utils_base.get_json_attr_top(json_obj, "clock_freq_mhz")
        instance.max_batch_clocks = utils_base.get_json_attr_top(json_obj, "max_batch_clocks")
        instance.bar4_size = utils_base.get_json_attr_top(json_obj, "bar4_size")
        return instance

    def add(self, accel: tt_sys.TenstorrentAccel) -> None:
        utils_base.has_expected_type(accel, tt_sys.TenstorrentAccel)
        if len(self._components) >= 1:
            raise RuntimeError("TTSimDevSim simulates exactly one accelerator")
        super().add(accel)

    def run_cmd(self, inst: inst_base.Instantiation) -> str:
        accels = self.filter_components_by_type(ty=tt_sys.TenstorrentAccel)
        assert len(accels) == 1
        accel = accels[0]

        pci_channels = sim_base.Simulator.filter_channels_by_sys_type(
            self.get_channels(), sys_pcie.PCIeChannel
        )
        latency, sync_period, run_sync = sim_base.Simulator.get_unique_latency_period_sync(
            pci_channels
        )

        socket = inst.get_socket(interface=accel._pci_if)
        assert socket is not None and socket._type == inst_socket.SockType.LISTEN
        params_url = self.get_parameters_url(
            inst, socket, sync=run_sync, latency=latency, sync_period=sync_period
        )

        cmd = f"{self._executable} {params_url} {self._start_tick}"
        cmd += f" --lib {self.lib_path}"
        cmd += f" --clock-freq-mhz {self.clock_freq_mhz}"
        cmd += f" --max-batch-clocks {self.max_batch_clocks}"
        if self.bar4_size is not None:
            cmd += f" --bar4-size {self.bar4_size}"
        if self.extra_args is not None:
            cmd += " " + self.extra_args

        return cmd
