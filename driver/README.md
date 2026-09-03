# nvtunedrv (kernel driver)

The WDM kernel driver that gives the usermode tool bounds-checked access to the
GPU's BAR0 and to PCI config space. C, built with the WDK/EWDK. Windows-only.

## Layout

```
driver/
├─ nvtunedrv.c                 the driver
├─ nvtunedrv.vcxproj           x64 MSBuild project
├─ build.cmd                   forces x64 cl.exe, invokes the compile
├─ include/
│  ├─ nvtunedrv_ioctl.h        the IOCTL wire contract (shared with the tool)
│  └─ nvtune_ranges.h          the MMIO allowlist (the kernel-enforced boundary)
└─ scripts/
   ├─ install-driver.ps1       local test-sign + install
   └─ sign-for-target.ps1      sign + package for a different machine
```

## The security boundary

`nvtune_ranges.h` is the allowlist: every MMIO offset the driver will read or
write, and whether each range is writable. The driver checks every access
against it in kernel space, so a compromised or buggy usermode tool still can't
touch anything outside the FBPA/clock/topology windows. Usermode never supplies
a physical address -- it passes a PCI bus/device/function, and the driver reads
BAR0 out of config space itself and refuses non-NVIDIA-display devices.

## Building

Needs the Windows Driver Kit or Enterprise WDK. 
https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
From x64 Native Tools Command Prompt:

```
build.cmd
```

If using EWDK build environment:
In powershell launch with:
```
.\LaunchBuildEnv.cmd amd64
```
cd to nvtune\driver\ folder

```
msbuild nvtunedrv.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off
```


## Signing and install

Test-signing (development):

```
install-driver.ps1 -Sign -EnableTestSigning
```

Signing for a different machine:

```
sign-for-target.ps1
```
Deployable package will be made in a deploy folder, copy the folder to the target machine
and run 
```
.\install-on-target.ps1 -EnableTestSigning
```
(reboot)
```
.\install-on-target.ps1 -Install
```


