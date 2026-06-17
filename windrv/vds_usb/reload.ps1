# SPDX-License-Identifier: MIT

param(
    [string]$PackageDir = ""
)

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Join-Path $ProjectDir "package"
}

function Test-VdsUsbRootDevice {
    param(
        [Parameter(Mandatory = $true)]
        $Device
    )

    if ($Device.InstanceId -eq "ROOT\DEVGEN\VDSUSB0" -or
        $Device.FriendlyName -eq "vDS Virtual DualSense USB Root") {
        return $true
    }

    if ($Device.InstanceId -notlike "ROOT\DEVGEN\*" -and
        $Device.InstanceId -notlike "ROOT\USB\*") {
        return $false
    }

    $DeviceRegistryPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($Device.InstanceId)"
    try {
        $DeviceProperties = Get-ItemProperty -Path $DeviceRegistryPath -ErrorAction Stop
    } catch {
        return $false
    }

    foreach ($HardwareId in @($DeviceProperties.HardwareID)) {
        if ($HardwareId -ieq "Root\VDSUSB") {
            return $true
        }
    }
    return $false
}

if (!(Test-Path (Join-Path $PackageDir "vds_usb.inf"))) {
    throw "packaged vDS USB INF not found: $PackageDir"
}

Push-Location $PackageDir
try {
    $Device = Get-PnpDevice |
        Where-Object {
            Test-VdsUsbRootDevice $_
        } |
        Select-Object -First 1
    if (!$Device) {
        $Devgen = (Get-Command devgen).Source
        & $Devgen /add /bus ROOT /instanceid "VDSUSB0" /hardwareid "Root\VDSUSB" | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "devgen failed with exit code $LASTEXITCODE"
        }

        $Devcon = (Get-Command devcon).Source
        & $Devcon update "vds_usb.inf" "Root\VDSUSB" | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "devcon update failed with exit code $LASTEXITCODE"
        }

        $Device = Get-PnpDevice |
            Where-Object {
                Test-VdsUsbRootDevice $_
            } |
            Select-Object -First 1
    }

    if (!$Device) {
        throw "vDS USB root device was not found after device install"
    }

    pnputil /restart-device $Device.InstanceId | Out-Host
    if ($LASTEXITCODE -eq 3010) {
        Write-Warning "pnputil restart-device requested a reboot to complete installation."
        $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -ne 0) {
        throw "pnputil restart-device failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
