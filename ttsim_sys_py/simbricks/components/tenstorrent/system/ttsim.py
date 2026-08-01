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

from simbricks.orchestration.system import base as sys_base
from simbricks.orchestration.system import pcie as sys_pcie
from simbricks.orchestration.system.host import app as sys_app
from simbricks.orchestration.system.host import base as sys_host
from simbricks.utils import base as utils_base

#: PCI identity reported by the chip model, used to bind a guest driver.
TT_PCI_VENDOR_ID = 0x1E52
TT_PCI_DEVICE_ID_WORMHOLE = 0x401E
TT_PCI_DEVICE_ID_BLACKHOLE = 0xB140

TT_DEVICE_IDS = {
    "wh": TT_PCI_DEVICE_ID_WORMHOLE,
    "bh": TT_PCI_DEVICE_ID_BLACKHOLE,
}


class TenstorrentAccel(sys_pcie.PCIeSimpleDevice):
    """A Tenstorrent accelerator as seen by the host: a plain PCIe endpoint.

    Deliberately not a `SimplePCIeNIC` -- the chip has no Ethernet interface
    exposed to SimBricks, so it carries only the single PCIe interface that
    `PCIeSimpleDevice` provides.
    """

    def __init__(self, s: sys_base.System, chip: str = "wh") -> None:
        super().__init__(s)
        if chip not in TT_DEVICE_IDS:
            raise ValueError(f"unknown chip '{chip}', expected one of {list(TT_DEVICE_IDS)}")
        self.chip: str = chip

    @property
    def pci_device_id(self) -> int:
        return TT_DEVICE_IDS[self.chip]

    def toJSON(self) -> dict:
        json_obj = super().toJSON()
        json_obj["chip"] = self.chip
        return json_obj

    @classmethod
    def fromJSON(cls, system: sys_base.System, json_obj: dict) -> tpe.Self:
        instance = super().fromJSON(system, json_obj)
        instance.chip = utils_base.get_json_attr_top(json_obj, "chip")
        return instance


class TenstorrentVfioHost(sys_host.LinuxHost):
    """Linux host that hands the accelerator to userspace through vfio-pci.

    The chip model exposes no interrupts and no capability list, so a userspace
    driver that mmaps the BARs is the shortest path to exercising it. IOMMU-less
    operation is enabled because the simulated platform has no IOMMU.
    """

    def __init__(self, sys: sys_base.System, chip: str = "wh") -> None:
        super().__init__(sys)
        self.chip: str = chip
        self.drivers.append("vfio-pci")

    def prepare_pre_cp(self, inst) -> list[str]:
        device_id = TT_DEVICE_IDS[self.chip]
        return super().prepare_pre_cp(inst) + [
            # The payload runs as init, so systemd never starts and nothing has
            # mounted these yet. lspci needs /proc/bus/pci; the vfio and sysfs
            # pokes below need /sys.
            "mount -t proc proc /proc || true",
            "mount -t sysfs sysfs /sys || true",
            "modprobe vfio || true",
            "modprobe vfio-pci || true",
            "echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode || true",
            f"echo {TT_PCI_VENDOR_ID:04x} {device_id:04x}"
            " > /sys/bus/pci/drivers/vfio-pci/new_id || true",
        ]

    def toJSON(self) -> dict:
        json_obj = super().toJSON()
        json_obj["chip"] = self.chip
        return json_obj

    @classmethod
    def fromJSON(cls, system: sys_base.System, json_obj: dict) -> tpe.Self:
        instance = super().fromJSON(system, json_obj)
        instance.chip = utils_base.get_json_attr_top(json_obj, "chip")
        return instance


