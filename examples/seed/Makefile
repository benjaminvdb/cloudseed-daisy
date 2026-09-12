# Cloud Seed on a bare Daisy Seed. LIBDAISY_DIR points at a built libDaisy
# checkout (`make` in it first); the default expects it next to this
# repository:
#
#   make LIBDAISY_DIR=/path/to/libDaisy
#   make program-dfu
#
TARGET = cloudseed_seed
CPP_SOURCES = main.cpp
LIBDAISY_DIR ?= ../../../libDaisy

# The reverb's kernels are compiled for speed whatever this is (cloudseed.mk).
OPT = -Os

include ../../cloudseed.mk
