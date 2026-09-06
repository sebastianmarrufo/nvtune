@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem XP Professional x64 SP2 only. Run as an Administrator.
setlocal EnableExtensions DisableDelayedExpansion
ver | findstr.exe /L /C:"5.2." >nul
if errorlevel 1 (
    echo ERROR: this installer requires the NT 5.2 kernel of XP x64 SP2.
    echo Use install-on-target.cmd for the separate Vista / Windows 7 driver.
    exit /b 1
)
if /i not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
    echo ERROR: use the native 64-bit Command Prompt on XP x64.
    exit /b 1
)
if /i "%~1"=="install" goto install
if /i "%~1"=="start" goto start
if /i "%~1"=="stop" goto stop
if /i "%~1"=="status" goto status
if /i "%~1"=="uninstall" goto uninstall
echo Usage: %~nx0 install ^| start ^| stop ^| status ^| uninstall
echo Keep the XP build of nvtunedrv.sys beside this script.
echo This demand-start non-PnP driver requires an Administrator account.
exit /b 2

:install
if not exist "%~dp0nvtunedrv.sys" (
    echo ERROR: the XP build of nvtunedrv.sys is missing beside the installer.
    exit /b 1
)
sc.exe query nvtunedrv >nul 2>&1
if not errorlevel 1 (
    echo ERROR: nvtunedrv already exists. Use start, or uninstall before replacing it.
    exit /b 1
)
rem SC checks administrator access. No Server-service-dependent net session
rem check, certificate import, or boot configuration change is needed on XP.
rem Kernel ImagePath is a filename, so do not store literal quotes around it.
sc.exe create nvtunedrv type= kernel start= demand binPath= "%~dp0nvtunedrv.sys" DisplayName= "nvtune BAR0 accessor"
if errorlevel 1 (
    echo ERROR: service creation failed. Run from an Administrator account.
    exit /b 1
)
goto start

:start
sc.exe start nvtunedrv
if errorlevel 1 (
    echo ERROR: driver did not start. Check the service error above.
    echo Ensure this is the XP x64 driver and the package remains at its installed path.
    exit /b 1
)
exit /b 0

:stop
sc.exe stop nvtunedrv
exit /b %errorlevel%

:status
sc.exe query nvtunedrv
exit /b %errorlevel%

:uninstall
sc.exe stop nvtunedrv
set "NVT_STOP_RESULT=%errorlevel%"
rem 1062 means the service was already stopped. Preserve it on other failures.
if not "%NVT_STOP_RESULT%"=="0" if not "%NVT_STOP_RESULT%"=="1062" exit /b %NVT_STOP_RESULT%
sc.exe delete nvtunedrv
if errorlevel 1 exit /b 1
echo Service removed. This installer made no certificate or boot-setting changes.
exit /b 0
