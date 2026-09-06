@echo off
REM ===================================================================
REM  install-on-target.bat  --  nvtune driver install / uninstall
REM
REM  Double-click from Explorer. It self-elevates, then:
REM    - First run  : enables test signing, tells you to reboot.
REM    - After reboot, if NOT yet installed: trusts the cert + loads driver.
REM    - After it's installed: shows a menu -> [R]eload or [U]ninstall.
REM
REM ===================================================================

REM --- self-elevate ---------------------------------------------------
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting Administrator rights...
  powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b
)

cd /d "%~dp0"
set "SVC=nvtunedrv"
set "SYS=%~dp0nvtunedrv.sys"
set "CER=%~dp0nvtunedrv-cert.cer"

REM --- is the service already installed? ------------------------------
set "INSTALLED=no"
sc query %SVC% >nul 2>&1
if %errorlevel% equ 0 set "INSTALLED=yes"

REM If already installed, go straight to the manage menu (install vs remove).
if /i "%INSTALLED%"=="yes" goto :menu

REM --- not installed: we need the payload files to install ------------
if not exist "%SYS%" ( echo ERROR: nvtunedrv.sys not found next to this file. & pause & exit /b 1 )
if not exist "%CER%" ( echo ERROR: nvtunedrv-cert.cer not found next to this file. & pause & exit /b 1 )

REM --- is test signing already on? -----------------------------------
REM  bcdedit prints:  testsigning  Yes  -> for splits into token1/token2.
set "TESTSIGN=off"
for /f "tokens=1,2" %%A in ('bcdedit /enum "{current}" ^| findstr /i "testsigning"') do (
  if /i "%%B"=="Yes" set "TESTSIGN=on"
)

if /i "%TESTSIGN%"=="off" (
  echo Enabling test signing...
  bcdedit /set "{current}" testsigning on
  if errorlevel 1 (
    echo.
    echo bcdedit failed. If Secure Boot is ON, disable it in firmware first,
    echo then run this file again.
    pause & exit /b 1
  )
  echo.
  echo ============================================================
  echo   Test signing enabled.  REBOOT NOW.
  echo   After rebooting, run this file again to load the driver.
  echo ============================================================
  echo.
  pause & exit /b 0
)

REM Test signing on, service not installed -> do a fresh install.
goto :install


REM ===================================================================
:menu
echo.
echo   nvtunedrv is currently INSTALLED.
echo.
echo     [R]  Reload
echo     [U]  Uninstall it
echo     [Q]  Quit
echo.
set "CHOICE="
set /p "CHOICE=Choose R, U, or Q: "
if /i "%CHOICE%"=="R" goto :install
if /i "%CHOICE%"=="U" goto :uninstall
if /i "%CHOICE%"=="Q" exit /b 0
echo Unrecognized choice.
goto :menu


REM ===================================================================
:install
echo.
if not exist "%SYS%" ( echo ERROR: nvtunedrv.sys not found next to this file. & pause & exit /b 1 )
if not exist "%CER%" ( echo ERROR: nvtunedrv-cert.cer not found next to this file. & pause & exit /b 1 )

echo Trusting certificate...
certutil -addstore -f Root "%CER%" >nul
certutil -addstore -f TrustedPublisher "%CER%" >nul

echo (Re)creating the %SVC% service...
sc query %SVC% >nul 2>&1
if %errorlevel% equ 0 (
  sc stop %SVC% >nul 2>&1
  sc delete %SVC% >nul 2>&1
  timeout /t 1 >nul
)
sc create %SVC% type= kernel start= demand binPath= "%SYS%" DisplayName= "nvtune BAR0 accessor"
if errorlevel 1 ( echo ERROR: sc create failed. & pause & exit /b 1 )

sc start %SVC%
if errorlevel 1 (
  echo.
  echo ERROR: sc start failed. Common causes:
  echo   577  = signature rejected  ^(did you reboot after enabling test signing?^)
  echo   1275 = blocked by driver blocklist / HVCI  ^(disable Memory Integrity^)
  echo   2    = nvtunedrv.sys path wrong
  pause & exit /b 1
)

echo.
echo ============================================================
echo   nvtunedrv loaded and running.  You can now use nvtune.exe.
echo ============================================================
echo.
pause
exit /b 0


REM ===================================================================
:uninstall
echo.
echo Stopping and removing the %SVC% service...
sc stop   %SVC% >nul 2>&1
sc delete %SVC% >nul 2>&1

REM Remove the certificate we added to the machine's trust stores. This uses
REM the cert's Subject CN; -delstore removes matching certs. Harmless if absent.
echo Removing the trusted certificate...
certutil -delstore Root "nvtune test signing" >nul 2>&1
certutil -delstore TrustedPublisher "nvtune test signing" >nul 2>&1

echo.
echo nvtunedrv uninstalled (service deleted, certificate untrusted).
echo.
echo NOTE: test signing was left ENABLED. To turn it off too, run (elevated):
echo     bcdedit /set "{current}" testsigning off
echo   then reboot. Leaving it on is harmless if you plan to reinstall.
echo.
pause
exit /b 0
