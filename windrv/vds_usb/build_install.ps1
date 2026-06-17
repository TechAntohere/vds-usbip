# SPDX-License-Identifier: MIT

param(
    [switch]$BuildOnly,
    [switch]$SkipReload,
    [switch]$SkipRemovePrevious,
    [string]$DriverVersion = "",
    [string]$DriverDate = ""
)

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$WindrvDir = Split-Path -Parent $ProjectDir
. (Join-Path $WindrvDir "generate_version.ps1")

$RepoRoot = Split-Path -Parent $WindrvDir
$DriverVer = Resolve-VdsDriverVer `
    -RepoRoot $RepoRoot `
    -DriverVersion $DriverVersion `
    -DriverDate $DriverDate
$ResolvedDriverDate = $DriverVer.Date
$ResolvedDriverVersion = $DriverVer.Version
Write-Output "Using DriverVer=$ResolvedDriverDate,$ResolvedDriverVersion"

$Project = Join-Path $ProjectDir "vds_usb.vcxproj"
$RegistrySettings = Join-Path $ProjectDir "vds_usb.reg"
$CertificateSubject = "CN=vDS Test Driver Certificate"
$Configuration = "Debug"
$Platform = "x64"

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

function Remove-VdsUsbRootDevices {
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
}

$RemovePrevious = -not $SkipRemovePrevious -and -not $BuildOnly

if ($RemovePrevious) {
    Remove-VdsUsbRootDevices
    Remove-PnpDriverByOriginalName -OriginalName "vds_usb.inf" -UninstallDevices $true
} else {
    Write-Output "Skipped vDS USB driver package removal."
}
if ($RemovePrevious) {
    Stop-DriverServiceIfPresent -Name "vds_usb"
    Remove-ServiceKey -Name "vds_usb"
}

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $VsWhere)) {
    throw "vswhere.exe not found"
}

$VsPath = & $VsWhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (!$VsPath) {
    throw "Visual Studio Build Tools with C++ tools not found"
}

$MSBuild = Join-Path $VsPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
if (!(Test-Path $MSBuild)) {
    $MSBuild = Join-Path $VsPath "MSBuild\Current\Bin\MSBuild.exe"
}
if (!(Test-Path $MSBuild)) {
    throw "MSBuild.exe not found"
}

Remove-Item -Recurse -Force (Join-Path $ProjectDir "build") `
    -ErrorAction SilentlyContinue

& $MSBuild `
    $Project `
    /m `
    /t:Clean,Build `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /p:SignMode=Off `
    /p:EnableInf2cat=false `
    /p:SkipPackageVerification=true
if ($LASTEXITCODE -ne 0) {
    throw "driver build failed with exit code $LASTEXITCODE"
}

$BuildDir = Join-Path $ProjectDir "build\$Platform\$Configuration"
$SysPath = Join-Path $BuildDir "vds_usb.sys"
if (!(Test-Path $SysPath)) {
    throw "built driver not found: $SysPath"
}

$PackageDir = Join-Path $ProjectDir "package"
Remove-Item -Recurse -Force $PackageDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null
Copy-Item $SysPath (Join-Path $PackageDir "vds_usb.sys") -Force
New-VdsStagedInf `
    -TemplatePath (Join-Path $ProjectDir "vds_usb.inf") `
    -OutputPath (Join-Path $PackageDir "vds_usb.inf") `
    -DriverDate $ResolvedDriverDate `
    -DriverVersion $ResolvedDriverVersion

$Cert = Get-ChildItem Cert:\LocalMachine\My |
    Where-Object { $_.Subject -eq $CertificateSubject } |
    Select-Object -First 1
if (!$Cert) {
    $Cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertificateSubject `
        -CertStoreLocation Cert:\LocalMachine\My `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256

    $RootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
    $RootStore.Open("ReadWrite")
    $RootStore.Add($Cert)
    $RootStore.Close()

    $PublisherStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "LocalMachine")
    $PublisherStore.Open("ReadWrite")
    $PublisherStore.Add($Cert)
    $PublisherStore.Close()
}

Push-Location $PackageDir
try {
    signtool sign /fd SHA256 /sm /s My /n "vDS Test Driver Certificate" vds_usb.sys
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE"
    }

    inf2cat /driver:. /os:10_X64 /uselocaltime
    if ($LASTEXITCODE -ne 0) {
        throw "inf2cat failed with exit code $LASTEXITCODE"
    }

    signtool sign /fd SHA256 /sm /s My /n "vDS Test Driver Certificate" vds_usb.cat
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE"
    }

    $InfText = Get-Content "vds_usb.inf" -Raw
    if (!$InfText.Contains("Root\VDSUSB")) {
        throw "packaged INF validation failed"
    }

    if ($BuildOnly) {
        Write-Output "vDS virtual USB package built."
        return
    }

    $PnpUtilAddDriverArgs = @("/add-driver", "vds_usb.inf")
    if (!$SkipReload) {
        $PnpUtilAddDriverArgs += "/install"
    }
    pnputil @PnpUtilAddDriverArgs | Out-Host
    if ($LASTEXITCODE -eq 1072) {
        Write-Warning "pnputil add-driver reported service-marked-for-delete; reboot may be required before the new vds_usb.sys is loaded."
        $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -eq 3010) {
        Write-Warning "pnputil add-driver requested a reboot to complete installation."
        $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -ne 0) {
        throw "pnputil add-driver failed with exit code $LASTEXITCODE"
    }

    if (!(Test-Path $RegistrySettings)) {
        throw "vDS USB registry settings not found: $RegistrySettings"
    }
    reg.exe import $RegistrySettings | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "reg import failed with exit code $LASTEXITCODE"
    }

    $JoystickOemEntries = @(
        [pscustomobject]@{
            DeviceKey = "VID_054C&PID_0CE6"
            DeviceName = "DualSense Wireless Controller"
        },
        [pscustomobject]@{
            DeviceKey = "VID_054C&PID_0DF2"
            DeviceName = "DualSense Edge Wireless Controller"
        }
    )
    foreach ($JoystickOemEntry in $JoystickOemEntries) {
        $JoystickOemPaths = @(
            "HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$($JoystickOemEntry.DeviceKey)",
            "HKCU:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$($JoystickOemEntry.DeviceKey)"
        )
        foreach ($JoystickOemPath in $JoystickOemPaths) {
            New-Item -Path $JoystickOemPath -Force | Out-Null
            Set-ItemProperty -Path $JoystickOemPath `
                -Name "OEMName" `
                -Type String `
                -Value $JoystickOemEntry.DeviceName
        }
    }

    if ($SkipReload) {
        Write-Output "Skipped vDS USB root device restart/create."
        return
    }

    & (Join-Path $ProjectDir "reload.ps1") -PackageDir $PackageDir
} finally {
    Pop-Location
}

Write-Output "vDS virtual USB driver installed."
Write-Output "Imported vDS USB registry settings from $RegistrySettings."
Write-Output "Check the created ROOT\DEVGEN device with pnputil /enum-devices."
