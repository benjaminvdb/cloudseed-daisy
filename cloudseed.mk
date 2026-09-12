# cloudseed-daisy: make integration for libDaisy projects.
#
# A project's Makefile sets TARGET, its own CPP_SOURCES, LIBDAISY_DIR (and
# DAISYSP_DIR if it uses DaisySP), the build options below it wants to
# change and OPT, then includes this file, which adds the library's sources,
# include path, defines and flags, includes libDaisy's core Makefile, and
# adds the rules the library needs. Example (see examples/seed/Makefile):
#
#   TARGET = myreverb
#   CPP_SOURCES = main.cpp
#   LIBDAISY_DIR = ../libDaisy
#   include ../cloudseed-daisy/cloudseed.mk
#
# See README.md, "Building" and "Build options".

# Where this file is (evaluated now: MAKEFILE_LIST grows with every include).
ifndef CLOUDSEED_DAISY_DIR
CLOUDSEED_DAISY_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
endif

ifndef LIBDAISY_DIR
$(error LIBDAISY_DIR must point at a built libDaisy checkout)
endif

# --- Build options ----------------------------------------------------------

# Most late delay lines per channel any program may use (1 to 12): the main
# CPU and memory knob. Programs asking for more are clamped.
CLOUDSEED_MAX_LINES ?= 12
ifneq ($(words $(CLOUDSEED_MAX_LINES)),1)
$(error CLOUDSEED_MAX_LINES must be one integer from 1 to 12)
endif
ifeq ($(filter 1 2 3 4 5 6 7 8 9 10 11 12,$(CLOUDSEED_MAX_LINES)),)
$(error CLOUDSEED_MAX_LINES must be one integer from 1 to 12)
endif
# The sample rate the delay memory is sized for (Engine::Init fails above it).
CLOUDSEED_SAMPLE_RATE ?= 48000
# Per-program delay memory placed in the internal RAM: 0 off, 1 on.
CLOUDSEED_PLACEMENT ?= 1
# Sizes of the internal-RAM delay pools (KB): the AXI SRAM (512 KB in all)
# and the D2 SRAM behind libDaisy's audio DMA buffers and the MDMA list
# (288 KB in all). The defaults leave 34 KB of AXI SRAM and 640 B of D2 SRAM
# for the application; reduce them when it needs more.
CLOUDSEED_AXI_POOL_KB ?= 448
CLOUDSEED_D2_POOL_KB ?= 232
# Delay memory staged in the tightly coupled memories: 0 off, 1 the MDMA
# moves the windows, 2 the CPU copies them.
CLOUDSEED_STAGING ?= 1
# DTCM given to the staging (KB); the rest above the engine's objects is the
# stack (the profiling build reports the stack never used).
CLOUDSEED_DTCM_STAGING_KB ?= 24
# Experiment: the Cortex-M7's fairness counter for the MDMA's port to the
# TCMs (CM7_AHBSCR.INITCOUNT, 1 to 31; 1 is the reset value, larger values
# let the callback win more contended TCM cycles).
CLOUDSEED_AHBS_INITCOUNT ?= 1
# Profiling build: logs the CPU load over USB serial (Engine::PrintReport).
CLOUDSEED_PROFILE ?= 0
# Optimization level of the engine (the audio callback's side of the
# library, the staging's list builder included). The profiling build of the
# reference firmware sets -Os here: it needs the flash more than the speed.
CLOUDSEED_ENGINE_OPT ?= -O2
# Keep libDaisy's USB host stack out of the image (some 11 KB of flash) when
# the firmware does not use it. See src/cloudseed_daisy/usb_host_stub.cpp.
CLOUDSEED_STRIP_USB_HOST ?= 0

