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

`list, fields, dump, get, set, save, restore, apply, daemon, probe,
peek, poke, vbios`. Run `nvtune` with no arguments for the full usage.

`save` writes a raw register backup for `restore` (by default in the tool's
config directory). To also capture a field profile for `apply` or `daemon`,
select one GPU and provide a separate profile path:

```
nvtune save --device 0000:08:00.0 --profile timings.json
nvtune daemon --profile timings.json
```

The profile contains current documented, tunable timing fields as integer
values. It records the source device, which `apply` and `daemon` use by
default. Export requires the active partitions to agree with broadcast values;
if they differ, a single profile cannot represent them and export stops.
`--output backup.json` chooses a raw
backup path, and `restore --input backup.json` restores it. A field profile is
not a raw backup and cannot be used with `restore`.
With `--profile` and no `--output`, the default stock backup is written only
if it does not already exist, so a
later profile capture cannot replace the original rollback values. Providing
`--output` explicitly writes that raw backup path on each save. Profile export
requires a recognized GPU chipset.
