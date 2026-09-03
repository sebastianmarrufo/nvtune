// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// FBPA memory-controller register / bitfield definitions.
//
// Field layouts are transcribed from NVIDIA's published VBIOS Memory Tweak
// Table specification (open-gpu-doc/MemoryTweakTable, table version 0x20),
// which documents CONFIG0..CONFIG5 and the auxiliary fields bit-for-bit.
//
// Register *offsets* within the FBPA aperture are the classic nouveau timing
// block at +0x290..+0x2AC. The CONFIG0 layout in the NVIDIA doc
// (RC[7:0] / RFC[16:8] / RAS[23:17] / RP[30:24]) is byte-identical to
// nouveau's gf100 timing[0] word, which anchors the block.
//
// Independent confirmation: field offsets reverse-engineered by PCI bus
// tracing (with no NVIDIA documentation) land precisely on the CONFIG3/CONFIG4
// bitfields defined here, which cross-checks the table against silicon.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nvtune {

enum class Confidence {
    Documented,  // field layout from NVIDIA spec, offset corroborated in the wild
    Inferred,    // offset derived from block ordering, not directly observed
};

const char* to_string(Confidence c) noexcept;

struct Field {
    const char* name;
    std::uint8_t lsb;
    std::uint8_t width;
    const char* desc;
    // Advisory sane range for a cycle-count field. Absent = unknown.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> typical;
    // False for structural/training values that are not latency knobs.
    bool tunable;

    constexpr std::uint8_t msb() const noexcept {
        return static_cast<std::uint8_t>(lsb + width - 1);
    }
    constexpr std::uint32_t mask() const noexcept {
        return ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << lsb;
    }
    constexpr std::uint32_t maxval() const noexcept {
        return (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    }
    constexpr std::uint32_t extract(std::uint32_t word) const noexcept {
        return (word >> lsb) & maxval();
    }
    std::uint32_t insert(std::uint32_t word, std::uint32_t value) const;
    std::string bits() const;
};

struct Register {
    const char* name;
    std::uint32_t offset;  // relative to the FBPA aperture base
    std::vector<Field> fields;
    Confidence confidence = Confidence::Documented;
    const char* note = "";
};

// The timing block. Offsets are FBPA-relative; add the aperture base for the
// architecture (0x10F000 pre-Pascal, 0x9A0000 Pascal and later).
const std::vector<Register>& timing_registers();   // CONFIG0..CONFIG5
const std::vector<Register>& optional_registers(); // TIMING22
const std::vector<Register>& all_registers();

const Register& config0();
const Register& config3();
const Register& config4();
const Register& timing22();

// Fields documented by NVIDIA in the tweak table whose runtime FBPA register
// location is not established. Surfaced by `nvtune vbios` (parsed straight out
// of the ROM) but not writable at runtime.
struct VbiosOnlyField {
    const char* name;
    const char* desc;
};
const std::vector<VbiosOnlyField>& vbios_only_fields();

struct FieldRef {
    const Register* reg;
    const Field* field;
};

// Accepts "FAW" or "CONFIG3.FAW" (case-insensitive). Throws std::out_of_range.
FieldRef lookup(const std::string& name);
bool try_lookup(const std::string& name, FieldRef& out) noexcept;

// Normalizes a field name (currently a pass-through; kept as the single point
// where any future name normalization would live).
std::string resolve_alias(const std::string& name);

std::string to_upper(std::string s);

}  // namespace nvtune
