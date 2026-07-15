# SPDX-License-Identifier: MIT
# vDS auto-start: ensure vdsd is running (USB/IP transport) and the virtual
# DualSense is attached. Safe to run repeatedly (idempotent). Registered to
# run at logon; the usbip-win2 driver also auto-reattaches persistent devices
# at boot (see `usbip port -s`), so this mainly guarantees vdsd is up and
# covers the case where the boot-time reattach raced ahead of vdsd.
$ErrorActionPreference = 'SilentlyContinue'

$Build  = 'C:\Users\Antonio\Documents\vds\build'
$Usbip  = 'C:\Program Files\USBip\usbip.exe'
$VdsdExe = Join-Path $Build 'vdsd.exe'
$Log = 'C:\ProgramData\vDS\autostart.log'
function LogLine($m) { "$(Get-Date -Format o)  $m" | Out-File -FilePath $Log -Append -Encoding utf8 }
LogLine "=== autostart run ==="

# 1. Start vdsd if not already running.
if (-not (Get-Process -Name vdsd -ErrorAction SilentlyContinue)) {
  $env:VDS_TRANSPORT = 'usbip'
  Start-Process -FilePath $VdsdExe -WorkingDirectory $Build -WindowStyle Hidden
  LogLine "started vdsd"
} else { LogLine "vdsd already running" }

# 2. Wait (up to ~15s) for the USB/IP server to start listening on 3240.
$listening = $false
for ($i = 0; $i -lt 30; $i++) {
  if (Get-NetTCPConnection -LocalPort 3240 -State Listen -ErrorAction SilentlyContinue) { $listening = $true; break }
  Start-Sleep -Milliseconds 500
}
LogLine "listening=$listening after $($i*0.5)s"

# 3. Attach only if the device isn't already imported.
$port = & $Usbip port 2>&1 | Out-String
LogLine "port output: $($port.Trim())"
if ($port -notmatch '127\.0\.0\.1:3240') {
  $r = & $Usbip attach -r 127.0.0.1 -b 1-1 2>&1 | Out-String
  LogLine "attach result: $($r.Trim())"
} else { LogLine "already attached, skip" }
LogLine "=== done ==="
