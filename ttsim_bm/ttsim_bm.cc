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
 * SimBricks PCIe adapter for Tenstorrent's ttsim.
 *
 * Presents libttsim.so as a SimBricks PCIe device: forwards host MMIO into
 * libttsim_pci_mem_{rd,wr}_bytes, services libttsim's DMA callbacks out of the
 * host's memory over the SimBricks D2H channel, and drives simulated time with
 * libttsim_clock.
 *
 * ---------------------------------------------------------------------------
 * The two invariants this file exists to maintain
 * ---------------------------------------------------------------------------
 *
 * 1. libttsim is NOT reentrant. It must therefore be entered from exactly one
 *    place. PollPcie() consequently never calls into libttsim: it copies every
 *    MMIO request out of its shared-memory slot into `mmio_q` and returns. The
 *    queue is drained by DrainDeferredMmio(), which is the only caller of
 *    libttsim's MMIO entry points, and which never runs while another libttsim
 *    call is on the stack.
 *
 * 2. libttsim's DMA callbacks are SYNCHRONOUS -- they must have the bytes moved
 *    by the time they return -- while SimBricks DMA is request/completion
 *    asynchronous. DmaPumpUntil() bridges this by running a nested poll loop
 *    until the matching completion arrives. This is safe because DMA callbacks
 *    are never nested: libttsim chunks each transfer at DMA_BUFFER_SIZE (4 KiB)
 *    and issues them strictly sequentially.
 *
 *    Note that DMA is initiated from *both* directions of control flow --- from
 *    libttsim_clock (a NoC access to the host window, ttsim/src/tile.cpp:2786)
 *    and from an MMIO write to the BAR2 doorbell (ttsim/src/libttsim.cpp:467).
 *    So the pump has to work from inside any libttsim call, not just clocking.
 *
 * A corollary that is easy to get wrong: an outgoing message slot must never be
 * held across a libttsim call. SimbricksBaseIfOutAlloc hands out ring slots in
 * order and the peer consumes them in order, so if a nested DMA allocated and
 * sent slot N+1 while we still held an unsent slot N, the peer would block on N
 * -- which we cannot send until the DMA it is waiting on completes. Deadlock.
 * Every path here therefore completes its libttsim call into a local buffer
 * first, and only then allocates the slot it sends.
 */

#include "ttsim_bm.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <ctime>
#include <deque>

/* Must precede the extern "C" block: the SimBricks headers reach <atomic>
 * through this, and templates cannot have C linkage. nicbm.h does the same. */
#include <simbricks/base/cxxatomicfix.h>

extern "C" {
#include <simbricks/parser/parser.h>
#include <simbricks/pcie/if.h>
}

#include "libttsim_abi.h"

/******************************************************************************/
/* Configuration and state */

namespace {

/* Largest MMIO access we accept in one message. Real accesses are <= 8 bytes;
 * the slack is only so an oversized request produces a diagnostic instead of a
 * stack overwrite. */
constexpr uint16_t kMaxMmioLen = 64;

/* Fallback cap on a single DMA sub-operation. The real limit is derived from
 * the negotiated message size at startup; libttsim chunks at 4 KiB anyway. */
constexpr uint32_t kMaxDmaChunkCap = 4096;

/* Ceiling on one libttsim_clock() call. Keeps the count inside the uint32_t the
 * ABI takes, and bounds how long a synchronized batch can go without emitting a
 * progress line or noticing a terminated peer. At 1 clock per ns this is ~1 ms
 * of simulated time, far above the window a 500 ns sync interval produces, so
 * it does not bind in practice. */
constexpr uint64_t kMaxClocksPerCall = 1u << 20;

struct LibTtsim lib;
const struct TtsimChipInfo *chip;

struct SimbricksPcieIf pcie_if;
struct SimbricksBaseIfSHMPool shm_pool;
struct SimbricksBaseIfParams pcie_params;

/** Current simulation time, in picoseconds. */
uint64_t main_time = 0;
bool is_sync = false;
bool exiting = false;
bool link_up = false;

/** Picoseconds of simulated time per libttsim clock step. */
uint64_t ps_per_clock = 1000; /* 1000 MHz nominal AICLK -> 1 clock == 1 ns */
/** Upper bound on the clock steps executed per libttsim_clock() call. */
uint32_t max_batch_clocks = 500;
uint32_t max_dma_chunk = kMaxDmaChunkCap;

uint64_t bar4_size_override = 0;
uint64_t devctrl_flags = 0;

/* Statistics, printed at exit; mirrors the accel-sim example's counters. */
uint64_t stat_mmio_reads = 0, stat_mmio_writes = 0;
/* Of the writes, how many the host posted. Posted writes cost the guest nothing
 * beyond the trap, while every read and every non-posted write stalls it for a
 * round trip -- which is what makes the batch size below matter. */
uint64_t stat_mmio_writes_posted = 0;
uint64_t stat_dma_reads = 0, stat_dma_writes = 0;
uint64_t stat_clocks = 0;

/* Synchronized runs go exactly as fast as their slowest component at each
 * instant, so the only thing that makes the device model worth optimizing is
 * time the *peer* spends blocked on it. From this side that shows up as its
 * complement: stretches where we have run to the deadline and have nothing left
 * to do until the peer advances. A run that is mostly `stat_wait_peer_s` is
 * QEMU-bound, and making libttsim faster buys nothing.
 *
 * Timed per blocked->running transition rather than per loop iteration: a
 * clock_gettime on every iteration would be a measurable share of one. */
double stat_wait_peer_s = 0.0;
bool waiting_for_peer = false;
double wait_began = 0.0;
double link_up_at = 0.0;

double MonotonicSeconds();

/** Seconds between progress lines on stderr; 0 disables them. Runs of the full
 * Tenstorrent stack take long enough that "is it stuck or just slow?" is not
 * otherwise answerable. */
double progress_interval = 10.0;

/** An MMIO request copied out of its shared-memory slot, awaiting a libttsim
 * call. See invariant 1 at the top of this file. */
struct MmioReq {
  bool is_read;
  bool posted;
  uint64_t req_id;
  uint64_t offset;
  uint16_t len;
  uint8_t bar;
  uint8_t data[kMaxMmioLen];
};

std::deque<MmioReq> mmio_q;

/** One outstanding DMA sub-operation. Lives on the stack of the libttsim DMA
 * callback; its address is the SimBricks req_id. */
struct DmaSlot {
  bool done;
  void *buf; /* destination for reads; unused for writes */
  uint32_t len;
};

}  // namespace

