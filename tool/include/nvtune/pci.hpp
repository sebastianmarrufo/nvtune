// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// Enumerate NVIDIA GPUs (Windows, via SetupAPI).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nvtune {

inline constexpr std::uint32_t kVendorNvidia = 0x10DE;
inline constexpr std::uint32_t kPciClassDisplay = 0x03;

struct PciDevice {
    std::string   slot;              // e.g. 0000:08:00.0
    // Decomposed location, from SetupAPI. This is what the driver is handed.
    std::uint32_t segment = 0;
    std::uint32_t bus = 0;
    std::uint32_t dev = 0;
    std::uint32_t func = 0;
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;
    std::uint32_t subsystem_vendor = 0;
    std::uint32_t subsystem_device = 0;
    bool          has_subsystem = false;
    std::uint32_t pci_class = 0;
    std::uint64_t bar0_start = 0;
    std::uint64_t bar0_size = 0;
};

// Format a slot as 0000:BB:DD.F, and parse one back.
std::string format_slot(std::uint32_t segment, std::uint32_t bus,
                        std::uint32_t dev, std::uint32_t func);
bool parse_slot(const std::string& slot, std::uint32_t& segment,
                std::uint32_t& bus, std::uint32_t& dev, std::uint32_t& func);



std::vector<PciDevice> enumerate_gpus(bool all_vendors = false);

// Exact slot, or a suffix match (so "08:00.0" finds "0000:08:00.0").
// Throws std::out_of_range if not found.
PciDevice find(const std::string& slot);

}  // namespace nvtune
