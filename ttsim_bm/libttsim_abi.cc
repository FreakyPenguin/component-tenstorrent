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

#include "libttsim_abi.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>

namespace {

/* dlsym returns void*; converting that to a function pointer is not strictly
 * conforming C++ but is the documented POSIX idiom and what every dlopen user
 * does. Funnel it through one place so the cast is auditable. */
template <typename Fn>
bool Resolve(void *handle, const char *name, Fn *out, bool required) {
  dlerror();
  void *sym = dlsym(handle, name);
  const char *err = dlerror();
  if (sym == nullptr || err != nullptr) {
    if (required) {
      fprintf(stderr, "libttsim: required symbol '%s' not found: %s\n", name,
              err ? err : "(null)");
    } else {
      fprintf(stderr, "libttsim: note: optional symbol '%s' not present\n",
              name);
    }
    *out = nullptr;
    return !required;
  }
  memcpy(out, &sym, sizeof(sym));
  return true;
}

}  // namespace

bool LibTtsimLoad(struct LibTtsim *lib, const char *path) {
  memset(lib, 0, sizeof(*lib));

  /* RTLD_LOCAL keeps libttsim's (hidden anyway) symbols out of the global
   * namespace; RTLD_NOW surfaces any missing dependency here rather than at the
   * first call. */
  lib->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (lib->handle == nullptr) {
    fprintf(stderr, "libttsim: dlopen('%s') failed: %s\n", path, dlerror());
    return false;
  }

  bool ok = true;
  ok &= Resolve(lib->handle, "libttsim_init", &lib->init, true);
  ok &= Resolve(lib->handle, "libttsim_exit", &lib->exit, true);
  ok &= Resolve(lib->handle, "libttsim_set_pci_dma_mem_callbacks",
                &lib->set_pci_dma_mem_callbacks, true);
  ok &= Resolve(lib->handle, "libttsim_pci_config_rd32", &lib->pci_config_rd32,
                true);
  ok &= Resolve(lib->handle, "libttsim_pci_mem_rd_bytes",
                &lib->pci_mem_rd_bytes, true);
  ok &= Resolve(lib->handle, "libttsim_pci_mem_wr_bytes",
                &lib->pci_mem_wr_bytes, true);
  ok &= Resolve(lib->handle, "libttsim_clock", &lib->clock, true);

  Resolve(lib->handle, "libttsim_pci_config_wr32", &lib->pci_config_wr32,
          false);
  Resolve(lib->handle, "libttsim_tile_rd_bytes", &lib->tile_rd_bytes, false);
  Resolve(lib->handle, "libttsim_tile_wr_bytes", &lib->tile_wr_bytes, false);

  if (!ok) {
    LibTtsimUnload(lib);
    return false;
  }
  return true;
}

void LibTtsimUnload(struct LibTtsim *lib) {
  if (lib->handle != nullptr) {
    dlclose(lib->handle);
  }
  memset(lib, 0, sizeof(*lib));
}
