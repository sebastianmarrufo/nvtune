# nvtune

Windows tool for reading and tuning NVIDIA GPU framebuffer (FBPA) memory
timings and clocks. Works on Pascal and earlier, newer generations restrict 
changes.

The repository contains two components, the actual tuning tool, and the driver
it requires.

```
nvtune/
├─ tool/      the usermode program (nvtune.exe) -- C++17
├─ driver/    the kernel driver (nvtunedrv.sys) -- C / WDK
```

**`tool/`** is the command-line program. It never touches hardware directly --
Windows has no usermode MMIO path -- so it delegates every register access to
the **`driver/`** kernel driver.

**`driver/`** is the WDM kernel driver. It maps the GPU's BAR0 and enforces an
allowlist (`driver/include/nvtune_ranges.h`) in kernel space, so the usermode
tool can only touch the registers it's meant to. Usermode never supplies a
physical address -- it passes a PCI bus/device/function, and the driver reads
BAR0 out of config space itself and refuses non-NVIDIA-display devices.

# Brief Usage Guide

Timings depend on current frequency which depends on pstate. For best experience, use 
some form of boost lock. Otherwise, any pstate changes will reset timings/show 
idle timings.

.\nvtune.exe dump 
.\nvtune.exe set RC=xx RAS=yy etc.


## Building

- **tool** --  See `tool/README.md`.
- **driver** --  See `driver/README.md`.


## License

nvtune is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the [GNU General Public License](LICENSE) for details.

Every source file carries an `SPDX-License-Identifier: GPL-3.0-or-later` tag, so
the terms travel with the file if it is copied out of the tree.

