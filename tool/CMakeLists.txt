# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Sebastian Marrufo

cmake_minimum_required(VERSION 3.20)
project(nvtune LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "nvtune targets Windows only. Configure with a Windows toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(NVTUNE_CORE
    src/main.cpp src/cli.cpp src/arch.cpp src/regs.cpp src/gpu.cpp
    src/vbios.cpp src/json.cpp
)

add_executable(nvtune ${NVTUNE_CORE}
    src/pci_win.cpp src/mmio_win.cpp src/platform_win.cpp)
target_include_directories(nvtune PRIVATE include ../driver/include)
target_link_libraries(nvtune PRIVATE setupapi cfgmgr32 advapi32)

# The fake CLI uses deterministic registers and in-memory writes. It exercises
# the public command path without opening the driver or touching a GPU.
add_executable(nvtune_fake ${NVTUNE_CORE} tests/fake_backend.cpp)
target_include_directories(nvtune_fake PRIVATE include ../driver/include)

foreach(target nvtune nvtune_fake)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endforeach()

enable_testing()
add_test(NAME profile_cli COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:nvtune_fake>
    -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/profile_cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/profile_cli.cmake)
