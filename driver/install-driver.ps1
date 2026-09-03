<#
.SYNOPSIS
    Test-sign, install, start, stop and remove nvtunedrv.

.DESCRIPTION
    A test-signed driver still needs a certificate: Windows will not load an
    unsigned binary even with test signing on. What test signing changes is
    *which* root the certificate has to chain to. So the sequence is:

        1. create a self-signed code-signing cert
        2. trust it (Root + TrustedPublisher in the local machine store)
        3. sign nvtunedrv.sys with it
        4. bcdedit /set testsigning on, reboot
        5. sc create + sc start

    Steps 1-3 are -Sign. Step 4 is -EnableTestSigning. Step 5 is -Install.

.EXAMPLE
    # One time, from an elevated prompt:
    .\install-driver.ps1 -Sign -EnableTestSigning
    # ... reboot ...
    .\install-driver.ps1 -Install

.EXAMPLE
    .\install-driver.ps1 -Status
    .\install-driver.ps1 -Stop
    .\install-driver.ps1 -Uninstall
#>

[CmdletBinding()]
param(
    [switch]$Sign,
    [switch]$EnableTestSigning,
    [switch]$Install,
    [switch]$Start,
    [switch]$Stop,
    [switch]$Uninstall,
    [switch]$Status,
    [string]$SysPath = (Join-Path $PSScriptRoot "..\driver\nvtunedrv.sys"),
    [string]$CertName = "nvtune test signing"
)

$ErrorActionPreference = "Stop"
$ServiceName = "nvtunedrv"

function Assert-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script must run from an elevated PowerShell prompt."
    }
}

function Find-SignTool {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { $_ -and (Test-Path $_) }

    foreach ($root in $roots) {
        $hit = Get-ChildItem -Path $root -Filter signtool.exe -Recurse `
                   -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match '\\x64\\' } |
               Sort-Object FullName -Descending |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "signtool.exe not found. Install the Windows SDK, or run this from an EWDK build environment."
}

function Do-Sign {
    Assert-Elevated
    if (-not (Test-Path $SysPath)) {
        throw "$SysPath not found. Build it first with windows\driver\build.cmd."
    }

    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -eq "CN=$CertName" } |
            Select-Object -First 1

    if (-not $cert) {
        Write-Host "Creating self-signed code-signing certificate '$CertName'..."
        $cert = New-SelfSignedCertificate `
            -Subject "CN=$CertName" `
            -Type CodeSigningCert `
            -CertStoreLocation Cert:\CurrentUser\My `
            -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable `
            -NotAfter (Get-Date).AddYears(5)
    } else {
        Write-Host "Reusing existing certificate $($cert.Thumbprint)."
    }

    # Trust it as a root and as a publisher, machine-wide.
    $tmp = Join-Path $env:TEMP "nvtune-test-cert.cer"
    Export-Certificate -Cert $cert -FilePath $tmp -Force | Out-Null
    foreach ($store in @("Root", "TrustedPublisher")) {
        Import-Certificate -FilePath $tmp `
            -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        Write-Host "  trusted in LocalMachine\$store"
    }
    Remove-Item $tmp -Force

    $signtool = Find-SignTool
    Write-Host "Signing with $signtool ..."
    & $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint `
        /t http://timestamp.digicert.com $SysPath
    if ($LASTEXITCODE -ne 0) {
        # Offline box: a timestamp server is unreachable, and that is fine for
        # a test-signed driver -- it just means the signature expires with the
        # certificate rather than outliving it.
        Write-Warning "Timestamping failed (offline?). Signing without a timestamp."
        & $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint $SysPath
        if ($LASTEXITCODE -ne 0) { throw "signtool failed." }
    }
    Write-Host "Signed $SysPath"
}

function Do-EnableTestSigning {
    Assert-Elevated
    Write-Host "Enabling test signing..."
    & bcdedit.exe /set "{current}" testsigning on
    if ($LASTEXITCODE -ne 0) {
        Write-Warning @"
bcdedit failed. If Secure Boot is on, test signing cannot be enabled until you
turn Secure Boot off in firmware.
"@
        return
    }
    Write-Host ""
    Write-Host "Reboot required. After the reboot you should see a"
    Write-Host "'Test Mode' watermark on the desktop." -ForegroundColor Yellow
}

function Do-Install {
    Assert-Elevated
    $full = (Resolve-Path $SysPath).Path

    $sig = Get-AuthenticodeSignature $full
    if ($sig.Status -ne "Valid") {
        Write-Warning "Signature status is '$($sig.Status)'. Run -Sign first, or the load will fail."
    }

    $existing = & sc.exe query $ServiceName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Service already exists; stopping and removing it first."
        & sc.exe stop $ServiceName   | Out-Null
        & sc.exe delete $ServiceName | Out-Null
        Start-Sleep -Milliseconds 500
    }

    Write-Host "Creating service..."
    & sc.exe create $ServiceName type= kernel start= demand `
        binPath= $full DisplayName= "nvtune BAR0 accessor"
    if ($LASTEXITCODE -ne 0) { throw "sc create failed." }

    Do-Start
}

function Do-Start {
    Assert-Elevated
    Write-Host "Starting $ServiceName ..."
    & sc.exe start $ServiceName
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Warning @"
Start failed. The usual causes, in order of likelihood:

  577  - signature rejected. Either test signing is not on yet (reboot after
         -EnableTestSigning), or the .sys is unsigned. Check with:
             bcdedit /enum {current} | findstr testsigning
  1275 - blocked by the vulnerable-driver blocklist or by HVCI/Memory
         Integrity. Turn off Core Isolation > Memory Integrity in Windows
         Security, then reboot.
  2    - binPath is wrong.
"@
        throw "sc start failed."
    }
    Write-Host "Running. The tool can now reach BAR0." -ForegroundColor Green
}

function Do-Stop {
    Assert-Elevated
    & sc.exe stop $ServiceName
}

function Do-Uninstall {
    Assert-Elevated
    & sc.exe stop $ServiceName   2>$null | Out-Null
    & sc.exe delete $ServiceName 2>$null | Out-Null
    Write-Host "Removed $ServiceName."
    Write-Host "The test certificate is still trusted. To remove it:"
    Write-Host "  Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |"
    Write-Host "    Where-Object { `$_.Subject -eq 'CN=$CertName' } | Remove-Item"
    Write-Host "To leave test mode:  bcdedit /set testsigning off   (then reboot)"
}

