# Windows XP Professional x64 SP2 driver

`build.cmd xp` compiles the same WDM driver for NT 5.2, the kernel used by
Windows XP Professional x64 Edition SP2. This is a separate target from the
Vista / Windows 7 package. It does not support 32-bit Windows XP.

The IOCTL ABI, Administrator/SYSTEM-only device ACL, NVIDIA display-device
check, MMIO bounds and write allowlist are unchanged. No GPU registers are
written by installing or starting the driver. Hardware tuning still requires
validation with a supported physical NVIDIA GPU.

## Build and audit on a modern build host

Install an x64 MSVC compiler and the WDK kernel headers/libraries, then open an
x64 Native Tools Command Prompt. The build was compiled with MSVC
14.51.36231 and WDK 10.0.28000.0. This is a source compatibility build; those
current tools do not themselves promise support for XP.

```bat
cd driver
set "WindowsSdkDir=C:\Program Files (x86)\Windows Kits\10\"
set "Version_Number=10.0.28000.0"
build.cmd xp
```

This selects `_WIN32_WINNT=WINVER=0x0502` and `NTDDI_VERSION=0x05020200`
during compilation, then links `/SUBSYSTEM:NATIVE,5.02 /OSVERSION:5.2`.
The output is `nvtunedrv-xp.sys`; intermediates go into `obj-xp`. The default
`build.cmd` still emits the separate Vista-targeted `nvtunedrv.sys`.
The Visual Studio project is not an XP build path.

Stack protection stays enabled with `/GS` and the `GsDriverEntry` startup
from `BufferOverflowK.lib`. Do not substitute `BufferOverflowFastFailK.lib`,
which requires newer kernel-loader support. `wdmsec.lib` supplies the legacy
`IoCreateDeviceSecure` implementation so the device ACL is not weakened.

Copy `ntoskrnl.exe` and `hal.dll` from the XP x64 SP2 target's
`%SystemRoot%\System32` into a build-host audit directory. Do not redistribute
these Windows files. On the build host only:

```bat
python -m pip install pefile
python check-xp-imports.py nvtunedrv-xp.sys C:\xp-kernel-copy
```

The audit checks AMD64/native 5.2 targets, the driver checksum and security
cookie, and every static import against the actual NT 5.2.3790 SP2 kernel and
HAL exports. It rejects other providers, unresolved imports, newer kernel
copies, and delay imports. Passing this audit is necessary but cannot replace
actually loading the driver and exercising its IOCTL contract on XP.

## Install on XP x64

Copy the XP build to a folder on the target's local disk, such as
`C:\nvtune-xp`, under the filename `nvtunedrv.sys`, beside
`install-on-xp.cmd`. A VMware shared folder, UNC path or mapped network drive
is not a kernel-driver installation location. Keep the local folder in place
while the service is installed. From a native 64-bit Command Prompt running
as an Administrator:

```bat
install-on-xp.cmd install
install-on-xp.cmd status
```

The installer creates the demand-start, non-PnP `nvtunedrv` kernel service.
It refuses to replace an existing service. The optional `start` and `stop`
commands control that service; `uninstall` stops it and removes it.

```bat
install-on-xp.cmd uninstall
```

This XP path does not import certificates or change boot settings. Microsoft's
mandatory x64 kernel-mode signing policy starts with Vista; this non-PnP XP
driver does not require Vista's BCDEdit/test-signing workflow. See Microsoft's
[kernel-mode code-signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-requirements--windows-vista-and-later-).
Continue to use `sign-for-target.ps1` and `install-on-target.cmd` for the
separate Vista / Windows 7 build.

## Validation status

The NT 5.2 build compiles with warnings treated as errors. All 41 static
imports resolve against `ntoskrnl.exe` and `hal.dll` copied from an actual
XP x64 SP2 guest, both version 5.2.3790.3959. The audit also rejects the Vista
driver target and newer kernel copies. Twelve host mock cases cover the CMD
installer, including paths with spaces and service creation/start/stop errors.
The default Vista rebuild retains identical executable `.text` and imports.

On 2026-09-06, that driver loaded successfully on an actual Windows XP
Professional x64 SP2 VM (NT 5.2 build 3790). Guest installation used a native
Service Control Manager validation helper to copy the driver onto the local
disk, create a demand-start kernel service and verify the running state.
XP normalized the service's filename to `\??\C:\Druta-XP-Test\nvtunedrv.sys`.
The CMD installer itself was exercised by the twelve host mock cases above.

All eight read-only driver checks passed: administrator open, ABI/version
query, short-output rejection, invalid-handle rejection, zero-count rejection,
non-NVIDIA-device rejection, unknown-IOCTL rejection and denial of a token
whose Administrator SID was made deny-only. The XP code-integrity information
query returned `STATUS_INVALID_INFO_CLASS`; the harness treats that optional
Vista-era diagnostic as informational on XP.

These checks sent no write IOCTLs. No physical NVIDIA hardware results are
claimed here.
