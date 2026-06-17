# SPDX-License-Identifier: MIT

param(
    [switch]$SkipReload,
    [switch]$SkipRemovePrevious,
    [string]$DriverVersion = "",
    [string]$DriverDate = ""
)

$ErrorActionPreference = "Stop"

$PackageDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $PackageDir "generate_version.ps1")

$RepoRoot = Split-Path -Parent $PackageDir
$DriverVer = Resolve-VdsDriverVer `
    -RepoRoot $RepoRoot `
    -DriverVersion $DriverVersion `
    -DriverDate $DriverDate
$ResolvedDriverDate = $DriverVer.Date
$ResolvedDriverVersion = $DriverVer.Version
Write-Output "Using DriverVer=$ResolvedDriverDate,$ResolvedDriverVersion"

function Remove-PnpDriverByOriginalName {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OriginalName,
        [bool]$UninstallDevices = $true
    )

    $Removed = $false
    Get-WindowsDriver -Online -All -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Driver -and
            $_.OriginalFileName -and
            (Split-Path $_.OriginalFileName -Leaf).Equals(
                $OriginalName, [System.StringComparison]::OrdinalIgnoreCase)
        } |
        ForEach-Object {
            Write-Output "Removing driver package $($_.Driver) ($OriginalName)"
            $PnpUtilArgs = @("/delete-driver", $_.Driver)
            if ($UninstallDevices) {
                $PnpUtilArgs += @("/uninstall", "/force")
            }
            pnputil @PnpUtilArgs | Out-Host
            if ($LASTEXITCODE -eq 3010) {
                Write-Warning "pnputil requested a reboot while removing $($_.Driver)."
                $global:LASTEXITCODE = 0
            } elseif ($LASTEXITCODE -ne 0) {
                if (!$UninstallDevices) {
                    Write-Warning "pnputil could not remove in-use package $($_.Driver) without touching devices."
                    $global:LASTEXITCODE = 0
                    return
                }
                throw "pnputil delete-driver failed with exit code $LASTEXITCODE"
            }
            $Removed = $true
        }
    if ($Removed) {
        return
    }

    $PublishedName = $null
    foreach ($Line in (pnputil /enum-drivers)) {
        if ($Line -match "Published Name\s*:\s*(\S+)") {
            $PublishedName = $Matches[1]
            continue
        }
        if ($Line -match "Original Name\s*:\s*(\S+)" -and
            $Matches[1].Equals($OriginalName, [System.StringComparison]::OrdinalIgnoreCase) -and
            $PublishedName) {
            Write-Output "Removing driver package $PublishedName ($OriginalName)"
            $PnpUtilArgs = @("/delete-driver", $PublishedName)
            if ($UninstallDevices) {
                $PnpUtilArgs += @("/uninstall", "/force")
            }
            pnputil @PnpUtilArgs | Out-Host
            if ($LASTEXITCODE -eq 3010) {
                Write-Warning "pnputil requested a reboot while removing $PublishedName."
                $global:LASTEXITCODE = 0
            } elseif ($LASTEXITCODE -ne 0) {
                if (!$UninstallDevices) {
                    Write-Warning "pnputil could not remove in-use package $PublishedName without touching devices."
                    $global:LASTEXITCODE = 0
                    $PublishedName = $null
                    continue
                }
                throw "pnputil delete-driver failed with exit code $LASTEXITCODE"
            }
            $PublishedName = $null
        }
    }
}

function Remove-ServiceKey {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    & sc.exe query $Name >$null 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Output "Deleting driver service $Name"
        & sc.exe delete $Name | Out-Host
        if ($LASTEXITCODE -eq 1072) {
            throw "driver service $Name is marked for deletion; reboot before reinstalling"
        } elseif ($LASTEXITCODE -ne 0) {
            Write-Warning "sc delete $Name failed with exit code $LASTEXITCODE"
            $global:LASTEXITCODE = 0
        }
        for ($Attempt = 0; $Attempt -lt 20; ++$Attempt) {
            & sc.exe query $Name >$null 2>$null
            if ($LASTEXITCODE -ne 0) {
                $global:LASTEXITCODE = 0
                return
            }
            $global:LASTEXITCODE = 0
            Start-Sleep -Milliseconds 500
        }
        throw "driver service $Name is still present after delete; reboot before reinstalling"
        return
    }
    $global:LASTEXITCODE = 0

    $Path = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
    if (Test-Path $Path) {
        Write-Output "Removing orphaned service key $Name"
        Remove-Item -Recurse -Force $Path
    }
}

function Stop-DriverServiceIfPresent {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    & sc.exe query $Name >$null 2>$null
    if ($LASTEXITCODE -ne 0) {
        $global:LASTEXITCODE = 0
        return
    }

    Write-Output "Stopping driver service $Name"
    & sc.exe stop $Name | Out-Host
    if ($LASTEXITCODE -ne 0) {
        $global:LASTEXITCODE = 0
    }
    Start-Sleep -Milliseconds 500
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

$RemovePrevious = -not $SkipRemovePrevious

if ($RemovePrevious) {
    $RootDevices = Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object {
            Test-VdsUsbRootDevice $_
        }
    foreach ($Device in $RootDevices) {
        Write-Output "Removing vDS USB root device $($Device.InstanceId)"
        pnputil /remove-device $Device.InstanceId | Out-Host
        if ($LASTEXITCODE -eq 3010) {
            Write-Warning "pnputil requested a reboot while removing $($Device.InstanceId)."
            $global:LASTEXITCODE = 0
        } elseif ($LASTEXITCODE -ne 0) {
            Write-Warning "pnputil remove-device failed with exit code $LASTEXITCODE"
            $global:LASTEXITCODE = 0
        }
    }
} else {
    Write-Output "Skipped vDS USB root device removal."
}

if ($RemovePrevious) {
    Remove-PnpDriverByOriginalName -OriginalName "vds_filter.inf" -UninstallDevices $true
    Remove-PnpDriverByOriginalName -OriginalName "vds_usb.inf" -UninstallDevices $true
} else {
    Write-Output "Skipped driver package removal."
}

if ($RemovePrevious) {
    Stop-DriverServiceIfPresent -Name "vds_filter"
    Stop-DriverServiceIfPresent -Name "vds_usb"
    Remove-ServiceKey -Name "vds_filter"
    Remove-ServiceKey -Name "vds_usb"
}

& (Join-Path $PackageDir "vds_usb\build_install.ps1") `
    -DriverVersion $ResolvedDriverVersion `
    -DriverDate $ResolvedDriverDate `
    -SkipReload:$SkipReload `
    -SkipRemovePrevious:$true
if ($LASTEXITCODE -ne 0) {
    throw "vds_usb install failed with exit code $LASTEXITCODE"
}

& (Join-Path $PackageDir "vds_filter\build_install.ps1") `
    -DriverVersion $ResolvedDriverVersion `
    -DriverDate $ResolvedDriverDate `
    -SkipReload:$SkipReload `
    -SkipRemovePrevious:$true
if ($LASTEXITCODE -ne 0) {
    throw "vds_filter install failed with exit code $LASTEXITCODE"
}

Write-Output "vDS Windows driver package installed."
