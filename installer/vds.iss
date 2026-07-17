; vDS installer (Inno Setup 6). Bundles vdsd + vdsctl + the self-contained
; tray app, optionally the signed usbip-win2 + HidHide driver installers, and
; wires up HidHide config + logon autostart.
;
; Build:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" vds.iss
; (run from this folder)

#define AppName "vDS"
#define AppVersion "0.1.0"
#define AppPublisher "vDS"
#define BuildDir "..\build"
#define PublishDir "..\ui\VdsTray\bin\Release\net10.0-windows\win-x64\publish"

; Drop the signed third-party installers here to bundle them (rename to match):
;   redist\usbip-win2.exe   (from github.com/vadimgrn/usbip-win2 releases, x64)
;   redist\HidHide.exe      (from github.com/nefarius/HidHide releases, x64)
; If absent, the installer skips them and assumes they are already installed.
; usbip-win2 is an Inno Setup installer; HidHide is an Advanced Installer.
#define UsbipExe "redist\usbip-win2.exe"
#define HidHideExe "redist\HidHide.exe"

[Setup]
AppId={{7C2B9E4A-9E2F-4B7C-9C1E-VDS0DUALSENSE}}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\vDS
DefaultGroupName=vDS
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=vDS-Setup-{#AppVersion}-usbip
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
Source: "{#PublishDir}\VdsTray.exe"; DestDir: "{app}"; Flags: ignoreversion; Tasks: traytask
Source: "{#PublishDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: traytask
Source: "..\hidhide_setup.ps1";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\hidhide_teardown.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bringup.ps1";          DestDir: "{app}"; Flags: ignoreversion; Check: not WillInstallTray
#if FileExists(AddBackslash(SourcePath) + UsbipExe)
Source: "{#UsbipExe}";  DestDir: "{tmp}"; DestName: "usbip-win2.exe"; Flags: deleteafterinstall; Check: UsbipMissing
#endif
#if FileExists(AddBackslash(SourcePath) + HidHideExe)
Source: "{#HidHideExe}"; DestDir: "{tmp}"; DestName: "HidHide.exe"; Flags: deleteafterinstall; Check: HidHideMissing
#endif

[Tasks]
Name: "traytask"; Description: "Install the vDS tray app (status widget, battery %, mic controls)"; GroupDescription: "Optional components:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Optional components:"; Flags: unchecked

[Icons]
; Manual launch entries (only when the tray app is installed).
Name: "{group}\vDS";           Filename: "{app}\VdsTray.exe"; Comment: "Start vDS (DualSense over USB/IP)"; Tasks: traytask
Name: "{group}\Uninstall vDS"; Filename: "{uninstallexe}"
Name: "{autodesktop}\vDS";     Filename: "{app}\VdsTray.exe"; Tasks: traytask desktopicon

[Registry]
; Tray installed: launch the tray (which brings up the bridge) at logon.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "VdsTray"; ValueData: """{app}\VdsTray.exe"""; \
  Flags: uninsdeletevalue; Tasks: traytask
; Tray NOT installed: bring the bridge up headlessly at logon.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "VdsBridge"; \
  ValueData: "powershell.exe -WindowStyle Hidden -ExecutionPolicy Bypass -File ""{app}\bringup.ps1"""; \
  Flags: uninsdeletevalue; Check: not WillInstallTray

[Run]
#if FileExists(AddBackslash(SourcePath) + UsbipExe)
Filename: "{tmp}\usbip-win2.exe"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"; \
  StatusMsg: "Installing USB/IP driver..."; Flags: waituntilterminated; Check: UsbipMissing
#endif
#if FileExists(AddBackslash(SourcePath) + HidHideExe)
Filename: "{tmp}\HidHide.exe"; Parameters: "/exenoui /qn"; \
  StatusMsg: "Installing HidHide..."; Flags: waituntilterminated; Check: HidHideMissing
#endif
; Configure HidHide: whitelist vdsd + hide the Bluetooth DualSense (dynamic).
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -File ""{app}\hidhide_setup.ps1"" -VdsdPath ""{app}\vdsd.exe"""; \
  StatusMsg: "Configuring controller hiding..."; Flags: runhidden waituntilterminated
; Tray NOT installed: start the bridge now (best-effort; the logon entry covers reboots).
Filename: "powershell.exe"; \
  Parameters: "-WindowStyle Hidden -ExecutionPolicy Bypass -File ""{app}\bringup.ps1"""; \
  Flags: runhidden nowait; Check: not WillInstallTray
; Tray installed: launch it now.
Filename: "{app}\VdsTray.exe"; Description: "Launch vDS now"; \
  Flags: nowait postinstall skipifsilent; Tasks: traytask

[UninstallRun]
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -File ""{app}\hidhide_teardown.ps1"" -VdsdPath ""{app}\vdsd.exe"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "HidHideTeardown"
Filename: "taskkill.exe"; Parameters: "/IM VdsTray.exe /F"; Flags: runhidden; RunOnceId: "KillTray"
Filename: "taskkill.exe"; Parameters: "/IM vdsd.exe /F"; Flags: runhidden; RunOnceId: "KillVdsd"

[Code]
{ Whether the user opted to install the tray app. When false, the installer
  wires up a headless logon bring-up (bringup.ps1) instead. }
function WillInstallTray: Boolean;
begin
  Result := WizardIsTaskSelected('traytask');
end;

{ Skip a bundled driver install if that driver is already present, so re-running
  the setup on a machine that already has usbip-win2 / HidHide doesn't trigger a
  disruptive uninstall-then-reinstall of the kernel driver. }
function UsbipMissing: Boolean;
begin
  Result := not FileExists(ExpandConstant('{commonpf}\USBip\usbip.exe'));
end;

function HidHideMissing: Boolean;
begin
  Result := not FileExists(ExpandConstant('{commonpf}\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'));
end;

{ Stop a running tray/bridge before copying files (avoid locked binaries). When a
  driver is being installed fresh, ask Setup to recommend a restart at the end --
  the usbip UDE virtual host controller only attaches cleanly after a reboot. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var rc: Integer;
begin
  Exec('taskkill.exe', '/IM VdsTray.exe /F', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec('taskkill.exe', '/IM vdsd.exe /F',    '', SW_HIDE, ewWaitUntilTerminated, rc);
  if UsbipMissing or HidHideMissing then
    NeedsRestart := True;
  Result := '';
end;
