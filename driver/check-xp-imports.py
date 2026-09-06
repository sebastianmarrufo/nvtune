#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check an XP x64 driver against exports copied from the target's kernel.

Build-host dependency: python -m pip install pefile
No target binaries are executed, patched, or redistributed by this check.
"""

import argparse
import hashlib
import json
from pathlib import Path

import pefile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(driver, kernel_directory):
    image = pefile.PE(str(driver), fast_load=False)
    errors = []
    target = image.OPTIONAL_HEADER
    if image.FILE_HEADER.Machine != 0x8664:
        errors.append("driver must be AMD64")
    if target.Subsystem != 1:
        errors.append("driver must use the native subsystem")
    if (target.MajorSubsystemVersion, target.MinorSubsystemVersion) != (5, 2):
        errors.append("driver subsystem target must be exactly 5.2")
    if (target.MajorOperatingSystemVersion, target.MinorOperatingSystemVersion) != (5, 2):
        errors.append("driver OS target must be exactly 5.2")
    if getattr(image, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        errors.append("unexpected delay imports require a separate audit")
    if not image.verify_checksum():
        errors.append("driver checksum is invalid")
    load_config = getattr(image, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    if not load_config or not load_config.struct.SecurityCookie:
        errors.append("driver must retain its /GS security cookie")
    imports = {}
    providers = {}
    for entry in getattr(image, "DIRECTORY_ENTRY_IMPORT", []):
        dll = entry.dll.decode("ascii").lower()
        if dll not in ("ntoskrnl.exe", "hal.dll"):
            errors.append("unexpected import provider: " + dll)
            continue
        provider_path = kernel_directory / dll
        provider = pefile.PE(str(provider_path), fast_load=False)
        if provider.FILE_HEADER.Machine != 0x8664:
            errors.append(dll + " must be copied from an AMD64 target")
        version = getattr(provider, "VS_FIXEDFILEINFO", [])
        version_number = None
        if version:
            fixed = version[0]
            version_number = [fixed.FileVersionMS >> 16, fixed.FileVersionMS & 0xFFFF,
                              fixed.FileVersionLS >> 16, fixed.FileVersionLS & 0xFFFF]
        if not version_number or version_number[:3] != [5, 2, 3790] or version_number[3] < 3959:
            errors.append(dll + " must be the NT 5.2.3790 SP2 kernel (revision 3959 or later)")
        exports = {
            symbol.name
            for symbol in provider.DIRECTORY_ENTRY_EXPORT.symbols
            if symbol.name and not symbol.forwarder
        }
        providers[dll] = {"sha256": sha256(provider_path), "file_version": version_number}
        imports[dll] = []
        for symbol in entry.imports:
            name = symbol.name.decode("ascii") if symbol.name else "ordinal:" + str(symbol.ordinal)
            imports[dll].append(name)
            if symbol.name not in exports:
                errors.append(dll + " does not directly export " + name)
    if not imports:
        errors.append("driver has no kernel imports")
    return {
        "driver": str(driver),
        "sha256": sha256(driver),
        "target": "Windows XP Professional x64 SP2 / NT 5.2",
        "providers": providers,
        "imports": imports,
        "errors": errors,
        "passed": not errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("kernel_directory", type=Path,
                        help="XP x64 SP2 ntoskrnl.exe and hal.dll copied from System32")
    args = parser.parse_args()
    result = check(args.driver, args.kernel_directory)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
