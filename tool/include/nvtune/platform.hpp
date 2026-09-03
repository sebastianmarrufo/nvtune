// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// Thin platform shims so cli.cpp stays free of #ifdef.

#pragma once

#include <string>

namespace nvtune::platform {

// Running with enough privilege to reach BAR0: root on Linux, elevated on
// Windows (the nvtunedrv device ACL admits only SYSTEM and Administrators).
bool is_elevated();

// What to tell the user when they are not. "root" / "Administrator".
const char* privilege_name();

// Where backups live: ~/.nvtune or %LOCALAPPDATA%\nvtune.
std::string config_dir();

bool ensure_dir(const std::string& path);
bool file_exists(const std::string& path);

// Slot strings contain colons, which are not legal in Windows filenames.
std::string sanitize_filename(const std::string& s);

void sleep_ms(int ms);

// Ctrl-C / termination. The handler must be async-signal-safe: set a flag.
void install_interrupt_handler(void (*handler)(int));

}  // namespace nvtune::platform
