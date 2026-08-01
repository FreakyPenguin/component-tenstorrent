#!/bin/bash
set -eux

# Build and install the ttsim SimBricks adapter binary. The top-level Makefile
# drives ttsim_bm/Makefile; header and static-lib dirs default from PREFIX
# ($PREFIX/include and $PREFIX/lib, both provided by simbricks-lib).
make ttsim-install PREFIX="${PREFIX}"
