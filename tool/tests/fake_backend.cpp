// SPDX-License-Identifier: GPL-3.0-or-later
// Fake CLI backend: deterministic reads and no possible hardware writes.

#include "nvtune/mmio.hpp"
#include "nvtune/arch.hpp"
#include "nvtune/pci.hpp"
#include "nvtune/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

#include "nvtune/regs.hpp"

namespace nvtune {

namespace {
PciDevice fake_device(const std::string& slot = "0000:08:00.0") {
    PciDevice d;
    d.slot = slot;
    d.vendor = kVendorNvidia;
    d.device = 0x1B80;
    return d;
}
std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> words;
void (*interrupt_handler)(int) = nullptr;

std::string number(std::uint32_t value) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex << std::setw(8)
       << std::setfill('0') << value;
    return os.str();
}
void log_line(const std::string& line) {
    if (const char* path = std::getenv("NVTUNE_FAKE_LOG")) {
        std::ofstream(path, std::ios::app) << line << '\n';
    }
}
std::uint32_t initial_word(std::uint32_t offset) {
    if (offset == NV_PMC_BOOT_0)
        return std::getenv("NVTUNE_FAKE_UNKNOWN") ? 0x1FE00000u : 0x13400000u;
    if (offset == NV_PTOP_SCAL_NUM_FBPAS) return 2;
    if (offset == NV_FUSE_STATUS_OPT_FBIO) return 0;
    if (offset == NV_PTOP_SCAL_NUM_FBPA_PER_FBP) return 1;
    for (const Register& reg : all_registers()) {
        if (offset == kModern.broadcast + reg.offset ||
            offset == kModern.unicast + reg.offset ||
            offset == kModern.unicast + kModern.unicast_stride + reg.offset) {
            const bool second = offset == kModern.unicast +
                                          kModern.unicast_stride + reg.offset;
            std::uint32_t value = 0x0108100Au;
            if (reg.name == std::string("CONFIG0")) {
                if (const char* rc = std::getenv("NVTUNE_FAKE_RC"))
                    value = (value & ~0xFFu) | static_cast<std::uint32_t>(std::atoi(rc));
                if (std::getenv("NVTUNE_FAKE_DIVERGENT") && second) ++value;
            }
            return value;
        }
    }
    return 0;
}
}

std::vector<PciDevice> enumerate_gpus(bool) {
    if (std::getenv("NVTUNE_FAKE_TWO_GPUS"))
        return {fake_device(), fake_device("0000:09:00.0")};
    return {fake_device()};
}
PciDevice find(const std::string& slot) {
    if (slot == "0000:08:00.0" || slot == "08:00.0") return fake_device();
    if (slot == "0000:09:00.0" || slot == "09:00.0")
        return fake_device("0000:09:00.0");
    throw std::out_of_range("fake device not found: " + slot);
}

struct Bar0::Impl { PciDevice dev; bool writable; bool open = false; };
Bar0::Bar0(PciDevice dev, bool writable) : impl_(std::make_unique<Impl>()) {
    impl_->dev = std::move(dev);
    impl_->writable = writable;
}
Bar0::~Bar0() {
    if (!impl_ || !std::getenv("NVTUNE_FAKE_LOG")) return;
    for (const Register& reg : all_registers()) {
        for (std::uint32_t base : {kModern.broadcast, kModern.unicast,
                                   kModern.unicast + kModern.unicast_stride}) {
            const std::uint32_t offset = base + reg.offset;
            const auto it = words.find({impl_->dev.slot, offset});
            const std::uint32_t value = it == words.end() ? initial_word(offset)
                                                            : it->second;
            log_line("FINAL " + impl_->dev.slot + " " + number(offset) +
                     " " + number(value));
        }
    }
}
Bar0::Bar0(Bar0&&) noexcept = default;
Bar0& Bar0::operator=(Bar0&&) noexcept = default;
void Bar0::open() { impl_->open = true; }
void Bar0::close() noexcept { impl_->open = false; }
bool Bar0::is_open() const noexcept { return impl_->open; }
bool Bar0::writable() const noexcept { return impl_->writable; }
std::size_t Bar0::size() const noexcept { return 0x1000000; }
std::uint64_t Bar0::phys_base() const noexcept { return 0; }
const char* Bar0::backend_name() noexcept { return "fake"; }

std::uint32_t Bar0::rd32(std::uint32_t offset) const {
    if (!impl_->open) throw MmioError("fake backend closed");
    const auto it = words.find({impl_->dev.slot, offset});
    return it == words.end() ? initial_word(offset) : it->second;
}
void Bar0::wr32(std::uint32_t offset, std::uint32_t value) {
    if (!impl_->writable) throw MmioError("fake backend is read-only");
    words[{impl_->dev.slot, offset}] = value;
    log_line("WRITE " + impl_->dev.slot + " " + number(offset) + " " +
             number(value));
}
std::vector<std::uint32_t> Bar0::rd_block(std::uint32_t offset,
                                          std::size_t count) const {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < count; ++i) out.push_back(rd32(offset + 4 * i));
    return out;
}

namespace platform {
bool is_elevated() { return true; }
const char* privilege_name() { return "Administrator"; }
std::string config_dir() { return "."; }
bool ensure_dir(const std::string& path) {
    return std::filesystem::exists(path) || std::filesystem::create_directories(path);
}
bool file_exists(const std::string& path) { return std::filesystem::exists(path); }
std::string sanitize_filename(const std::string& s) {
    std::string out = s;
    for (char& c : out) if (c == ':') c = '_';
    return out;
}
void sleep_ms(int ms) {
    if (std::getenv("NVTUNE_FAKE_STOP_AFTER_CYCLE") && interrupt_handler) {
        interrupt_handler(0);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
void install_interrupt_handler(void (*handler)(int)) {
    interrupt_handler = handler;
}
}

} // namespace nvtune