/******************************************************************************/
/* Chip table */

static const struct TtsimChipInfo kChips[] = {
    {0x401E, "wormhole", TTSIM_BAR0_BASE, 512ull << 20, TTSIM_BAR2_BASE,
     1ull << 20, TTSIM_BAR4_BASE, 32ull << 20},
    {0xB140, "blackhole", TTSIM_BAR0_BASE, 512ull << 20, TTSIM_BAR2_BASE,
     1ull << 20, TTSIM_BAR4_BASE, 32ull << 30},
};

const struct TtsimChipInfo *TtsimChipLookup(uint16_t device_id) {
  for (const auto &c : kChips) {
    if (c.device_id == device_id) {
      return &c;
    }
  }
  return nullptr;
}

namespace {

/** Translate a SimBricks (bar, offset) pair into the physical address libttsim
 * decodes. Returns false for an unmapped BAR or an out-of-range offset. */
bool BarTranslate(uint8_t bar, uint64_t offset, uint32_t len, uint64_t *paddr) {
  uint64_t base, size;
  switch (bar) {
    case TTSIM_BAR_IDX_0:
      base = chip->bar0_base;
      size = chip->bar0_size;
      break;
    case TTSIM_BAR_IDX_2:
      base = chip->bar2_base;
      size = chip->bar2_size;
      break;
    case TTSIM_BAR_IDX_4:
      base = chip->bar4_base;
      size = bar4_size_override ? bar4_size_override : chip->bar4_size;
      break;
    default:
      return false;
  }
  if (offset > size || len > size - offset) {
    return false;
  }
  *paddr = base + offset;
  return true;
}

/******************************************************************************/
/* SimBricks output helpers */

volatile union SimbricksProtoPcieD2H *AllocPcieOut() {
  if (SimbricksBaseIfInTerminated(&pcie_if.base)) {
    fprintf(stderr, "ttsim_bm: peer terminated while sending\n");
    exit(1);
  }

  volatile union SimbricksProtoPcieD2H *msg;
  bool warned = false;
  while ((msg = SimbricksPcieIfD2HOutAlloc(&pcie_if, main_time)) == nullptr) {
    if (!warned) {
      fprintf(stderr, "ttsim_bm: warning: waiting for outgoing queue entry\n");
      warned = true;
    }
    if (SimbricksBaseIfInTerminated(&pcie_if.base)) {
      fprintf(stderr, "ttsim_bm: peer terminated while waiting for queue\n");
      exit(1);
    }
  }
  return msg;
}

void SendPcieOut(volatile union SimbricksProtoPcieD2H *msg, uint8_t type) {
  SimbricksPcieIfD2HOutSend(&pcie_if, msg, type);
}

/******************************************************************************/
/* Time */

/** The next timestamp at which SimBricks needs our attention: either an
 * incoming message becomes due, or we owe the peer a sync message. Only
 * meaningful once the incoming queue has been polled dry, because
 * H2DInTimestamp is refreshed as a side effect of polling. */
uint64_t NextSimbricksDeadline() {
  uint64_t next_in = SimbricksPcieIfH2DInTimestamp(&pcie_if);
  uint64_t next_sync = SimbricksPcieIfD2HOutNextSync(&pcie_if);
  return next_in <= next_sync ? next_in : next_sync;
}

/******************************************************************************/
/* Incoming message handling */

/** Handle at most one incoming message. Never calls into libttsim: MMIO is
 * copied into `mmio_q` for DrainDeferredMmio to execute later. Returns true if
 * a message was consumed. */
bool PollPcie() {
  volatile union SimbricksProtoPcieH2D *msg =
      SimbricksPcieIfH2DInPoll(&pcie_if, main_time);
  if (msg == nullptr) {
    return false;
  }

  uint8_t type = SimbricksPcieIfH2DInType(&pcie_if, msg);
  switch (type) {
    case SIMBRICKS_PROTO_PCIE_H2D_MSG_READ: {
      MmioReq r;
      r.is_read = true;
      r.posted = false;
      r.req_id = msg->read.req_id;
      r.offset = msg->read.offset;
      r.len = msg->read.len;
      r.bar = msg->read.bar;
      mmio_q.push_back(r);
      break;
    }

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE:
    case SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE_POSTED: {
      MmioReq r;
      r.is_read = false;
      r.posted = (type == SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE_POSTED);
      r.req_id = msg->write.req_id;
      r.offset = msg->write.offset;
      r.len = msg->write.len;
      r.bar = msg->write.bar;
      if (r.len > kMaxMmioLen) {
        fprintf(stderr, "ttsim_bm: MMIO write len %u exceeds %u\n", r.len,
                kMaxMmioLen);
        exit(1);
      }
      /* Must copy before InDone releases the slot back to the producer. */
      memcpy(r.data, (const void *)msg->write.data, r.len);
      mmio_q.push_back(r);
      break;
    }

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP: {
      struct DmaSlot *slot =
          reinterpret_cast<struct DmaSlot *>(msg->readcomp.req_id);
      memcpy(slot->buf, (const void *)msg->readcomp.data, slot->len);
      slot->done = true;
      stat_dma_reads++;
      break;
    }

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITECOMP: {
      struct DmaSlot *slot =
          reinterpret_cast<struct DmaSlot *>(msg->writecomp.req_id);
      slot->done = true;
      stat_dma_writes++;
      break;
    }

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_DEVCTRL:
      /* The chip advertises no INTx/MSI/MSI-X, so nothing acts on these bits.
       * Latched anyway so the state is visible when debugging. */
      devctrl_flags = msg->devctrl.flags;
      break;

    case SIMBRICKS_PROTO_MSG_TYPE_SYNC:
      break;

    case SIMBRICKS_PROTO_MSG_TYPE_TERMINATE:
      exiting = true;
      break;

    default:
      fprintf(stderr, "ttsim_bm: unhandled H2D message type 0x%x\n", type);
      break;
  }

  SimbricksPcieIfH2DInDone(&pcie_if, msg);
  return true;
}

/** Drain the incoming queue. */
void PollPcieAll() {
  while (PollPcie()) {
  }
}

/******************************************************************************/
/* DMA: synchronous callbacks on top of asynchronous SimBricks messages */

/** Advance time and process messages until `slot` completes. Deliberately does
 * NOT drain `mmio_q` or run clocks: a libttsim call is on the stack. */
void DmaPumpUntil(struct DmaSlot *slot) {
  while (!slot->done) {
    while (SimbricksPcieIfD2HOutSync(&pcie_if, main_time)) {
    }

    PollPcieAll();
    if (slot->done) {
      break;
    }

    if (exiting) {
      /* We cannot unwind out of libttsim, and completing the transfer is now
       * impossible, so stop here rather than letting it consume junk. */
      fprintf(stderr,
              "ttsim_bm: peer terminated with a DMA in flight; exiting\n");
      exit(0);
    }

    uint64_t next_ts;
    if (is_sync) {
      next_ts = NextSimbricksDeadline();
      uint64_t cap = main_time + max_batch_clocks * ps_per_clock;
      if (next_ts > cap) {
        next_ts = cap;
      }
    } else {
      next_ts = main_time + max_batch_clocks * ps_per_clock;
    }
    if (next_ts > main_time) {
      main_time = next_ts;
    }
  }
}

void DmaMemRead(uint64_t paddr, void *dst, uint32_t size) {
  if (!link_up) {
    fprintf(stderr,
            "ttsim_bm: DMA read at paddr=0x%" PRIx64
            " before the SimBricks link was established\n",
            paddr);
    exit(1);
  }

  for (uint32_t off = 0; off < size;) {
    uint32_t chunk = size - off;
    if (chunk > max_dma_chunk) {
      chunk = max_dma_chunk;
    }

    struct DmaSlot slot;
    slot.done = false;
    slot.buf = static_cast<uint8_t *>(dst) + off;
    slot.len = chunk;

    volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
    msg->read.req_id = reinterpret_cast<uintptr_t>(&slot);
    msg->read.offset = paddr + off; /* D2H 'offset' carries the host address */
    msg->read.len = chunk;
    SendPcieOut(msg, SIMBRICKS_PROTO_PCIE_D2H_MSG_READ);

    DmaPumpUntil(&slot);
    off += chunk;
  }
}

void DmaMemWrite(uint64_t paddr, const void *src, uint32_t size) {
  if (!link_up) {
    fprintf(stderr,
            "ttsim_bm: DMA write at paddr=0x%" PRIx64
            " before the SimBricks link was established\n",
            paddr);
    exit(1);
  }

  for (uint32_t off = 0; off < size;) {
    uint32_t chunk = size - off;
    if (chunk > max_dma_chunk) {
      chunk = max_dma_chunk;
    }

    struct DmaSlot slot;
    slot.done = false;
    slot.buf = nullptr;
    slot.len = chunk;

    volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
    msg->write.req_id = reinterpret_cast<uintptr_t>(&slot);
    msg->write.offset = paddr + off;
    msg->write.len = chunk;
    memcpy((void *)msg->write.data, static_cast<const uint8_t *>(src) + off,
           chunk);
    SendPcieOut(msg, SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITE);

    DmaPumpUntil(&slot);
    off += chunk;
  }
}

/******************************************************************************/
/* MMIO */

/** Execute queued MMIO against libttsim. The only place libttsim's MMIO entry
 * points are called; never runs with a libttsim call already on the stack.
 *
 * A write here can re-enter DMA (a BAR2 doorbell kicks libttsim's DMA engine
 * inline), which is why the completion slot is allocated only after the
 * libttsim call has returned -- see the deadlock note at the top of the file. */
void DrainDeferredMmio() {
  while (!mmio_q.empty()) {
    MmioReq r = mmio_q.front();
    mmio_q.pop_front();

    uint64_t paddr = 0;
    bool ok = BarTranslate(r.bar, r.offset, r.len, &paddr);
    if (!ok) {
      fprintf(stderr,
              "ttsim_bm: %s to unmapped BAR %u offset 0x%" PRIx64 " len %u\n",
              r.is_read ? "read" : "write", r.bar, r.offset, r.len);
    }

    if (r.is_read) {
      stat_mmio_reads++;

      uint8_t buf[kMaxMmioLen];
      memset(buf, 0, r.len);
      if (ok) {
        lib.pci_mem_rd_bytes(paddr, buf, r.len);
      }

      volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
      msg->readcomp.req_id = r.req_id;
      memcpy((void *)msg->readcomp.data, buf, r.len);
      SendPcieOut(msg, SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP);
    } else {
      stat_mmio_writes++;
      if (r.posted) {
        stat_mmio_writes_posted++;
      }

      if (ok) {
        lib.pci_mem_wr_bytes(paddr, r.data, r.len);
      }

      if (!r.posted) {
        volatile union SimbricksProtoPcieD2H *msg = AllocPcieOut();
        msg->writecomp.req_id = r.req_id;
        SendPcieOut(msg, SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITECOMP);
      }
    }
  }
}

/******************************************************************************/
/* Clocking */

/** Run a batch of libttsim clock steps and advance main_time accordingly.
 *
 * The two modes bound the batch for entirely different reasons, so they do not
 * share a knob:
 *
 * - Synchronized: SimBricks computes exactly how far ahead it is safe to run --
 *   the earlier of the next incoming message becoming due and the next sync we
 *   owe the peer. Run that whole window. Stopping short of it would only add
 *   loop iterations, because by construction nothing can need our attention
 *   before the deadline. `max_batch_clocks` deliberately does not apply here.
 *
 * - Unsynchronized: there is no horizon to respect, so the only thing bounding
 *   the batch is how long we are prepared to leave an incoming MMIO request
 *   unanswered. That is what `max_batch_clocks` is for.
 *
 * Batch size turns out to matter far less than it looks: tests/tt_clock_bench
 * measures 10.96 MHz at a batch of 1 against 11.84 MHz at a batch of a million,
 * so libttsim_clock's per-call cost is a couple of nanoseconds against ~87 ns
 * per clock step. The device model is simply slow; chopping it up is not what
 * makes it slow.
 *
 * If the deadline has already passed we run nothing and let the caller poll
 * again -- that spin is the correct synchronized behavior while waiting for the
 * peer to advance.
 *
 * A DMA inside libttsim_clock advances main_time on its own via DmaPumpUntil,
 * so the final assignment takes the later of the two rather than adding to it. */
void RunClocks() {
  uint64_t start = main_time;
  uint64_t n;

  if (is_sync) {
    uint64_t deadline = NextSimbricksDeadline();
    if (deadline <= main_time) {
      /* Already at the point where SimBricks needs us. Spin and let the caller
       * poll again; the peer has to advance before we may. */
      if (!waiting_for_peer) {
        waiting_for_peer = true;
        wait_began = MonotonicSeconds();
      }
      return;
    }
    n = (deadline - main_time) / ps_per_clock;
    if (n == 0) {
      /* The deadline is less than one clock step away, so there is no device
       * work that can happen before it. Jump straight to it: returning without
       * advancing would livelock, because nothing else moves main_time and the
       * budget would round to zero again on every iteration. */
      main_time = deadline;
      return;
    }
  } else {
    n = max_batch_clocks;
  }

  /* About to do real work, so any stretch of waiting on the peer ends here. */
  if (waiting_for_peer) {
    stat_wait_peer_s += MonotonicSeconds() - wait_began;
    waiting_for_peer = false;
  }

  /* libttsim_clock takes a uint32_t, and an unbounded batch would also stall
   * progress reporting and delay noticing a peer that has terminated. Far above
   * any window a 500 ns sync interval produces, so it never binds in practice. */
  if (n > kMaxClocksPerCall) {
    n = kMaxClocksPerCall;
  }

  lib.clock(static_cast<uint32_t>(n));
  stat_clocks += n;

  uint64_t target = start + static_cast<uint64_t>(n) * ps_per_clock;
  if (main_time < target) {
    main_time = target;
  }
}

double MonotonicSeconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

/** Dump a counter snapshot. Driven by SIGUSR1, which SimBricks sends to every
 * simulator every `--profile-int` seconds.
 *
 * A single total at exit is close to useless for deciding whether the device is
 * holding the simulation back: it averages in the startup window, where the peer
 * has not been launched yet and this side necessarily waits, and the shutdown
 * window, where the peer is already gone. A time series lets both ends be
 * trimmed and the steady state read off directly. */
void DumpProfile(int /*sig*/) {
  double now = MonotonicSeconds();
  double waited = stat_wait_peer_s;
  if (waiting_for_peer) {
    waited += now - wait_began;
  }
  /* async-signal-safety: this is a diagnostic path in a single-threaded program
   * whose only other work is a compute loop, so a stdio call here cannot
   * interleave with one in progress except through the progress printer, which
   * is rare enough to accept for a debugging aid. */
  fprintf(stderr,
          "ttsim_bm: PROFILE t=%.3f dev_ns=%" PRIu64 " clocks=%" PRIu64
          " wait_peer_s=%.3f mmio=%" PRIu64 " dma=%" PRIu64 "\n",
          link_up_at > 0.0 ? now - link_up_at : 0.0, main_time / 1000,
          stat_clocks, waited, stat_mmio_reads + stat_mmio_writes,
          stat_dma_reads + stat_dma_writes);
}

/** Rates since the previous call, so a stall is visible as zeros rather than as
 * a flat total. */
void ReportProgress() {
  static double last_t = 0.0;
  static uint64_t last_clocks = 0, last_mmio = 0, last_dma = 0;
  static double last_wait = 0.0;

  double now = MonotonicSeconds();
  if (last_t == 0.0) {
    last_t = now;
    return;
  }
  double dt = now - last_t;
  if (dt < progress_interval) {
    return;
  }

  uint64_t mmio = stat_mmio_reads + stat_mmio_writes;
  uint64_t dma = stat_dma_reads + stat_dma_writes;

  /* Include the stretch currently in progress, so a device that is blocked for
   * the whole interval reports ~100% rather than 0% until it happens to
   * unblock. */
  double waited = stat_wait_peer_s;
  if (waiting_for_peer) {
    waited += now - wait_began;
  }
  char wait_note[48] = "";
  if (is_sync) {
    snprintf(wait_note, sizeof(wait_note), ", idle waiting on peer %.0f%%",
             100.0 * (waited - last_wait) / dt);
  }

  fprintf(stderr,
          "ttsim_bm: %6.2f MHz  %8.0f mmio/s  %7.0f dma/s   "
          "[dev %.3f ms, mmio %" PRIu64 ", dma %" PRIu64 "%s]\n",
          static_cast<double>(stat_clocks - last_clocks) / dt / 1e6,
          static_cast<double>(mmio - last_mmio) / dt,
          static_cast<double>(dma - last_dma) / dt,
          static_cast<double>(main_time) / 1e9, mmio, dma, wait_note);
  last_wait = waited;

  last_t = now;
  last_clocks = stat_clocks;
  last_mmio = mmio;
  last_dma = dma;
}

/******************************************************************************/
/* Main loop */

void RunLoop() {
  uint32_t tick = 0;
  while (!exiting) {
    while (SimbricksPcieIfD2HOutSync(&pcie_if, main_time)) {
    }

    PollPcieAll();
    DrainDeferredMmio();
    RunClocks();

    /* Amortized: an iteration is only a few microseconds, and clock_gettime
     * every one of them would be a measurable fraction of it. */
    if (progress_interval > 0.0 && (++tick & 0xFFF) == 0) {
      ReportProgress();
    }

    if (SimbricksBaseIfInTerminated(&pcie_if.base)) {
      exiting = true;
    }
  }
}

/******************************************************************************/
/* Setup */

/** Read identity out of libttsim's config space and fill in the SimBricks
 * device intro. Config space is only read, never written: SimBricks assigns the
 * host-side BAR addresses and the adapter translates, so libttsim's internal
 * bases must stay exactly where libttsim_init put them. */
bool SetupIntro(struct SimbricksProtoPcieDevIntro *di) {
  uint32_t id = lib.pci_config_rd32(0, 0x00);
  uint16_t vendor_id = static_cast<uint16_t>(id & 0xFFFF);
  uint16_t device_id = static_cast<uint16_t>(id >> 16);

  if (vendor_id != TTSIM_PCI_VENDOR_ID) {
    fprintf(stderr, "ttsim_bm: unexpected vendor id 0x%04x (expected 0x%04x)\n",
            vendor_id, TTSIM_PCI_VENDOR_ID);
    return false;
  }

  chip = TtsimChipLookup(device_id);
  if (chip == nullptr) {
    fprintf(stderr,
            "ttsim_bm: unknown device id 0x%04x; extend the chip table in "
            "ttsim_bm.cc\n",
            device_id);
    return false;
  }

  uint32_t class_rev = lib.pci_config_rd32(0, 0x08);

  uint64_t bar4_size = bar4_size_override ? bar4_size_override : chip->bar4_size;

  memset(di, 0, sizeof(*di));
  di->bars[TTSIM_BAR_IDX_0].len = chip->bar0_size;
  di->bars[TTSIM_BAR_IDX_0].flags = SIMBRICKS_PROTO_PCIE_BAR_64;
  di->bars[TTSIM_BAR_IDX_2].len = chip->bar2_size;
  di->bars[TTSIM_BAR_IDX_2].flags = SIMBRICKS_PROTO_PCIE_BAR_64;
  di->bars[TTSIM_BAR_IDX_4].len = bar4_size;
  di->bars[TTSIM_BAR_IDX_4].flags = SIMBRICKS_PROTO_PCIE_BAR_64;

  di->pci_vendor_id = vendor_id;
  di->pci_device_id = device_id;
  di->pci_revision = static_cast<uint8_t>(class_rev & 0xFF);
  di->pci_progif = static_cast<uint8_t>((class_rev >> 8) & 0xFF);
  di->pci_subclass = static_cast<uint8_t>((class_rev >> 16) & 0xFF);
  di->pci_class = static_cast<uint8_t>((class_rev >> 24) & 0xFF);

  /* The chip exposes no capability list and reports interrupt pin 0, so there
   * is no INTx, MSI or MSI-X. Software polls; the adapter never sends an
   * interrupt message. */
  di->pci_msi_nvecs = 0;
  di->pci_msix_nvecs = 0;

  fprintf(stderr,
          "ttsim_bm: %s %04x:%04x class %02x:%02x rev %02x, "
          "BAR0 %" PRIu64 " MiB, BAR2 %" PRIu64 " MiB, BAR4 %" PRIu64 " MiB\n",
          chip->name, vendor_id, device_id, di->pci_class, di->pci_subclass,
          di->pci_revision, chip->bar0_size >> 20, chip->bar2_size >> 20,
          bar4_size >> 20);
  return true;
}

bool InitSimBricks(const char *url, struct SimbricksProtoPcieDevIntro *di) {
  struct SimbricksAdapterParams *p = SimbricksParametersParse(url);
  if (p == nullptr) {
    fprintf(stderr, "ttsim_bm: failed to parse parameters url '%s'\n", url);
    return false;
  }
  if (!p->listen) {
    fprintf(stderr, "ttsim_bm: device must listen, got a connect url\n");
    return false;
  }
  if (p->shm_path == nullptr) {
    fprintf(stderr, "ttsim_bm: listen url is missing the shm path\n");
    return false;
  }

  SimbricksPcieIfDefaultParams(&pcie_params);
  pcie_params.sock_path = p->socket_path;
  pcie_params.sync_mode = p->sync ? kSimbricksBaseIfSyncRequired
                                  : kSimbricksBaseIfSyncDisabled;
  /* The url carries nanoseconds while SimbricksBaseIfParams is in picoseconds,
   * and SimbricksParametersParse does not convert -- it hands back the raw
   * number. The peer does convert: QEMU's simbricks-pci turns `sync-period=500`
   * into sync messages stamped 500000 ps apart, and the orchestration documents
   * both fields in nanoseconds. Getting this wrong is not merely a scaling
   * error; syncing 1000x too often makes the simulation crawl. */
  if (p->link_latency_set) {
    pcie_params.link_latency = p->link_latency * 1000;
  }
  if (p->sync_interval_set) {
    pcie_params.sync_interval = p->sync_interval * 1000;
  }

  if (SimbricksBaseIfSHMPoolCreate(&shm_pool, p->shm_path,
                                   SimbricksBaseIfSHMSize(&pcie_params)) != 0) {
    perror("ttsim_bm: SimbricksBaseIfSHMPoolCreate failed");
    return false;
  }
  if (SimbricksBaseIfInit(&pcie_if.base, &pcie_params) != 0) {
    perror("ttsim_bm: SimbricksBaseIfInit failed");
    return false;
  }
  if (SimbricksBaseIfListen(&pcie_if.base, &shm_pool) != 0) {
    perror("ttsim_bm: SimbricksBaseIfListen failed");
    return false;
  }

  struct SimbricksProtoPcieHostIntro host_intro;
  struct SimBricksBaseIfEstablishData estd;
  memset(&estd, 0, sizeof(estd));
  estd.base_if = &pcie_if.base;
  estd.tx_intro = di;
  estd.tx_intro_len = sizeof(*di);
  estd.rx_intro = &host_intro;
  estd.rx_intro_len = sizeof(host_intro);

  if (SimBricksBaseIfEstablish(&estd, 1) != 0) {
    fprintf(stderr, "ttsim_bm: SimBricksBaseIfEstablish failed\n");
    return false;
  }

  is_sync = SimbricksBaseIfSyncEnabled(&pcie_if.base);

  /* Cap DMA sub-operations at what actually fits in a message payload. */
  size_t payload = SimbricksPcieIfD2HOutMsgLen(&pcie_if) -
                   sizeof(struct SimbricksProtoPcieD2HWrite);
  if (payload < max_dma_chunk) {
    max_dma_chunk = static_cast<uint32_t>(payload);
  }

  fprintf(stderr,
          "ttsim_bm: link up (sync=%s, latency=%" PRIu64
          " ps, sync_interval=%" PRIu64 " ps, max dma chunk=%u B)\n",
          is_sync ? "on" : "off", pcie_params.link_latency,
          pcie_params.sync_interval, max_dma_chunk);

  SimbricksParametersFree(p);
  return true;
}

/******************************************************************************/
/* Entry point */

void PrintUsage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s PCI-PARAMS-URL [START-TICK] --lib LIBTTSIM.SO [options]\n"
          "\n"
          "  PCI-PARAMS-URL     listen:<sock>:<shm>:sync=<bool>:latency=<ps>"
          ":sync_interval=<ps>\n"
          "  START-TICK         initial simulation time in picoseconds\n"
          "\n"
          "Options:\n"
          "  --lib PATH             path to libttsim.so (required)\n"
          "  --clock-freq-mhz N     device clock frequency (default 1000)\n"
          "  --max-batch-clocks N   clock steps per batch (default 500)\n"
          "  --bar4-size N          override BAR4 size in bytes\n"
          "  --progress-secs N      progress line interval, 0 to disable"
          " (default 10)\n"
          "  --selftest             drive libttsim directly, no SimBricks\n",
          argv0);
}

