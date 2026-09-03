#include "nvtune/platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <csignal>
#include <cstdlib>

namespace nvtune::platform {

bool is_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok != 0 && elevation.TokenIsElevated != 0;
}

const char* privilege_name() { return "Administrator"; }

std::string config_dir() {
    const char* base = std::getenv("LOCALAPPDATA");
    if (base == nullptr || *base == '\0') base = std::getenv("USERPROFILE");
    if (base == nullptr || *base == '\0') base = ".";
    return std::string(base) + "\\nvtune";
}

bool ensure_dir(const std::string& path) {
    if (::_mkdir(path.c_str()) == 0) return true;
    struct ::_stat st {};
    return ::_stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
}

bool file_exists(const std::string& path) {
    struct ::_stat st {};
    return ::_stat(path.c_str(), &st) == 0;
}

std::string sanitize_filename(const std::string& s) {
    // Colons are legal in Linux filenames and in PCI slot strings, but on
    // Windows a colon opens an NTFS alternate data stream. Replace the lot.
    std::string out = s;
    for (char& c : out) {
        if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            c = '-';
        }
    }
    return out;
}

void sleep_ms(int ms) { ::Sleep(static_cast<DWORD>(ms)); }

void install_interrupt_handler(void (*handler)(int)) {
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
    std::signal(SIGBREAK, handler);
}

}  // namespace nvtune::platform
