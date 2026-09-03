// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

#include "nvtune/arch.hpp"

#include <cstdio>
#include <map>

namespace nvtune {
namespace {

struct ChipInfo {
    const char* codename;
    const char* family;
    const char* series;
};

// chipset id -> codename / family / marketing series. Values follow nouveau's
// device table. Anything absent falls through to generic().
const std::map<std::uint32_t, ChipInfo>& chips() {
    static const std::map<std::uint32_t, ChipInfo> m = {
        {0x0C0, {"GF100",  "Fermi",   "GTX 400"}},
        {0x0C1, {"GF108",  "Fermi",   "GT 400/500"}},
        {0x0C3, {"GF106",  "Fermi",   "GTS 400/500"}},
        {0x0C4, {"GF104",  "Fermi",   "GTX 400"}},
        {0x0C8, {"GF110",  "Fermi",   "GTX 500"}},
        {0x0CE, {"GF114",  "Fermi",   "GTX 500"}},
        {0x0CF, {"GF116",  "Fermi",   "GTX 500"}},
        {0x0D7, {"GF117",  "Fermi",   "GT 600M"}},
        {0x0D9, {"GF119",  "Fermi",   "GT 500"}},
        {0x0E4, {"GK104",  "Kepler",  "GTX 600/700"}},
        {0x0E6, {"GK106",  "Kepler",  "GTX 600"}},
        {0x0E7, {"GK107",  "Kepler",  "GT 600/700"}},
        {0x0F0, {"GK110",  "Kepler",  "GTX 700 / Titan"}},
        {0x0F1, {"GK110B", "Kepler",  "GTX 700 / Titan Black"}},
        {0x106, {"GK208B", "Kepler",  "GT 700"}},
        {0x108, {"GK208",  "Kepler",  "GT 700"}},
        {0x117, {"GM107",  "Maxwell", "GTX 750 / 900M"}},
        {0x118, {"GM108",  "Maxwell", "GT 900M"}},
        {0x120, {"GM200",  "Maxwell", "GTX 980 Ti / Titan X"}},
        {0x124, {"GM204",  "Maxwell", "GTX 900"}},
        {0x126, {"GM206",  "Maxwell", "GTX 950/960"}},
        {0x130, {"GP100",  "Pascal",  "Tesla P100"}},
        {0x132, {"GP102",  "Pascal",  "GTX 1080 Ti / Titan Xp"}},
        {0x134, {"GP104",  "Pascal",  "GTX 1070/1080"}},
        {0x136, {"GP106",  "Pascal",  "GTX 1060"}},
        {0x137, {"GP107",  "Pascal",  "GTX 1050"}},
        {0x138, {"GP108",  "Pascal",  "GT 1030"}},
        {0x140, {"GV100",  "Volta",   "Titan V"}},
        {0x162, {"TU102",  "Turing",  "RTX 2080 Ti"}},
        {0x164, {"TU104",  "Turing",  "RTX 2070S/2080"}},
        {0x166, {"TU106",  "Turing",  "RTX 2060/2070"}},
        {0x167, {"TU116",  "Turing",  "GTX 1660 Ti"}},
        {0x168, {"TU117",  "Turing",  "GTX 1650"}},
        {0x172, {"GA102",  "Ampere",  "RTX 3080/3090"}},
        {0x174, {"GA104",  "Ampere",  "RTX 3060 Ti/3070"}},
        {0x176, {"GA106",  "Ampere",  "RTX 3060"}},
        {0x177, {"GA107",  "Ampere",  "RTX 3050"}},
        {0x192, {"AD102",  "Ada",     "RTX 4090"}},
        {0x193, {"AD103",  "Ada",     "RTX 4080"}},
        {0x194, {"AD104",  "Ada",     "RTX 4070 Ti"}},
        {0x196, {"AD106",  "Ada",     "RTX 4060 Ti"}},
        {0x197, {"AD107",  "Ada",     "RTX 4060"}},
    };
    return m;
}

// Families where the memory subsystem is increasingly owned by signed
// microcontroller firmware. Reads are fine; writes may be reverted.
const std::map<std::string, std::string>& hardened() {
    static const std::map<std::string, std::string> m = {
        {"Turing",
         "Memory reclocking is driven by PMU falcon ucode; values may be "
         "reasserted on p-state change. Re-apply loop is mandatory."},
        {"Ampere",
         "GDDR6/6X link-ECC (EDC) silently retries on marginal timings, so "
         "instability shows up as a bandwidth *loss*, not a crash. Benchmark, "
         "do not just check for artifacts."},
        {"Ada",
         "GSP firmware owns memory training. Writes are frequently reverted "
         "within milliseconds. Verify readback."},
    };
    return m;
}

Arch generic(std::uint32_t chipset) {
    Arch a;
    a.chipset = chipset;
    char buf[32];
    std::snprintf(buf, sizeof buf, "UNKNOWN_%03X", chipset);
    a.codename = buf;
    if (chipset >= 0x130) {
        a.layout = kModern;
        a.family = "Pascal-or-newer";
    } else {
        a.layout = kLegacy;
        a.family = "pre-Pascal";
    }
    a.series = "unknown";
    a.writes_expected = chipset < 0x160;
    a.caveat =
        "Chipset id not in the name table, but the FBPA timing layout is "
        "chosen correctly by the Pascal cutover rule (id >= 0x130 uses the "
        "0x9A0000 aperture, older ids use 0x110000). Timing decode is valid; "
        "only the codename is unknown. Run 'nvtune list' to see the raw id, "
        "and 'nvtune probe' to confirm before writing.";
    return a;
}

}  // namespace

Arch identify(std::uint32_t boot0) {
    const std::uint32_t chipset = (boot0 & 0x1FF00000u) >> 20;
    auto it = chips().find(chipset);
    if (it == chips().end()) return generic(chipset);

    Arch a;
    a.chipset  = chipset;
    a.codename = it->second.codename;
    a.family   = it->second.family;
    a.series   = it->second.series;
    a.layout   = (chipset >= 0x130) ? kModern : kLegacy;
    a.writes_expected = (a.family != "Ada");
    auto h = hardened().find(a.family);
    if (h != hardened().end()) a.caveat = h->second;
    return a;
}

std::string decode_boot0(std::uint32_t boot0) {
    const std::uint32_t chipset = (boot0 & 0x1FF00000u) >> 20;
    const std::uint32_t minor = boot0 & 0xFu;
    const std::uint32_t major = (boot0 >> 4) & 0xFu;
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "BOOT_0=0x%08X chipset=0x%03X (arch=0x%02X impl=0x%X) rev=%u.%u",
                  boot0, chipset, chipset >> 4, chipset & 0xF, major, minor);
    return buf;
}

}  // namespace nvtune
