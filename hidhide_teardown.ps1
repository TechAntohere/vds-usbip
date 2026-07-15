# SPDX-License-Identifier: MIT
# Reverse hidhide_setup.ps1: disable cloaking, unhide all DualSense devices,
# and de-whitelist vdsd. For uninstall / troubleshooting.
param(
  [string]$VdsdPath = 'C:\Users\Antonio\Documents\vds\build\vdsd.exe',
  [string]$Cli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
)
$ErrorActionPreference = 'SilentlyContinue'
if (-not (Test-Path $Cli)) { throw "HidHideCLI not found at $Cli" }

& $Cli --cloak-off | Out-Null

# Unhide every currently-hidden device.
$hidden = & $Cli --dev-list 2>&1
foreach ($line in $hidden) {
  if ($line -match '--dev-hide\s+"(.+)"') {
    & $Cli --dev-unhide $matches[1] | Out-Null
  }
}

& $Cli --app-unreg $VdsdPath | Out-Null
Write-Host "HidHide cloaking disabled, DualSense devices unhidden, vdsd de-whitelisted."