class TenstorrentEnumerateApp(sys_app.BaseLinuxApplication):
    """Check that the guest sees the accelerator and which driver claims it.

    Uses only tools present in a stock image: proving enumeration and BAR sizing
    needs nothing beyond lspci, and it is the first thing that breaks if the
    device intro the adapter sends is wrong.

    Lives here rather than in an experiment script because instantiations are
    serialized and rebuilt in another process, which resolves classes by dotted
    module path -- a class defined in the script itself cannot be reconstructed.
    """

    def __init__(self, h: sys_host.LinuxHost, chip: str = "wh") -> None:
        super().__init__(h)
        self.chip: str = chip

    def toJSON(self) -> dict:
        json_obj = super().toJSON()
        json_obj["chip"] = self.chip
        return json_obj

    @classmethod
    def fromJSON(cls, system: sys_base.System, json_obj: dict) -> tpe.Self:
        instance = super().fromJSON(system, json_obj)
        instance.chip = utils_base.get_json_attr_top(json_obj, "chip")
        return instance

    def run_cmds(self, inst) -> list[str]:
        ids = f"{TT_PCI_VENDOR_ID:04x}:{TT_DEVICE_IDS[self.chip]:04x}"
        return [
            "echo '=== ttsim device ==='",
            f"lspci -nn -d {ids}",
            "echo '=== BARs ==='",
            f"lspci -vv -d {ids}",
            "echo '=== driver / resources ==='",
            f"SLOT=$(lspci -d {ids} -n | cut -d' ' -f1 | head -1);"
            ' echo "slot=$SLOT";'
            ' readlink -f /sys/bus/pci/devices/0000:$SLOT/driver'
            ' || echo "no driver bound";'
            ' head -6 /sys/bus/pci/devices/0000:$SLOT/resource',
            # Report a single unambiguous line the checker can grep for.
            f"if lspci -d {ids} | grep -q .; then"
            " echo 'TTSIM SMOKE: PASS device present';"
            " else echo 'TTSIM SMOKE: FAIL device missing'; fi",
        ]


class TenstorrentLinuxHost(sys_host.LinuxHost):
    """Linux host that binds the in-kernel `tenstorrent` driver (tt-kmd).

    The driver must already be present in the guest image; build one with
    image/install-tt-kmd.sh. Use TenstorrentVfioHost instead to drive the device
    from userspace without the kernel driver.
    """

    def __init__(self, sys: sys_base.System) -> None:
        super().__init__(sys)
        self.drivers.append("tenstorrent")

    def prepare_pre_cp(self, inst) -> list[str]:
        # The payload runs as init, so systemd never starts and nothing has
        # mounted these yet; the driver's sysfs/procfs surface needs them.
        return super().prepare_pre_cp(inst) + [
            "mount -t proc proc /proc || true",
            "mount -t sysfs sysfs /sys || true",
        ]


class TenstorrentKmdApp(sys_app.BaseLinuxApplication):
    """Check that tt-kmd binds the simulated device and exposes a chardev.

    This is the functional gate for the Tenstorrent stack: the real driver
    probing the model end to end exercises far more of the device than
    enumeration does -- BAR mapping, the ARC handshake and telemetry reads all
    happen during probe.
    """

    def run_cmds(self, inst) -> list[str]:
        ids = f"{TT_PCI_VENDOR_ID:04x}:{TT_DEVICE_IDS['wh']:04x}"
        return [
            "echo '=== load tt-kmd ==='",
            "modprobe tenstorrent || insmod"
            " /lib/modules/$(uname -r)/kernel/drivers/misc/tenstorrent.ko || true",
            "echo '=== driver binding ==='",
            f"lspci -k -d {ids}",
            "echo '=== chardev ==='",
            "ls -l /dev/tenstorrent/ 2>&1 || echo 'no /dev/tenstorrent'",
            "echo '=== kernel log ==='",
            "dmesg | grep -i -E 'tenstorrent|tt-kmd' || echo 'no driver messages'",
            "echo '=== sysfs ==='",
            "for d in /sys/bus/pci/drivers/tenstorrent/0000:*; do"
            ' [ -e "$d" ] && echo "bound: $d"; done',
            # Single unambiguous verdict line.
            "if [ -e /dev/tenstorrent/0 ]; then"
            " echo 'TTSIM KMD: PASS /dev/tenstorrent/0 present';"
            " else echo 'TTSIM KMD: FAIL no chardev'; fi",
        ]
