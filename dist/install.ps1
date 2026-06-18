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

# Unsigned build → warn if test-signing is off (the driver won't load otherwise).
$ts = bcdedit /enum "{current}" 2>$null | Select-String -Pattern 'testsigning\s+Yes'
if (-not $ts) {
    Write-Warning "Test-signing is OFF. This is an UNSIGNED build, so the driver will not load."
    Write-Host   "  Enable it first:  bcdedit /set testsigning on   (then reboot)" -ForegroundColor Yellow
    Write-Host   "  (Once Argus is signed via SignPath, this step won't be needed on x64.)" -ForegroundColor DarkGray
    Write-Host   "Registering the driver package anyway so it's ready after you enable test-signing..."
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
