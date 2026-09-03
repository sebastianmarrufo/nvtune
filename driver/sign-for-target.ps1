# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Sebastian Marrufo

<#
.SYNOPSIS
    Test-sign nvtunedrv.sys on a build machine and package it for a *different*
    target machine, without installing or trusting anything locally.

.DESCRIPTION
    Use this when you build on one box and run on another. It:

        1. creates (or reuses) a self-signed code-signing certificate
        2. signs nvtunedrv.sys with it
        3. exports the PUBLIC certificate (.cer) -- no private key
        4. drops a ready-to-run install script into the output folder

    Nothing is added to this machine's trust store, no service is created, and
    test signing is NOT toggled here. All of that happens on the target, via
    the generated deploy\install-on-target.ps1.

    IMPORTANT REALITY CHECK. A self-signed test certificate is not trusted
    anywhere by default. For the driver to load on the target, the target must:
        - have Secure Boot OFF (test signing cannot be enabled with it on)
        - have test signing ON (bcdedit) and be rebooted
        - trust this .cer in LocalMachine\Root and \TrustedPublisher
        - have Memory Integrity / HVCI OFF
    The generated install-on-target.ps1 does the trust + service steps. The
    firmware/boot ones are the operator's job and cannot be scripted from
    userspace.

    The private key stays in this machine's CurrentUser store and is never
    exported. The target only ever receives the public cert, which is all it
    needs to verify the signature.

.PARAMETER SysPath
    Path to the built nvtunedrv.sys. Default: ..\driver\nvtunedrv.sys

.PARAMETER OutDir
    Folder to write the deployable package to. Default: .\deploy

.PARAMETER CertName
    Subject CN of the signing certificate. Default: "nvtune test signing".

.PARAMETER Pfx
    Optional. Sign with an existing PFX (e.g. a shared team cert) instead of a
    self-signed one. You'll be prompted for its password.

.EXAMPLE
    # On the build machine:
    .\sign-for-target.ps1
    # Copy the whole deploy\ folder to the target, then on the target:
    .\install-on-target.ps1

.EXAMPLE
    # Reuse a PFX you already have:
    .\sign-for-target.ps1 -Pfx C:\certs\mysign.pfx
#>

[CmdletBinding()]
param(
    [string]$SysPath  = (Join-Path $PSScriptRoot "..\driver\nvtunedrv.sys"),
    [string]$OutDir   = (Join-Path $PSScriptRoot "deploy"),
    [string]$CertName = "nvtune test signing",
    [string]$Pfx
)

$ErrorActionPreference = "Stop"

function Find-SignTool {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { $_ -and (Test-Path $_) }
    foreach ($root in $roots) {
        $hit = Get-ChildItem -Path $root -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match '\\x64\\' } |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "signtool.exe not found. Install the Windows SDK or run from an EWDK environment."
}

if (-not (Test-Path $SysPath)) {
    throw "$SysPath not found. Build the driver first (windows\driver\build.cmd or the .vcxproj)."
}

$null = New-Item -ItemType Directory -Force -Path $OutDir
$signtool = Find-SignTool

# --- obtain a signing certificate -----------------------------------------
$cert = $null
$usingPfx = $false

if ($Pfx) {
    if (-not (Test-Path $Pfx)) { throw "PFX not found: $Pfx" }
    $pw = Read-Host "PFX password" -AsSecureString
    $cert = Import-PfxCertificate -FilePath $Pfx `
                -CertStoreLocation Cert:\CurrentUser\My -Password $pw
    $usingPfx = $true
    Write-Host "Imported signing cert from PFX: $($cert.Thumbprint)"
} else {
    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -eq "CN=$CertName" } |
            Select-Object -First 1
    if (-not $cert) {
        Write-Host "Creating self-signed code-signing certificate 'CN=$CertName'..."
        $cert = New-SelfSignedCertificate `
            -Subject "CN=$CertName" `
            -Type CodeSigningCert `
            -CertStoreLocation Cert:\CurrentUser\My `
            -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable `
            -NotAfter (Get-Date).AddYears(5)
        Write-Host "  created $($cert.Thumbprint)"
    } else {
        Write-Host "Reusing existing certificate $($cert.Thumbprint)."
    }
}

# --- sign the driver -------------------------------------------------------
$outSys = Join-Path $OutDir "nvtunedrv.sys"
Copy-Item $SysPath $outSys -Force

Write-Host "Signing $outSys ..."
& $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint `
    /t http://timestamp.digicert.com $outSys
if ($LASTEXITCODE -ne 0) {
    # Offline build box: no timestamp server. Sign without one. The signature
    # then expires with the certificate rather than outliving it, which is fine
    # for a test-signed driver.
    Write-Warning "Timestamping failed (offline?). Signing without a timestamp."
    & $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint $outSys
    if ($LASTEXITCODE -ne 0) { throw "signtool failed." }
}

# --- export the PUBLIC certificate (no private key) ------------------------
$cerPath = Join-Path $OutDir "nvtunedrv-cert.cer"
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
Write-Host "Exported public certificate -> $cerPath"

# --- verify the signature we just produced ---------------------------------
Write-Host "`nVerifying signature..."
& $signtool verify /v /pa $outSys
if ($LASTEXITCODE -ne 0) {
    Write-Warning "signtool verify /pa reported an issue. On the build box this"
    Write-Warning "is expected if the cert isn't trusted here -- it will verify"
    Write-Warning "on the target once the .cer is imported. The signature itself"
    Write-Warning "is present."
}

