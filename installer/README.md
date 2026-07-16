# Building the vDS installer

The setup executable is produced by **[Inno Setup 6](https://jrsoftware.org/isdl.php)**
(`ISCC.exe`) from the script [`vds.iss`](vds.iss). Inno Setup is a free
third-party tool and is **not** part of this repository — install it first.

The built installer and the bundled third-party driver binaries are
intentionally **git-ignored**; the released `.exe` is published on the repo's
GitHub **Releases** page instead of being committed.

## Prerequisites

- Inno Setup 6 (`ISCC.exe`)
- The .NET SDK used by the tray app (for `dotnet publish`)
- A toolchain able to build `vdsd` / `vdsctl` (see the repo's top-level build)

## Steps

1. **Build the bridge binaries** → `build\vdsd.exe`, `build\vdsctl.exe`
   (use the repo's build script, e.g. `build.ps1`).

2. **Publish the tray app** (self-contained single file) so its output lands in
   the path `vds.iss` expects:

   ```powershell
   dotnet publish ui\VdsTray -c Release -r win-x64 --self-contained true `
     -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
   # -> ui\VdsTray\bin\Release\net10.0-windows\win-x64\publish\VdsTray.exe
   ```

3. **Drop the signed drivers** into [`redist\`](redist/) (see
   [`redist\README.md`](redist/README.md)):
   - `redist\usbip-win2.exe` — from the usbip-win2 releases (x64)
   - `redist\HidHide.exe` — from the HidHide releases (x64)

   If either is absent the installer still compiles; it just skips that driver
   and assumes it is already present on the target machine.

4. **Compile the installer:**

   ```powershell
   & "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\vds.iss
   # -> installer\output\vDS-Setup-<version>-usbip.exe
   ```

## What the installed setup does

Installs (as administrator): the signed **usbip-win2** + **HidHide** drivers
(skipped if already present), the `vdsd` / `vdsctl` bridge, and the self-contained
tray app; configures HidHide (whitelists `vdsd`, hides the Bluetooth DualSense);
adds a Start Menu shortcut and logon autostart. When a driver is installed fresh
it recommends a restart — the usbip virtual host controller attaches cleanly only
after a reboot.
