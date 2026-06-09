# vDS (virtual DualSense)

[![Sponsor](https://img.shields.io/badge/Sponsor-hurryman2212-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/hurryman2212)

Linux virtual USB-to-Bluetooth bridge for DualSense and DualSense Edge Wireless
Controllers, focused on USB-only features such as 4-channel audio-based haptic
feedback and adaptive triggers, but not limited to them. Currently, all
USB-based features are supported except headset output, microphone input, and
firmware updates (firmware updates are not possible).

Detailed DualSense output and haptics packet handling is based on
[DS5Dongle](https://github.com/awalol/DS5Dongle) and protocol capture research.
In theory, other physical gamepad controllers could be implemented as vDS
backends and attached to vDS, as long as they can provide the
DualSense-specific features required by games.

> [!WARNING]
> This project is still in development. Performance has not been tuned, and
> race conditions may still exist. Many components currently run in userspace,
> so continuous context switching can add significant overhead.
>
> ```text
> Application
>   -> Userspace audio server (e.g. PipeWire)
>   -(ALSA PCM)-> Linux ALSA stack [snd-usb-audio.ko]
>   -(USB isochronous OUT URBs)-> **Linux USB stack [vds_hcd.ko]**
>   **-(/dev/vdsX)-> vdsd (implements raw Bluetooth HID handling)**
>   **-(AF_BLUETOOTH L2CAP socket)->** Linux Bluetooth stack
>   -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
> ```

This project includes a custom virtual USB HCD primarily because Linux's
`dummy_hcd` module does not support isochronous transfers, which are required
for the USB audio path used by DualSense haptics. The custom HCD currently also
hosts the `/dev/vdsX` bridge and virtual port lifecycle, but that architecture
may be revisited if `dummy_hcd` gains suitable isochronous transfer support.
The current architecture should therefore be treated as temporary. Future work
will move more of the bridge from userspace into the kernel to reduce
context-switching overhead.

Currently, the virtual USB HID polling intervals match each controller's USB
HID specification: DualSense HID IN/OUT run at 250 Hz, while DualSense Edge HID
IN runs at 1000 Hz and HID OUT runs at 250 Hz. These values affect host-side
HID report scheduling, input/output latency, USB/HCD wakeups, and
CPU/context-switch load.

## Initial Bluetooth Pairing

> [!IMPORTANT]
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

For PipeWire/WirePlumber, install the included rule:

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp 99-vds-dualsense.conf ~/.config/wireplumber/wireplumber.conf.d/
```

Restart the user audio services after changing this file:

```sh
systemctl --user restart pipewire pipewire-pulse wireplumber
```

> [!IMPORTANT]
> The microphone/source node is disabled on purpose. vDS currently supports
> controller speaker and haptics output, but not microphone input. If
> WirePlumber, games, or tools such as `pavucontrol` open the capture endpoint,
> the extra USB audio traffic can disrupt controller speaker and haptics
> output. Disabling the source node avoids that conflict.

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
