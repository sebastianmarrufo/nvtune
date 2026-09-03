// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

#include "nvtune/regs.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace nvtune {

const char* to_string(Confidence c) noexcept {
    switch (c) {
        case Confidence::Documented: return "DOCUMENTED";
        case Confidence::Inferred:   return "INFERRED";
    }
    return "?";
}

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::uint32_t Field::insert(std::uint32_t word, std::uint32_t value) const {
    if (value > maxval()) {
        throw std::out_of_range(
            std::string(name) + ": " + std::to_string(value) +
            " out of range for " + std::to_string(width) + "-bit field (0.." +
            std::to_string(maxval()) + ")");
    }
    return (word & ~mask()) | (value << lsb);
}

std::string Field::bits() const {
    if (width == 1) return "[" + std::to_string(lsb) + "]";
    return "[" + std::to_string(msb()) + ":" + std::to_string(lsb) + "]";
}

namespace {

using Range = std::optional<std::pair<std::uint32_t, std::uint32_t>>;
Range r(std::uint32_t lo, std::uint32_t hi) { return std::make_pair(lo, hi); }
const Range kNone = std::nullopt;

const std::vector<Register>& make_timing() {
    static const std::vector<Register> v = {
        {"CONFIG0", 0x290, {
            {"RC",       0, 8, "Row cycle time: ACT to ACT, same bank", r(20, 90), true},
            {"RFC",      8, 9, "Refresh cycle time",                    r(30, 400), true},
            {"RAS",     17, 7, "Row active time: ACT to PRE",           r(10, 60), true},
            {"RP",      24, 7, "Row precharge time: PRE to ACT",        r(8, 50),  true},
        }},
        {"CONFIG1", 0x294, {
            {"CL",       0, 7, "CAS read latency",   r(8, 40), true},
            {"WL",       7, 7, "CAS write latency",  r(2, 30), true},
            {"RD_RCD",  14, 6, "ACT to READ delay",  r(8, 40), true},
            {"WR_RCD",  20, 6, "ACT to WRITE delay", r(8, 40), true},
        }},
        {"CONFIG2", 0x298, {
            {"RPRE",     0, 4, "Read preamble length",  r(1, 8), true},
            {"WPRE",     4, 4, "Write preamble length", r(1, 8), true},
            {"CDLR",     8, 7, "CAS to CAS delay, last read (read data turnaround)", r(4, 40), true},
            {"WR",      16, 7, "Write recovery time: last write data to PRE",        r(8, 40), true},
            {"W2R_BUS", 24, 4, "Write to read bus turnaround", r(0, 12), true},
            {"R2W_BUS", 28, 4, "Read to write bus turnaround", r(0, 12), true},
        }},
        {"CONFIG3", 0x29C, {
            {"PDEX",       0, 5, "Power-down exit latency",                 r(2, 24), true},
            {"PDEN2PDEX",  5, 4, "Power-down entry to power-down exit",     r(1, 12), true},
            {"FAW",        9, 8, "Four-activate window", r(8, 48), true},
            {"AOND",      17, 7, "ACT to ACT, other bank / ODT delay",      r(3, 40), true},
            {"CCDL",      24, 4, "CAS to CAS delay, long (same bank group)", r(1, 12), true},
            {"CCDS",      28, 4, "CAS to CAS delay, short (diff bank group)", r(1, 12), true},
        }},
        {"CONFIG4", 0x2A0, {
            {"REFRESH_LO", 0,  3, "Refresh interval, low bits", kNone, false},
            {"REFRESH",    3, 12, "Refresh interval (tREFI)",   r(100, 4000), true},
            {"RRD",       15,  6, "Row to row activate delay", r(2, 24), true},
            {"DELAY0",    21,  6, "Delay0, low bits (see CONFIG5.DELAY0_MSB/_HI)", kNone, false},
        }},
        {"CONFIG5", 0x2A4, {
            {"ADR_MIN",     0, 3, "Minimum address bus hold",   kNone, false},
            {"WRCRC",       4, 7, "Write CRC latency adder",    r(0, 64), true},
            {"OFFSET0",    12, 6, "Training / phase offset 0",  kNone, false},
            {"DELAY0_MSB", 18, 2, "Delay0, mid bits",           kNone, false},
            {"OFFSET1",    20, 4, "Training / phase offset 1",  kNone, false},
            {"OFFSET2",    24, 4, "Training / phase offset 2",  kNone, false},
            {"DELAY0_HI",  28, 4, "Delay0, high bits",          kNone, false},
        }},
    };
    return v;
}

const std::vector<Register>& make_optional() {
    // The NVIDIA spec names this word TIMING22. If the block is a dense array
    // of TIMINGn at 0x290 + n*4 then n=22 lands at 0x2E8. Not corroborated by
    // any observed trace -- opt-in only.
    static const std::vector<Register> v = {
        {"TIMING22", 0x2E8, {
            {"RFCSBA",  0, 10, "Same-bank refresh, all-bank (tRFCsb)", kNone, true},
            {"RFCSBR", 10,  8, "Same-bank refresh, per-bank",          kNone, true},
         },
         Confidence::Inferred,
         "Offset inferred from TIMINGn = 0x290 + n*4. Verify before writing."},
    };
    return v;
}

const std::map<std::string, FieldRef>& field_index() {
    static const std::map<std::string, FieldRef> idx = [] {
        std::map<std::string, FieldRef> m;
        for (const Register& reg : all_registers()) {
            for (const Field& f : reg.fields) {
                m[to_upper(f.name)] = FieldRef{&reg, &f};
                m[to_upper(std::string(reg.name) + "." + f.name)] =
                    FieldRef{&reg, &f};
            }
        }
        return m;
    }();
    return idx;
}

}  // namespace

const std::vector<Register>& timing_registers()   { return make_timing(); }
const std::vector<Register>& optional_registers() { return make_optional(); }

const std::vector<Register>& all_registers() {
    static const std::vector<Register> v = [] {
        std::vector<Register> out = make_timing();
        for (const Register& r2 : make_optional()) out.push_back(r2);
        return out;
    }();
    return v;
}

const Register& config0()   { return all_registers()[0]; }
const Register& config3()   { return all_registers()[3]; }
const Register& config4()   { return all_registers()[4]; }
const Register& timing22()  { return all_registers()[6]; }

const std::vector<VbiosOnlyField>& vbios_only_fields() {
    static const std::vector<VbiosOnlyField> v = {
        {"R2P",            "Minimum cycles from READ to PRE, same bank (5 bits)"},
        {"RDCRC",          "Read CRC latency adder (4 bits)"},
        {"DRIVE_STRENGTH", "MR1 driver strength (2 bits, GDDR3/GDDR5 semantics)"},
        {"VOLTAGE0..5",    "Per-rail voltage selector indices (3 bits each)"},
    };
    return v;
}

bool try_lookup(const std::string& name, FieldRef& out) noexcept {
    const auto& idx = field_index();
    std::string key = to_upper(name);
    // trim
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front())))
        key.erase(key.begin());
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
        key.pop_back();
    auto it = idx.find(key);
    if (it == idx.end()) return false;
    out = it->second;
    return true;
}

FieldRef lookup(const std::string& name) {
    FieldRef ref{};
    if (!try_lookup(name, ref)) {
        throw std::out_of_range("unknown field '" + name +
                                "'. Run 'nvtune fields' for the full list.");
    }
    return ref;
}


}  // namespace nvtune
