# nvtune (usermode tool)

The command-line program. C++17, Windows-only. Talks to hardware through the
nvtunedrv kernel driver.

## Layout

```
tool/
├─ CMakeLists.txt              build (CMake)
├─ Makefile                    mingw-w64 cross-build convenience
├─ include/nvtune/             public headers (the tool's own API)
├─ src/
│  ├─ main, cli, arch, regs, gpu, vbios, clocks, json   backend-agnostic logic
│  ├─ pci_win.cpp              SetupAPI PCI enumeration
│  ├─ mmio_win.cpp             BAR0 access via the nvtunedrv driver
│  └─ platform_win.cpp         admin check, config dir, error text
├─ tests/
│  ├─ selftest.cpp             CLI preview/commit contract tests
│  ├─ fake_backend.cpp         in-memory PCI, BAR0 and platform services
│  └─ fixtures/                input profile for preview tests
└─ profiles/                   bank-activation-*.json, explore-latency.json
```

## Building
Download WDK or Enterprise WDK from: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk

With a C++ compiler and cmake installed
```
cmake -B build
cmake --build build
```
Or just open from Visual Studio.

Or from Enterprise WDK (Doesn't require installing anything) environment launched within powershell with:

```
.\LaunchBuildEnv.cmd amd64
```
cd into the nvtune\tool folder

```
build.cmd
```

The build pulls the IOCTL contract header from `../driver/include`.

For Windows 7 SP1 x64, use the explicit compatibility build described in
[WINDOWS7.md](../WINDOWS7.md). A normal build with a current compiler is not
automatically a Windows 7 runtime.

Run `ctest --test-dir build -C Release --output-on-failure` after building.
The self-test links an in-memory backend, so it needs neither a GPU nor a
loaded driver and cannot access hardware. `-DBUILD_TESTING=OFF` skips it.

## Commands

`list, fields, dump, get, set, save, restore, apply, daemon, probe, clocks,
peek, poke, vbios`. Run `nvtune` with no arguments for the full usage.

Preview a change without writing registers or creating a stock backup:

```powershell
.\nvtune.exe set -d 0000:01:00.0 RC=42 --dry-run
.\nvtune.exe apply profile.json -d 0000:01:00.0 --dry-run
```

`--dry-run` opens the GPU read-only, including when combined with `--force`.
Changed registers print `[would write]`; a completed preview ends with
`dry run complete: no registers written` and exit status 0. Wrappers must
check both the status and this marker, including when every value is unchanged.
An unavailable selected GPU makes the preview fail.

`set`, `apply` and `restore` retain their existing write-by-default behavior.
They also accept `--commit` to explicitly request that behavior. `--dry-run`
is supported only for `set` and `apply`, and cannot be combined with `--commit`.
An older build rejects `--dry-run`; never retry its preview as a bare `set`.
