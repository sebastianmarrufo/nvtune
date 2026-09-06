// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only native token checks for the XP platform backend. No driver access.
#include "nvtune/platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <vector>

int main() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE,
                           &token)) {
        std::fprintf(stderr, "OpenProcessToken failed: %lu\n", ::GetLastError());
        return 1;
    }
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!::AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators)) {
        ::CloseHandle(token);
        return 1;
    }

    DWORD size = 0;
    ::GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    if (!size) {
        ::FreeSid(administrators);
        ::CloseHandle(token);
        return 1;
    }
    std::vector<unsigned char> storage(size);
    auto* groups = reinterpret_cast<TOKEN_GROUPS*>(storage.data());
    if (!::GetTokenInformation(token, TokenGroups, groups, size, &size)) {
        ::FreeSid(administrators);
        ::CloseHandle(token);
        return 1;
    }
    bool enabled_admin = false;
    for (DWORD index = 0; index < groups->GroupCount; ++index) {
        const auto& group = groups->Groups[index];
        if (::EqualSid(group.Sid, administrators) &&
                (group.Attributes & SE_GROUP_ENABLED) != 0 &&
                (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) {
            enabled_admin = true;
        }
    }
    int failures = 0;
    if (nvtune::platform::is_elevated() != enabled_admin) {
        std::fprintf(stderr, "FAIL current token: enabled admin=%d\n", enabled_admin);
        ++failures;
    } else {
        std::printf("PASS current token: enabled admin=%d\n", enabled_admin);
    }

    SID_AND_ATTRIBUTES disabled = {administrators, 0};
    HANDLE restricted = nullptr;
    if (!::CreateRestrictedToken(token, 0, 1, &disabled, 0, nullptr,
                                 0, nullptr, &restricted)) {
        std::fprintf(stderr, "CreateRestrictedToken failed: %lu\n", ::GetLastError());
        ++failures;
    } else if (!::ImpersonateLoggedOnUser(restricted)) {
        std::fprintf(stderr, "ImpersonateLoggedOnUser failed: %lu\n", ::GetLastError());
        ++failures;
    } else {
        const bool accepted = nvtune::platform::is_elevated();
        const BOOL reverted = ::RevertToSelf();
        if (accepted || !reverted) {
            std::fprintf(stderr, "FAIL deny-only Administrators SID\n");
            ++failures;
        } else {
            std::printf("PASS deny-only Administrators SID rejected\n");
        }
    }
    if (restricted) ::CloseHandle(restricted);
    ::FreeSid(administrators);
    ::CloseHandle(token);
    std::printf("2 native platform cases, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
