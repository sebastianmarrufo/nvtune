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

option(NVTUNE_WINDOWS7 "Build for Windows 7 SP1 x64 (validate on the target OS)" OFF)
option(NVTUNE_WINDOWS_VISTA "Build for Windows Vista SP2 / Windows 7 x64" OFF)
if(NVTUNE_WINDOWS7 AND NVTUNE_WINDOWS_VISTA)
    message(FATAL_ERROR "Choose NVTUNE_WINDOWS_VISTA or NVTUNE_WINDOWS7, not both.")
endif()
set(NVTUNE_LEGACY_WINDOWS OFF)
if(NVTUNE_WINDOWS7 OR NVTUNE_WINDOWS_VISTA)
    set(NVTUNE_LEGACY_WINDOWS ON)
endif()
option(NVTUNE_STATIC_RUNTIME "Link the compiler runtime statically" ${NVTUNE_LEGACY_WINDOWS})

if(NVTUNE_LEGACY_WINDOWS)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The legacy Windows build requires an x64 toolchain.")
    endif()
    if(NVTUNE_WINDOWS_VISTA AND NOT MINGW)
        message(FATAL_ERROR
            "Use mingw-w64 with the system msvcrt runtime for the Vista build; "
            "current MSVC/UCRT runtimes are not a stock Vista compatibility target.")
    endif()
    if(MSVC AND MSVC_VERSION GREATER_EQUAL 1930)
        message(FATAL_ERROR
            "Use the VS 2019 v142 toolset or mingw-w64 for the Windows 7 build; "
            "a current MSVC runtime is not a Windows 7 compatibility target.")
    endif()
endif()

set(NVTUNE_CORE_SRC
    src/cli.cpp
    src/arch.cpp
    src/regs.cpp
    src/gpu.cpp
    src/vbios.cpp
    src/clocks.cpp
    src/json.cpp
)

add_executable(nvtune src/main.cpp ${NVTUNE_CORE_SRC}
    src/pci_win.cpp src/mmio_win.cpp src/platform_win.cpp
)

function(nvtune_configure target)
    target_include_directories(${target} PRIVATE include
        ${CMAKE_CURRENT_SOURCE_DIR}/../driver/include)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(NVTUNE_STATIC_RUNTIME)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(MINGW AND NVTUNE_STATIC_RUNTIME)
            target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endif()
    if(NVTUNE_LEGACY_WINDOWS)
        set(api_version 0x0601)
        set(minor_version 1)
        if(NVTUNE_WINDOWS_VISTA)
            set(api_version 0x0600)
            set(minor_version 0)
        endif()
        target_compile_definitions(${target} PRIVATE
            WINVER=${api_version} _WIN32_WINNT=${api_version})
        if(MSVC)
            target_link_options(${target} PRIVATE /SUBSYSTEM:CONSOLE,6.01)
        elseif(MINGW)
            target_link_options(${target} PRIVATE
                -Wl,--major-os-version,6,--minor-os-version,${minor_version}
                -Wl,--major-subsystem-version,6,--minor-subsystem-version,${minor_version})
        endif()
    endif()
    if(NVTUNE_WINDOWS_VISTA)
        if(NOT CMAKE_OBJDUMP)
            message(FATAL_ERROR "The Vista build needs the toolchain's objdump to audit runtime dependencies.")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DNVTUNE_OBJDUMP=${CMAKE_OBJDUMP}"
                "-DNVTUNE_BINARY=$<TARGET_FILE:${target}>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_vista_imports.cmake"
            VERBATIM)
    endif()
endfunction()

nvtune_configure(nvtune)
target_link_libraries(nvtune PRIVATE setupapi cfgmgr32 advapi32)

# --- tests ------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    # The test implements PCI, BAR0 and platform services in memory. Never link
    # the live driver backend into the CLI contract tests.
    add_executable(selftest tests/selftest.cpp tests/fake_backend.cpp ${NVTUNE_CORE_SRC})
    nvtune_configure(selftest)
    add_test(NAME selftest COMMAND selftest ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
endif()
