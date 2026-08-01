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

CXX               ?= c++
SIMBRICKS_INC_DIR ?= /usr/local/include
SIMBRICKS_LIB_DIR ?= /usr/local/lib

CPPFLAGS  += -I$(SIMBRICKS_INC_DIR)
CXXFLAGS  += -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu++17
DEPS      := $(SIMBRICKS_LIB_DIR)/libparser.a $(SIMBRICKS_LIB_DIR)/libpcie.a \
             $(SIMBRICKS_LIB_DIR)/libbase.a
LDLIBS    += -lpthread

BIN := tt_host_probe
BENCH := tt_clock_bench

.PHONY: all
all: $(BIN) $(BENCH)

$(BIN): tt_host_probe.cc $(DEPS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(DEPS) $(LDLIBS)

# Needs no SimBricks libraries at all -- it drives libttsim directly, which is
# the point: it measures the chip model in isolation from the adapter.
$(BENCH): tt_clock_bench.cc ../ttsim_bm/libttsim_abi.cc
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ -ldl

.PHONY: clean
clean:
	rm -f $(BIN) $(BENCH)
