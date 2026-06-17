# vDS Windows Guide

[Back to README.md](README.md)

Windows uses a UdeCx virtual USB driver, a Bluetooth HID visibility filter, and
the HIDClass/HidBth Bluetooth HID stack. Games and applications should see the
vDS virtual USB controller, not the physical Bluetooth HID device.

The current architecture uses both Windows kernel drivers and the userspace
daemon for runtime communication. For example, haptic feedback currently follows
this flow:

```text
Application
  -(WASAPI render stream)-> Audio Engine [audiodg.exe / audioeng.dll]
  -(USB audio PCM)-> USB Audio class system driver [Usbaudio.sys]
  -(USB isochronous OUT URBs)-> Windows USB stack [vds_usb.sys]
  -(\\.\vdsX)-> vdsd
  -(Bluetooth HID output reports)-> HIDClass + HidBth transport minidriver [hidclass.sys + Hidbth.sys]
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

The virtual USB HID endpoint descriptors match each controller's USB HID
specification: DualSense HID IN/OUT use 4 ms intervals, while DualSense Edge HID
IN uses 1 ms and HID OUT uses 4 ms. vDS uses the HID IN interval to pace virtual
USB input URB completion.

The future goal is to remove userspace from runtime communication where possible
and reduce overhead.

## Dependencies

### Runtime Dependencies

- Microsoft Visual C++ Redistributable
- Opus runtime DLL

### Build Dependencies

- Visual Studio Build Tools with the C++ toolchain
- Windows Driver Kit
- `git` (for versioning)
- `cmake` (3.12 or newer)
- Opus CMake package

Install the dependencies with `winget` and `vcpkg`:

```powershell
winget install --exact --id Microsoft.VCRedist.2015+.x64
winget install --exact --id Git.Git; winget install --exact --id Kitware.CMake; winget install --exact --id Microsoft.VisualStudio.2022.BuildTools; winget install --exact --id Microsoft.WindowsWDK
vcpkg install opus
```

## Driver Install

> [!CAUTION]
>
> The vDS Windows drivers are experimental. Installing or running them can cause
> unexpected system crashes or make Windows fail to boot. Use at your own risk.
>
> Open an elevated PowerShell session, enable Windows test signing, and reboot
> before installing the driver package:
>
> ```powershell
> bcdedit /set testsigning on
> shutdown /r /t 0
> ```
>
> The driver install scripts create a local test code-signing certificate, trust
> it in the Local Machine Root and TrustedPublisher stores, sign the driver
> package, and install it with `pnputil`. A reboot may still be required if
> Windows reports that an old driver service is pending deletion or a device
> restart cannot complete.

Windows uses one package with these driver roles:

- `vds_usb.sys`: UdeCx virtual USB root, logical ports, `\\.\vds0` through
  `\\.\vds3`, and USB child creation.
- `vds_filter.sys`: physical Bluetooth DualSense/DualSense Edge HID access
  control. Normal applications should use the vDS virtual USB device.

Install or remove the Windows driver package from an elevated PowerShell
session:

```powershell
.\windrv\install.ps1
.\windrv\uninstall.ps1
```

`MaxPort` must be in the range `1..4`. The installer imports the package default
value `4` from `windrv\vds_usb\vds_usb.reg` into:

```text
HKLM\SYSTEM\CurrentControlSet\Services\vds_usb\Parameters\MaxPort
```

Changing `MaxPort` requires restarting the vDS USB root device or rebooting.
Edit `windrv\vds_usb\vds_usb.reg` before installation to change the imported
default.

## Userspace Install

Build and install the daemon and CLI with CMake:

```powershell
cmake -S . -B build
cmake --build build
cmake --install build
```

Remove the installed userspace tools with CMake:

```powershell
cmake --build build --target uninstall
```

The installed tools are:

```text
C:\Program Files\vds\vdsd.exe
C:\Program Files\vds\vdsctl.exe
```

Additionally, you can add the install directory to `PATH` so the tools can be
called directly:

```powershell
.\install_env.ps1 "C:\Program Files\vds"
```
