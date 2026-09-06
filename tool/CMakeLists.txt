# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Sebastian Marrufo

cmake_minimum_required(VERSION 3.20)
project(nvtune LANGUAGES CXX)

# ---------------------------------------------------------------------------
# nvtune -- the usermode NVIDIA FBPA memory-timing tool (Windows).
#
# Talks to the nvtunedrv kernel driver (../driver) for all register access.
# Windows only; there is intentionally no Linux path.
# ---------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR
        "nvtune targets Windows only. Configure with a Windows toolchain "
        "(MSVC, or mingw-w64 for cross-compiling).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Windows 7 (0x0601) target + self-contained runtime -------------------
# Target Windows 7 so we don't pull in Win8+ API symbols, and link the C/C++
# runtime statically so the .exe launches on a Win7 box without the Universal
# CRT update (KB2999226) or a VC++ redistributable installed.
add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # /MT (static CRT)
endif()

set(NVTUNE_SRC
    src/main.cpp
    src/cli.cpp
    src/arch.cpp
    src/regs.cpp
    src/gpu.cpp
    src/vbios.cpp
    src/json.cpp
    src/pci_win.cpp
    src/mmio_win.cpp
    src/platform_win.cpp
)

add_executable(nvtune ${NVTUNE_SRC})

target_include_directories(nvtune PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver/include   # the IOCTL contract header
)

if(MSVC)
    target_compile_options(nvtune PRIVATE /W4 /permissive-)
    target_compile_definitions(nvtune PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(nvtune PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

target_link_libraries(nvtune PRIVATE setupapi cfgmgr32 advapi32)

# --- tests ------------------------------------------------------------------
enable_testing()
add_executable(selftest
    tests/selftest.cpp
    src/gpu.cpp src/mmio_win.cpp src/platform_win.cpp src/pci_win.cpp
)
target_include_directories(selftest PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver/include
)
target_link_libraries(selftest PRIVATE setupapi cfgmgr32 advapi32)
add_test(NAME selftest COMMAND selftest)
