// SPDX-License-Identifier: GPL-3.0-or-later
// This translation unit replaces every MMIO, PCI and platform implementation.
// Do not link *_win.cpp into this test: no driver handle or hardware is used.

#include "fake_backend.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

#include "nvtune/arch.hpp"
#include "nvtune/mmio.hpp"
#include "nvtune/pci.hpp"
#include "nvtune/platform.hpp"

namespace nvtune::test {

BackendState& backend() {
    static BackendState state;
    return state;
}

void reset_backend() {
    backend() = BackendState{};
    backend().words = {
        {NV_PMC_BOOT_0, kBoot0},
        {NV_PTOP_SCAL_NUM_FBPAS, 2},
        {NV_FUSE_STATUS_OPT_FBIO, 0},
        {kConfig3, kInitialConfig3},
        {0x90029C, kInitialConfig3},
        {0x90429C, kInitialConfig3},
    };
}

}  // namespace nvtune::test

namespace nvtune {

struct Bar0::Impl {
    PciDevice dev;
    bool writable;
    bool opened = false;
};

Bar0::Bar0(PciDevice dev, bool writable)
    : impl_(std::make_unique<Impl>(Impl{std::move(dev), writable, false})) {}
Bar0::~Bar0() { close(); }
Bar0::Bar0(Bar0&&) noexcept = default;
Bar0& Bar0::operator=(Bar0&&) noexcept = default;

void Bar0::open() {
    ++test::backend().open_attempts;
    if (impl_->opened) throw MmioError("fake BAR0 was opened twice");
    if (impl_->dev.slot == test::backend().fail_open_slot)
        throw MmioError("injected fixture open failure");
    impl_->opened = true;
    if (impl_->writable) ++test::backend().writable_opens;
    else ++test::backend().readonly_opens;
    test::backend().opened_slots.push_back(impl_->dev.slot);
}

void Bar0::close() noexcept {
    if (impl_ && impl_->opened) {
        impl_->opened = false;
        ++test::backend().closes;
    }
}

bool Bar0::is_open() const noexcept { return impl_ && impl_->opened; }
bool Bar0::writable() const noexcept { return impl_ && impl_->writable; }
std::size_t Bar0::size() const noexcept { return 0x1000000; }
std::uint64_t Bar0::phys_base() const noexcept { return 0; }
const char* Bar0::backend_name() noexcept { return "test-only in-memory BAR0"; }

std::uint32_t Bar0::rd32(std::uint32_t offset) const {
    ++test::backend().reads;
    if (!is_open()) throw MmioError("read from closed fake BAR0");
    if (offset == test::backend().fail_read_offset)
        throw MmioError("injected fixture read failure");
    const auto found = test::backend().words.find(offset);
    return found == test::backend().words.end() ? 0 : found->second;
}

void Bar0::wr32(std::uint32_t offset, std::uint32_t value) {
    ++test::backend().write_attempts;
    if (!is_open() || !writable())
        throw MmioError("write attempted on read-only fake BAR0");
    ++test::backend().writes;
    test::backend().words[offset] = value;
}

std::vector<std::uint32_t> Bar0::rd_block(std::uint32_t offset,
                                        std::size_t count) const {
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < count; ++i)
        words.push_back(rd32(offset + static_cast<std::uint32_t>(i * 4)));
    return words;
}

std::string format_slot(std::uint32_t segment, std::uint32_t bus,
                        std::uint32_t dev, std::uint32_t func) {
    char value[48];
    std::snprintf(value, sizeof value, "%04x:%02x:%02x.%x", segment, bus, dev, func);
    return value;
}

bool parse_slot(const std::string& slot, std::uint32_t& segment,
                std::uint32_t& bus, std::uint32_t& dev, std::uint32_t& func) {
    // Only this test fixture exists; accepting other slots would hide bad
    // targeting in the CLI invocation under test.
    if (slot != test::kSlot && slot != "02:00.0") return false;
    segment = 0; bus = 2; dev = 0; func = 0;
    return true;
}

std::vector<PciDevice> enumerate_gpus(bool) {
    ++test::backend().pci_queries;
    if (test::backend().no_devices) return {};
    PciDevice dev;
    dev.slot = test::kSlot;
    dev.bus = 2;
    dev.vendor = kVendorNvidia;
    dev.device = 0x1B02;
    dev.pci_class = kPciClassDisplay;
    dev.bar0_size = 0x1000000;
    std::vector<PciDevice> devices{dev};
    if (test::backend().include_second_gpu) {
        dev.slot = test::kSecondSlot;
        dev.bus = 3;
        devices.push_back(dev);
    }
    return devices;
}

PciDevice find(const std::string& slot) {
    for (const auto& dev : enumerate_gpus()) {
        if (dev.slot == slot || dev.slot.substr(5) == slot) return dev;
    }
    throw std::out_of_range("test fixture has no device " + slot);
}

}  // namespace nvtune

namespace nvtune::platform {

bool is_elevated() { ++test::backend().privilege_checks; return true; }
const char* privilege_name() { return "fixture Administrator"; }
std::string config_dir() {
    ++test::backend().config_dir_calls;
    return "__nvtune_test_no_files__";
}
bool ensure_dir(const std::string&) {
    ++test::backend().ensure_dir_calls;
    return true;  // Never creates a directory.
}
bool file_exists(const std::string&) {
    ++test::backend().file_exists_calls;
    return true;  // Existing stock backup suppresses all backup file writes.
}
std::string sanitize_filename(const std::string&) { return "fixture"; }
void sleep_ms(int) { throw std::logic_error("unexpected daemon/watch in CLI test"); }
void install_interrupt_handler(void (*)(int)) {
    throw std::logic_error("unexpected interrupt handler in CLI test");
}

}  // namespace nvtune::platform
