/*
 * Copyright 2026 SimBricks
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Minimal SimBricks PCIe *host* that drives simb_ttsim_bm directly.
 *
 * This stands in for QEMU so the adapter can be tested without a guest image:
 * it connects to the adapter's socket, issues MMIO into the BARs, and serves
 * the device's DMA requests out of a local buffer that plays the role of guest
 * physical memory.
 *
 * It covers the two paths that carry all the risk:
 *   - MMIO round-trips through libttsim (BAR0 TLB config, then a write and
 *     read-back of a Tensix tile's L1 through the configured window);
 *   - a DMA transfer kicked off by an MMIO write to the BAR2 doorbell, which is
 *     precisely the re-entrant case the adapter's nested pump exists to handle
 *     (libttsim calls its synchronous DMA callback from inside the MMIO write).
 *
 * Wormhole specific: register offsets and NoC coordinates come from
 * ttsim/src/libttsim.cpp and ttsim/src/tile.cpp.
 */

#include <unistd.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <simbricks/base/cxxatomicfix.h>

extern "C" {
#include <simbricks/parser/parser.h>
#include <simbricks/pcie/if.h>
}

/******************************************************************************/
/* Wormhole register map (see ttsim/src/libttsim.cpp) */

/* BAR0: TLB config array. Entry i is a 64-bit word at +8*i. */
#define WH_TLB_CFG_BASE 0x1FC00000u

/* BAR2: DMA engine. Writing a doorbell runs the engine inline. */
#define WH_DMA_READ_ENGINE_EN 0x02Cu
#define WH_DMA_READ_DOORBELL 0x030u
#define WH_DMA_READ_DONE_IMWR_LOW 0x0CCu
#define WH_DMA_READ_DONE_IMWR_HIGH 0x0D0u
#define WH_DMA_READ_IMWR_DATA 0x0DCu
#define WH_DMA_READ_TRANSFER_SIZE 0x308u
#define WH_DMA_READ_SAR_LOW 0x30Cu /* host source */
#define WH_DMA_READ_SAR_HIGH 0x310u
#define WH_DMA_READ_DAR_LOW 0x314u /* device destination, a BAR0 window offset */

/* Tensix tile 0 sits at NoC coordinate (x=1, y=1); coords pack as x | (y<<6). */
#define WH_TENSIX0_COORD (1u | (1u << 6))

/* TLB window 0 is the first 1 MiB window of BAR0. For a 1 MiB window the config
 * word holds 16 address bits, then the target coordinate. */
#define WH_TLB0_WINDOW_BITS 20
#define WH_TLB0_ADDR_BITS (36 - WH_TLB0_WINDOW_BITS)

/******************************************************************************/

