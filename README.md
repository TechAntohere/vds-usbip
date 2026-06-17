# vDS (virtual DualSense)

[![Sponsor](https://img.shields.io/badge/Sponsor-hurryman2212-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/hurryman2212)

Virtual USB-to-Bluetooth bridge for DualSense and DualSense Edge Wireless
Controllers. vDS exposes a Bluetooth-connected controller as a virtual USB
DualSense-class device so games and applications can use features normally
available only over USB. `vdsd` translates the virtual USB traffic to and from
the physical controller's Bluetooth protocol.

Linux uses a custom virtual USB HCD and raw BlueZ/L2CAP input. Windows uses a
UdeCx virtual USB driver, a Bluetooth HID visibility filter, and the Windows
Bluetooth HID path. Currently, vDS supports USB-based features that can be
carried over Bluetooth, except headset output, microphone input, and firmware
updates (firmware updates is not possible).

Detailed DualSense output and haptics packet handling is based on
[DS5Dongle](https://github.com/awalol/DS5Dongle) and protocol capture research.
In theory, other physical gamepad controllers could be implemented as vDS
backends and attached to vDS, as long as they can provide the DualSense-specific
features required by games. The vDS infrastructure could also be extended to
support physical transports other than Bluetooth.

## Platform Setup Guides

- [Linux](README-LINUX.md)
- [Windows](README-WINDOWS.md)

## Common Controller Configuration

`vdsctl attach` registers paired physical Bluetooth controllers in `vdsd.db`.
The same command format and JSONL database format are used on Linux and Windows.

```sh
vdsctl attach aa:bb:cc:dd:ee:01 --profile ds5 --ports 0
vdsctl attach aa:bb:cc:dd:ee:02 --profile dse --ports 1
```

Omit `--ports` or pass `--ports ""` to allow all configured ports. Omit
`--profile` or pass `--profile ""` to use the physical controller profile.

```jsonl
{"address":"aa:bb:cc:dd:ee:01","profile":"","ports":[]}
{"address":"aa:bb:cc:dd:ee:02","profile":"dse","ports":[1]}
```

`--ports 0,2` maps to `/dev/vds0,/dev/vds2` on Linux and `\\.\vds0,\\.\vds2` on
Windows.

```sh
vdsctl list
vdsctl trace on --scope all
vdsctl trace off --scope all
vdsctl detach aa:bb:cc:dd:ee:XX
```
