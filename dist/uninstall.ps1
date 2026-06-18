#requires -Version 5.1
<#
  Argus virtual display driver — uninstaller.

  Removes the Argus driver package from the Windows DriverStore. Matches ONLY the package whose
  original name is MttVDD.inf — never touches third-party display drivers. Run elevated.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $admin) { Write-Error "Please run this script elevated (Administrator)."; exit 1 }

$enum = pnputil /enum-drivers | Out-String
$published = $null; $removed = 0
foreach ($line in ($enum -split "`r?`n")) {
    if ($line -match 'Published Name\s*:\s*(oem\d+\.inf)') { $published = $Matches[1] }
    if ($line -match 'Original Name\s*:\s*MttVDD\.inf' -and $published) {
        Write-Host "Removing Argus driver package: $published" -ForegroundColor Cyan
        pnputil /delete-driver $published /uninstall /force
        if ($LASTEXITCODE -eq 0) { $removed++ }
        $published = $null
    }
}
if ($removed -eq 0) { Write-Host "No Argus (MttVDD.inf) package found in the DriverStore." -ForegroundColor Yellow }
else { Write-Host "Removed $removed Argus driver package(s)." -ForegroundColor Green }
