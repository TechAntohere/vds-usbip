# SPDX-License-Identifier: MIT

param(
  [string]$OutputDir = "",
  [string]$ToolsDir = "",
  [string]$DriverPackageRoot = "",
  # Optional: directory holding the published tray app (VdsTray.exe + assets\).
  # When present, the installer offers an opt-out "Install the vDS tray app"
  # checkbox. When absent, the tray is simply not part of the build.
  [string]$TrayDir = "",
  # Optional USB/IP backend: paths to the signed usbip-win2 + HidHide installers.
  # When both are provided, the installer installs the usbip-win2 + HidHide stack
  # (WHLK-signed, no test-signing) and runs vdsd in usbip transport mode, instead
  # of the custom test-signed vds_usb/vds_filter drivers. See DRAFT notes below.
  [string]$UsbipInstaller = "",
  [string]$HidHideInstaller = "",
  [string]$PkgRel = "",
  [string]$Arch = "x64",
  [string]$Wix = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$WixDir = Join-Path $ScriptDir "wix"
$GeneratedDir = Join-Path $ScriptDir "generated"

. (Join-Path $RepoRoot "generate_version.ps1")

function Resolve-VdsPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return [System.IO.Path]::GetFullPath($Path)
}

function Resolve-VdsOutputDir {
  if (![string]::IsNullOrWhiteSpace($OutputDir)) {
    return Resolve-VdsPath -Path $OutputDir
  }
  return Join-Path $RepoRoot "dist\windows"
}

function Resolve-VdsMsiVersion {
  $VersionInfo = Resolve-VdsDriverVer -RepoRoot $RepoRoot
  $Parts = $VersionInfo.Version -split "\."
  return "$($Parts[0]).$($Parts[1]).$($Parts[2])"
}

function Resolve-Wix {
  if (![string]::IsNullOrWhiteSpace($Wix)) {
    return Resolve-VdsPath -Path $Wix
  }

  $Command = Get-Command wix.exe -ErrorAction SilentlyContinue
  if ($Command) {
    return $Command.Source
  }

  throw "wix.exe was not found. Install the WiX Toolset CLI and ensure wix.exe is in PATH, or pass -Wix."
}

function Test-VdsToolsDir {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  if ([string]::IsNullOrWhiteSpace($Path)) {
    return $false
  }

  $RequiredFiles = @("vdsd.exe", "vdsctl.exe", "opus.dll")
  foreach ($FileName in $RequiredFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $Path $FileName) -PathType Leaf)) {
      return $false
    }
  }
  return $true
}

function Resolve-VdsToolsDir {
  if (![string]::IsNullOrWhiteSpace($ToolsDir)) {
    $Resolved = Resolve-VdsPath -Path $ToolsDir
    if (!(Test-VdsToolsDir -Path $Resolved)) {
      throw "tools directory must contain vdsd.exe, vdsctl.exe, and opus.dll: $Resolved"
    }
    return $Resolved
  }

  $Candidates = @(
    (Join-Path $RepoRoot "build\Release"),
    (Join-Path $RepoRoot "build")
  )
  foreach ($Candidate in $Candidates) {
    if (Test-VdsToolsDir -Path $Candidate) {
      return Resolve-VdsPath -Path $Candidate
    }
  }

  throw "tools directory was not found. Build vdsd.exe/vdsctl.exe first or pass -ToolsDir."
}

function Test-VdsTrayDir {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  if ([string]::IsNullOrWhiteSpace($Path)) {
    return $false
  }

  $RequiredFiles = @(
    "VdsTray.exe",
    "assets\dualsense.png",
    "assets\dualsense_edge.png",
    "assets\headphones.png",
    "assets\mic.png",
    "assets\battery.png"
  )
  foreach ($FileName in $RequiredFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $Path $FileName) -PathType Leaf)) {
      return $false
    }
  }
  return $true
}

