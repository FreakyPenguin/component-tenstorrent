# MIT License
#
# Copyright (c) 2026 SimBricks
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Compilers and python interpreter (overridable by conda / the environment).
CXX               ?= c++
PYTHON            ?= python

# Where "make ttsim-install" places the binary. Inside a conda build this is the
# host/build prefix; for a local dev build override it, e.g. PREFIX=$(pwd)/out.
PREFIX            ?= $(shell pwd)/out

# simbricks-lib install layout: the headers and the static libs the adapter
# links against. Default under PREFIX; override for local dev to wherever
# simbricks-lib is installed (in the SimBricks devcontainer that is $CONDA_PREFIX).
SIMBRICKS_INC_DIR ?= $(PREFIX)/include
SIMBRICKS_LIB_DIR ?= $(PREFIX)/lib

# Python packages (each has its own pyproject.toml).
TTSIM_PY_SIM      := ttsim_sim_bm_py
TTSIM_PY_SYS      := ttsim_sys_py

# Optional: redirect conda-build output, e.g. OUTPUT_FOLDER=./conda-out.
OUTPUT_FOLDER     ?=
OUTPUT_FLAG       := $(if $(OUTPUT_FOLDER),--output-folder $(OUTPUT_FOLDER))
# Conda channels searched by `conda build`. The SimBricks channel hosts external
# deps not built here (e.g. simbricks-lib, simbricks-orchestration); conda-forge
# provides the rest.
SIMB_CONDA_CHANNEL:= -c https://conda.simbricks.io/latest
BASE_BUILD_CMD    := conda build $(SIMB_CONDA_CHANNEL) -m conda-recipes/conda_build_config.yaml $(OUTPUT_FLAG)

# Where the ttsim checkout lives and which chip build to use for local testing.
TTSIM_DIR         ?= $(abspath $(CURDIR)/../ttsim)
TTSIM_CHIP        ?= wh
TTSIM_LIB         ?= $(TTSIM_DIR)/src/_out/release_$(TTSIM_CHIP)/libttsim.so

.PHONY: all \
        ttsim-build ttsim-install \
        ttsim-python-develop \
        ttsim-sim-bm-py-conda ttsim-sys-py-conda ttsim-sim-bm-bin-conda \
        conda-packages pypi-build pypi-publish \
        libttsim selftest clean

## --- ttsim_bm SimBricks adapter (C++ sources in ttsim_bm/) -----------------

# Standalone dev build: just the binary, no conda package. The compile rules
# live in ttsim_bm/Makefile (a self-contained makefile); we only drive them.
ttsim-build:
	$(MAKE) -C ttsim_bm all CXX="$(CXX)" \
	    SIMBRICKS_INC_DIR="$(SIMBRICKS_INC_DIR)" \
	    SIMBRICKS_LIB_DIR="$(SIMBRICKS_LIB_DIR)"

# Install the binary into $(PREFIX)/bin as simb_ttsim_bm (builds first via the
# dependency; the install step itself only needs PREFIX).
ttsim-install: ttsim-build
	$(MAKE) -C ttsim_bm install-ttsim PREFIX="$(PREFIX)"

## --- libttsim.so ------------------------------------------------------------

# Build Tenstorrent's chip model from the ttsim checkout. Not packaged here --
# the adapter dlopens whichever libttsim.so it is pointed at -- but this is what
# the tests run against. Upstream also publishes release binaries.
libttsim:
	cd $(TTSIM_DIR) && ./make.py src/_out/release_$(TTSIM_CHIP)/libttsim.so

# Drive libttsim directly, with no SimBricks plumbing. Fastest way to tell an
# ABI/dlopen problem apart from a protocol problem.
selftest: ttsim-build
	./ttsim_bm/simb_ttsim_bm --selftest --lib $(TTSIM_LIB)

## --- Python packages --------------------------------------------------------

# Editable installs for local development.
ttsim-python-develop:
	$(PYTHON) -m pip install -e ./$(TTSIM_PY_SYS)
	$(PYTHON) -m pip install -e ./$(TTSIM_PY_SIM)

## --- Conda packages ---------------------------------------------------------

ttsim-sys-py-conda:
	$(BASE_BUILD_CMD) conda-recipes/simbricks-tenstorrent-sys-py

ttsim-sim-bm-py-conda: ttsim-sys-py-conda
	$(BASE_BUILD_CMD) conda-recipes/simbricks-tenstorrent-sim-bm-py

ttsim-sim-bm-bin-conda:
	$(BASE_BUILD_CMD) conda-recipes/simbricks-tenstorrent-sim-bm-bin

# Build all conda packages (python hulls first, then the compiled binary).
conda-packages: ttsim-sim-bm-py-conda ttsim-sys-py-conda ttsim-sim-bm-bin-conda

## --- PyPI packages ----------------------------------------------------------

pypi-build:
	poetry build -C $(TTSIM_PY_SIM)
	poetry build -C $(TTSIM_PY_SYS)

pypi-publish: pypi-build
	poetry publish -C $(TTSIM_PY_SIM)
	poetry publish -C $(TTSIM_PY_SYS)

## --- Default target ---------------------------------------------------------

# Default: build all conda packages.
all: conda-packages

## --- Housekeeping -----------------------------------------------------------

clean:
	-$(MAKE) -C ttsim_bm clean
	rm -rf $(TTSIM_PY_SIM)/dist $(TTSIM_PY_SYS)/dist
