@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Run from an elevated Command Prompt. Works on stock Vista without PowerShell.
setlocal EnableExtensions DisableDelayedExpansion
if /i "%~1"=="enable-testsigning" goto enable
if /i "%~1"=="install" goto install
if /i "%~1"=="start" goto start
if /i "%~1"=="stop" goto stop
if /i "%~1"=="status" goto status
if /i "%~1"=="uninstall" goto uninstall
echo Usage: %~nx0 enable-testsigning ^| install ^| start ^| stop ^| status ^| uninstall
echo Enable test signing once, reboot, then install the development driver.
echo Keep nvtunedrv.sys and nvtunedrv-cert.cer beside this script.
exit /b 2

:enable
call :require_admin
if errorlevel 1 exit /b 1
bcdedit.exe /set {current} testsigning on
if errorlevel 1 exit /b 1
echo Reboot before running install. This changes this target's boot settings.
exit /b 0

:install
call :require_admin
if errorlevel 1 exit /b 1
bcdedit.exe /enum {current} | findstr.exe /R /I /C:"testsigning *Yes" >nul
if errorlevel 1 (
    echo ERROR: test signing is not configured for this boot entry.
    echo Run enable-testsigning, reboot, then install.
    exit /b 1
)
rem BCDEdit reports configured boot settings. The reboot is still required.
if not exist "%~dp0nvtunedrv.sys" (
    echo ERROR: nvtunedrv.sys is missing beside the installer.
    exit /b 1
)
if not exist "%~dp0nvtunedrv-cert.cer" (
    echo ERROR: nvtunedrv-cert.cer is missing beside the installer.
    exit /b 1
)
sc.exe query nvtunedrv >nul 2>&1
if not errorlevel 1 (
    echo ERROR: nvtunedrv already exists. Use start, or uninstall before replacing it.
    exit /b 1
)
certutil.exe -addstore -f Root "%~dp0nvtunedrv-cert.cer"
if errorlevel 1 exit /b 1
certutil.exe -addstore -f TrustedPublisher "%~dp0nvtunedrv-cert.cer"
if errorlevel 1 exit /b 1
rem Quotes group the shell argument only. Kernel ImagePath is a filename, not
rem a usermode service command line; storing literal quotes makes Vista fail 123.
sc.exe create nvtunedrv type= kernel start= demand binPath= "%~dp0nvtunedrv.sys" DisplayName= "nvtune BAR0 accessor"
if errorlevel 1 exit /b 1
goto start

:start
call :require_admin
if errorlevel 1 exit /b 1
sc.exe start nvtunedrv
if errorlevel 1 (
    echo ERROR: driver did not start. Check the service error above.
    echo Error 577: verify the signature and reboot after enabling test signing.
    exit /b 1
)
exit /b 0

:stop
call :require_admin
if errorlevel 1 exit /b 1
sc.exe stop nvtunedrv
exit /b %errorlevel%

:status
sc.exe query nvtunedrv
exit /b %errorlevel%

:uninstall
call :require_admin
if errorlevel 1 exit /b 1
sc.exe stop nvtunedrv
sc.exe delete nvtunedrv
if errorlevel 1 exit /b 1
echo Service removed. The public test certificate and boot settings remain.
echo Remove this certificate by thumbprint from Root and TrustedPublisher if no longer needed.
echo To restore normal signing: bcdedit /set {current} testsigning off, then reboot.
exit /b 0

:require_admin
rem Listing filters is read-only and needs elevation even if the Server service
rem is stopped. Unlike net session, this does not depend on that service.
fltmc.exe filters >nul 2>&1
if errorlevel 1 (
    echo ERROR: run this script from an elevated Command Prompt.
    exit /b 1
)
exit /b 0
