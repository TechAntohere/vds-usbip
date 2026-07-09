# SPDX-License-Identifier: MIT

param(
  [ValidateSet("all", "vds_usb", "vds_filter")]
  [string]$Target = "all",
  [switch]$SkipReload,
  [switch]$SkipRemovePrevious
)

$ErrorActionPreference = "Stop"

$WindrvDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-VdsStep {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Message
  )

  Write-Output "[vDS installer] $Message"
}

function Write-VdsException {
  param(
    [Parameter(Mandatory = $true)]
    $ErrorRecord
  )

  Write-Output "[vDS installer] ERROR: failed: $($ErrorRecord.Exception.Message)"
  Write-Output "[vDS installer] ERROR: category: $($ErrorRecord.CategoryInfo)"
  Write-Output "[vDS installer] ERROR: fully qualified error id: $($ErrorRecord.FullyQualifiedErrorId)"
  if ($ErrorRecord.InvocationInfo) {
    Write-Output "[vDS installer] ERROR: script position: $($ErrorRecord.InvocationInfo.PositionMessage)"
  }
  if ($ErrorRecord.ScriptStackTrace) {
    Write-Output "[vDS installer] ERROR: stack trace: $($ErrorRecord.ScriptStackTrace)"
  }
  $Details = $ErrorRecord | Format-List * -Force | Out-String
  foreach ($Line in ($Details -split "`r?`n")) {
    if (![string]::IsNullOrWhiteSpace($Line)) {
      Write-Output "[vDS installer] ERROR: error detail: $Line"
    }
  }
}

