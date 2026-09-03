# nvtune

Windows tool for reading and tuning NVIDIA GPU framebuffer (FBPA) memory
timings and clocks.

The repository contains two components, the actual tuning tool, and the driver
it requires.

```
nvtune/
├─ tool/      the usermode program (nvtune.exe) -- C++17
├─ driver/    the kernel driver (nvtunedrv.sys) -- C / WDK
└─ docs/      reference shared by both
```

**`tool/`** is the command-line program. It never touches hardware directly --
Windows has no usermode MMIO path -- so it delegates every register access to
the **`driver/`** kernel driver.

**`driver/`** is the WDM kernel driver. It maps the GPU's BAR0 and enforces an
allowlist (`driver/include/nvtune_ranges.h`) in kernel space, so the usermode
tool can only touch the registers it's meant to. Usermode never supplies a
physical address -- it passes a PCI bus/device/function, and the driver reads
BAR0 out of config space itself and refuses non-NVIDIA-display devices.


## Building

The two halves build with different toolchains and are independent:

- **tool** -- CMake or MSVC. See `tool/README.md`.
- **driver** -- the WDK / EWDK via MSBuild. See `driver/README.md`.