# Resolve the published tray directory. Returns "" when the tray isn't available
# (explicit -TrayDir that's incomplete throws; auto-discovery just opts out).
function Resolve-VdsTrayDir {
  if (![string]::IsNullOrWhiteSpace($TrayDir)) {
    $Resolved = Resolve-VdsPath -Path $TrayDir
    if (!(Test-VdsTrayDir -Path $Resolved)) {
      throw "tray directory must contain VdsTray.exe and assets\*.png: $Resolved"
    }
    return $Resolved
  }

  $Candidate = Join-Path $RepoRoot "ui\VdsTray\bin\Release\net10.0-windows\win-x64\publish"
  if (Test-VdsTrayDir -Path $Candidate) {
    return Resolve-VdsPath -Path $Candidate
  }

  return ""
}

function Test-VdsDriverPackageRoot {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  if ([string]::IsNullOrWhiteSpace($Path)) {
    return $false
  }

  $RequiredFiles = @(
    "vds_usb\package\vds_usb.inf",
    "vds_usb\package\vds_usb.sys",
    "vds_usb\package\vds_usb.cat",
    "vds_usb\vds_usb.reg",
    "vds_filter\package\vds_filter.inf",
    "vds_filter\package\vds_filter.sys",
    "vds_filter\package\vds_filter.cat",
    "vds_filter\package\vds_test_driver.cer"
  )
  foreach ($FileName in $RequiredFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $Path $FileName) -PathType Leaf)) {
      return $false
    }
  }
  return $true
}

function Resolve-VdsDriverPackageRoot {
  # Legacy vds_usb.sys/vds_filter.sys drivers are now OPTIONAL. USB/IP is the
  # permanent runtime transport (fix #6); the legacy test-signed driver
  # package is only useful as a dev fallback. A missing/invalid driver
  # package root now degrades to $null instead of throwing, so a USB/IP-only
  # build no longer requires a WDK install.
  if (![string]::IsNullOrWhiteSpace($DriverPackageRoot)) {
    $Resolved = Resolve-VdsPath -Path $DriverPackageRoot
    if (!(Test-VdsDriverPackageRoot -Path $Resolved)) {
      throw "driver package root does not contain signed vDS driver packages: $Resolved"
    }
    return $Resolved
  }

  $Candidate = Join-Path $RepoRoot "windrv"
  if (Test-VdsDriverPackageRoot -Path $Candidate) {
    return Resolve-VdsPath -Path $Candidate
  }

  Write-Warning "legacy driver package root not found under 'windrv' and -DriverPackageRoot was not passed. Building a USB/IP-only installer without the legacy vds_usb.sys/vds_filter.sys fallback."
  return $null
}

function ConvertTo-RtfEscapedText {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  $Builder = [System.Text.StringBuilder]::new()
  [void]$Builder.Append("{\rtf1\ansi\deff0{\fonttbl{\f0 Consolas;}}\fs18 ")

  foreach ($Char in $Text.ToCharArray()) {
    switch ($Char) {
      "\" {
        [void]$Builder.Append("\\")
      }
      "{" {
        [void]$Builder.Append("\{")
      }
      "}" {
        [void]$Builder.Append("\}")
      }
      "`r" {
      }
      "`n" {
        [void]$Builder.Append("\par`r`n")
      }
      default {
        $CodePoint = [int][char]$Char
        if ($CodePoint -ge 32 -and $CodePoint -le 126) {
          [void]$Builder.Append($Char)
        } elseif ($CodePoint -lt 32) {
        } else {
          [void]$Builder.Append("\u$CodePoint?")
        }
      }
    }
  }

  [void]$Builder.Append("}")
  return $Builder.ToString()
}

function New-LicenseRtf {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $LicensePath = Join-Path $RepoRoot "LICENSE"
  $LicenseText = [System.IO.File]::ReadAllText($LicensePath)
  $LicenseRtf = ConvertTo-RtfEscapedText -Text $LicenseText
  [System.IO.File]::WriteAllText(
    $OutputPath,
    $LicenseRtf,
    [System.Text.Encoding]::ASCII)
}