function Do-Status {
    Write-Host "--- driver binary ---"
    if (Test-Path $SysPath) {
        $f = Get-Item $SysPath
        Write-Host ("  {0}  {1:N0} bytes  {2}" -f $f.FullName, $f.Length, $f.LastWriteTime)
        $sig = Get-AuthenticodeSignature $f.FullName
        Write-Host "  signature: $($sig.Status)  $($sig.SignerCertificate.Subject)"
    } else {
        Write-Host "  not built ($SysPath)"
    }

    Write-Host "--- test signing ---"
    (& bcdedit.exe /enum "{current}") |
        Select-String -Pattern "testsigning|nointegritychecks" |
        ForEach-Object { Write-Host "  $_" }

    Write-Host "--- service ---"
    $q = & sc.exe query $ServiceName 2>$null
    if ($LASTEXITCODE -eq 0) {
        $q | Select-String -Pattern "STATE" | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "  not installed"
    }

    Write-Host "--- device ---"
    if (Test-Path "\\.\nvtunedrv") {
        Write-Host "  \\.\nvtunedrv present"
    } else {
        Write-Host "  \\.\nvtunedrv not present (driver not running, or not elevated)"
    }
}

$did = $false
if ($Sign)              { Do-Sign;              $did = $true }
if ($EnableTestSigning) { Do-EnableTestSigning; $did = $true }
if ($Install)           { Do-Install;           $did = $true }
if ($Start -and -not $Install) { Do-Start;      $did = $true }
if ($Stop)              { Do-Stop;              $did = $true }
if ($Uninstall)         { Do-Uninstall;         $did = $true }
if ($Status)            { Do-Status;            $did = $true }

if (-not $did) {
    Get-Help $PSCommandPath -Detailed
}
