# SPDX-License-Identifier: MIT
# Configure HidHide so games see only the vDS virtual controller, not the raw
# Bluetooth DualSense. Whitelists vdsd (so vDS itself keeps HID access), hides
# every Bluetooth-attached DualSense/DualSense Edge gaming device, and enables
# cloaking. Idempotent. Requires HidHide to be installed.
param(
  # If not passed, resolved automatically: (1) the real running vdsd Windows
  # service binary path, if the service is installed, since that is correct
  # regardless of install location; (2) vdsd.exe next to this script, for dev
  # trees/manual runs. Fix for #8: the old hardcoded script-relative default
  # silently whitelisted the wrong (often nonexistent) path for any install
  # that did not happen to place the script next to vdsd.exe.
  [string]$VdsdPath = "",
  [string]$Cli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Cli)) { throw "HidHideCLI not found at $Cli (install HidHide first)" }

function Resolve-VdsdPathForHidHide {
  param([string]$Explicit, [string]$ScriptRoot)

  if (![string]::IsNullOrWhiteSpace($Explicit)) {
    if (!(Test-Path -LiteralPath $Explicit -PathType Leaf)) {
      throw "vdsd.exe not found at explicitly-passed -VdsdPath: $Explicit"
    }
    return $Explicit
  }

  try {
    $svc = Get-CimInstance Win32_Service -Filter "Name='vdsd'" -ErrorAction Stop
    if ($svc -and ![string]::IsNullOrWhiteSpace($svc.PathName)) {
      $exe = $svc.PathName
      if ($exe -match '^"([^"]+)"') { $exe = $Matches[1] }
      elseif ($exe -match '^(\S+\.exe)') { $exe = $Matches[1] }
      if (Test-Path -LiteralPath $exe -PathType Leaf) {
        return $exe
      }
    }
  } catch {
    # vdsd service not installed / WMI unavailable -- fall through to the
    # script-relative default below. Not fatal on its own.
  }

  $Candidate = Join-Path $ScriptRoot 'vdsd.exe'
  if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
    return $Candidate
  }

  throw ("could not locate vdsd.exe automatically (no vdsd service installed, and none found next to this script at $Candidate). Pass -VdsdPath explicitly.")
}

$ResolvedVdsdPath = Resolve-VdsdPathForHidHide -Explicit $VdsdPath -ScriptRoot $PSScriptRoot

# 1. Whitelist vDS so it can still open the (hidden) Bluetooth controller.
& $Cli --app-reg $ResolvedVdsdPath | Out-Null

# 2. Find DualSense/DualSense Edge devices whose base container is Bluetooth
#    (BTHENUM) and hide them. Leave the USB virtual controller (base container
#    USB\...) visible so games can use it.
#
#    Fix for #7: the previous version used '--dev-gaming' and matched on
#    $dev.product -match 'DualSense'. Empirically (checked live against
#    HidHideCLI --dev-all output): HidHide's gamingDevice heuristic reports
#    false for this controller's HID collection, so --dev-gaming returns
#    nothing at all here, and the per-device "product" field is an empty
#    string (the name only appears in the group-level friendlyName) -- so the
#    old match could never succeed even when parsing worked. Separately,
#    HidHideCLI has a known issue where a description field containing raw
#    unescaped backslashes can break ConvertFrom-Json for the whole payload.
#    Fixed by: using --dev-all (not the unreliable gaming filter), matching on
#    VID/PID in the path instead of the empty product field, and falling back
#    to deriving deviceInstancePath from the symbolicLink field via regex when
#    ConvertFrom-Json throws, instead of failing the whole script.
$raw = & $Cli --dev-all | Out-String

function ConvertFrom-VdsSymbolicLink {
  # \\?\hid#{...}_vid&0002054c_pid&0ce6#8&7f45d35&2&0000#{guid} (or, for
  # non-BT/composite devices) \\?\hid#vid_054c&pid_0ce6&mi_03#...#{guid}
  # -> HID\{...}_VID&0002054C_PID&0CE6\8&7f45d35&2&0000 (etc). This mirrors
  # the deviceInstancePath format Windows itself reports for the same node,
  # which is what --dev-hide expects.
  param([string]$SymbolicLink)
  $body = $SymbolicLink -replace '^\\\\\?\\', '' -replace '#\{[0-9a-fA-F-]+\}$', ''
  $parts = $body -split '#'
  if ($parts.Count -lt 2) { return $null }
  return ($parts -join '\')
}

$candidates = @()
try {
  $data = $raw | ConvertFrom-Json
  foreach ($group in $data) {
    foreach ($dev in $group.devices) {
      $candidates += [pscustomobject]@{
        DeviceInstancePath = $dev.deviceInstancePath
        BaseContainer = $dev.baseContainerDeviceInstancePath
        SymbolicLink = $dev.symbolicLink
      }
    }
  }
} catch {
  Write-Warning "HidHideCLI --dev-all JSON did not parse cleanly ($($_.Exception.Message)); falling back to deriving instance paths from symbolicLink."
  $blockPattern = '"symbolicLink"\s*:\s*"([^"]*)"[\s\S]*?"baseContainerDeviceInstancePath"\s*:\s*"([^"]*)"'
  foreach ($m in [regex]::Matches($raw, $blockPattern)) {
    $derivedPath = ConvertFrom-VdsSymbolicLink -SymbolicLink ($m.Groups[1].Value -replace '\\\\', '\')
    $candidates += [pscustomobject]@{
      DeviceInstancePath = $derivedPath
      BaseContainer = ($m.Groups[2].Value -replace '\\\\', '\')
      SymbolicLink = $m.Groups[1].Value
    }
  }
}

$hidden = @()
foreach ($dev in $candidates) {
  if ([string]::IsNullOrWhiteSpace($dev.DeviceInstancePath)) { continue }
  $isDualSense = ($dev.DeviceInstancePath -match 'vid[_&]0002054c[_&]pid[_&]0ce6') -or
                 ($dev.DeviceInstancePath -match 'vid[_&]0002054c[_&]pid[_&]0df2') -or
                 ($dev.SymbolicLink -match 'vid[_&]0002054c[_&]pid[_&]0ce6') -or
                 ($dev.SymbolicLink -match 'vid[_&]0002054c[_&]pid[_&]0df2')
  $isBluetooth = $dev.BaseContainer -match '^BTHENUM'
  if ($isDualSense -and $isBluetooth) {
    & $Cli --dev-hide $dev.DeviceInstancePath | Out-Null
    $hidden += $dev.DeviceInstancePath
  }
}

# 3. Enable cloaking.
& $Cli --cloak-on | Out-Null

Write-Host "HidHide configured. Whitelisted: $ResolvedVdsdPath"
Write-Host ("Hidden BT DualSense devices: {0}" -f ($(if ($hidden) { $hidden -join '; ' } else { 'none found (controller connected?)' })))