function ConvertTo-CxxByteArray {
  param(
    [Parameter(Mandatory = $true)]
    [byte[]]$Bytes
  )

  $Lines = New-Object System.Collections.Generic.List[string]
  for ($Offset = 0; $Offset -lt $Bytes.Length; $Offset += 12) {
    $End = [Math]::Min($Offset + 11, $Bytes.Length - 1)
    $Values = for ($Index = $Offset; $Index -le $End; ++$Index) {
      "0x{0:x2}" -f $Bytes[$Index]
    }
    $Lines.Add("  " + ($Values -join ", ") + ",")
  }
  return $Lines -join "`r`n"
}

function New-UninstallPayloadHeader {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $Payloads = @(
    [pscustomobject]@{
      Symbol = "kPayloadStopServicePs1"
      RelativePath = "stop_service.ps1"
      SourcePath = Join-Path $RepoRoot "stop_service.ps1"
    },
    [pscustomobject]@{
      Symbol = "kPayloadUninstallServicePs1"
      RelativePath = "uninstall_service.ps1"
      SourcePath = Join-Path $RepoRoot "uninstall_service.ps1"
    },
    [pscustomobject]@{
      Symbol = "kPayloadUninstallEnvPs1"
      RelativePath = "uninstall_env.ps1"
      SourcePath = Join-Path $RepoRoot "uninstall_env.ps1"
    },
    [pscustomobject]@{
      Symbol = "kPayloadWindrvUninstallPs1"
      RelativePath = "windrv\\uninstall.ps1"
      SourcePath = Join-Path $RepoRoot "windrv\uninstall.ps1"
    }
  )

  $Builder = [System.Text.StringBuilder]::new()
  [void]$Builder.AppendLine("#pragma once")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("#include <cstddef>")
  [void]$Builder.AppendLine("#include <cstdint>")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("struct VdsUninstallPayload {")
  [void]$Builder.AppendLine("  const wchar_t *relative_path;")
  [void]$Builder.AppendLine("  const std::uint8_t *data;")
  [void]$Builder.AppendLine("  std::size_t size;")
  [void]$Builder.AppendLine("};")
  [void]$Builder.AppendLine()

  foreach ($Payload in $Payloads) {
    $Bytes = [System.IO.File]::ReadAllBytes($Payload.SourcePath)
    [void]$Builder.AppendLine("inline constexpr std::uint8_t $($Payload.Symbol)[] = {")
    [void]$Builder.AppendLine((ConvertTo-CxxByteArray -Bytes $Bytes))
    [void]$Builder.AppendLine("};")
    [void]$Builder.AppendLine()
  }

  [void]$Builder.AppendLine("inline constexpr VdsUninstallPayload kVdsUninstallPayloads[] = {")
  foreach ($Payload in $Payloads) {
    [void]$Builder.AppendLine(
      "  {L`"$($Payload.RelativePath)`", $($Payload.Symbol), sizeof($($Payload.Symbol))},"
    )
  }
  [void]$Builder.AppendLine("};")

  [System.IO.File]::WriteAllText(
    $OutputPath,
    $Builder.ToString(),
    [System.Text.Encoding]::ASCII)
}

function New-SetupPayloadHeader {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$MainMsiPath,
    [string]$UsbMsiPath,
    [string]$FilterMsiPath,
    [Parameter(Mandatory = $true)]
    [string]$DisplayVersion
  )

  $Payloads = [System.Collections.Generic.List[object]]::new()
  $Payloads.Add([pscustomobject]@{
    Symbol = "kPayloadMainMsi"
    FileName = "vDS-setup.msi"
    SourcePath = $MainMsiPath
  })

  # Legacy vds_usb.sys/vds_filter.sys driver MSIs are optional now (fix: make
  # WDK-signed legacy driver bundle non-mandatory). Only embed them if they
  # were actually built; kVdsLegacyDriversBundled tells the native launcher
  # whether to expect and install them at all.
  $LegacyDriversBundled = (![string]::IsNullOrWhiteSpace($UsbMsiPath) -and (Test-Path -LiteralPath $UsbMsiPath)) -and
                          (![string]::IsNullOrWhiteSpace($FilterMsiPath) -and (Test-Path -LiteralPath $FilterMsiPath))
  if ($LegacyDriversBundled) {
    $Payloads.Add([pscustomobject]@{
      Symbol = "kPayloadUsbMsi"
      FileName = "vDS-usb-setup.msi"
      SourcePath = $UsbMsiPath
    })
    $Payloads.Add([pscustomobject]@{
      Symbol = "kPayloadFilterMsi"
      FileName = "vDS-filter-setup.msi"
      SourcePath = $FilterMsiPath
    })
  }

  $Builder = [System.Text.StringBuilder]::new()
  [void]$Builder.AppendLine("#pragma once")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("#include <cstddef>")
  [void]$Builder.AppendLine("#include <cstdint>")
  [void]$Builder.AppendLine()
  $EscapedDisplayVersion = $DisplayVersion.Replace("\", "\\").Replace('"', '\"')
  [void]$Builder.AppendLine("inline constexpr const wchar_t *kVdsSetupVersion = L`"$EscapedDisplayVersion`";")
  [void]$Builder.AppendLine("inline constexpr bool kVdsLegacyDriversBundled = $(if ($LegacyDriversBundled) { "true" } else { "false" });")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("struct VdsSetupPayload {")
  [void]$Builder.AppendLine("  const wchar_t *file_name;")
  [void]$Builder.AppendLine("  const std::uint8_t *data;")
  [void]$Builder.AppendLine("  std::size_t size;")
  [void]$Builder.AppendLine("};")
  [void]$Builder.AppendLine()

  foreach ($Payload in $Payloads) {
    $Bytes = [System.IO.File]::ReadAllBytes($Payload.SourcePath)
    [void]$Builder.AppendLine("inline constexpr std::uint8_t $($Payload.Symbol)[] = {")
    [void]$Builder.AppendLine((ConvertTo-CxxByteArray -Bytes $Bytes))
    [void]$Builder.AppendLine("};")
    [void]$Builder.AppendLine()
  }

  [void]$Builder.AppendLine("inline constexpr VdsSetupPayload kVdsSetupPayloads[] = {")
  foreach ($Payload in $Payloads) {
    [void]$Builder.AppendLine(
      "  {L`"$($Payload.FileName)`", $($Payload.Symbol), sizeof($($Payload.Symbol))},"
    )
  }
  [void]$Builder.AppendLine("};")

  [System.IO.File]::WriteAllText(
    $OutputPath,
    $Builder.ToString(),
    [System.Text.Encoding]::ASCII)
}

function New-SetupLauncherManifest {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $ManifestLines = @(
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
    '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">',
    '  <assemblyIdentity version="1.0.0.0" processorArchitecture="*" name="vDS.Setup" type="win32"/>',
    '  <description>vDS Setup</description>',
    '  <dependency>',
    '    <dependentAssembly>',
    '      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>',
    '    </dependentAssembly>',
    '  </dependency>',
    '  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">',
    '    <security>',
    '      <requestedPrivileges>',
    '        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>',
    '      </requestedPrivileges>',
    '    </security>',
    '  </trustInfo>',
    '  <application xmlns="urn:schemas-microsoft-com:asm.v3">',
    '    <windowsSettings>',
    '      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>',
    '      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>',
    '    </windowsSettings>',
    '  </application>',
    '</assembly>'
  )

  [System.IO.File]::WriteAllText(
    $OutputPath,
    (($ManifestLines -join "`r`n") + "`r`n"),
    [System.Text.Encoding]::UTF8)
}

function Invoke-NativeCompiler {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,
    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  $Compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
  if ($Compiler) {
    & $Compiler.Source @Arguments | Out-Host
  } else {
    $VsDevCmd = Resolve-VsDevCmd
    $VsArch = Resolve-VsArch
    $QuotedArgs = ($Arguments | ForEach-Object { "`"$_`"" }) -join " "
    $Command = "`"$VsDevCmd`" -arch=$VsArch -host_arch=x64 >nul && cl.exe $QuotedArgs"
    & cmd.exe /d /s /c $Command | Out-Host
  }

  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage with exit code $LASTEXITCODE"
  }
}

function Resolve-VsDevCmd {
  $VsWhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
  if ($VsWhereCommand) {
    $VsWhere = $VsWhereCommand.Source
  } else {
    $VsWhereCandidates = @(
      (Join-Path "${env:ProgramFiles(x86)}" "Microsoft Visual Studio\Installer\vswhere.exe"),
      (Join-Path "$env:ProgramFiles" "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    $VsWhere = $VsWhereCandidates |
      Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
      Select-Object -First 1
  }
  if ([string]::IsNullOrWhiteSpace($VsWhere)) {
    throw "vswhere.exe was not found"
  }

  $InstallPath = & $VsWhere `
    -latest `
    -products "*" `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
  if ($LASTEXITCODE -eq 0 -and ![string]::IsNullOrWhiteSpace($InstallPath)) {
    $VsDevCmd = Join-Path $InstallPath "Common7\Tools\VsDevCmd.bat"
    if (Test-Path -LiteralPath $VsDevCmd -PathType Leaf) {
      return $VsDevCmd
    }
  }

  throw "VsDevCmd.bat was not found. Install Visual Studio or Build Tools with the C++ toolchain."
}

function Resolve-VsArch {
  switch ($Arch) {
    "x64" {
      return "x64"
    }
    default {
      return $Arch
    }
  }
}

function Build-NativeLauncher {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$IncludeDir = "",
    [string]$ManifestPath = "",
    [string[]]$LinkLibraries = @()
  )

  $Args = @(
    "/nologo",
    "/EHsc",
    "/std:c++17",
    "/W4",
    "/DUNICODE",
    "/D_UNICODE",
    "/DWINVER=0x0A00",
    "/D_WIN32_WINNT=0x0A00"
  )
  if (![string]::IsNullOrWhiteSpace($IncludeDir)) {
    $Args += "/I$IncludeDir"
  }

  $Args += @(
    "/Fe:$OutputPath",
    $SourcePath,
    "/link",
    "Advapi32.lib",
    "Shell32.lib",
    "User32.lib"
  )
  foreach ($Library in $LinkLibraries) {
    $Args += $Library
  }
  if (![string]::IsNullOrWhiteSpace($ManifestPath)) {
    $Args += @(
      "/MANIFEST:EMBED",
      "/MANIFESTINPUT:$ManifestPath"
    )
  }

  Invoke-NativeCompiler -Arguments $Args -FailureMessage "native launcher build failed"
}

function Build-NativeDll {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $Args = @(
    "/nologo",
    "/EHsc",
    "/std:c++17",
    "/W4",
    "/DUNICODE",
    "/D_UNICODE",
    "/LD",
    "/Fe:$OutputPath",
    $SourcePath,
    "/link",
    "Msi.lib",
    "Advapi32.lib",
    "User32.lib"
  )

  Invoke-NativeCompiler -Arguments $Args -FailureMessage "native DLL build failed"
}

function Invoke-WixBuild {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,
    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  & $ResolvedWix build @Arguments | Out-Host
  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage with exit code $LASTEXITCODE"
  }
}

$ResolvedOutputDir = Resolve-VdsOutputDir
$ResolvedVersion = Resolve-VdsMsiVersion
$ResolvedDisplayVersion = Get-VdsGitVersionFromGit -RepoRoot $RepoRoot
if ([string]::IsNullOrWhiteSpace($ResolvedDisplayVersion)) {
  $ResolvedDisplayVersion = "unknown"
}
$ResolvedWix = Resolve-Wix
$ResolvedToolsDir = Resolve-VdsToolsDir
$ResolvedDriverPackageRoot = Resolve-VdsDriverPackageRoot
$ResolvedTrayDir = Resolve-VdsTrayDir
# USB/IP + HidHide is the permanent transport for this installer -- no
# longer optional. A build without both paths used to silently produce a
# legacy-only MSI (no usbip-win2, no HidHide, no working transport at all
# for a fresh user) instead of failing loudly, which was the actual root
# cause behind "fresh install does not work for any user". Fail the build
# instead of shipping that silently-broken installer.
if ([string]::IsNullOrWhiteSpace($UsbipInstaller) -or [string]::IsNullOrWhiteSpace($HidHideInstaller)) {
  throw "UsbipInstaller and HidHideInstaller are both required: USB/IP is the " + `
        "permanent transport for this build and the installer no longer " + `
        "supports shipping without the usbip-win2 + HidHide bundle. Pass " + `
        "-UsbipInstaller <path> -HidHideInstaller <path>."
}
$ResolvedUsbipInstaller = Resolve-VdsPath -Path $UsbipInstaller
$ResolvedHidHideInstaller = Resolve-VdsPath -Path $HidHideInstaller
$HidHideSetupPath = Join-Path $RepoRoot "hidhide_setup.ps1"
$UsbipBackend = $true

$PackageFileNameArgs = @{
  Name = "vDSSetup"
  Arch = $Arch
}
if (![string]::IsNullOrWhiteSpace($PkgRel)) {
  $PackageFileNameArgs.PkgRel = $PkgRel
}
$SetupExeFileName = Resolve-VdsPackageFileName @PackageFileNameArgs
$SetupPath = Join-Path $ResolvedOutputDir $SetupExeFileName

$UninstallRunnerSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\uninstall_runner.cc")
$DriverScriptRunnerSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\driver_script_runner.cc")
$RootDeviceInstallerSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\root_device_installer.cc")
$SetupActionsSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\setup_actions.cc")
$SetupLauncherSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\setup_launcher.cc")

$LicenseRtf = Join-Path $GeneratedDir "license.rtf"
$UninstallPayloadHeader = Join-Path $GeneratedDir "uninstall_payload.hh"
$SetupPayloadHeader = Join-Path $GeneratedDir "setup_payload.hh"
$SetupLauncherManifest = Join-Path $GeneratedDir "setup-launcher.manifest"
$UninstallRunnerPath = Join-Path $GeneratedDir "vds-uninstall-runner.exe"
$DriverScriptRunnerPath = Join-Path $GeneratedDir "vds-driver-script-runner.exe"
$RootDeviceInstallerPath = Join-Path $GeneratedDir "vds-root-device-installer.exe"
$SetupActionsPath = Join-Path $GeneratedDir "vds-setup-actions.dll"
$MainMsiPath = Join-Path $GeneratedDir "vDS-setup.msi"
$UsbMsiPath = Join-Path $GeneratedDir "vDS-usb-setup.msi"
$FilterMsiPath = Join-Path $GeneratedDir "vDS-filter-setup.msi"

New-Item -ItemType Directory -Force -Path $GeneratedDir | Out-Null
New-Item -ItemType Directory -Force -Path $ResolvedOutputDir | Out-Null
New-LicenseRtf -OutputPath $LicenseRtf
New-UninstallPayloadHeader -OutputPath $UninstallPayloadHeader

Build-NativeLauncher `
  -SourcePath $UninstallRunnerSource `
  -OutputPath $UninstallRunnerPath `
  -IncludeDir $GeneratedDir
Build-NativeLauncher `
  -SourcePath $DriverScriptRunnerSource `
  -OutputPath $DriverScriptRunnerPath
Build-NativeLauncher `
  -SourcePath $RootDeviceInstallerSource `
  -OutputPath $RootDeviceInstallerPath `
  -LinkLibraries @("Newdev.lib", "Setupapi.lib")
Build-NativeDll `
  -SourcePath $SetupActionsSource `
  -OutputPath $SetupActionsPath

# Legacy vds_usb.sys/vds_filter.sys MSIs are only built when a signed driver
# package root is actually available. USB/IP is the permanent transport
# (fix #6), so these are a dev-only fallback now, not a hard requirement --
# this is what lets a USB/IP-only build skip needing WDK installed at all.
$LegacyDriversAvailable = ![string]::IsNullOrWhiteSpace($ResolvedDriverPackageRoot)
if ($LegacyDriversAvailable) {
  Invoke-WixBuild `
    -Arguments @(
    (Join-Path $WixDir "DriverUsb.wxs"),
    "-arch", $Arch,
    "-d", "VdsVersion=$ResolvedVersion",
    "-d", "DriverPackageRoot=$ResolvedDriverPackageRoot",
    "-d", "WindrvDir=$(Join-Path $RepoRoot "windrv")",
    "-d", "DriverScriptRunner=$DriverScriptRunnerPath",
    "-d", "RootDeviceInstaller=$RootDeviceInstallerPath",
    "-out", $UsbMsiPath
  ) `
    -FailureMessage "wix USB driver MSI build failed"

  Invoke-WixBuild `
    -Arguments @(
    (Join-Path $WixDir "DriverFilter.wxs"),
    "-arch", $Arch,
    "-d", "VdsVersion=$ResolvedVersion",
    "-d", "DriverPackageRoot=$ResolvedDriverPackageRoot",
    "-d", "WindrvDir=$(Join-Path $RepoRoot "windrv")",
    "-d", "DriverScriptRunner=$DriverScriptRunnerPath",
    "-out", $FilterMsiPath
  ) `
    -FailureMessage "wix filter driver MSI build failed"
} else {
  Write-Warning "skipping legacy vds_usb.sys/vds_filter.sys MSI build -- no driver package root available. This setup.exe will be USB/IP-only."
  $UsbMsiPath = $null
  $FilterMsiPath = $null
}

$MainMsiArguments = [System.Collections.Generic.List[string]]::new()
$MainMsiArguments.Add((Join-Path $WixDir "Product.wxs"))
$MainMsiArguments.AddRange([string[]]@(
  "-arch", $Arch,
  "-d", "VdsVersion=$ResolvedVersion",
  "-d", "VdsDisplayVersion=$ResolvedDisplayVersion",
  "-d", "LicenseRtf=$LicenseRtf",
  "-d", "ToolsDir=$ResolvedToolsDir",
  "-d", "SetupActions=$SetupActionsPath",
  "-d", "UninstallRunner=$UninstallRunnerPath"
))
# Only define TrayDir when a published tray is available; Product.wxs gates the
# optional tray feature behind <?ifdef TrayDir?>.
if (![string]::IsNullOrWhiteSpace($ResolvedTrayDir)) {
  $MainMsiArguments.AddRange([string[]]@("-d", "TrayDir=$ResolvedTrayDir"))
}
# USB/IP backend: define UsbipInstaller/HidHideInstaller/HidHideSetup so
# Product.wxs (<?ifdef UsbipInstaller?>) installs the signed usbip-win2 + HidHide
# stack and runs vdsd in usbip mode instead of the test-signed custom drivers.
if ($UsbipBackend) {
  $MainMsiArguments.AddRange([string[]]@(
    "-d", "UsbipInstaller=$ResolvedUsbipInstaller",
    "-d", "HidHideInstaller=$ResolvedHidHideInstaller",
    "-d", "HidHideSetup=$HidHideSetupPath"
  ))
}
$MainMsiArguments.AddRange([string[]]@("-out", $MainMsiPath))

Invoke-WixBuild `
  -Arguments $MainMsiArguments.ToArray() `
  -FailureMessage "wix main MSI build failed"

New-SetupPayloadHeader `
  -OutputPath $SetupPayloadHeader `
  -MainMsiPath $MainMsiPath `
  -UsbMsiPath $UsbMsiPath `
  -FilterMsiPath $FilterMsiPath `
  -DisplayVersion $ResolvedDisplayVersion
New-SetupLauncherManifest -OutputPath $SetupLauncherManifest
Build-NativeLauncher `
  -SourcePath $SetupLauncherSource `
  -OutputPath $SetupPath `
  -IncludeDir $GeneratedDir `
  -ManifestPath $SetupLauncherManifest

Write-Output $SetupPath
