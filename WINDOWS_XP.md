# Windows XP Professional x64 Edition SP2

This separate build targets XP Professional x64 SP2 (NT 5.2.3790). It includes
the CLI and WDM kernel driver. It does not support 32-bit XP (NT 5.1).
The CLI statically links its compiler runtimes and uses the system msvcrt;
no Python, PowerShell, Visual C++ redistributable, GCC DLLs or UCRT installation
is required on the guest. Driver installation still requires Administrator.

## Build on a modern host

Use an x64 mingw-w64 compiler targeting msvcrt, such as w64devkit. CMake,
Ninja, MSVC and the WDK are build-host tools only.

```powershell
cmake -S tool -B build/xp -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=g++.exe" -DNVTUNE_WINDOWS_XP=ON
cmake --build build/xp
ctest --test-dir build/xp --output-on-failure
```

`NVTUNE_WINDOWS_XP` sets `WINVER` and `_WIN32_WINNT` to `0x0502`,
`NTDDI_VERSION` to `0x05020200`, and PE OS/subsystem minimums to 5.2. It
defaults to static compiler runtimes and rejects MSVC, x86 toolchains and
conflicting legacy targets. The post-build audit rejects non-system DLL
dependencies. Actual target export checks and execution remain necessary
when changing toolchains; lowering a PE header alone does not establish
compatibility.

XP administrator detection checks the effective token's enabled
Administrators SID. A disabled or deny-only SID cannot pass. The Vista and
Win7 targets retain their existing `TokenElevation` path.

Build the separate driver with `driver/build.cmd xp` in an x64 MSVC/WDK
environment. See [driver/WINDOWS_XP.md](driver/WINDOWS_XP.md) for commands,
kernel export auditing and `/GS` startup details. The output is
`driver/nvtunedrv-xp.sys`; rename that file to `nvtunedrv.sys` in the XP
package. Do not substitute the Vista driver.

## Use the portable package

Extract the whole package into a permanent local folder. The CLI runs in
place. From a native 64-bit Command Prompt under Administrator:

```bat
nvtune.exe --help
nvtune.exe fields
driver\install-on-xp.cmd install
nvtune.exe list
```

The installer creates a demand-start kernel service and starts the driver;
it refuses to replace an existing `nvtunedrv` service. Keep its folder in
place while installed. Use `driver\install-on-xp.cmd start` after a reboot,
or `driver\install-on-xp.cmd uninstall` to stop and remove the service.
The XP installer does not change certificates or boot settings.

To preview a timing change, always pass `set --dry-run` or `apply --dry-run`.
A bare `set` retains its existing write behavior. For example:

```bat
nvtune.exe set FAW=13 --dry-run
```

The portable test folder contains an in-memory CLI suite and a native token
suite. Run `selftest.exe .` from that folder and `platform_selftest.exe` to
repeat them. These tests do not install a driver or write GPU registers.

## Validation

The CLI was built with w64devkit GCC 16.1.0. On the existing VMware XP
Professional x64 SP2 guest (5.2.3790, native system files 5.2.3790.3959), all
17 in-memory CLI contract cases and both native administrator-token checks
passed. `--help` and `fields` returned 0; `list` and an explicit timing preview
returned 1 because the guest has no NVIDIA GPU. Conflicting preview/commit
flags and `restore --dry-run` returned usage status 2.

All 168 CLI imports resolve against that guest's actual System32 exports,
including forwarded exports into NTDLL. The two test executables also pass
the actual-export audit. The CLI depends only on ADVAPI32, KERNEL32, msvcrt
and SETUPAPI. The native 5.2 driver retains `/GS`, its device ACL, NVIDIA
device checks, register allowlist and IOCTL ABI; all 41 kernel/HAL imports
resolve against the guest kernel files.

The XP driver loaded from `C:\Druta-XP-Test\nvtunedrv.sys` through a native
SCM helper, with an unquoted, normalized `\??\` image path and a running
demand-start service. All eight read-only driver checks passed: administrator
open, ABI/version response, short-buffer/invalid-handle/zero-count rejection,
non-NVIDIA mapping rejection, unknown-IOCTL rejection and deny-only
administrator-token access denial. No hardware write IOCTL was sent.
XP's unavailable code-integrity information class was reported as information.

Cleanup stopped the driver, deleted the test service and removed its copied
SYS. A subsequent service query confirmed absence (1060). No certificates,
boot settings or account policies were changed. A baseline VMware snapshot
was retained. The stock CMD installer passed 12 host-side mocked command
cases; guest loading and cleanup used native SCM helpers rather than that
batch script. Logs and build hashes accompany the delivered artifact.

The retained Vista and Win7 targets each pass the 17-case host suite. The
rebuilt Vista executable is byte-for-byte identical to the previously
guest-tested Vista artifact. The default Vista driver has identical code
and imports after adding the separate XP build mode.

This VM can verify OS loading, API contracts and failure paths. It has no
physical NVIDIA GPU, so BAR0 mapping, timing reads/writes, readback and
restore remain unverified. No GPU generation support is inferred from the
OS build target. Newer architecture code remains available without adding
newer Windows runtime imports. There is no production signing claim.
