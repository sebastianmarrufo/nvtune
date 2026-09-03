// Clock-tree readout for Fermi..Pascal.
//
// The clock domains on these parts are driven by PLLs in the PBUS/PCLOCK
// region around BAR0 +0x137000. Each domain is either a PLL (with N/M/P
// coefficients) or a divider off another domain's output.
//
// The PLL frequency math is exact and family-independent:
//
//     f_out = f_ref * N / (M * 2^P)
//
// with f_ref the 27 MHz crystal on these boards. What differs across
// gf100 (Fermi) / gk104 (Kepler) / gm10x (Maxwell) / gp10x (Pascal) is the
// *offset* of each domain's coefficient register. nouveau carries a separate
// clk driver per family for exactly this reason.
//
// Rather than hard-code offsets I cannot verify for every family, this exposes:
//   - the decode (correct everywhere),
//   - a table of candidate offsets tagged by confidence,
//   - and (via the existing peek/probe commands) a way to confirm them against
//     a known-good clock on the actual card.
//
// The domains the user is usually after here are the "non-standard" ones:
// video (NVENC/NVDEC), system/hub, and xbar/crossbar -- the ones no vendor
// tool surfaces.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nvtune/regs.hpp"   // Confidence

namespace nvtune {

// How a domain derives its frequency.
enum class ClockKind {
    Pll,        // has a coefficient register: N/M/P
    Divider,    // integer/fractional divide off a source domain
    Derived,    // tied 1:1 to another domain
};

struct ClockDomain {
    const char* name;        // "gpc", "video", "xbar", "hub/system", ...
    const char* human;       // description
    ClockKind   kind;
    // For Pll: ctrl_off is the PLL enable/control reg, coef_off the N/M/P word.
    // For Divider/Derived: coef_off is the divider register, src names source.
    std::uint32_t ctrl_off;
    std::uint32_t coef_off;
    const char* src;         // source domain for Divider/Derived, else ""
    Confidence  confidence;
    const char* note;
};

// Coefficient bit layout. The common Fermi+ form packs the PLL word as
//   M [7:0]   N [15:8]   P [18:16]
// but a couple of families shift these. Exposed as a struct so the decoder
// and the "verify against a known clock" path both use one definition.
struct PllLayout {
    std::uint8_t m_lsb, m_width;
    std::uint8_t n_lsb, n_width;
    std::uint8_t p_lsb, p_width;
};

inline constexpr PllLayout kPllCommon{0, 8, 8, 8, 16, 3};

struct PllCoef {
    std::uint32_t n, m, p;
    bool valid;   // M != 0 and looks sane
};

inline PllCoef decode_pll(std::uint32_t coef_word, const PllLayout& L = kPllCommon) {
    auto ext = [&](std::uint8_t lsb, std::uint8_t w) {
        return (coef_word >> lsb) & ((1u << w) - 1u);
    };
    PllCoef c;
    c.m = ext(L.m_lsb, L.m_width);
    c.n = ext(L.n_lsb, L.n_width);
    c.p = ext(L.p_lsb, L.p_width);
    c.valid = (c.m != 0);
    return c;
}

// kHz, given the reference crystal (default 27 MHz).
inline std::uint32_t pll_freq_khz(const PllCoef& c,
                                  std::uint32_t ref_khz = 27000) {
    if (!c.valid) return 0;
    std::uint64_t vco = static_cast<std::uint64_t>(ref_khz) * c.n;
    return static_cast<std::uint32_t>(vco / (static_cast<std::uint64_t>(c.m)
                                             << c.p));
}

// The candidate domain table. Offsets in the 0x137xxx region. Everything here
// is tagged INFERRED except where a value is cross-checkable, because the exact
// per-family offset is not something to assert from memory. Use `nvtune clocks`
// to read them and compare against a known clock; use `peek`/`probe` to correct
// any that don't line up on your specific card, then update this table.
const std::vector<ClockDomain>& clock_domains();

}  // namespace nvtune
