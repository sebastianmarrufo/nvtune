@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Copyright (C) 2026 Sebastian Marrufo
REM ---------------------------------------------------------------------------
REM  Build nvtunedrv.sys.
REM
REM  Two ways to build. The MSBuild path is preferred because it cannot pick
REM  the wrong toolchain -- pass /p:Platform=x64 and you are guaranteed the
REM  64-bit compiler.
REM
REM     msbuild nvtunedrv.vcxproj /p:Configuration=Release /p:Platform=x64
REM
REM  This script is the manual fallback. The single most important thing it
REM  does is force the *x64* cl.exe. The earlier failure --
REM
REM     basetsd.h C4305: truncation from 'UINT_PTR' to 'void *'
REM     wdm.h     C2118: negative subscript
REM
REM  -- is what you get when a 32-bit cl.exe compiles a driver with _AMD64_
REM  defined: pointers are 4 bytes, so the WDK's compile-time sizeof asserts
REM  fail. Defining _AMD64_ does not change the compiler; the compiler has to
REM  actually be the x64 one.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions

REM XP x64 is NT 5.2 (the Server 2003 SP2 kernel), not 32-bit XP's NT 5.1.
REM Compile against that API contract as well as setting the linker target.
REM Keep the default Vista output and signing workflow unchanged.
set "NVT_WINVER=0x0600"
set "NVT_NTDDI=0x06000000"
set "NVT_SUBSYSTEM=6.00"
set "NVT_OSVERSION=6.0"
set "NVT_OBJDIR=obj"
set "NVT_OUTPUT=nvtunedrv.sys"
if /i "%~1"=="xp" (
  set "NVT_WINVER=0x0502"
  set "NVT_NTDDI=0x05020200"
  set "NVT_SUBSYSTEM=5.02"
  set "NVT_OSVERSION=5.2"
  set "NVT_OBJDIR=obj-xp"
  set "NVT_OUTPUT=nvtunedrv-xp.sys"
) else if not "%~1"=="" if /i not "%~1"=="vista" (
  echo Usage: build.cmd [vista ^| xp]
  exit /b 2
)

if "%WindowsSdkDir%"=="" (
  echo ERROR: WindowsSdkDir is not set.
  echo Run this from an EWDK build environment ^(LaunchBuildEnv.cmd^) or a
  echo Visual Studio "x64 Native Tools Command Prompt" with the WDK installed.
  exit /b 1
)

REM --- Resolve the SDK version number ----------------------------------------
if "%Version_Number%"=="" (
  if not "%WindowsSDKVersion%"=="" set "Version_Number=%WindowsSDKVersion:\=%"
)
if "%Version_Number%"=="" (
  echo ERROR: could not determine the SDK version. Set it, e.g.
  echo    set Version_Number=10.0.28000.0
  exit /b 1
)

REM --- Locate the x64 host/x64 target compiler explicitly --------------------
set "CL_X64="
for /f "delims=" %%I in ('where cl.exe 2^>nul') do (
  echo %%~dpI| find /I "\Hostx64\x64\" >nul && if not defined CL_X64 set "CL_X64=%%~fI"
  echo %%~dpI| find /I "\bin\amd64\"   >nul && if not defined CL_X64 set "CL_X64=%%~fI"
  echo %%~dpI| find /I "\x64\"         >nul && if not defined CL_X64 set "CL_X64=%%~fI"
)

if not defined CL_X64 (
  echo ERROR: an x64 cl.exe was not found on PATH.
  echo.
  echo   You are almost certainly in a 32-bit or default tools prompt, which
  echo   is exactly what caused the basetsd.h / wdm.h errors before.
  echo.
  echo   Fix: open "x64 Native Tools Command Prompt for VS 2026" ^(note the
  echo   x64^), or run:  vcvarsall.bat x64
  echo   then re-run this script. Or just use MSBuild:
  echo       msbuild nvtunedrv.vcxproj /p:Configuration=Release /p:Platform=x64
  exit /b 1
)

echo Using x64 compiler:
echo   %CL_X64%
echo.

set "KMINC=%WindowsSdkDir%Include\%Version_Number%\km"
set "SHAREDINC=%WindowsSdkDir%Include\%Version_Number%\shared"
set "KMLIB=%WindowsSdkDir%Lib\%Version_Number%\km\x64"

if not exist "%KMINC%\ntddk.h" (
  echo ERROR: kernel headers not found at "%KMINC%".
  echo The WDK component of the SDK is probably missing or Version_Number is wrong.
  exit /b 1
)
if not exist "%KMLIB%\ntoskrnl.lib" (
  echo ERROR: x64 kernel libs not found at "%KMLIB%".
  exit /b 1
)

if not exist "%NVT_OBJDIR%" mkdir "%NVT_OBJDIR%"

echo Compiling nvtunedrv.c ...
REM Keep /GS enabled. BufferOverflowK initializes the cookie before DriverEntry;
REM BufferOverflowFastFailK instead requires the Windows 8 loader and is invalid
REM for these targets. Check the final XP imports with check-xp-imports.py.
"%CL_X64%" /nologo /c /W4 /WX /O2 /Zi /GS /Gz /kernel /std:c11 ^
   /D_AMD64_ /DAMD64 /D_WIN64 /DNDEBUG ^
   /D_WIN32_WINNT=%NVT_WINVER% /DWINVER=%NVT_WINVER% /DNTDDI_VERSION=%NVT_NTDDI% ^
   /I"%KMINC%" /I"%SHAREDINC%" /Iinclude ^
   /Fo:%NVT_OBJDIR%\ /Fd:%NVT_OBJDIR%\nvtunedrv.pdb ^
   nvtunedrv.c
if errorlevel 1 exit /b 1

echo Linking %NVT_OUTPUT% ...
for %%D in ("%CL_X64%") do set "LINK_X64=%%~dpDlink.exe"

"%LINK_X64%" /nologo /OUT:%NVT_OUTPUT% ^
   /DRIVER /SUBSYSTEM:NATIVE,%NVT_SUBSYSTEM% /OSVERSION:%NVT_OSVERSION% /ENTRY:GsDriverEntry ^
   /NODEFAULTLIB /INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF ^
   /RELEASE /MANIFEST:NO /MACHINE:X64 ^
   /LIBPATH:"%KMLIB%" ^
   ntoskrnl.lib hal.lib wdmsec.lib BufferOverflowK.lib ^
   %NVT_OBJDIR%\nvtunedrv.obj
if errorlevel 1 exit /b 1

echo.
echo Built %NVT_OUTPUT% ^(x64, NT %NVT_OSVERSION%^)
if /i "%~1"=="xp" (
  echo Next: verify the XP kernel imports and use install-on-xp.cmd.
) else (
  echo Next: .\sign-for-target.ps1, then install the signed package on the target.
)
endlocal
