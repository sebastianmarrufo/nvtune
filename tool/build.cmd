@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Copyright (C) 2026 Sebastian Marrufo
REM ===================================================================
REM  build.cmd -- compile nvtune.exe with cl.exe, no CMake required.
REM
REM  Run from inside the EWDK's LaunchBuildEnv.cmd prompt, launched for
REM  amd64:   LaunchBuildEnv.cmd amd64
REM  (or a Developer Command Prompt for VS.)
REM
REM  The EWDK starts with -winsdk=none, so INCLUDE/LIB carry only the MSVC
REM  headers, not the ucrt/SDK ones -- that's why a plain cl can't find
REM  crtdbg.h. This script adds the SDK include/lib paths, taking the SDK
REM  version from WindowsTargetPlatformVersion (which setupbuildenv.cmd
REM  always sets) and the roots from WindowsSdkDir / VCToolsInstallDir.
REM
REM  Usage:   build.cmd            build nvtune.exe
REM           build.cmd test       also build selftest.exe
REM ===================================================================

setlocal enableextensions

set "CXXFLAGS=/std:c++17 /EHsc /W4 /O2 /nologo /D_CRT_SECURE_NO_WARNINGS"
set "OWN=/I include /I ..\driver\include"
set "SYSLIBS=setupapi.lib cfgmgr32.lib advapi32.lib"

if not defined VCToolsInstallDir goto :no_env
set "MSVC=%VCToolsInstallDir%"
if not exist "%MSVC%include\yvals.h" goto :no_env

REM Force x64 host/target compiler.
set "PATH=%MSVC%bin\Hostx64\x64;%PATH%"

REM If the environment already put a ucrt path on INCLUDE (e.g. a Developer
REM Command Prompt, or an EWDK launched with an SDK), just trust it.
echo(%INCLUDE%| findstr /i "ucrt" >nul 2>&1
if not errorlevel 1 (
  echo Using the INCLUDE/LIB already set by this environment.
  set "INCS=%OWN%"
  set "LIBS="
  goto :compile
)

REM Otherwise add the SDK paths ourselves.
set "SDK=%WindowsSdkDir%"
if not defined SDK set "SDK=%UniversalCRTSdkDir%"

set "SDKVER=%WindowsTargetPlatformVersion%"
if not defined SDKVER set "SDKVER=%Version_Number%"

if not defined SDK    goto :no_sdk
if not defined SDKVER goto :no_sdk
if not exist "%SDK%Include\%SDKVER%\ucrt\stdio.h" goto :no_sdk

set INCS=%OWN% /I "%MSVC%include" /I "%SDK%Include\%SDKVER%\ucrt" /I "%SDK%Include\%SDKVER%\um" /I "%SDK%Include\%SDKVER%\shared"
set LIBS=/LIBPATH:"%MSVC%lib\x64" /LIBPATH:"%SDK%Lib\%SDKVER%\ucrt\x64" /LIBPATH:"%SDK%Lib\%SDKVER%\um\x64"
echo Added Windows SDK %SDKVER% include/lib paths.

:compile
echo Building nvtune.exe (x64) ...
cl %CXXFLAGS% %INCS% src\*.cpp /Fe:nvtune.exe /link %LIBS% %SYSLIBS%
if errorlevel 1 (echo BUILD FAILED & del /q *.obj 2>nul & exit /b 1)
echo Built nvtune.exe

if /i "%~1"=="test" (
  echo Building selftest.exe ...
  cl %CXXFLAGS% %INCS% tests\selftest.cpp src\arch.cpp src\regs.cpp src\vbios.cpp src\clocks.cpp src\json.cpp src\gpu.cpp src\mmio_win.cpp src\platform_win.cpp src\pci_win.cpp /Fe:selftest.exe /link %LIBS% %SYSLIBS%
  if errorlevel 1 (echo SELFTEST BUILD FAILED & del /q *.obj 2>nul & exit /b 1)
  echo Built selftest.exe -- run it to verify: selftest.exe
)

del /q *.obj 2>nul
endlocal
exit /b 0

:no_sdk
echo ERROR: could not locate the Windows SDK headers.
echo   WindowsSdkDir = %WindowsSdkDir%
echo   SDK version   = %WindowsTargetPlatformVersion%   (Version_Number=%Version_Number%)
echo Expected: "%WindowsSdkDir%Include\<ver>\ucrt\stdio.h"
echo Launch the EWDK for amd64 (LaunchBuildEnv.cmd amd64) and retry.
endlocal
exit /b 1

:no_env
echo ERROR: MSVC toolset not found. Run from the EWDK LaunchBuildEnv.cmd
echo prompt (launched: LaunchBuildEnv.cmd amd64) or a Developer Command
echo Prompt for VS.
endlocal
exit /b 1
