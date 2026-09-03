// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// Device abstraction: bind a PCI GPU to an arch layout and a register map.

#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nvtune/arch.hpp"
#include "nvtune/mmio.hpp"
#include "nvtune/pci.hpp"
#include "nvtune/regs.hpp"

namespace nvtune {

// An FBPA scope: the broadcast aperture, or one unicast partition.
using Scope = std::optional<unsigned>;
inline constexpr Scope kBroadcast = std::nullopt;

struct FieldChange {
    std::string   name;
    std::uint32_t old_value;
    std::uint32_t new_value;
};

struct WriteOp {
    const Register* reg = nullptr;
    std::uint32_t   offset = 0;   // absolute BAR0 offset
    std::uint32_t   old_word = 0;
    std::uint32_t   new_word = 0;
    std::vector<FieldChange> changes;
    std::vector<std::string> warnings;

    bool dirty() const noexcept { return new_word != old_word; }
};

// Ordered so that "the value the user typed last wins" is not a thing --
// duplicate assignments are rejected by the CLI before they reach here.
using Assignments = std::map<std::string, std::uint32_t>;

class Gpu {
public:
    Gpu(PciDevice dev, bool writable);

    // Reads BOOT_0 and identifies the chip. Throws MmioError.
    void open();
    void close() noexcept;

    const PciDevice& dev()   const noexcept { return dev_; }
    const Arch&      arch()  const noexcept { return arch_; }
    std::uint32_t    boot0() const noexcept { return boot0_; }
    Bar0&            bar()         noexcept { return bar_; }
    const Bar0&      bar()   const noexcept { return bar_; }

    // -- topology --------------------------------------------------------
    unsigned num_fbpa() const;
    unsigned fbpa_per_fbp() const;
    std::uint32_t floorswept_mask() const;   // set bit = partition disabled
    std::vector<unsigned> active_fbpas() const;
    std::uint32_t fbpa_ram_amount(unsigned index) const;

    // -- aperture --------------------------------------------------------
    std::uint32_t aperture(Scope scope) const;
    std::uint32_t reg_offset(const Register& r, Scope scope) const;

    // -- read ------------------------------------------------------------
    std::uint32_t read_reg(const Register& r, Scope scope) const;
    std::uint32_t read_field(const std::string& name, Scope scope) const;

    // -- planning --------------------------------------------------------
    // Groups field assignments into read-modify-write ops, one per register.
    // Nothing is written; the caller prints these, then opts in via commit().
    std::vector<WriteOp> plan(const Assignments& a, Scope scope) const;

    // -- write -----------------------------------------------------------
    // Returns verification failures; empty means every write read back clean.
    std::vector<std::string> commit(const std::vector<WriteOp>& ops,
                                    bool verify = true);

    // -- backup / restore -------------------------------------------------
    void backup(const std::string& path, bool include_optional = true) const;
    std::vector<std::string> restore(const std::string& path,
                                     bool verify = true);

private:
    static std::vector<std::string> field_warnings(const Field& f,
                                                   std::uint32_t old_v,
                                                   std::uint32_t new_v);

    PciDevice     dev_;
    Bar0          bar_;
    Arch          arch_;
    std::uint32_t boot0_ = 0;
    bool          opened_ = false;
};

}  // namespace nvtune