function Write-RebootPendingState {
  $Checks = @(
    [pscustomobject]@{
      Name = "Component Based Servicing"
      Path = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending"
    },
    [pscustomobject]@{
      Name = "Windows Update"
      Path = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired"
    },
    [pscustomobject]@{
      Name = "UpdateExeVolatile"
      Path = "HKLM:\SOFTWARE\Microsoft\Updates"
      Value = "UpdateExeVolatile"
    },
    [pscustomobject]@{
      Name = "PendingFileRenameOperations"
      Path = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager"
      Value = "PendingFileRenameOperations"
    }
  )

  foreach ($Check in $Checks) {
    if ($Check.Value) {
      $Value = $null
      try {
        $Value = Get-ItemPropertyValue `
          -Path $Check.Path `
          -Name $Check.Value `
          -ErrorAction Stop
      } catch {
      }
      Write-VdsStep "pending reboot marker '$($Check.Name)': $([bool]$Value)"
      continue
    }
    Write-VdsStep "pending reboot marker '$($Check.Name)': $(Test-Path $Check.Path)"
  }
}

function Write-TestSigningState {
  Write-VdsStep "checking Windows test signing state"
  $Output = & bcdedit.exe /enum "{current}" 2>&1
  $ExitCode = $LASTEXITCODE
  if ($Output.Count -eq 0) {
    Write-VdsStep "bcdedit /enum {current} output: <no output>"
  }
  $FoundTestSigningLine = $false
  $TestSigningEnabled = $false
  foreach ($Line in $Output) {
    Write-VdsStep "bcdedit /enum {current} output: $Line"
    if ($Line -notmatch "testsigning") {
      continue
    }
    $FoundTestSigningLine = $true
    if ($Line -match "\byes\b") {
      $TestSigningEnabled = $true
    }
  }
  if ($ExitCode -ne 0) {
    Write-VdsStep "bcdedit /enum {current} failed; test signing state is unknown"
  } elseif (!$FoundTestSigningLine) {
    Write-VdsStep "bcdedit testsigning line was not found; treating test signing as disabled"
  } elseif ($TestSigningEnabled) {
    Write-VdsStep "bcdedit testsigning state parsed as enabled"
  } else {
    Write-VdsStep "bcdedit testsigning state parsed as disabled"
  }
  Write-VdsStep "bcdedit /enum {current} exit code: $ExitCode"
  $global:LASTEXITCODE = 0
}

function Write-MatchingDriverPackages {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$OriginalNames
  )

  Write-VdsStep "enumerating existing driver packages for: $($OriginalNames -join ', ')"
  $PublishedName = ""
  foreach ($Line in (pnputil /enum-drivers 2>&1)) {
    if ($Line -match "Published Name\s*:\s*(\S+)") {
      $PublishedName = $Matches[1]
      continue
    }
    if ($Line -match "Original Name\s*:\s*(\S+)" -and $PublishedName) {
      foreach ($OriginalName in $OriginalNames) {
        if ($Matches[1].Equals($OriginalName, [System.StringComparison]::OrdinalIgnoreCase)) {
          Write-VdsStep "found driver package $PublishedName ($($Matches[1]))"
          break
        }
      }
      $PublishedName = ""
    }
  }
  Write-VdsStep "pnputil /enum-drivers exit code: $LASTEXITCODE"
  $global:LASTEXITCODE = 0
}

function New-TargetSpecs {
  $UsbDir = Join-Path $WindrvDir "vds_usb"
  $FilterDir = Join-Path $WindrvDir "vds_filter"

  return [ordered]@{
    vds_usb = [pscustomobject]@{
      Name = "vds_usb"
      DisplayName = "vDS virtual USB"
      InfName = "vds_usb.inf"
      ServiceName = "vds_usb"
      DriverFile = "vds_usb.sys"
      PackageDir = Join-Path $UsbDir "package"
      RegistrySettings = Join-Path $UsbDir "vds_usb.reg"
      AllowPnPExit259 = $true
    }
    vds_filter = [pscustomobject]@{
      Name = "vds_filter"
      DisplayName = "vDS filter"
      InfName = "vds_filter.inf"
      ServiceName = "vds_filter"
      DriverFile = "vds_filter.sys"
      PackageDir = Join-Path $FilterDir "package"
      RegistrySettings = ""
      AllowPnPExit259 = $true
    }
  }
}

function Resolve-TargetSpecs {
  param(
    [Parameter(Mandatory = $true)]
    $Specs,
    [Parameter(Mandatory = $true)]
    [string]$TargetName
  )

  if ($TargetName -eq "all") {
    return @($Specs["vds_usb"], $Specs["vds_filter"])
  }
  return @($Specs[$TargetName])
}

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
    Write-VdsStep "running pnputil $($PnpUtilArgs -join ' ')"
    pnputil @PnpUtilArgs | Out-Host
    Write-VdsStep "pnputil delete-driver exit code: $LASTEXITCODE"
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
      Write-VdsStep "running pnputil $($PnpUtilArgs -join ' ')"
      pnputil @PnpUtilArgs | Out-Host
      Write-VdsStep "pnputil delete-driver exit code: $LASTEXITCODE"
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
    Write-VdsStep "running sc.exe delete $Name"
    & sc.exe delete $Name | Out-Host
    Write-VdsStep "sc.exe delete $Name exit code: $LASTEXITCODE"
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
  Write-VdsStep "running sc.exe stop $Name"
  & sc.exe stop $Name | Out-Host
  Write-VdsStep "sc.exe stop $Name exit code: $LASTEXITCODE"
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
    $Device.FriendlyName -eq "vDS USB Root Hub") {
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
    $PnpUtilArgs = @("/remove-device", $Device.InstanceId, "/subtree", "/force")
    Write-VdsStep "running pnputil $($PnpUtilArgs -join ' ')"
    pnputil @PnpUtilArgs | Out-Host
    Write-VdsStep "pnputil remove-device exit code: $LASTEXITCODE"
    if ($LASTEXITCODE -eq 3010) {
      Write-Warning "pnputil requested a reboot while removing $($Device.InstanceId)."
      $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -ne 0) {
      Write-Warning "pnputil remove-device failed with exit code $LASTEXITCODE"
      $global:LASTEXITCODE = 0
    }
  }
}

function Test-VdsFilterDevice {
  param(
    [Parameter(Mandatory = $true)]
    $Device
  )

  if ($Device.FriendlyName -ne "vDS Filter") {
    return $false
  }

  if ($Device.InstanceId -like "USB\VID_054C&PID_0CE6&MI_03\*" -or
    $Device.InstanceId -like "USB\VID_054C&PID_0DF2&MI_03\*" -or
    $Device.InstanceId -like "HID\VID_054C&PID_0CE6&MI_03\*" -or
    $Device.InstanceId -like "HID\VID_054C&PID_0DF2&MI_03\*" -or
    $Device.InstanceId -like "HID\{00001124-0000-1000-8000-00805F9B34FB}_VID&0002054C_PID&0CE6\*" -or
    $Device.InstanceId -like "HID\{00001124-0000-1000-8000-00805F9B34FB}_VID&0002054C_PID&0DF2\*" -or
    $Device.InstanceId -like "BTHENUM\{00001124-0000-1000-8000-00805F9B34FB}_VID&0002054C_PID&0CE6\*" -or
    $Device.InstanceId -like "BTHENUM\{00001124-0000-1000-8000-00805F9B34FB}_VID&0002054C_PID&0DF2\*") {
    return $true
  }

  return $false
}

function Remove-VdsFilterDevices {
  $FilterDevices = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object {
    Test-VdsFilterDevice $_
  }
  foreach ($Device in $FilterDevices) {
    Write-Output "Removing vDS filter device $($Device.InstanceId)"
    $PnpUtilArgs = @("/remove-device", $Device.InstanceId, "/subtree", "/force")
    Write-VdsStep "running pnputil $($PnpUtilArgs -join ' ')"
    pnputil @PnpUtilArgs | Out-Host
    Write-VdsStep "pnputil remove-device exit code: $LASTEXITCODE"
    if ($LASTEXITCODE -eq 3010) {
      Write-Warning "pnputil requested a reboot while removing $($Device.InstanceId)."
      $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -ne 0) {
      Write-Warning "pnputil remove-device failed with exit code $LASTEXITCODE"
      $global:LASTEXITCODE = 0
    }
  }
}

function Import-VdsUsbRegistrySettings {
  param(
    [Parameter(Mandatory = $true)]
    $Spec
  )

  if (!(Test-Path $Spec.RegistrySettings)) {
    throw "vDS USB registry settings not found: $($Spec.RegistrySettings)"
  }
  Write-VdsStep "importing registry settings: $($Spec.RegistrySettings)"
  reg.exe import $Spec.RegistrySettings | Out-Host
  Write-VdsStep "reg import exit code: $LASTEXITCODE"
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
}

function Confirm-VdsFilterServiceRegistration {
  param(
    [Parameter(Mandatory = $true)]
    $Spec
  )

  if ($Spec.Name -ne "vds_filter") {
    return
  }

  Write-VdsStep "checking PnP driver service registration for $($Spec.ServiceName)"
  & sc.exe query $Spec.ServiceName | Out-Host
  Write-VdsStep "sc.exe query $($Spec.ServiceName) exit code: $LASTEXITCODE"
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "$($Spec.ServiceName) service is not registered yet. This is normal when no matching HID/Bluetooth device was installed during setup; Windows will create and load it from the staged INF when a matching device enumerates."
    $global:LASTEXITCODE = 0
  }
}

function Invoke-VdsDriverReload {
  param(
    [Parameter(Mandatory = $true)]
    $Spec
  )

  if ($SkipReload) {
    Write-Output "Skipped $($Spec.DisplayName) device restart."
    return
  }

  $ReloadScript = Join-Path $WindrvDir "reload.ps1"
  if (Test-Path $ReloadScript) {
    Write-VdsStep "running reload script for $($Spec.Name): $ReloadScript"
    & $ReloadScript -Target $Spec.Name
    Write-VdsStep "reload script exit code: $LASTEXITCODE"
    return
  }

  if ($Spec.Name -eq "vds_filter") {
    Write-Warning "reload.ps1 is not present; continuing after staging the vds_filter driver package."
    return
  }

  throw "reload.ps1 is required to activate $($Spec.DisplayName)"
}

function Install-TargetPackage {
  param(
    [Parameter(Mandatory = $true)]
    $Spec
  )

  $InfPath = Join-Path $Spec.PackageDir $Spec.InfName
  if (!(Test-Path $InfPath)) {
    throw "packaged INF not found: $InfPath"
  }

  Write-VdsStep "installing $($Spec.Name): package=$($Spec.PackageDir) inf=$InfPath"
  Push-Location $Spec.PackageDir
  try {
    $PnpUtilAddDriverArgs = @("/add-driver", $Spec.InfName, "/install")
    Write-VdsStep "running pnputil $($PnpUtilAddDriverArgs -join ' ')"
    pnputil @PnpUtilAddDriverArgs | Out-Host
    Write-VdsStep "pnputil add-driver exit code: $LASTEXITCODE"
    if ($LASTEXITCODE -eq 1072) {
      Write-Warning "pnputil add-driver reported service-marked-for-delete; reboot may be required before the new $($Spec.ServiceName).sys is loaded."
      $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -eq 3010) {
      Write-Warning "pnputil add-driver requested a reboot to complete installation."
      $global:LASTEXITCODE = 0
    } elseif ($Spec.AllowPnPExit259 -and $LASTEXITCODE -eq 259) {
      $global:LASTEXITCODE = 0
    } elseif ($LASTEXITCODE -ne 0) {
      throw "pnputil add-driver failed with exit code $LASTEXITCODE"
    }
  } finally {
    Pop-Location
  }

  if ($Spec.Name -eq "vds_usb") {
    Import-VdsUsbRegistrySettings -Spec $Spec
  }

  Invoke-VdsDriverReload -Spec $Spec
  Confirm-VdsFilterServiceRegistration -Spec $Spec
}

try {
  Write-VdsStep "driver install started: target=$Target skipReload=$SkipReload skipRemovePrevious=$SkipRemovePrevious windrv=$WindrvDir"
  Write-VdsStep "PowerShell version: $($PSVersionTable.PSVersion); 64-bit process: $([Environment]::Is64BitProcess); 64-bit OS: $([Environment]::Is64BitOperatingSystem)"
  Write-TestSigningState
  Write-RebootPendingState

  $Specs = New-TargetSpecs
  $SelectedSpecs = Resolve-TargetSpecs -Specs $Specs -TargetName $Target
  $RemovePrevious = -not $SkipRemovePrevious
  Write-MatchingDriverPackages -OriginalNames @($SelectedSpecs | ForEach-Object { $_.InfName })

  if ($RemovePrevious) {
    Write-VdsStep "removing previous driver packages and devices before install"
    if (($SelectedSpecs | Where-Object { $_.Name -eq "vds_usb" })) {
      Remove-VdsUsbRootDevices
    }
    if (($SelectedSpecs | Where-Object { $_.Name -eq "vds_filter" })) {
      Remove-VdsFilterDevices
    }
    foreach ($Spec in $SelectedSpecs) {
      Remove-PnpDriverByOriginalName -OriginalName $Spec.InfName -UninstallDevices $true
    }
    foreach ($Spec in $SelectedSpecs) {
      Stop-DriverServiceIfPresent -Name $Spec.ServiceName
      Remove-ServiceKey -Name $Spec.ServiceName
    }
  } else {
    Write-Output "Skipped driver package and device removal."
  }

  foreach ($Spec in $SelectedSpecs) {
    Install-TargetPackage -Spec $Spec
  }

  Write-Output "vDS Windows driver package installed."
  exit 0
} catch {
  Write-VdsException -ErrorRecord $_
  exit 1
}
