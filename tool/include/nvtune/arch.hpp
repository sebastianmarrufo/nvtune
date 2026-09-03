// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// Architecture identification and FBPA aperture geometry.
//
// Rather than carrying an ever-stale PCI device-ID table, the chip identifies
// itself: NV_PMC_BOOT_0 lives at BAR0 offset 0 on every NVIDIA GPU since the
// NV40 era and encodes the chipset ID in bits [28:20].
//
// Aperture geometry comes from NVIDIA's gp100-fbpa.txt:
//
//   Name                Old Range               New Range (Pascal+)
//   NV_PFB_FBPA         0x10F000 (0x1000)       0x9A0000 (0x4000)
//   NV_PFB_FBPA[i]      0x110000+(i*0x1000)     0x900000+(i*0x4000)
//   NV_PFB_FBPA_MC[i]   0x11D000+(i*0x1000)     0x980000+(i*0x4000)

#pragma once

#include <cstdint>
#include <string>

namespace nvtune {

// Topology / fuse registers, absolute BAR0 offsets (Pascal+ per NVIDIA doc;
// the same addresses are used on earlier chips).
inline constexpr std::uint32_t NV_PMC_BOOT_0                 = 0x000000;
inline constexpr std::uint32_t NV_PTOP_SCAL_NUM_FBPAS        = 0x02243C; // [4:0]
inline constexpr std::uint32_t NV_PTOP_SCAL_NUM_FBPA_PER_FBP = 0x022458; // [4:0]
inline constexpr std::uint32_t NV_FUSE_STATUS_OPT_FBIO       = 0x021C14; // [15:0]
inline constexpr std::uint32_t NV_PFB_FBPA_CSTATUS_RAMAMOUNT = 0x00020C; // FBPA-rel

struct Layout {
    std::uint32_t broadcast;       // NV_PFB_FBPA
    std::uint32_t window;          // aperture size
    std::uint32_t unicast;         // NV_PFB_FBPA[0]
    std::uint32_t unicast_stride;
    std::uint32_t mc;              // NV_PFB_FBPA_MC[0]
    std::uint32_t mc_stride;
    unsigned      max_fbpa;
};

inline constexpr Layout kLegacy{0x10F000, 0x1000, 0x110000, 0x1000,
                                0x11D000, 0x1000, 8};   // Fermi..Maxwell
inline constexpr Layout kModern{0x9A0000, 0x4000, 0x900000, 0x4000,
                                0x980000, 0x4000, 16};  // Pascal+

struct Arch {
    std::uint32_t chipset = 0;
    std::string   codename;
    std::string   family;
    std::string   series;
    Layout        layout = kModern;
    // Whether runtime writes are expected to stick. On the newest parts the
    // PMU/GSP microcontrollers own memory reclocking behind signed firmware
    // and may reassert or ignore our values.
    bool          writes_expected = true;
    std::string   caveat;
};

Arch identify(std::uint32_t boot0);
std::string decode_boot0(std::uint32_t boot0);

}  // namespace nvtune
