#include "nvtune/clocks.hpp"

namespace nvtune {

const std::vector<ClockDomain>& clock_domains() {
    // The 0x137xxx PBUS/PCLOCK region. These offsets are the nouveau-era
    // clock-tree neighbourhood; treat every one as INFERRED and confirm on the
    // actual card (see the header). The point of shipping them is to give
    // `nvtune clocks` somewhere to start and something to decode, not to claim
    // byte-exactness across four GPU families.
    static const std::vector<ClockDomain> v = {
        {"gpc/graphics", "shader / core clock (GPCPLL)",
         ClockKind::Pll, 0x137000, 0x137020, "",
         Confidence::Inferred,
         "core clock"},

        {"xbar/crossbar", "L2 <-> FBPA interconnect clock",
         ClockKind::Pll, 0x137040, 0x137060, "",
         Confidence::Inferred,
         "often derived from core on some families; if coef reads 0 it is a "
         "divider, not an independent PLL"},

        {"hub/system", "host / PBUS side clock",
         ClockKind::Pll, 0x137080, 0x1370A0, "",
         Confidence::Inferred,
         "the 'system' domain; frequently a divider off core"},

        {"video/vdec", "NVENC / NVDEC / video engine clock",
         ClockKind::Pll, 0x1370C0, 0x1370E0, "",
         Confidence::Inferred,
         "the video engines' clock"},

        {"rop/l2", "ROP / L2 clock",
         ClockKind::Derived, 0, 0x137100, "gpc/graphics",
         Confidence::Inferred,
         "usually tied to core; read-only reference"},

        {"memory", "FBPA / DRAM clock (MPLL)",
         ClockKind::Pll, 0x132000, 0x132020, "",
         Confidence::Inferred,
         "read-only here; changing it is a full reclock sequence, out of scope"},
    };
    return v;
}

}  // namespace nvtune
