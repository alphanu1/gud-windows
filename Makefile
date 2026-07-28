# SPDX-License-Identifier: MIT
#
# Host-side tests, built and run on Linux. Nothing here produces a Windows
# binary -- see build.cmd for that.
#
# The loopback test links blitsCRT_Mister's own device.c and runs the host
# protocol layer against it, so both implementations are under test at once and
# a disagreement about structure layout, request codes or the status handshake
# shows up here rather than on a CRT. Point DEVICE at a checkout:
#
#   make test DEVICE=../blitsCRT_Mister/sw

DEVICE ?= ../blitsCRT_Mister/sw

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -I include -I $(DEVICE)
LDLIBS   = -lm

COMMON  = src/common/gud_host.c src/common/modeline.c \
          src/common/convert.c  src/common/lz4enc.c

DEVSRC  = $(DEVICE)/device.c $(DEVICE)/modes.c $(DEVICE)/pll.c \
          $(DEVICE)/pll_reconfig.c $(DEVICE)/fabric.c $(DEVICE)/edid.c \
          $(DEVICE)/lz4dec.c

# Windows cross-checks. mingw-w64 carries real winusb.h, setupapi.h, d3d11.h,
# dxgi1_5.h and wrl, so gudprobe links against Microsoft's actual import
# libraries and the D3D code in SwapChain.cpp is checked against Microsoft's
# actual declarations. IddCx and WDF are WDK-only, so those are stubbed in
# tests/wdkstub -- which checks the driver against *those* declarations and
# proves nothing about whether they match the real ones. It still catches
# everything a compiler catches, which on unbuilt code is a lot.
CROSS ?= x86_64-w64-mingw32-
WINCFLAGS = -O2 -Wall -Wextra -Wshadow -Wsign-compare -Wno-unknown-pragmas

.PHONY: test clean check-device probe syntax windows golden

test: check-device build/test_lz4 build/test_loopback
	@./build/test_lz4
	@echo ""
	@./build/test_loopback

check-device:
	@test -f $(DEVICE)/device.c || { \
	  echo "no device source at $(DEVICE)"; \
	  echo "clone https://github.com/alphanu1/blitsCRT_Mister and pass DEVICE=<path>/sw"; \
	  exit 1; }

build/test_lz4: tests/test_lz4.c src/common/lz4enc.c $(DEVICE)/lz4dec.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

build/test_loopback: tests/test_loopback.c $(COMMON) $(DEVSRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# gudprobe.exe, cross-compiled and linked for real.
probe:
	@mkdir -p build
	$(CROSS)gcc $(WINCFLAGS) -I include \
	    src/probe/*.c src/common/*.c -o build/gudprobe.exe -lwinusb -lsetupapi
	@echo "built build/gudprobe.exe"

# The driver, compiled to objects against the stub headers. Not linkable and
# not shippable -- this is a compiler pass over code that otherwise gets its
# first one on a machine where it is hard to debug.
syntax:
	@mkdir -p build/syntax
	$(CROSS)g++ -std=c++17 $(WINCFLAGS) -Wcast-qual -Wconversion \
	    -I include -I tests/wdkstub -I src/driver \
	    -c src/driver/Driver.cpp    -o build/syntax/Driver.o
	$(CROSS)g++ -std=c++17 $(WINCFLAGS) -Wcast-qual -Wconversion \
	    -I include -I tests/wdkstub -I src/driver \
	    -c src/driver/SwapChain.cpp -o build/syntax/SwapChain.o
	@echo "driver compiles clean against the stubs"

windows: probe syntax

# Regenerate docs/expected-gudprobe-output.txt from the loopback harness.
golden: check-device build/golden
	@./build/golden > /tmp/golden.txt
	@sed -n '/^----/,$$p' docs/expected-gudprobe-output.txt | tail -n +3 \
	    | diff -u - /tmp/golden.txt > /dev/null \
	  && echo "expected-gudprobe-output.txt is current" \
	  || echo "expected-gudprobe-output.txt is STALE -- update it"

build/golden: tests/golden.c src/common/gud_host.c src/common/gud_dump.c $(DEVSRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -w -o $@ $^ $(LDLIBS)

clean:
	rm -rf build
