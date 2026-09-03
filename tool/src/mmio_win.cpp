// Windows BAR0 backend.
//
// Windows has no userspace MMIO path -- no /dev/mem, no equivalent of mmapping
// a PCI resource file. Every access therefore goes through the nvtunedrv
// kernel driver, which validates the offset against an allowlist before
// touching the register.
//
// This costs one DeviceIoControl round trip per dword (a few microseconds), so
// rd_block() uses the driver's batch read rather than looping rd32 -- that
// matters for `probe --watch`, which otherwise issues thousands of ioctls per
// second.

#include "nvtune/mmio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <utility>

#include "nvtunedrv_ioctl.h"

namespace nvtune {

// Shared with cfg_win.cpp (same backend).
std::string last_error_text(DWORD e) {
    char* msg = nullptr;
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string out;
    if (n != 0 && msg != nullptr) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
            out.pop_back();
        }
    } else {
        char buf[64];
        std::snprintf(buf, sizeof buf, "error %lu",
                      static_cast<unsigned long>(e));
        out = buf;
    }
    if (msg != nullptr) ::LocalFree(msg);
    return out;
}

namespace {

}  // namespace

struct Bar0::Impl {
    PciDevice     dev;
    bool          writable = false;
    HANDLE        drv = INVALID_HANDLE_VALUE;
    std::uint32_t handle = 0;
    bool          mapped = false;
    std::size_t   length = 0;
    std::uint64_t phys = 0;

    void ioctl(DWORD code, const void* in, DWORD in_len, void* out,
               DWORD out_len, DWORD* returned, const char* what) const {
        DWORD got = 0;
        const BOOL ok = ::DeviceIoControl(
            drv, code, const_cast<void*>(in), in_len, out, out_len, &got,
            nullptr);
        if (!ok) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_ACCESS_DENIED) {
                throw MmioError(
                    std::string(what) +
                    ": the driver refused this offset. nvtunedrv only permits "
                    "the FBPA apertures and a short list of topology "
                    "registers -- see driver/include/nvtune_ranges.h.");
            }
            throw MmioError(std::string(what) + ": " + last_error_text(e));
        }
        if (returned != nullptr) *returned = got;
    }
};

const char* Bar0::backend_name() noexcept { return "nvtunedrv"; }

Bar0::Bar0(PciDevice dev, bool writable) : impl_(std::make_unique<Impl>()) {
    impl_->dev = std::move(dev);
    impl_->writable = writable;
}

Bar0::~Bar0() { close(); }

Bar0::Bar0(Bar0&&) noexcept = default;
Bar0& Bar0::operator=(Bar0&& o) noexcept {
    if (this != &o) {
        close();
        impl_ = std::move(o.impl_);
    }
    return *this;
}

bool Bar0::is_open() const noexcept { return impl_ && impl_->mapped; }
bool Bar0::writable() const noexcept { return impl_ && impl_->writable; }
std::size_t Bar0::size() const noexcept { return impl_ ? impl_->length : 0; }
std::uint64_t Bar0::phys_base() const noexcept {
    return impl_ ? impl_->phys : 0;
}

void Bar0::open() {
    if (impl_->mapped) return;

    impl_->drv = ::CreateFileA(NVTUNEDRV_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->drv == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            throw MmioError(
                "nvtunedrv is not loaded. Install and start it with "
                "windows\\scripts\\install-driver.ps1, or check "
                "`sc query nvtunedrv`.");
        }
        if (e == ERROR_ACCESS_DENIED) {
            throw MmioError(
                "access to nvtunedrv denied. The device ACL admits only SYSTEM "
                "and Administrators -- run from an elevated prompt.");
        }
        throw MmioError("opening nvtunedrv: " + last_error_text(e));
    }

    NVTUNE_VERSION_OUT ver{};
    impl_->ioctl(IOCTL_NVTUNE_GET_VERSION, nullptr, 0, &ver, sizeof ver,
                 nullptr, "querying driver version");
    if (ver.abi_version != NVTUNEDRV_ABI_VERSION) {
        ::CloseHandle(impl_->drv);
        impl_->drv = INVALID_HANDLE_VALUE;
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "nvtunedrv ABI %u does not match this build's ABI %u. "
                      "Rebuild and reinstall the driver.",
                      ver.abi_version, NVTUNEDRV_ABI_VERSION);
        throw MmioError(buf);
    }

    NVTUNE_MAP_IN in{};
    in.bus = impl_->dev.bus;
    in.device = impl_->dev.dev;
    in.function = impl_->dev.func;
    in.length_hint = static_cast<std::uint32_t>(impl_->dev.bar0_size);

    NVTUNE_MAP_OUT out{};
    impl_->ioctl(IOCTL_NVTUNE_MAP_BAR0, &in, sizeof in, &out, sizeof out,
                 nullptr, "mapping BAR0");

    impl_->handle = out.handle;
    impl_->length = out.length;
    impl_->phys = out.phys_base;
    impl_->mapped = true;

    impl_->dev.bar0_start = out.phys_base;
    impl_->dev.bar0_size = out.length;
}