struct Options {
  const char *url = nullptr;
  const char *lib_path = nullptr;
  uint64_t start_tick = 0;
  uint64_t clock_freq_mhz = 1000;
  bool selftest = false;
};

bool ParseOptions(int argc, char *argv[], struct Options *o) {
  int positional = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strncmp(a, "--", 2) != 0) {
      if (positional == 0) {
        o->url = a;
      } else if (positional == 1) {
        o->start_tick = strtoull(a, nullptr, 0);
      } else {
        fprintf(stderr, "ttsim_bm: unexpected argument '%s'\n", a);
        return false;
      }
      positional++;
      continue;
    }

    if (strcmp(a, "--selftest") == 0) {
      o->selftest = true;
      continue;
    }

    /* everything below takes a value */
    if (i + 1 >= argc) {
      fprintf(stderr, "ttsim_bm: option '%s' requires a value\n", a);
      return false;
    }
    const char *v = argv[++i];

    if (strcmp(a, "--lib") == 0) {
      o->lib_path = v;
    } else if (strcmp(a, "--clock-freq-mhz") == 0) {
      o->clock_freq_mhz = strtoull(v, nullptr, 0);
    } else if (strcmp(a, "--max-batch-clocks") == 0) {
      max_batch_clocks = static_cast<uint32_t>(strtoul(v, nullptr, 0));
    } else if (strcmp(a, "--bar4-size") == 0) {
      bar4_size_override = strtoull(v, nullptr, 0);
    } else if (strcmp(a, "--progress-secs") == 0) {
      progress_interval = strtod(v, nullptr);
    } else {
      fprintf(stderr, "ttsim_bm: unknown option '%s'\n", a);
      return false;
    }
  }

  if (o->lib_path == nullptr) {
    fprintf(stderr, "ttsim_bm: --lib is required\n");
    return false;
  }
  if (!o->selftest && o->url == nullptr) {
    fprintf(stderr, "ttsim_bm: a PCI parameters url is required\n");
    return false;
  }
  if (o->clock_freq_mhz == 0) {
    fprintf(stderr, "ttsim_bm: --clock-freq-mhz must be non-zero\n");
    return false;
  }
  if (max_batch_clocks == 0) {
    fprintf(stderr, "ttsim_bm: --max-batch-clocks must be non-zero\n");
    return false;
  }
  return true;
}