C_DEFS += -DCLOUDSEED_MAX_LINES=$(strip $(CLOUDSEED_MAX_LINES)) \
-DCLOUDSEED_SAMPLE_RATE=$(strip $(CLOUDSEED_SAMPLE_RATE)) \
-DCLOUDSEED_PLACEMENT=$(strip $(CLOUDSEED_PLACEMENT)) \
-DCLOUDSEED_AXI_POOL_KB=$(strip $(CLOUDSEED_AXI_POOL_KB)) \
-DCLOUDSEED_D2_POOL_KB=$(strip $(CLOUDSEED_D2_POOL_KB)) \
-DCLOUDSEED_STAGING=$(strip $(CLOUDSEED_STAGING)) \
-DCLOUDSEED_STAGED_MEMORY=$(if $(filter 0,$(strip $(CLOUDSEED_STAGING))),0,1) \
-DCLOUDSEED_DTCM_STAGING_KB=$(strip $(CLOUDSEED_DTCM_STAGING_KB)) \
-DCLOUDSEED_AHBS_INITCOUNT=$(strip $(CLOUDSEED_AHBS_INITCOUNT)) \
-DCLOUDSEED_PROFILE=$(strip $(CLOUDSEED_PROFILE))
# The DSP's sine table joins the engine's objects in the DTCM.
C_DEFS += -DCLOUDSEED_TABLE_SECTION='__attribute__((section(".dtcmram_bss")))'
# The options, recorded in a build prerequisite (below): compiler defines are
# not tracked by libDaisy's dependency files, so a changed option must
# rebuild the objects itself.
CLOUDSEED_CONFIG = $(strip $(CLOUDSEED_MAX_LINES) $(CLOUDSEED_SAMPLE_RATE) \
$(CLOUDSEED_PLACEMENT) $(CLOUDSEED_AXI_POOL_KB) $(CLOUDSEED_D2_POOL_KB) \
$(CLOUDSEED_STAGING) $(CLOUDSEED_DTCM_STAGING_KB) $(CLOUDSEED_AHBS_INITCOUNT) \
$(CLOUDSEED_PROFILE) $(CLOUDSEED_ENGINE_OPT) $(CLOUDSEED_STRIP_USB_HOST) \
$(CLOUDSEED_EXTRA_CONFIG))

# --- Sources and flags ------------------------------------------------------

CPP_SOURCES += \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/biquad.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/fast_sin.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/fdlibm_trig.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/reverb_channel.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/reverb_controller.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/reverb_setup.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed/sha_random.cpp \
$(CLOUDSEED_DAISY_DIR)/src/cloudseed_daisy/engine.cpp
ifeq ($(strip $(CLOUDSEED_STRIP_USB_HOST)),1)
CPP_SOURCES += $(CLOUDSEED_DAISY_DIR)/src/cloudseed_daisy/usb_host_stub.cpp
LDFLAGS += -Wl,--wrap=HAL_HCD_IRQHandler
endif

C_INCLUDES += -I$(CLOUDSEED_DAISY_DIR)/src

# No fused multiply-adds: the Cortex-M7 issues a vfma every third cycle but a
# vmul or vadd every cycle (TECHNICAL.md, "Numerical decisions"), so the
# kernels run faster unfused; the host builds, whose results the tests
# compare, never fuse; and the sin/cos reduction mirrors libm's object code
# with its fused operations written out (src/cloudseed/fdlibm_trig.cpp).
# GCC's default for C++ is -ffp-contract=fast, in standard modes too. This
# applies to the project's own objects as well.
CPPFLAGS += -ffp-contract=off

# libDaisy's core Makefile defines BUILD_DIR after this point; the rules
# below need it now (a command-line BUILD_DIR overrides both).
BUILD_DIR ?= build
SYSTEM_FILES_DIR ?= $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# --- Rules ------------------------------------------------------------------

# The reverb's inner loops all live in reverb_channel.cpp (everything they
# call is compiled there): always -O2, whatever the project's OPT.
$(BUILD_DIR)/reverb_channel.o: OPT = -O2
$(BUILD_DIR)/engine.o: OPT = $(CLOUDSEED_ENGINE_OPT)

.PHONY: FORCE
FORCE:

$(BUILD_DIR)/cloudseed-config: FORCE | $(BUILD_DIR)
	@echo '$(CLOUDSEED_CONFIG)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(OBJECTS): $(BUILD_DIR)/cloudseed-config $(CLOUDSEED_DAISY_DIR)/cloudseed.mk

# The image's CRC-32, which the profiling build prints at boot and with every
# program (`image=`): it names the build a log came from. zlib's crc32 is
# the one of gzip's trailer, so a machine without python3 can use
# `gzip -nc build/$(TARGET).bin | tail -c 8 | head -c 4 | od -An -tx4`.
.PHONY: image-crc
image-crc: $(BUILD_DIR)/$(TARGET).bin
	@command -v python3 >/dev/null 2>&1 && python3 -c 'import sys, zlib; print("image=%08x %s" % (zlib.crc32(open(sys.argv[1], "rb").read()), sys.argv[1]))' $< || true
all: image-crc
