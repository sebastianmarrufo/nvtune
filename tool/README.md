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
│  ├─ selftest.cpp             allowlist sweep + decode unit tests
│  └─ fixtures/                sample ROMs for the vbios tests
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

## Commands

`list, fields, dump, get, set, save, restore, apply, daemon, probe, clocks,
peek, poke, vbios`. Run `nvtune` with no arguments for the full usage.
