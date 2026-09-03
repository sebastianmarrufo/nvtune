// Windows PCI enumeration.
//
// SetupAPI gives us the display-class devices, their hardware IDs (which carry
// VEN/DEV/SUBSYS/REV), and their bus/device/function. cfgmgr32 then yields the
// PnP-assigned memory resources, from which we take BAR0's base and length --
// authoritative, and without the destructive all-ones size probe.
//
// The BAR0 values obtained here are only a hint. nvtunedrv re-reads the base
// out of PCI config space itself and ignores anything usermode claims, so a
// wrong or hostile value here cannot widen what the driver will touch.

#include "nvtune/pci.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// initguid.h must precede devguid.h: it redefines DEFINE_GUID so the class
// GUIDs are emitted here rather than referenced from uuid.lib. Keeps the link
// line identical between MSVC and mingw-w64.
#include <initguid.h>

#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nvtune {
namespace {

// Pull "VEN_10DE", "DEV_1B80", "SUBSYS_85921043", "REV_A1" out of a hardware ID
// such as PCI\VEN_10DE&DEV_1B80&SUBSYS_85921043&REV_A1.
bool extract_hex_token(const std::string& id, const char* key,
                       std::uint32_t& out, int digits) {
    const std::size_t pos = id.find(key);
    if (pos == std::string::npos) return false;
    const std::size_t start = pos + std::strlen(key);
    if (start + static_cast<std::size_t>(digits) > id.size()) return false;
    const std::string tok = id.substr(start, static_cast<std::size_t>(digits));
    for (char c : tok) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    out = static_cast<std::uint32_t>(std::stoul(tok, nullptr, 16));
    return true;
}

std::string to_narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

bool get_dword_prop(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD prop,
                    DWORD& out) {
    DWORD type = 0, size = 0;
    if (!::SetupDiGetDeviceRegistryPropertyW(set, &data, prop, &type,
                                             reinterpret_cast<PBYTE>(&out),
                                             sizeof(out), &size)) {
        return false;
    }
    return size == sizeof(DWORD);
}

std::string get_hardware_id(HDEVINFO set, SP_DEVINFO_DATA& data) {
    DWORD type = 0, size = 0;
    ::SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, &type,
                                        nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (!::SetupDiGetDeviceRegistryPropertyW(
            set, &data, SPDRP_HARDWAREID, &type,
            reinterpret_cast<PBYTE>(buf.data()), size, &size)) {
        return {};
    }
    return to_narrow(std::wstring(buf.data()));  // first string of the REG_MULTI_SZ
}

// Walk the device's allocated configuration for its first memory range. That
// is BAR0 on every NVIDIA part, and it is what the PnP manager actually
// programmed.
bool get_bar0_resource(DEVINST inst, std::uint64_t& base, std::uint64_t& len) {
    LOG_CONF logconf = 0;
    if (::CM_Get_First_Log_Conf(&logconf, inst, ALLOC_LOG_CONF) != CR_SUCCESS) {
        if (::CM_Get_First_Log_Conf(&logconf, inst, BOOT_LOG_CONF) !=
            CR_SUCCESS) {
            return false;
        }
    }

    RES_DES res = 0;
    CONFIGRET cr = ::CM_Get_Next_Res_Des(&res, logconf, ResType_Mem, nullptr, 0);
    bool found = false;
    while (cr == CR_SUCCESS) {
        ULONG size = 0;
        if (::CM_Get_Res_Des_Data_Size(&size, res, 0) == CR_SUCCESS &&
            size >= sizeof(MEM_RESOURCE)) {
            std::vector<BYTE> buf(size, 0);
            if (::CM_Get_Res_Des_Data(res, buf.data(), size, 0) == CR_SUCCESS) {
                auto* mem = reinterpret_cast<MEM_RESOURCE*>(buf.data());
                base = mem->MEM_Header.MD_Alloc_Base;
                const ULONGLONG end = mem->MEM_Header.MD_Alloc_End;
                len = (end >= base) ? (end - base + 1) : 0;
                found = true;
            }
        }
        RES_DES next = 0;
        const CONFIGRET nr =
            ::CM_Get_Next_Res_Des(&next, res, ResType_Mem, nullptr, 0);
        ::CM_Free_Res_Des_Handle(res);
        if (found) {
            if (nr == CR_SUCCESS) ::CM_Free_Res_Des_Handle(next);
            break;
        }
        res = next;
        cr = nr;
    }
    ::CM_Free_Log_Conf_Handle(logconf);
    return found;
}

}  // namespace

std::string format_slot(std::uint32_t segment, std::uint32_t bus,
                        std::uint32_t dev, std::uint32_t func) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04x:%02x:%02x.%u", segment, bus, dev,
                  func);
    return buf;
}

bool parse_slot(const std::string& slot, std::uint32_t& segment,
                std::uint32_t& bus, std::uint32_t& dev, std::uint32_t& func) {
    unsigned seg = 0, b = 0, d = 0, f = 0;
    if (std::sscanf(slot.c_str(), "%x:%x:%x.%u", &seg, &b, &d, &f) != 4) {
        return false;
    }
    segment = seg; bus = b; dev = d; func = f;
    return true;
}

std::vector<PciDevice> enumerate_gpus(bool all_vendors) {
    std::vector<PciDevice> out;

    HDEVINFO set = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, L"PCI",
                                          nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &data); ++i) {
        const std::string hwid = get_hardware_id(set, data);
        if (hwid.empty()) continue;

        std::uint32_t vendor = 0, device = 0;
        if (!extract_hex_token(hwid, "VEN_", vendor, 4)) continue;
        if (!extract_hex_token(hwid, "DEV_", device, 4)) continue;
        if (!all_vendors && vendor != kVendorNvidia) continue;

        DWORD bus = 0, addr = 0;
        if (!get_dword_prop(set, data, SPDRP_BUSNUMBER, bus)) continue;
        if (!get_dword_prop(set, data, SPDRP_ADDRESS, addr)) continue;

        PciDevice p;
        p.segment = 0;  // HalGetBusDataByOffset is single-segment anyway
        p.bus = bus;
        p.dev = (addr >> 16) & 0xFFFFu;
        p.func = addr & 0xFFFFu;
        p.slot = format_slot(p.segment, p.bus, p.dev, p.func);
        p.vendor = vendor;
        p.device = device;
        p.pci_class = (kPciClassDisplay << 16);

        std::uint32_t subsys = 0;
        if (extract_hex_token(hwid, "SUBSYS_", subsys, 8)) {
            p.subsystem_device = (subsys >> 16) & 0xFFFFu;
            p.subsystem_vendor = subsys & 0xFFFFu;
            p.has_subsystem = true;
        }

        std::uint64_t base = 0, len = 0;
        if (get_bar0_resource(data.DevInst, base, len)) {
            p.bar0_start = base;
            p.bar0_size = len;
        }
        out.push_back(std::move(p));
    }
    ::SetupDiDestroyDeviceInfoList(set);

    std::sort(out.begin(), out.end(),
              [](const PciDevice& a, const PciDevice& b) {
                  return a.slot < b.slot;
              });
    return out;
}

PciDevice find(const std::string& slot) {
    for (const PciDevice& d : enumerate_gpus()) {
        if (d.slot == slot) return d;
        if (d.slot.size() >= slot.size() &&
            d.slot.compare(d.slot.size() - slot.size(), slot.size(), slot) ==
                0) {
            return d;
        }
    }
    throw std::out_of_range("no NVIDIA GPU at '" + slot +
                            "'. Run 'nvtune list' to see what is present.");
}

}  // namespace nvtune
