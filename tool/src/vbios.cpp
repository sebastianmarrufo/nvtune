#include "nvtune/vbios.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>

#include "nvtune/gpu.hpp"
#include "nvtune/regs.hpp"

namespace nvtune::vbios {
namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Try to interpret a header at `off`. Returns nothing if it does not hold up.
std::optional<TweakTable> plausible(const std::vector<std::uint8_t>& blob,
                                    std::size_t off) {
    if (off + kHeaderSize > blob.size()) return std::nullopt;

    const std::uint8_t ver    = blob[off + 0];
    const std::uint8_t hsize  = blob[off + 1];
    const std::uint8_t bsize  = blob[off + 2];
    const std::uint8_t esize  = blob[off + 3];
    const std::uint8_t ecount = blob[off + 4];
    const std::uint8_t count  = blob[off + 5];

    if (ver != kTweakVersion || hsize != kHeaderSize ||
        bsize != kBaseEntrySize || esize != kExtEntrySize) {
        return std::nullopt;
    }
    if (count < 1 || count > 64 || ecount > 16) return std::nullopt;

    const std::size_t stride = static_cast<std::size_t>(bsize) +
                               static_cast<std::size_t>(esize) * ecount;
    const std::size_t end = off + hsize + stride * count;
    if (end > blob.size()) return std::nullopt;

    TweakTable table;
    table.file_offset     = off;
    table.version         = ver;
    table.entry_count     = count;
    table.ext_entry_count = ecount;
    table.base_entry_size = bsize;
    table.ext_entry_size  = esize;

    for (unsigned i = 0; i < count; ++i) {
        const std::uint8_t* raw = blob.data() + off + hsize + stride * i;

        TweakEntry e;
        e.index = i;
        for (unsigned c = 0; c < 6; ++c) e.config[c] = le32(raw + 4 * c);

        const std::uint8_t b47 = raw[47], b48 = raw[48], b49 = raw[49],
                           b50 = raw[50], b51 = raw[51];
        e.drive_strength = b47 & 0x3u;
        e.voltages[0] = (b47 >> 2) & 0x7u;
        e.voltages[1] = (b47 >> 5) & 0x7u;
        e.voltages[2] = b48 & 0x7u;
        e.r2p         = (b48 >> 3) & 0x1Fu;
        e.voltages[3] = b49 & 0x7u;
        e.voltages[4] = (b49 >> 4) & 0x7u;
        e.voltages[5] = b50 & 0x7u;
        e.rdcrc       = b51 & 0xFu;
        e.timing22    = le32(raw + 56);

        const auto& tregs = timing_registers();
        for (std::size_t c = 0; c < tregs.size() && c < 6; ++c) {
            for (const Field& f : tregs[c].fields) {
                e.fields[f.name] = f.extract(e.config[c]);
            }
        }
        for (const Field& f : timing22().fields) {
            e.fields[f.name] = f.extract(e.timing22);
        }

        table.entries.push_back(std::move(e));
    }

    // Sanity gate: stock RC is a two-digit cycle count on every real part. A
    // random header-shaped byte run will not satisfy this across all entries.
    for (const TweakEntry& e : table.entries) {
        const std::uint32_t rc = e.field("RC");
        if (rc < 8 || rc > 200) return std::nullopt;
    }
    return table;
}

}  // namespace

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw VbiosError("cannot open " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_prom(Gpu& gpu) {
    // Clearing NV_PBUS_ROM_ACCESS exposes the ROM mirror. Some parts do not
    // need it; poking it is harmless and we always put it back, including if
    // the read throws.
    const bool can_write = gpu.bar().writable();
    std::uint32_t saved = 0;
    bool poked = false;
    if (can_write) {
        try {
            saved = gpu.bar().rd32(NV_PBUS_ROM_ACCESS);
            if (saved != 0) {
                gpu.bar().wr32(NV_PBUS_ROM_ACCESS, 0);
                poked = true;
            }
        } catch (const MmioError&) {
            poked = false;
        }
    }

    std::vector<std::uint8_t> blob;
    try {
        const std::size_t dwords = NV_PROM_SIZE / 4;
        std::vector<std::uint32_t> words = gpu.bar().rd_block(NV_PROM_BASE,
                                                              dwords);
        blob.resize(words.size() * 4);
        for (std::size_t i = 0; i < words.size(); ++i) {
            blob[4 * i + 0] = static_cast<std::uint8_t>(words[i] & 0xFF);
            blob[4 * i + 1] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
            blob[4 * i + 2] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
            blob[4 * i + 3] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
        }
    } catch (...) {
        if (poked) {
            try { gpu.bar().wr32(NV_PBUS_ROM_ACCESS, saved); } catch (...) {}
        }
        throw;
    }
    if (poked) {
        try { gpu.bar().wr32(NV_PBUS_ROM_ACCESS, saved); } catch (...) {}
    }

    if (blob.size() < 2 || blob[0] != 0x55 || blob[1] != 0xAA) {
        throw VbiosError(
            "PROM does not start with the 55 AA PCI expansion ROM signature. "
            "The mirror may be gated on this board; try --rom with a dump from "
            "GPU-Z or nvflash.");
    }
    return blob;
}

TweakTable find_tweak_table(const std::vector<std::uint8_t>& blob) {
    // Search by signature rather than walking the BIT table, because the BIT
    // pointer for this table is not part of the public spec. The 4-byte header
    // prefix plus the field-plausibility gate is specific enough in practice.
    const std::uint8_t sig[4] = {kTweakVersion, kHeaderSize, kBaseEntrySize,
                                 kExtEntrySize};
    if (blob.size() < sizeof sig) {
        throw VbiosError("ROM image is too small to contain a tweak table");
    }
    for (std::size_t i = 0; i + sizeof sig <= blob.size(); ++i) {
        if (std::memcmp(blob.data() + i, sig, sizeof sig) != 0) continue;
        if (auto t = plausible(blob, i)) return *t;
    }
    throw VbiosError(
        "no Memory Tweak Table found. The image may be compressed, may use a "
        "table version other than 0x20, or may be a partial dump.");
}

}  // namespace nvtune::vbios
