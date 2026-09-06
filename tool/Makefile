# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Sebastian Marrufo

# nvtune tool -- mingw-w64 cross-build convenience.
#
# Produces a Windows PE binary. This is NOT a Linux build of nvtune (there is
# none); it just lets you cross-compile the Windows tool from a Linux box.
# On Windows, prefer CMake (see CMakeLists.txt) or MSVC directly.
#
#   make            build nvtune.exe
#   make selftest   build selftest.exe (run under wine64)
#   make clean

WINCXX      ?= x86_64-w64-mingw32-g++
WINCXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow \
               -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -Iinclude -I../driver/include
WINLIBS     ?= -lsetupapi -lcfgmgr32 -ladvapi32 -static -static-libgcc -static-libstdc++ \
               -Wl,--major-os-version,6,--minor-os-version,1 \
               -Wl,--major-subsystem-version,6,--minor-subsystem-version,1

SRC := src/main.cpp src/cli.cpp src/arch.cpp src/regs.cpp src/gpu.cpp \
       src/vbios.cpp src/clocks.cpp src/json.cpp src/pci_win.cpp \
       src/mmio_win.cpp src/platform_win.cpp

SELFTEST_SRC := tests/selftest.cpp tests/fake_backend.cpp src/cli.cpp src/arch.cpp src/regs.cpp src/vbios.cpp \
                src/clocks.cpp src/json.cpp src/gpu.cpp

.PHONY: all selftest clean

all: nvtune.exe

nvtune.exe: $(SRC)
	$(WINCXX) $(WINCXXFLAGS) -o $@ $^ $(WINLIBS)

selftest: selftest.exe
selftest.exe: $(SELFTEST_SRC)
	$(WINCXX) $(WINCXXFLAGS) -o $@ $^ $(WINLIBS)
	@echo "run under wine:  wine64 selftest.exe"

clean:
	rm -f nvtune.exe selftest.exe