# --- emit the on-target installer ------------------------------------------
$installer = @'
<#
  Run on the TARGET machine, elevated. Copy the whole deploy\ folder over first.

  Prerequisites the operator must handle in firmware / boot (cannot be scripted
  from userspace):
    - Secure Boot OFF   (test signing cannot be enabled while it is on)
    - Memory Integrity / Core Isolation OFF  (Windows Security > Device security)

  This script does the parts that CAN be scripted: trust the certificate,
  enable test signing, register and start the service.
#>
[CmdletBinding()]
param(
    [switch]$EnableTestSigning,
    [switch]$Install,
    [switch]$All,
    [string]$Sys  = (Join-Path $PSScriptRoot "nvtunedrv.sys"),
    [string]$Cer  = (Join-Path $PSScriptRoot "nvtunedrv-cert.cer"),
    [string]$ServiceName = "nvtunedrv"
)
$ErrorActionPreference = "Stop"

function Assert-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this from an elevated PowerShell prompt."
    }
}

function Trust-Cert {
    Assert-Elevated
    if (-not (Test-Path $Cer)) { throw "certificate not found: $Cer" }
    foreach ($store in @("Root","TrustedPublisher")) {
        Import-Certificate -FilePath $Cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        Write-Host "  trusted in LocalMachine\$store"
    }
}

function Enable-TestSigning {
    Assert-Elevated
    & bcdedit.exe /set "{current}" testsigning on
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "bcdedit failed. If Secure Boot is on, disable it in firmware first."
        return
    }
    Write-Host "Test signing enabled. REBOOT before installing the service." -ForegroundColor Yellow
}

function Install-Driver {
    Assert-Elevated
    if (-not (Test-Path $Sys)) { throw "driver not found: $Sys" }
    Trust-Cert
    $full = (Resolve-Path $Sys).Path

    $sig = Get-AuthenticodeSignature $full
    Write-Host "Signature: $($sig.Status)  $($sig.SignerCertificate.Subject)"
    if ($sig.Status -ne "Valid") {
        Write-Warning "Signature is '$($sig.Status)'. If this says NotTrusted, the cert import above"
        Write-Warning "has not taken effect yet, or test signing is off / no reboot has happened."
    }

    $exists = & sc.exe query $ServiceName 2>$null
    if ($LASTEXITCODE -eq 0) {
        & sc.exe stop   $ServiceName | Out-Null
        & sc.exe delete $ServiceName | Out-Null
        Start-Sleep -Milliseconds 500
    }
    & sc.exe create $ServiceName type= kernel start= demand binPath= $full DisplayName= "nvtune BAR0 accessor"
    if ($LASTEXITCODE -ne 0) { throw "sc create failed." }

    & sc.exe start $ServiceName
    if ($LASTEXITCODE -ne 0) {
        Write-Warning @"
sc start failed. Common causes:
  577  signature rejected -> test signing not active (reboot after -EnableTestSigning),
       or the cert isn't trusted (this script imports it; confirm no error above).
  1275 blocked by Memory Integrity / vulnerable-driver blocklist -> turn off
       Core Isolation > Memory Integrity in Windows Security, reboot.
"@
        throw "sc start failed."
    }
    Write-Host "nvtunedrv is running. The tool can now reach BAR0." -ForegroundColor Green
}

if ($All) {
    Enable-TestSigning
    Write-Host "`nReboot now, then re-run:  .\install-on-target.ps1 -Install`n" -ForegroundColor Yellow
    return
}
if ($EnableTestSigning) { Enable-TestSigning; return }
if ($Install)           { Install-Driver;     return }

Write-Host @"
nvtunedrv on-target installer

First time on this machine:
  1. (firmware)  disable Secure Boot
  2. (Windows Security)  disable Core Isolation > Memory Integrity
  3.  .\install-on-target.ps1 -EnableTestSigning
  4.  reboot   (you should see a 'Test Mode' desktop watermark)
  5.  .\install-on-target.ps1 -Install

Already set up, just (re)loading:
     .\install-on-target.ps1 -Install
"@
'@

$installerPath = Join-Path $OutDir "install-on-target.ps1"
Set-Content -Path $installerPath -Value $installer -Encoding UTF8
Write-Host "Wrote on-target installer -> $installerPath"

# --- summary ---------------------------------------------------------------
Write-Host "`n----------------------------------------------------------------"
Write-Host "Deployable package ready in: $OutDir" -ForegroundColor Green
Get-ChildItem $OutDir | ForEach-Object { Write-Host ("  {0,-28} {1,10:N0} bytes" -f $_.Name, $_.Length) }
Write-Host "----------------------------------------------------------------"
Write-Host @"

Copy that entire folder to the target machine, then on the target (elevated):

    .\install-on-target.ps1 -EnableTestSigning
    (reboot)
    .\install-on-target.ps1 -Install

Nothing was installed or trusted on THIS machine.
"@
if (-not $usingPfx) {
    Write-Host "The private key remains in Cert:\CurrentUser\My on this machine and"
    Write-Host "was not exported. Only the public .cer travels to the target."
}
