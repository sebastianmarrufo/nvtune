# Windows Vista SP2 and Windows 7 SP1 x64

The Vista compatibility build has a Windows 6.0 minimum and also runs on
Windows 7. Build on a modern Windows host, then copy the output to the target.
No compiler, PowerShell, Visual C++ redistributable or GCC runtime needs to be
installed on the target. A loaded kernel driver and a physical supported
NVIDIA card are still required for register access.

## Portable usermode build

Use an x64 mingw-w64 toolchain configured for the system `msvcrt.dll`, such as
w64devkit. A UCRT-targeting MinGW distribution is not interchangeable with it:
stock Vista does not supply UCRT. CMake and Ninja are build-host dependencies.

```powershell
cmake -S tool -B build/vista -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=g++.exe" -DNVTUNE_WINDOWS_VISTA=ON
cmake --build build/vista
ctest --test-dir build/vista --output-on-failure
```

`NVTUNE_WINDOWS_VISTA` sets `WINVER`/`_WIN32_WINNT` to `0x0600`, the PE OS and
subsystem minimums to 6.0, and defaults to static compiler runtimes. It rejects
MSVC and cannot be combined with the existing `NVTUNE_WINDOWS7` option. Current
MSVC or UCRT output must not be labelled Vista-compatible based only on its PE
header. Inspect imports and execute the artifact on the target after changing
toolchains. Copy `build/vista/nvtune.exe`; `--help` and `fields` need no GPU or
driver. A post-build import audit rejects compiler DLLs and UCRT dependencies.
The self-test executable uses only an in-memory backend.

## Driver and development package

Run `driver/build.cmd` in an x64 MSVC/WDK environment. Its manual build targets
kernel 6.0 and retains `/GS`, `GsDriverEntry` and `BufferOverflowK.lib`, which
[Microsoft documents for both Vista and Windows 7](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-drivers-for-different-versions-of-windows).
The IOCTL ABI, administrator ACL, NVIDIA checks and MMIO allowlist are
unchanged. The `.vcxproj` remains the Windows 7 integrated-WDK path; use the
manual build for Vista and with newer kits that lack legacy MSBuild metadata.

Create a test package on the build host:

```powershell
driver/sign-for-target.ps1 -LegacySha1 -OutDir build/vista-driver
```

This explicitly chooses an RSA/SHA-1 test certificate and embedded signature
for the legacy development target. SHA-256 remains the script's default.
[Microsoft's SHA-2 kernel-signing update was provided for Windows 7, not Vista](https://learn.microsoft.com/en-us/security-updates/securityadvisories/2015/3033929).
Legacy test signatures have no timestamp and expire with the certificate.
The private key stays on the build host; only the SYS, public CER and installers
travel to the target. This is not a production-signed driver release.

Keep the package at a permanent path. From an elevated Command Prompt:

```bat
install-on-target.cmd enable-testsigning
rem Reboot before continuing.
install-on-target.cmd install
install-on-target.cmd status
```

The native installer uses Windows' `bcdedit`, `certutil` and `sc`, so it does
not need [Vista's optional PowerShell update](https://devblogs.microsoft.com/powershell/windows-powershell-2-0-on-windows-update/).
It imports the public certificate into LocalMachine Root and TrustedPublisher,
then creates a demand-start service. Before changing certificate stores, it
checks elevation and that test signing is configured in BCDEdit. BCDEdit reports
the configured boot state; rebooting before installation is still required.
It refuses to replace an existing service;
use `uninstall` before replacing its package. After later reboots run `start`.
The kernel service's stored `ImagePath` must be a plain filename, including
any spaces, without embedded quotes. The command script quotes the shell
argument only; literal quotes in the stored path cause Vista error 123.
The package generator explicitly writes the command script with CRLF line
endings, because Vista's CMD can fail label lookup in LF-only batch files.
`stop` and `uninstall` are available. To undo test setup, remove this certificate
by thumbprint from both stores, restore the previous test-signing setting and
reboot. The PowerShell installers remain available for Windows 7 or an updated
Vista target.

## Validation scope

On Windows Vista Ultimate SP2 x64 (6.0.6002), the static executable passed all
17 in-memory CLI contract cases. `--help` and `fields` exited 0; `list` and an
explicit timing preview returned the expected no-NVIDIA failure (1), while
conflicting preview/commit flags and `restore --dry-run` returned usage status 2.

The corrected CRLF native installer created and started the signed driver from
`C:\Druta\nvtune validation\vista-driver`, exercising a real path containing
spaces. Service configuration showed an unquoted kernel image path and the
service reached `RUNNING` with no Win32/service error. Eight native read-only
checks passed: administrator open, ABI/version response, rejection of a short
output buffer, invalid mapping handle, zero read count, non-NVIDIA device and
unknown IOCTL, and access denial for a token with the Administrators SID made
deny-only. No hardware write IOCTL was sent.

All 167 CLI imports and 41 driver imports, including export forwarders, were
also resolved against copies of that guest's actual system DLLs. The retained
Win7 build and the Vista build each passed the 17-case suite on the build host;
the earlier Win7 guest validation remains documented in [WINDOWS7.md](WINDOWS7.md).

The Vista build targets the OS loader and API contract. GPU register access
still requires testing on a physical supported card and its Vista NVIDIA
driver. A VMware display adapter cannot establish BAR0 mapping, timings,
readback or restore behavior. No hardware write is needed for the CLI tests.
Turing and newer GPU code remains present but is not a Vista hardware claim;
it does not add newer Windows runtime dependencies to this build.
