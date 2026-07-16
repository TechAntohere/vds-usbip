; vDS installer (Inno Setup 6). Bundles vdsd + vdsctl + the self-contained
; tray app, optionally the signed usbip-win2 + HidHide driver installers, and
; wires up HidHide config + logon autostart.
;
; Build:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" vds.iss
; (run from this folder)

#define AppName "vDS"
#define AppVersion "0.3.0"
#define AppPublisher "vDS"
#define BuildDir "..\build"
#define PublishDir "..\ui\VdsTray\bin\Release\net10.0-windows\win-x64\publish"

; Drop the signed third-party installers here to bundle them (rename to match):
;   redist\usbip-win2.msi   (from github.com/vadimgrn/usbip-win2 releases)
;   redist\HidHide.msi      (from github.com/nefarius/HidHide releases)
; If absent, the installer skips them and assumes they are already installed.
#define UsbipMsi "redist\usbip-win2.msi"
#define HidHideMsi "redist\HidHide.msi"

[Setup]
AppId={{7C2B9E4A-9E2F-4B7C-9C1E-VDS0DUALSENSE}}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\vDS
DefaultGroupName=vDS
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=vDS-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=vDS (DualSense over USB/IP)

[Files]
Source: "{#BuildDir}\vdsd.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\vdsctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PublishDir}\VdsTray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PublishDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\hidhide_setup.ps1";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\hidhide_teardown.ps1"; DestDir: "{app}"; Flags: ignoreversion
#if FileExists(AddBackslash(SourcePath) + UsbipMsi)
Source: "{#UsbipMsi}";  DestDir: "{tmp}"; DestName: "usbip-win2.msi"; Flags: deleteafterinstall
#endif
#if FileExists(AddBackslash(SourcePath) + HidHideMsi)
Source: "{#HidHideMsi}"; DestDir: "{tmp}"; DestName: "HidHide.msi"; Flags: deleteafterinstall
#endif

[Registry]
; Launch the tray app (which starts the bridge) at logon.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "VdsTray"; ValueData: """{app}\VdsTray.exe"""; \
  Flags: uninsdeletevalue

[Run]
#if FileExists(AddBackslash(SourcePath) + UsbipMsi)
Filename: "msiexec.exe"; Parameters: "/i ""{tmp}\usbip-win2.msi"" /qn /norestart"; \
  StatusMsg: "Installing USB/IP driver..."; Flags: waituntilterminated
#endif
#if FileExists(AddBackslash(SourcePath) + HidHideMsi)
Filename: "msiexec.exe"; Parameters: "/i ""{tmp}\HidHide.msi"" /qn /norestart"; \
  StatusMsg: "Installing HidHide..."; Flags: waituntilterminated
#endif
; Configure HidHide: whitelist vdsd + hide the Bluetooth DualSense (dynamic).
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -File ""{app}\hidhide_setup.ps1"" -VdsdPath ""{app}\vdsd.exe"""; \
  StatusMsg: "Configuring controller hiding..."; Flags: runhidden waituntilterminated
; Launch now.
Filename: "{app}\VdsTray.exe"; Description: "Launch vDS now"; \
  Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -File ""{app}\hidhide_teardown.ps1"" -VdsdPath ""{app}\vdsd.exe"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "HidHideTeardown"
Filename: "taskkill.exe"; Parameters: "/IM VdsTray.exe /F"; Flags: runhidden; RunOnceId: "KillTray"
Filename: "taskkill.exe"; Parameters: "/IM vdsd.exe /F"; Flags: runhidden; RunOnceId: "KillVdsd"
