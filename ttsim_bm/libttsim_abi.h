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
 * Runtime binding to Tenstorrent's libttsim.so.
 *
 * The library is loaded with dlopen rather than linked, so one adapter binary
 * serves every chip build (wh, bh, wh_x2, ...). The ABI is specified in
 * ttsim/docs/libttsim_api.md; only the ten libttsim_* symbols are exported.
 *
 * Two properties of the library shape everything built on top of this shim:
 *   - it is a process-wide singleton and is NOT reentrant, so every entry point
 *     must be called from a single serialized context;
 *   - any contract violation prints a diagnostic and _Exit(1)s the whole host
 *     process. There is no error return to check and nothing to catch.
 */

#ifndef TTSIM_BM_LIBTTSIM_ABI_H_
#define TTSIM_BM_LIBTTSIM_ABI_H_

#include <cstdint>

struct LibTtsim {
  /* Required entry points. */
  void (*init)(void);
  void (*exit)(void);
  void (*set_pci_dma_mem_callbacks)(void (*rd)(uint64_t, void *, uint32_t),
                                    void (*wr)(uint64_t, const void *,
                                               uint32_t));
  uint32_t (*pci_config_rd32)(uint32_t bdf, uint32_t offset);
  void (*pci_mem_rd_bytes)(uint64_t paddr, void *dst, uint32_t size);
  void (*pci_mem_wr_bytes)(uint64_t paddr, const void *src, uint32_t size);
  void (*clock)(uint32_t n_clocks);

  /* Optional. config_wr32 is documented as a fatal stub (though the current
   * source implements it) and the tile_* pair is transitional and slated for
   * removal, so none of these are required to be present and the adapter never
   * calls them. Resolved only so a missing symbol is reported as information
   * rather than discovered as a crash. */
  void (*pci_config_wr32)(uint32_t bdf, uint32_t offset, uint32_t data);
  void (*tile_rd_bytes)(uint32_t x, uint32_t y, uint64_t addr, void *dst,
                        uint32_t size);
  void (*tile_wr_bytes)(uint32_t x, uint32_t y, uint64_t addr, const void *src,
                        uint32_t size);

  void *handle;
};

/** dlopen `path` and resolve the entry points. Returns false and prints the
 * reason on failure. */
bool LibTtsimLoad(struct LibTtsim *lib, const char *path);

/** dlclose. Does not call libttsim_exit. */
void LibTtsimUnload(struct LibTtsim *lib);

#endif  // TTSIM_BM_LIBTTSIM_ABI_H_
