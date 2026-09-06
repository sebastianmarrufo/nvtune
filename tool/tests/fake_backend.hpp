// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nvtune::test {

inline constexpr const char* kSlot = "0000:02:00.0";
inline constexpr const char* kSecondSlot = "0000:03:00.0";
inline constexpr std::uint32_t kBoot0 = 0x13200000;  // GP102, bits [28:20].
inline constexpr std::uint32_t kConfig3 = 0x9A029C;
inline constexpr std::uint32_t kInitialConfig3 = 0x2200194A;  // FAW=12.
inline constexpr std::uint32_t kUpdatedConfig3 = 0x22001B4A;  // FAW=13.

struct BackendState {
    unsigned pci_queries = 0;
    unsigned privilege_checks = 0;
    unsigned open_attempts = 0;
    unsigned readonly_opens = 0;
    unsigned writable_opens = 0;
    unsigned closes = 0;
    unsigned reads = 0;
    unsigned write_attempts = 0;
    unsigned writes = 0;
    unsigned config_dir_calls = 0;
    unsigned ensure_dir_calls = 0;
    unsigned file_exists_calls = 0;
    // A later scope can fail after an earlier scope printed a valid plan.
    std::uint32_t fail_read_offset = 0xFFFFFFFF;
    std::string fail_open_slot;
    bool include_second_gpu = false;
    bool no_devices = false;
    std::map<std::uint32_t, std::uint32_t> words;
    std::vector<std::string> opened_slots;
};

BackendState& backend();
void reset_backend();

}  // namespace nvtune::test
