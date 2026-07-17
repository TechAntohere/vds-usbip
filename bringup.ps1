# vDS headless bridge bring-up.
#
# Used when the tray app is NOT installed: starts vdsd (USB/IP transport) and
# attaches the virtual device, with no UI. Registered to run at logon by the
# installer (and once immediately after install). Safe to run repeatedly --
# it no-ops if vdsd is already running / the device is already attached.
$ErrorActionPreference = 'SilentlyContinue'
$dir = $PSScriptRoot

if (-not (Get-Process vdsd -ErrorAction SilentlyContinue)) {
  $env:VDS_TRANSPORT = 'usbip'
  Start-Process -FilePath (Join-Path $dir 'vdsd.exe') -WorkingDirectory $dir -WindowStyle Hidden
}

# Wait (bounded) for vdsd's USB/IP listener before attaching.
for ($i = 0; $i -lt 40; $i++) {
  if (Get-NetTCPConnection -LocalPort 3240 -State Listen -ErrorAction SilentlyContinue) { break }
  Start-Sleep -Milliseconds 250
}

$usbip = 'C:\Program Files\USBip\usbip.exe'
if (Test-Path $usbip) {
  if ((& $usbip port 2>&1) -notmatch '127\.0\.0\.1:3240') {
    & $usbip attach -r 127.0.0.1 -b 1-1 | Out-Null
  }
}
