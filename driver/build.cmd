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

if not exist obj mkdir obj

echo Compiling nvtunedrv.c ...
"%CL_X64%" /nologo /c /W4 /WX /O2 /Zi /GS- /Gz /kernel /std:c11 ^
   /D_AMD64_ /DAMD64 /D_WIN64 /DNDEBUG /DPOOL_NX_OPTIN=1 ^
   /I"%KMINC%" /I"%SHAREDINC%" /Iinclude ^
   /Fo:obj\ /Fd:obj\nvtunedrv.pdb ^
   nvtunedrv.c
if errorlevel 1 exit /b 1

echo Linking nvtunedrv.sys ...
for %%D in ("%CL_X64%") do set "LINK_X64=%%~dpDlink.exe"

"%LINK_X64%" /nologo /OUT:nvtunedrv.sys ^
   /DRIVER /SUBSYSTEM:NATIVE,6.01 /ENTRY:DriverEntry ^
   /NODEFAULTLIB /INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF ^
   /RELEASE /MANIFEST:NO /MACHINE:X64 ^
   /LIBPATH:"%KMLIB%" ^
   ntoskrnl.lib hal.lib wdmsec.lib BufferOverflowFastFailK.lib ^
   obj\nvtunedrv.obj
if errorlevel 1 exit /b 1

echo.
echo Built nvtunedrv.sys ^(x64^)
echo Next: ..\scripts\install-driver.ps1 -Sign -Install
endlocal