namespace {

struct SimbricksPcieIf pcie_if;
struct SimbricksBaseIfParams params;
uint64_t cur_ts = 0;
bool is_sync = false;
bool peer_gone = false;
uint64_t step_ps = 1000;

/* Stands in for guest physical memory. Only the window below is addressable;
 * a DMA outside it is a test bug and is reported rather than silently served. */
constexpr uint64_t kHostMemBase = 0x40000000ull;
constexpr size_t kHostMemSize = 1u << 20;
uint8_t host_mem[kHostMemSize];

struct MmioPending {
  bool done;
  void *dst;
  uint16_t len;
};

int failures = 0;

void Fail(const char *what) {
  fprintf(stderr, "FAIL: %s\n", what);
  failures++;
}

bool HostMemRange(uint64_t addr, uint32_t len, uint8_t **out) {
  if (addr < kHostMemBase || addr - kHostMemBase > kHostMemSize ||
      len > kHostMemSize - (addr - kHostMemBase)) {
    return false;
  }
  *out = host_mem + (addr - kHostMemBase);
  return true;
}

volatile union SimbricksProtoPcieH2D *AllocOut() {
  volatile union SimbricksProtoPcieH2D *msg;
  while ((msg = SimbricksPcieIfH2DOutAlloc(&pcie_if, cur_ts)) == nullptr) {
    if (SimbricksBaseIfInTerminated(&pcie_if.base)) {
      fprintf(stderr, "probe: peer terminated while waiting for a queue slot\n");
      exit(1);
    }
  }
  return msg;
}

/** Handle one incoming message. Device-initiated reads/writes are DMA and are
 * served from host_mem; readcomp/writecomp complete an MMIO we issued. */
bool Poll() {
  volatile union SimbricksProtoPcieD2H *msg =
      SimbricksPcieIfD2HInPoll(&pcie_if, cur_ts);
  if (msg == nullptr) {
    return false;
  }

  uint8_t type = SimbricksPcieIfD2HInType(&pcie_if, msg);
  switch (type) {
    case SIMBRICKS_PROTO_PCIE_D2H_MSG_READ: {
      /* Device DMA read: reply with the requested bytes. */
      uint64_t req_id = msg->read.req_id;
      uint64_t addr = msg->read.offset;
      uint16_t len = msg->read.len;
      uint8_t *src = nullptr;
      bool ok = HostMemRange(addr, len, &src);
      if (!ok) {
        fprintf(stderr, "probe: DMA read outside host memory: 0x%" PRIx64 "\n",
                addr);
      }
      SimbricksPcieIfD2HInDone(&pcie_if, msg);

      volatile union SimbricksProtoPcieH2D *omsg = AllocOut();
      omsg->readcomp.req_id = req_id;
      if (ok) {
        memcpy((void *)omsg->readcomp.data, src, len);
      } else {
        memset((void *)omsg->readcomp.data, 0xFF, len);
      }
      SimbricksPcieIfH2DOutSend(&pcie_if, omsg,
                                SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP);
      return true;
    }

    case SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITE: {
      /* Device DMA write: absorb the bytes, then acknowledge. */
      uint64_t req_id = msg->write.req_id;
      uint64_t addr = msg->write.offset;
      uint16_t len = msg->write.len;
      uint8_t *dst = nullptr;
      if (HostMemRange(addr, len, &dst)) {
        memcpy(dst, (const void *)msg->write.data, len);
      } else {
        fprintf(stderr, "probe: DMA write outside host memory: 0x%" PRIx64 "\n",
                addr);
      }
      SimbricksPcieIfD2HInDone(&pcie_if, msg);

      volatile union SimbricksProtoPcieH2D *omsg = AllocOut();
      omsg->writecomp.req_id = req_id;
      SimbricksPcieIfH2DOutSend(&pcie_if, omsg,
                                SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITECOMP);
      return true;
    }

    case SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP: {
      struct MmioPending *p =
          reinterpret_cast<struct MmioPending *>(msg->readcomp.req_id);
      memcpy(p->dst, (const void *)msg->readcomp.data, p->len);
      p->done = true;
      break;
    }

    case SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITECOMP: {
      struct MmioPending *p =
          reinterpret_cast<struct MmioPending *>(msg->writecomp.req_id);
      p->done = true;
      break;
    }

    case SIMBRICKS_PROTO_PCIE_D2H_MSG_INTERRUPT:
      fprintf(stderr, "probe: unexpected interrupt (device declares none)\n");
      failures++;
      break;

    case SIMBRICKS_PROTO_MSG_TYPE_SYNC:
      break;

    case SIMBRICKS_PROTO_MSG_TYPE_TERMINATE:
      peer_gone = true;
      break;

    default:
      fprintf(stderr, "probe: unhandled D2H type 0x%x\n", type);
      break;
  }

  SimbricksPcieIfD2HInDone(&pcie_if, msg);
  return true;
}

/** Advance time and pump messages until `p` completes. */
void PumpUntil(struct MmioPending *p) {
  while (!p->done) {
    while (SimbricksPcieIfH2DOutSync(&pcie_if, cur_ts)) {
    }
    while (Poll()) {
    }
    if (p->done) {
      return;
    }
    if (peer_gone || SimbricksBaseIfInTerminated(&pcie_if.base)) {
      fprintf(stderr, "probe: peer terminated with an MMIO outstanding\n");
      exit(1);
    }

    uint64_t next_ts;
    if (is_sync) {
      uint64_t next_in = SimbricksPcieIfD2HInTimestamp(&pcie_if);
      uint64_t next_sync = SimbricksPcieIfH2DOutNextSync(&pcie_if);
      next_ts = next_in <= next_sync ? next_in : next_sync;
      uint64_t cap = cur_ts + 1000 * step_ps;
      if (next_ts > cap) {
        next_ts = cap;
      }
    } else {
      next_ts = cur_ts + step_ps;
    }
    if (next_ts > cur_ts) {
      cur_ts = next_ts;
    }
  }
}

void MmioWrite(uint8_t bar, uint64_t offset, const void *src, uint16_t len) {
  struct MmioPending p = {false, nullptr, len};
  volatile union SimbricksProtoPcieH2D *msg = AllocOut();
  msg->write.req_id = reinterpret_cast<uintptr_t>(&p);
  msg->write.offset = offset;
  msg->write.len = len;
  msg->write.bar = bar;
  memcpy((void *)msg->write.data, src, len);
  SimbricksPcieIfH2DOutSend(&pcie_if, msg,
                            SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE);
  PumpUntil(&p);
}

void MmioRead(uint8_t bar, uint64_t offset, void *dst, uint16_t len) {
  struct MmioPending p = {false, dst, len};
  volatile union SimbricksProtoPcieH2D *msg = AllocOut();
  msg->read.req_id = reinterpret_cast<uintptr_t>(&p);
  msg->read.offset = offset;
  msg->read.len = len;
  msg->read.bar = bar;
  SimbricksPcieIfH2DOutSend(&pcie_if, msg, SIMBRICKS_PROTO_PCIE_H2D_MSG_READ);
  PumpUntil(&p);
}

void MmioWrite32(uint8_t bar, uint64_t offset, uint32_t v) {
  MmioWrite(bar, offset, &v, 4);
}

uint32_t MmioRead32(uint8_t bar, uint64_t offset) {
  uint32_t v = 0;
  MmioRead(bar, offset, &v, 4);
  return v;
}

/******************************************************************************/

int Connect(const char *url) {
  struct SimbricksAdapterParams *p = SimbricksParametersParse(url);
  if (p == nullptr) {
    fprintf(stderr, "probe: cannot parse url '%s'\n", url);
    return -1;
  }
  if (p->listen) {
    fprintf(stderr, "probe: expected a connect url\n");
    return -1;
  }

  SimbricksPcieIfDefaultParams(&params);
  params.sock_path = p->socket_path;
  params.sync_mode =
      p->sync ? kSimbricksBaseIfSyncRequired : kSimbricksBaseIfSyncDisabled;
  /* Nanoseconds on the wire, picoseconds in the params struct -- same
   * conversion the adapter does. See the note in ttsim_bm.cc. */
  if (p->link_latency_set) {
    params.link_latency = p->link_latency * 1000;
  }
  if (p->sync_interval_set) {
    params.sync_interval = p->sync_interval * 1000;
  }
  params.blocking_conn = true;

  if (SimbricksBaseIfInit(&pcie_if.base, &params) != 0) {
    perror("probe: SimbricksBaseIfInit");
    return -1;
  }

  /* The device creates the socket, so it may not exist yet. */
  int rc = -1;
  for (int i = 0; i < 200; i++) {
    rc = SimbricksBaseIfConnect(&pcie_if.base);
    if (rc == 0) {
      break;
    }
    usleep(50000);
  }
  if (rc != 0) {
    perror("probe: SimbricksBaseIfConnect");
    return -1;
  }

  struct SimbricksProtoPcieHostIntro host_intro;
  memset(&host_intro, 0, sizeof(host_intro));
  struct SimbricksProtoPcieDevIntro dev_intro;
  memset(&dev_intro, 0, sizeof(dev_intro));

  struct SimBricksBaseIfEstablishData estd;
  memset(&estd, 0, sizeof(estd));
  estd.base_if = &pcie_if.base;
  estd.tx_intro = &host_intro;
  estd.tx_intro_len = sizeof(host_intro);
  estd.rx_intro = &dev_intro;
  estd.rx_intro_len = sizeof(dev_intro);

  if (SimBricksBaseIfEstablish(&estd, 1) != 0) {
    fprintf(stderr, "probe: SimBricksBaseIfEstablish failed\n");
    return -1;
  }

  is_sync = SimbricksBaseIfSyncEnabled(&pcie_if.base);

  printf("probe: connected, sync=%s\n", is_sync ? "on" : "off");
  printf("probe: device %04x:%04x class %02x:%02x rev %02x\n",
         dev_intro.pci_vendor_id, dev_intro.pci_device_id, dev_intro.pci_class,
         dev_intro.pci_subclass, dev_intro.pci_revision);
  for (int i = 0; i < SIMBRICKS_PROTO_PCIE_NBARS; i++) {
    if (dev_intro.bars[i].len != 0) {
      printf("probe:   BAR%d len=%" PRIu64 " (%" PRIu64 " MiB) flags=0x%" PRIx64
             "\n",
             i, dev_intro.bars[i].len, dev_intro.bars[i].len >> 20,
             dev_intro.bars[i].flags);
    }
  }

  /* The chip advertises no interrupts, so this only exercises the path. */
  volatile union SimbricksProtoPcieH2D *msg = AllocOut();
  msg->devctrl.flags = 0;
  SimbricksPcieIfH2DOutSend(&pcie_if, msg,
                            SIMBRICKS_PROTO_PCIE_H2D_MSG_DEVCTRL);

  /* Check the intro against what Wormhole should report. */
  if (dev_intro.pci_vendor_id != 0x1E52) {
    Fail("vendor id");
  }
  if (dev_intro.pci_device_id != 0x401E) {
    Fail("device id (expected wormhole 0x401e)");
  }
  if (dev_intro.pci_class != 0x12) {
    Fail("class code (expected 0x12 processing accelerator)");
  }
  if (dev_intro.bars[0].len != (512ull << 20)) {
    Fail("BAR0 size");
  }
  if (dev_intro.bars[2].len != (1ull << 20)) {
    Fail("BAR2 size");
  }
  if (dev_intro.bars[4].len != (32ull << 20)) {
    Fail("BAR4 size");
  }
  if (dev_intro.pci_msi_nvecs != 0 || dev_intro.pci_msix_nvecs != 0) {
    Fail("device should advertise no MSI/MSI-X");
  }

  SimbricksParametersFree(p);
  return 0;
}

/** Point TLB window 0 at Tensix tile 0's L1, so BAR0[0 .. 1 MiB) reaches it. */
void SetupTlb0() {
  uint64_t cfg = static_cast<uint64_t>(WH_TENSIX0_COORD) << WH_TLB0_ADDR_BITS;
  MmioWrite(0, WH_TLB_CFG_BASE, &cfg, 8);
  printf("probe: TLB window 0 -> tensix (1,1), cfg=0x%" PRIx64 "\n", cfg);
}

/** MMIO write then read-back through the TLB window into tile L1. */
void TestMmioL1() {
  const uint64_t off = 0x1000;
  uint32_t pattern[4] = {0xDEADBEEF, 0x01234567, 0x89ABCDEF, 0xFEEDFACE};

  for (int i = 0; i < 4; i++) {
    MmioWrite32(0, off + 4 * i, pattern[i]);
  }
  for (int i = 0; i < 4; i++) {
    uint32_t got = MmioRead32(0, off + 4 * i);
    if (got != pattern[i]) {
      fprintf(stderr, "  L1[%d]: got 0x%08x want 0x%08x\n", i, got, pattern[i]);
      Fail("MMIO L1 read-back mismatch");
      return;
    }
  }
  printf("probe: MMIO write/read-back through tile L1 ok\n");
}

/** Drive a host->device DMA through the BAR2 read engine.
 *
 * The doorbell write is the interesting part: libttsim runs the DMA engine
 * inline, so the adapter takes a synchronous DMA callback while it is still
 * inside our MMIO write and must service it without deadlocking. */
void TestDma() {
  const uint32_t len = 256;
  const uint64_t src_pa = kHostMemBase + 0x2000;
  const uint64_t imwr_pa = kHostMemBase + 0x8000;
  const uint64_t dev_off = 0x4000; /* BAR0 window offset -> tile L1 0x4000 */
  const uint32_t imwr_magic = 0xC0FFEE01;

  uint8_t *src = host_mem + (src_pa - kHostMemBase);
  for (uint32_t i = 0; i < len; i++) {
    src[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  memset(host_mem + (imwr_pa - kHostMemBase), 0, 4);

  MmioWrite32(2, WH_DMA_READ_ENGINE_EN, 1);
  MmioWrite32(2, WH_DMA_READ_SAR_LOW, static_cast<uint32_t>(src_pa));
  MmioWrite32(2, WH_DMA_READ_SAR_HIGH, static_cast<uint32_t>(src_pa >> 32));
  MmioWrite32(2, WH_DMA_READ_DAR_LOW, static_cast<uint32_t>(dev_off));
  MmioWrite32(2, WH_DMA_READ_TRANSFER_SIZE, len);
  MmioWrite32(2, WH_DMA_READ_DONE_IMWR_LOW, static_cast<uint32_t>(imwr_pa));
  MmioWrite32(2, WH_DMA_READ_DONE_IMWR_HIGH,
              static_cast<uint32_t>(imwr_pa >> 32));
  MmioWrite32(2, WH_DMA_READ_IMWR_DATA, imwr_magic);

  /* Runs the engine inline inside libttsim. */
  MmioWrite32(2, WH_DMA_READ_DOORBELL, 0);

  uint32_t done = 0;
  memcpy(&done, host_mem + (imwr_pa - kHostMemBase), 4);
  if (done != imwr_magic) {
    fprintf(stderr, "  completion word: got 0x%08x want 0x%08x\n", done,
            imwr_magic);
    Fail("DMA completion (imwr) not written back to host memory");
  } else {
    printf("probe: DMA completion word written back to host memory\n");
  }

  /* The payload should now be in tile L1; read it back through BAR0. */
  for (uint32_t i = 0; i < len; i += 4) {
    uint32_t got = MmioRead32(0, dev_off + i);
    uint32_t want;
    memcpy(&want, src + i, 4);
    if (got != want) {
      fprintf(stderr, "  L1+0x%x: got 0x%08x want 0x%08x\n", i, got, want);
      Fail("DMA payload mismatch in tile L1");
      return;
    }
  }
  printf("probe: DMA payload (%u B) verified in tile L1\n", len);
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr,
            "Usage: %s CONNECT-URL\n"
            "  e.g. connect:/tmp/tt.sock:sync=true:latency=500:"
            "sync_interval=500\n",
            argv[0]);
    return 1;
  }

  if (Connect(argv[1]) != 0) {
    return 1;
  }

  SetupTlb0();
  TestMmioL1();
  TestDma();

  /* Let the device shut down cleanly. */
  volatile union SimbricksProtoPcieH2D *msg = AllocOut();
  SimbricksPcieIfH2DOutSend(&pcie_if, msg, SIMBRICKS_PROTO_MSG_TYPE_TERMINATE);

  if (failures == 0) {
    printf("PROBE OK\n");
    return 0;
  }
  printf("PROBE FAILED (%d)\n", failures);
  return 1;
}
