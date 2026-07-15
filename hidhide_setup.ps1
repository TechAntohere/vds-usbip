# SPDX-License-Identifier: MIT
# Configure HidHide so games see only the vDS virtual controller, not the raw
# Bluetooth DualSense. Whitelists vdsd (so vDS itself keeps HID access), hides
# every Bluetooth-attached DualSense gaming device, and enables cloaking.
# Idempotent. Requires HidHide to be installed.
param(
  [string]$VdsdPath = 'C:\Users\Antonio\Documents\vds\build\vdsd.exe',
  [string]$Cli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Cli)) { throw "HidHideCLI not found at $Cli (install HidHide first)" }

# 1. Whitelist vDS so it can still open the (hidden) Bluetooth controller.
& $Cli --app-reg $VdsdPath | Out-Null

# 2. Find DualSense gaming devices whose base container is Bluetooth (BTHENUM)
#    and hide them. The USB virtual controller (base container USB\...) is left
#    visible so games can use it.
$json = & $Cli --dev-gaming | Out-String
$data = $json | ConvertFrom-Json
$hidden = @()
foreach ($group in $data) {
  foreach ($dev in $group.devices) {
    if ($dev.product -match 'DualSense' -and $dev.baseContainerDeviceInstancePath -match '^BTHENUM') {
      & $Cli --dev-hide $dev.deviceInstancePath | Out-Null
      $hidden += $dev.deviceInstancePath
    }
  }
}

# 3. Enable cloaking.
& $Cli --cloak-on | Out-Null

Write-Host "HidHide configured. Whitelisted: $VdsdPath"
Write-Host ("Hidden BT DualSense devices: {0}" -f ($(if ($hidden) { $hidden -join '; ' } else { 'none found (controller connected?)' })))
