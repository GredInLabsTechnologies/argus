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

# Parse pnputil output by BLOCKS, matching VALUES (oemNN.inf / MttVDD.inf) not the field LABELS.
# pnputil localizes the labels ("Published Name" → "Nombre publicado", etc.), so matching the
# English labels silently finds nothing on a non-English Windows. The values are language-neutral.
$enum = pnputil /enum-drivers | Out-String
$removed = 0
foreach ($block in ($enum -split "`r?`n`r?`n")) {
    if ($block -match 'MttVDD\.inf' -and $block -match '(oem\d+\.inf)') {
        $oem = $Matches[1]
        Write-Host "Removing Argus driver package: $oem" -ForegroundColor Cyan
        pnputil /delete-driver $oem /uninstall /force
        if ($LASTEXITCODE -eq 0) { $removed++ }
    }
}
if ($removed -eq 0) { Write-Host "No Argus (MttVDD.inf) package found in the DriverStore." -ForegroundColor Yellow }
else { Write-Host "Removed $removed Argus driver package(s)." -ForegroundColor Green }
