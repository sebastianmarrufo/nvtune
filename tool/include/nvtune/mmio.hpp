// BAR0 MMIO access.
//
// Register access goes through `volatile std::uint32_t*`, which the compiler
// must emit as a single naturally-aligned 32-bit load/store. This is the whole
// reason the C++ version is nicer than a scripted one: no memcpy of
// unspecified width can sneak in, and a torn write to a memory controller
// register is how you hang a GPU.
//
// There is no userspace MMIO path on Windows, so all access goes through the
// nvtunedrv kernel driver (../driver), which validates every offset against an
// allowlist (driver/include/nvtune_ranges.h) in kernel space.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nvtune/pci.hpp"

namespace nvtune {

class MmioError : public std::runtime_error {
public:
    explicit MmioError(const std::string& what) : std::runtime_error(what) {}
};

class Bar0 {
public:
    Bar0(PciDevice dev, bool writable);
    ~Bar0();

    Bar0(const Bar0&) = delete;
    Bar0& operator=(const Bar0&) = delete;
    Bar0(Bar0&&) noexcept;
    Bar0& operator=(Bar0&&) noexcept;

    void open();
    void close() noexcept;

    bool          is_open()  const noexcept;
    bool          writable() const noexcept;
    std::size_t   size()     const noexcept;   // valid after open()
    std::uint64_t phys_base()const noexcept;   // valid after open()

    std::uint32_t rd32(std::uint32_t offset) const;
    void          wr32(std::uint32_t offset, std::uint32_t value);
    std::vector<std::uint32_t> rd_block(std::uint32_t offset,
                                        std::size_t count) const;

    // "mmap(/sys/.../resource0)" or "nvtunedrv". For diagnostics.
    static const char* backend_name() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nvtune
