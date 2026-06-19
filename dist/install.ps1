#requires -Version 5.1
<#
  Argus virtual display driver — installer.

  Registers the Argus IddCx driver next to this script (MttVDD.inf/.dll/.cat). Argus is the
  on-demand virtual-display engine behind Istro: after install it sits at 0 monitors until a
  controller asks (or until numVirtualDisplays > 0 in vdd_settings.xml).

  This build is UNSIGNED (SignPath Foundation signing pending) → it loads only with test-signing
  enabled. Run elevated (Administrator).
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$inf  = Join-Path $here 'MttVDD.inf'

# Elevation
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $admin) { Write-Error "Please run this script elevated (Administrator)."; exit 1 }
if (-not (Test-Path $inf)) { Write-Error "MttVDD.inf not found next to this script ($here)."; exit 1 }

# A driver with no TRUSTED signature cannot be installed: Windows rejects unsigned drivers even with
# test-signing ON (test-signing requires a trusted *test* signature, not the *absence* of one). Detect
# it up front and explain, instead of letting pnputil fail with a cryptic code (observed: exit 259).
$cat = Join-Path $here 'mttvdd.cat'
$sigStatus = if (Test-Path $cat) { (Get-AuthenticodeSignature $cat).Status } else { 'NotSigned' }
if ($sigStatus -ne 'Valid') {
    Write-Warning "The driver catalog (mttvdd.cat) has no trusted signature (status: $sigStatus)."
    Write-Host "  Windows will NOT install an unsigned/untrusted driver, even with test-signing on." -ForegroundColor Yellow
    Write-Host "  Use a SIGNED Argus release (signed via SignPath), or sign mttvdd.cat with a trusted" -ForegroundColor Yellow
    Write-Host "  test certificate first. This v0.1.0 pre-release ships unsigned for the signing pipeline." -ForegroundColor Yellow
    exit 4
}

# Signed with a TEST cert (not SignPath/OV) -> the driver needs test-signing enabled to load.
$ts = bcdedit /enum "{current}" 2>$null | Select-String -Pattern 'testsigning\s+Yes'
if (-not $ts) {
    Write-Warning "Test-signing is OFF. If this build is test-signed (not SignPath-signed), the driver"
    Write-Host   "  won't load until you enable it:  bcdedit /set testsigning on   (then reboot)." -ForegroundColor Yellow
    Write-Host   "  (A SignPath OV-signed release loads without test-signing on x64.)" -ForegroundColor DarkGray
}

Write-Host "Registering Argus driver: $inf" -ForegroundColor Cyan
pnputil /add-driver "$inf" /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) {
    Write-Error "pnputil /add-driver failed (exit $LASTEXITCODE)."; exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Argus driver registered." -ForegroundColor Green
Write-Host "It is ON-DEMAND (starts with 0 monitors). To get a monitor:"
Write-Host "  - With a controller (e.g. Istro): it drives the pipe \\.\pipe\ArgusDisplay (ADD / REMOVE <i> / PING)."
Write-Host "  - Standalone: set numVirtualDisplays > 0 in vdd_settings.xml to pre-connect monitors at startup."
Write-Host ""
Write-Host "To remove later: run uninstall.ps1 elevated."
