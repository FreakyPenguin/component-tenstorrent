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

#ifndef TTSIM_BM_TTSIM_BM_H_
#define TTSIM_BM_TTSIM_BM_H_

#include <cstdint>

/*
 * Host-physical BAR layout of the simulated chip.
 *
 * libttsim keeps its own fixed internal BAR bases and decodes accesses by
 * absolute physical address; SimBricks, by contrast, assigns BARs host-side and
 * delivers accesses as (bar_index, offset). The adapter bridges the two by
 * declaring the sizes below in the device intro and then translating
 * (bar, offset) -> base + offset. Config-space writes are never forwarded, so
 * libttsim's bases stay at the values libttsim_init programmed.
 *
 * Values mirror ttsim/src/libttsim.cpp:12-31 and are keyed on the device ID
 * read back through libttsim_pci_config_rd32, so a single binary serves every
 * chip build.
 */
struct TtsimChipInfo {
  uint16_t device_id;
  const char *name;
  uint64_t bar0_base, bar0_size;
  uint64_t bar2_base, bar2_size;
  uint64_t bar4_base, bar4_size;
};

#define TTSIM_PCI_VENDOR_ID 0x1E52

/* BAR bases are identical across chips; only BAR4's size differs. */
#define TTSIM_BAR0_BASE 0x100000000ull
#define TTSIM_BAR2_BASE 0x120000000ull
#define TTSIM_BAR4_BASE 0x800000000ull

/* SimBricks BAR indices. libttsim's BAR numbering (0/2/4) maps one-to-one
 * because each is a 64-bit BAR and therefore consumes two config-space slots. */
#define TTSIM_BAR_IDX_0 0
#define TTSIM_BAR_IDX_2 2
#define TTSIM_BAR_IDX_4 4

/** Look up chip geometry by PCI device ID, or nullptr if unknown. */
const struct TtsimChipInfo *TtsimChipLookup(uint16_t device_id);

#endif  // TTSIM_BM_TTSIM_BM_H_