void Bar0::close() noexcept {
    if (!impl_) return;
    if (impl_->mapped && impl_->drv != INVALID_HANDLE_VALUE) {
        NVTUNE_HANDLE_IN in{};
        in.handle = impl_->handle;
        DWORD got = 0;
        ::DeviceIoControl(impl_->drv, IOCTL_NVTUNE_UNMAP_BAR0, &in, sizeof in,
                          nullptr, 0, &got, nullptr);
        impl_->mapped = false;
    }
    if (impl_->drv != INVALID_HANDLE_VALUE) {
        ::CloseHandle(impl_->drv);
        impl_->drv = INVALID_HANDLE_VALUE;
    }
}

namespace {
void check_offset(const Bar0& bar, std::uint32_t offset, std::size_t len) {
    if (!bar.is_open()) throw MmioError("BAR0 is not mapped");
    if (offset & 3u) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "offset 0x%X is not 32-bit aligned",
                      offset);
        throw MmioError(buf);
    }
    if (static_cast<std::size_t>(offset) + len > bar.size()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "offset 0x%X outside BAR0 (size 0x%zX)",
                      offset, bar.size());
        throw MmioError(buf);
    }
}
}  // namespace

std::uint32_t Bar0::rd32(std::uint32_t offset) const {
    check_offset(*this, offset, 4);
    NVTUNE_READ_IN in{};
    in.handle = impl_->handle;
    in.offset = offset;
    NVTUNE_READ_OUT out{};
    impl_->ioctl(IOCTL_NVTUNE_READ32, &in, sizeof in, &out, sizeof out,
                 nullptr, "reading a register");
    return out.value;
}

void Bar0::wr32(std::uint32_t offset, std::uint32_t value) {
    if (!impl_->writable) {
        throw MmioError("BAR0 opened read-only; this is a bug in the caller");
    }
    check_offset(*this, offset, 4);
    NVTUNE_WRITE_IN in{};
    in.handle = impl_->handle;
    in.offset = offset;
    in.value = value;
    impl_->ioctl(IOCTL_NVTUNE_WRITE32, &in, sizeof in, nullptr, 0, nullptr,
                 "writing a register");
}

std::vector<std::uint32_t> Bar0::rd_block(std::uint32_t offset,
                                          std::size_t count) const {
    check_offset(*this, offset, count * 4);
    std::vector<std::uint32_t> out;
    out.reserve(count);

    std::size_t done = 0;
    while (done < count) {
        const std::size_t chunk =
            (count - done > NVTUNE_MAX_BLOCK_DWORDS)
                ? static_cast<std::size_t>(NVTUNE_MAX_BLOCK_DWORDS)
                : (count - done);

        NVTUNE_READ_BLOCK_IN in{};
        in.handle = impl_->handle;
        in.offset = offset + static_cast<std::uint32_t>(4 * done);
        in.count = static_cast<std::uint32_t>(chunk);

        std::vector<std::uint32_t> buf(chunk, 0u);
        DWORD got = 0;
        impl_->ioctl(IOCTL_NVTUNE_READ_BLOCK, &in, sizeof in, buf.data(),
                     static_cast<DWORD>(chunk * 4), &got, "reading a block");
        if (got != chunk * 4) {
            throw MmioError("short block read from nvtunedrv");
        }
        out.insert(out.end(), buf.begin(), buf.end());
        done += chunk;
    }
    return out;
}

}  // namespace nvtune
