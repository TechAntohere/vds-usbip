# vDS manual launcher + timer.
# Run:  powershell -ExecutionPolicy Bypass -File C:\Users\Antonio\Documents\vds\launch.ps1
# Starts vdsd (USB/IP transport), attaches the virtual DualSense, and reports
# how long each phase takes. The physical controller must be awake/connected.
$ErrorActionPreference = 'SilentlyContinue'
$Build = 'C:\Users\Antonio\Documents\vds\build'
$Usbip = 'C:\Program Files\USBip\usbip.exe'

Write-Host "Tearing down any existing session..." -ForegroundColor DarkGray
& $Usbip detach -p 1 2>&1 | Out-Null
Stop-Process -Name vdsd -Force -EA SilentlyContinue
Start-Sleep 2

$sw = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host "Starting vdsd (USB/IP transport)..." -ForegroundColor Cyan
$env:VDS_TRANSPORT = 'usbip'
Start-Process -FilePath (Join-Path $Build 'vdsd.exe') -WorkingDirectory $Build -WindowStyle Hidden

Write-Host "Waiting for the USB/IP server (controller must be connected)..."
while (-not (Get-NetTCPConnection -LocalPort 3240 -State Listen -EA SilentlyContinue)) {
    if ($sw.Elapsed.TotalSeconds -gt 30) {
        Write-Host "  timed out after 30s -- is the DualSense awake? (press the PS button)" -ForegroundColor Yellow
        return
    }
    Start-Sleep -Milliseconds 50
}
$tReady = $sw.Elapsed.TotalSeconds
Write-Host ("  vdsd server ready in {0:N1}s" -f $tReady) -ForegroundColor Green

Write-Host "Attaching virtual device..."
& $Usbip attach -r 127.0.0.1 -b 1-1 2>&1 | Out-Null

Write-Host "Waiting for audio endpoints to enumerate..."
while ($true) {
    $n = (Get-PnpDevice -Class AudioEndpoint -EA SilentlyContinue |
          Where-Object { $_.FriendlyName -match 'DualSense' -and $_.Status -eq 'OK' } |
          Measure-Object).Count
    if ($n -ge 2) { break }
    if ($sw.Elapsed.TotalSeconds -gt 30) { Write-Host "  endpoints not ready in time" -ForegroundColor Yellow; break }
    Start-Sleep -Milliseconds 100
}
$tEnd = $sw.Elapsed.TotalSeconds
$sw.Stop()

Write-Host ""
Write-Host ("===  READY in {0:N1}s  ===" -f $tEnd) -ForegroundColor Green
Write-Host ("  vdsd server:  {0:N1}s" -f $tReady)
Write-Host ("  endpoints OK: {0:N1}s" -f $tEnd)
Get-PnpDevice -Class AudioEndpoint |
    Where-Object { $_.FriendlyName -match 'DualSense' -and $_.Present } |
    ForEach-Object { Write-Host ("  - {0}: {1}" -f $_.FriendlyName, $_.Status) }
Write-Host ""
Write-Host "Tip: run the tray app (VdsTray.exe) for the normal managed experience." -ForegroundColor DarkGray
