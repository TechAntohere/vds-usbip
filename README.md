# vDS (virtual DualSense)

[![Sponsor](https://img.shields.io/badge/Sponsor-hurryman2212-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/hurryman2212)

> [!NOTE]
>
> Starting with 0.2.0, the internal database format used by `vdsd` has changed
> for future extensibility and is not compatible with older releases. Remove
> `/var/lib/vds`, then register your controllers again with `vdsctl attach ...`.

Linux virtual USB-to-Bluetooth bridge for DualSense and DualSense Edge Wireless
Controllers, focused on USB-only features such as 4-channel audio-based haptic
feedback, but not limited to it. Currently, vDS supports all USB-based features
that can be carried over Bluetooth, except headset output, microphone input, and
firmware updates (firmware updates are not possible).

Detailed DualSense output and haptics packet handling is based on
[DS5Dongle](https://github.com/awalol/DS5Dongle) and protocol capture research.
In theory, other physical gamepad controllers could be implemented as vDS
backends and attached to vDS, as long as they can provide the DualSense-specific
features required by games. The vDS infrastructure could also be extended to
support physical transports other than Bluetooth.

This project includes a custom virtual USB HCD primarily because Linux's
`dummy_hcd` module does not support isochronous transfers, which are required
for the USB audio path used by DualSense haptics. The custom HCD currently also
hosts the `/dev/vdsX` bridge and virtual port lifecycle, but that architecture
may be revisited if `dummy_hcd` gains suitable isochronous transfer support. The
current architecture should therefore be treated as temporary. Future work will
move more of the bridge from userspace into the kernel to reduce
context-switching overhead from the current flow:

```text
Application
  -> Userspace audio server (e.g. PipeWire)
  -(ALSA PCM)-> Linux ALSA stack [snd-usb-audio.ko]
  -(USB isochronous OUT URBs)-> **Linux USB stack [vds_hcd.ko]**
  **-(/dev/vdsX)-> vdsd (implements raw Bluetooth HID handling)**
  **-(AF_BLUETOOTH L2CAP socket)->** Linux Bluetooth stack
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

Currently, the virtual USB HID polling intervals match each controller's USB HID
specification: DualSense HID IN/OUT run at 250 Hz, while DualSense Edge HID IN
runs at 1000 Hz and HID OUT runs at 250 Hz. These values affect host-side HID
report scheduling, input/output latency, USB/HCD wakeups, and CPU/context-switch
load.

## Dependencies

### Runtime Dependencies

- `libopus` (Bluetooth audio encoding)
- `libudev` (virtual device discovery)
- `libdbus-1` (BlueZ device metadata)
- `libbluetooth` (Bluetooth address and L2CAP helpers)

### Build Dependencies

- `gnu99` compiler toolchain compatible with the target kernel build
- `make`
- Target Linux kernel headers
- `dkms` (for installing and rebuilding the kernel module automatically)
- `c++20` compiler toolchain for userspace
- `cmake` (3.20 or newer)
- `pkg-config`

On Debian-based systems, install the dependencies with:

```sh
sudo apt install libopus-dev libudev-dev libdbus-1-dev libbluetooth-dev
sudo apt install build-essential dkms cmake pkg-config
```

For Arch/CachyOS-based systems:

```sh
sudo pacman -S opus systemd dbus bluez-libs
sudo pacman -S base-devel dkms cmake pkgconf
```

Install the header package matching your running kernel separately. If your
kernel was built with the LLVM toolchain, install the matching LLVM toolchain
instead of relying only on the default GNU toolchain.

## Module Install

Build the kernel module locally:

```sh
make -C module
```

Install or remove it through DKMS:

```sh
sudo make -C module install
sudo make -C module uninstall
```

Load the module:

```sh
sudo modprobe vds_hcd
```

To expose more than one virtual controller:

```sh
sudo modprobe vds_hcd max_port=2
```

## Userspace Install

Build the daemon and CLI:

```sh
cmake . -B build
make -C build
```

Install or remove the userspace tools:

```sh
sudo make -C build install
sudo make -C build uninstall
```

The installed tools are:

```sh
vdsd
vdsctl
```

## Initial Bluetooth Pairing

> [!IMPORTANT]
>
> Current limitation: run `bluetoothd` with the input plugin disabled
> (`--noplugin=input`). vDS needs direct ownership of the Bluetooth HID Control
> and Interrupt L2CAP channels. If the normal BlueZ input plugin is active, it
> can claim the controller first and expose it as a regular Bluetooth gamepad
> instead of letting vDS bridge it as a virtual USB controller.

Pair each physical controller once before registering it with `vdsctl`. Start
`bluetoothctl`, put the controller in Bluetooth pairing mode (hold
<img src="https://controller.dl.playstation.net/controller/images/b_create.png" height="16">
Create and PS until the light bar blinks rapidly), then use the MAC address
printed by `scan on`:

```text
agent NoInputNoOutput
default-agent
pairable on
scan on
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
scan off
quit
```

When re-pairing a controller that BlueZ already knows, run
`remove XX:XX:XX:XX:XX:XX` before `scan on`.

## Binding Physical Controllers

Load two virtual controller ports:

```sh
sudo modprobe vds_hcd max_port=2
```

Start the daemon:

```sh
sudo vdsd
```

Bind two paired/trusted Bluetooth DualSense or DualSense Edge controllers:

```sh
sudo vdsctl attach aa:bb:cc:dd:ee:01 --identity ds5 --limit-dev /dev/vds0
sudo vdsctl attach aa:bb:cc:dd:ee:02 --identity dse --limit-dev /dev/vds1
```

List persistent bindings:

```sh
sudo vdsctl list
```

Toggle packet-level daemon tracing:

```sh
sudo vdsctl trace on --scope all
sudo vdsctl trace on --scope input
sudo vdsctl trace on --scope output
sudo vdsctl trace off --scope all
```

Detach each binding:

```sh
sudo vdsctl detach aa:bb:cc:dd:ee:XX
```

## Audio Setup

Configure the virtual controller audio output as 48 kHz 4-channel S16_LE PCM.
The exact setup differs by audio stack, such as PipeWire, PulseAudio, ALSA, or
JACK.

For PipeWire/WirePlumber, set the controller card profile to `pro-audio`. Find
the WirePlumber device id:

```sh
wpctl status
```

List the available profile indexes for that device:

```sh
pw-cli e <device-id> EnumProfile | awk '/Profile:index/{getline; idx=$2} /Profile:name/{getline; gsub(/"/, "", $2); print idx, $2}'
```

Set the `pro-audio` profile:

```sh
wpctl set-profile <device-id> <pro-audio-profile-index>
```

> [!WARNING]
>
> The microphone/source node is disabled on purpose. vDS currently supports
> controller speaker and haptics output, but not microphone input. If
> WirePlumber, games, or tools such as `pavucontrol` open the capture endpoint,
> the extra USB audio traffic can disrupt controller speaker and haptics output,
> as well as main audio playback. Each system needs an audio-stack specific
> setting like the one below to disable the controller capture/source endpoint.

After setting the `pro-audio` profile, install the included WirePlumber rule. It
gives the virtual controller stable display names, sets the output node to 4
channels, disables channelmix normalization, disables the unsupported microphone
source node, and lowers its priority:

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp 99-vds-dualsense-wireplumber.conf ~/.config/wireplumber/wireplumber.conf.d/
```

Restart the user audio services after changing this file:

```sh
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## Input Setup

This step may not be strictly required on every system, but `vds_hcd` is a
platform controller. Without an override, a DualSense touchpad connected through
vDS can be classified as an `internal` touchpad instead of an `external`
touchpad like a USB-connected controller. Install the included udev rule so the
virtual controller touchpad is classified like the real USB controller:

```sh
sudo cp 99-vds-dualsense-udev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
```

## Testing

Check that the virtual USB controller enumerated:

```sh
lsusb -d 054c:
```

Check input devices and force-feedback support with standard tools:

```sh
evtest
fftest /dev/input/eventX
```

Check the virtual USB audio endpoint with standard ALSA tools:

```sh
aplay -l
speaker-test -D hw:<card-number>,<device-number> -c 4 -r 48000 -F S16_LE -t sine
```

Inspect daemon logs:

```sh
sudo tail -f /var/log/vdsd.log
```
