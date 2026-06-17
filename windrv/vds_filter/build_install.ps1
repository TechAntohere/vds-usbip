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

$Project = Join-Path $ProjectDir "vds_filter.vcxproj"
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

$RemovePrevious = -not $SkipRemovePrevious -and -not $BuildOnly

if ($RemovePrevious) {
    Remove-PnpDriverByOriginalName -OriginalName "vds_filter.inf" -UninstallDevices $true
    Stop-DriverServiceIfPresent -Name "vds_filter"
    Remove-ServiceKey -Name "vds_filter"
} else {
    Write-Output "Skipped vDS filter driver package removal."
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
$SysPath = Join-Path $BuildDir "vds_filter.sys"
if (!(Test-Path $SysPath)) {
    throw "built driver not found: $SysPath"
}

$PackageDir = Join-Path $ProjectDir "package"
Remove-Item -Recurse -Force $PackageDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null
Copy-Item $SysPath (Join-Path $PackageDir "vds_filter.sys") -Force
New-VdsStagedInf `
    -TemplatePath (Join-Path $ProjectDir "vds_filter.inf") `
    -OutputPath (Join-Path $PackageDir "vds_filter.inf") `
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
    signtool sign /fd SHA256 /sm /s My /n "vDS Test Driver Certificate" vds_filter.sys
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE"
    }

    inf2cat /driver:. /os:10_X64 /uselocaltime
    if ($LASTEXITCODE -ne 0) {
        throw "inf2cat failed with exit code $LASTEXITCODE"
    }

    signtool sign /fd SHA256 /sm /s My /n "vDS Test Driver Certificate" vds_filter.cat
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE"
    }

    if ($BuildOnly) {
        Write-Output "vDS filter package built."
        return
    }

    $PnpUtilAddDriverArgs = @("/add-driver", "vds_filter.inf")
    if (!$SkipReload) {
        $PnpUtilAddDriverArgs += "/install"
    }
    pnputil @PnpUtilAddDriverArgs | Out-Host
    if ($LASTEXITCODE -eq 3010) {
        Write-Warning "pnputil add-driver requested a reboot to complete installation."
        $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -eq 1072) {
        Write-Warning "pnputil add-driver reported service-marked-for-delete; reboot may be required before the new vds_filter.sys is loaded."
        $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
        throw "pnputil add-driver failed with exit code $LASTEXITCODE"
    }

    if (!$SkipReload) {
        & (Join-Path $ProjectDir "reload.ps1")
    } else {
        Write-Output "Skipped Bluetooth HID device restart."
    }
} finally {
    Pop-Location
}

Write-Output "vDS filter installed."
