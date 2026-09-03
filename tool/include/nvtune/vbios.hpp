// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// VBIOS Memory Tweak Table reader.
//
// The runtime FBPA registers hold whatever the driver last programmed for the
// current p-state. The *source* of those values is the Memory Tweak Table in
// the VBIOS: one entry per memory operating point, each carrying the full
// parameter set. Parsing it tells you what every p-state's stock timings are,
// including several fields (R2P, RDCRC, drive strength, voltage indices) whose
// runtime register homes are not publicly established.
//
// Layout is from NVIDIA's open-gpu-doc MemoryTweakTable spec, version 0x20.
//
// Header (6 bytes):
//   +0  version              (0x20)
//   +1  header size          (6)
//   +2  base entry size      (76)
//   +3  extended entry size  (12)
//   +4  extended entry count
//   +5  entry count
//
// Base entry (76 bytes):
//   +0   CONFIG0 .. +20  CONFIG5     (six u32)
//   +24  reserved, 23 bytes
//   +47  DriveStrength[1:0] Voltage0[4:2] Voltage1[7:5]
//   +48  Voltage2[2:0] R2P[7:3]
//   +49  Voltage3[2:0] rsvd[3] Voltage4[6:4] rsvd[7]
//   +50  Voltage5[2:0] rsvd[7:3]
//   +51  RDCRC[3:0], then 36 reserved bits
//   +56  TIMING22 (u32)
//   +60  reserved, 16 bytes

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace nvtune { class Gpu; }

namespace nvtune::vbios {

inline constexpr std::uint8_t kTweakVersion  = 0x20;
inline constexpr std::uint8_t kHeaderSize    = 6;
inline constexpr std::uint8_t kBaseEntrySize = 76;
inline constexpr std::uint8_t kExtEntrySize  = 12;

class VbiosError : public std::runtime_error {
public:
    explicit VbiosError(const std::string& w) : std::runtime_error(w) {}
};

struct TweakEntry {
    unsigned                    index = 0;
    std::array<std::uint32_t,6> config{};   // CONFIG0..CONFIG5 raw words
    std::uint32_t               timing22 = 0;
    std::uint32_t               drive_strength = 0;
    std::array<std::uint32_t,6> voltages{};  // Voltage0..Voltage5
    std::uint32_t               r2p = 0;
    std::uint32_t               rdcrc = 0;
    std::map<std::string, std::uint32_t> fields;  // decoded CONFIG + TIMING22

    std::uint32_t field(const std::string& name) const {
        auto it = fields.find(name);
        return it == fields.end() ? 0u : it->second;
    }
};

struct TweakTable {
    std::size_t   file_offset = 0;
    std::uint8_t  version = 0;
    unsigned      entry_count = 0;
    unsigned      ext_entry_count = 0;
    unsigned      base_entry_size = 0;
    unsigned      ext_entry_size = 0;
    std::vector<TweakEntry> entries;
};

// NVIDIA mirrors the VBIOS into BAR0 at PROM. Reading it there works
// on Windows and avoids any ROM-enable dance,
// which fails on boards that are actively driving a display.
inline constexpr std::uint32_t NV_PROM_BASE = 0x300000;
inline constexpr std::uint32_t NV_PROM_SIZE = 0x020000;   // 128 KiB
// PBUS register gating ROM visibility. Cleared before the read, restored after.
inline constexpr std::uint32_t NV_PBUS_ROM_ACCESS = 0x001850;

std::vector<std::uint8_t> read_file(const std::string& path);

// Read the VBIOS out of the BAR0 PROM mirror. Works on both platforms.
std::vector<std::uint8_t> read_prom(Gpu& gpu);

// Scan a ROM image for the Memory Tweak Table header.
TweakTable find_tweak_table(const std::vector<std::uint8_t>& blob);

}  // namespace nvtune::vbios