/** Drive libttsim without any SimBricks plumbing. Isolates dlopen/ABI problems
 * from protocol problems, and needs no peer to run against. */
int RunSelfTest() {
  struct SimbricksProtoPcieDevIntro di;
  if (!SetupIntro(&di)) {
    return 1;
  }

  for (uint32_t off = 0; off < 0x40; off += 4) {
    fprintf(stderr, "  config[0x%02x] = 0x%08x\n", off,
            lib.pci_config_rd32(0, off));
  }

  fprintf(stderr, "ttsim_bm: selftest running %u clocks\n", max_batch_clocks);
  lib.clock(max_batch_clocks);

  fprintf(stderr, "ttsim_bm: selftest ok\n");
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  struct Options opts;
  if (!ParseOptions(argc, argv, &opts)) {
    PrintUsage(argv[0]);
    return 1;
  }

  ps_per_clock = 1000000ull / opts.clock_freq_mhz;
  if (ps_per_clock == 0) {
    ps_per_clock = 1;
  }
  main_time = opts.start_tick;

  if (!LibTtsimLoad(&lib, opts.lib_path)) {
    return 1;
  }

  /* Order is mandated by the ABI: callbacks must be installed before init, and
   * config space may only be read once the simulator is running. */
  lib.set_pci_dma_mem_callbacks(DmaMemRead, DmaMemWrite);
  lib.init();

  int ret;
  if (opts.selftest) {
    ret = RunSelfTest();
  } else {
    struct SimbricksProtoPcieDevIntro di;
    if (!SetupIntro(&di)) {
      return 1;
    }
    if (!InitSimBricks(opts.url, &di)) {
      return 1;
    }
    link_up = true;
    link_up_at = MonotonicSeconds();
    /* SimBricks sends SIGUSR1 to every simulator every --profile-int seconds. */
    signal(SIGUSR1, DumpProfile);

    RunLoop();

    if (waiting_for_peer) {
      stat_wait_peer_s += MonotonicSeconds() - wait_began;
      waiting_for_peer = false;
    }

    fprintf(stderr,
            "ttsim_bm: MMIO READS = %" PRIu64 "\n"
            "ttsim_bm: MMIO WRITES = %" PRIu64 " (%" PRIu64 " posted)\n"
            "ttsim_bm: DMA READS = %" PRIu64 "\n"
            "ttsim_bm: DMA WRITES = %" PRIu64 "\n"
            "ttsim_bm: CLOCK STEPS = %" PRIu64 "\n"
            "ttsim_bm: final time = %" PRIu64 " ps\n",
            stat_mmio_reads, stat_mmio_writes, stat_mmio_writes_posted,
            stat_dma_reads, stat_dma_writes, stat_clocks, main_time);

    /* The number that decides whether optimizing libttsim is worth anything.
     * A synchronized run is only as fast as whichever side is behind, so time
     * we spend idle at the deadline is time the device was NOT holding the
     * simulation back. Near 100% means the run is peer-bound and a faster chip
     * model buys nothing. */
    if (is_sync) {
      double elapsed = MonotonicSeconds() - link_up_at;
      fprintf(stderr,
              "ttsim_bm: LINK TIME = %.1f s, of which idle waiting on peer"
              " %.1f s (%.0f%%)\n",
              elapsed, stat_wait_peer_s,
              elapsed > 0.0 ? 100.0 * stat_wait_peer_s / elapsed : 0.0);
    }
    ret = 0;
  }

  lib.exit();
  LibTtsimUnload(&lib);
  return ret;
}
