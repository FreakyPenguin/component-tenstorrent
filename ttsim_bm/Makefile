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

# Self-contained build for the ttsim SimBricks adapter. Meant to be run from
# inside this directory, e.g.:
#   make -C ttsim_bm all SIMBRICKS_INC_DIR=... SIMBRICKS_LIB_DIR=...
# The top-level Makefile drives it that way; for a bare local build, point
# SIMBRICKS_INC_DIR/SIMBRICKS_LIB_DIR at a simbricks-lib install.

PREFIX            ?= /usr/local
CXX               ?= c++

SIMBRICKS_INC_DIR ?= /usr/local/include
SIMBRICKS_LIB_DIR ?= /usr/local/lib

# Only the PCIe adapter, the URL parser and the base queue layer are needed.
# Unlike the NIC models we deliberately do not link libnicbm/libnicif: nicbm's
# device interface is NIC-shaped (it requires an Ethernet interface and its
# RunMain demands both PCI and ETH parameter urls), and its asynchronous
# DmaComplete callback is the wrong shape for libttsim's synchronous DMA. Not
# using it also keeps boost out of the dependency set.
TTSIM_DEPS        ?= $(SIMBRICKS_LIB_DIR)/libparser.a \
                     $(SIMBRICKS_LIB_DIR)/libpcie.a \
                     $(SIMBRICKS_LIB_DIR)/libbase.a
# -ldl for dlopen'ing libttsim.so.
TTSIM_LDLIBS      ?= -ldl -lpthread

DEPFLAGS   = -MMD -MP
CPPFLAGS  += -I$(SIMBRICKS_INC_DIR) $(EXTRA_CPPFLAGS)
CXXFLAGS  += -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -std=gnu++17 $(EXTRA_CXXFLAGS)

SRCS := ttsim_bm.cc libttsim_abi.cc
OBJS := $(SRCS:.cc=.o)
BIN  := simb_ttsim_bm

.PHONY: all
all: $(BIN)

$(BIN): $(OBJS) $(TTSIM_DEPS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(TTSIM_DEPS) $(TTSIM_LDLIBS)

%.o: %.cc
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# Install the already-built binary (the top Makefile sequences build -> install,
# so this needs no build vars, only PREFIX).
.PHONY: install-ttsim
install-ttsim:
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/simb_ttsim_bm

.PHONY: clean
clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)
