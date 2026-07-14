// SPDX-License-Identifier: MIT
// USBIP virtual device transport for vDS on Windows.
//
// Replaces the vds_usb.sys/vds_filter.sys kernel-driver transport with a
// userspace USB/IP server that a signed, already-WHQL'd client driver
// (usbip-win2's usbip2_ude vhci) can import as a virtual USB device. No
// custom kernel driver, no test-signing mode required.
//
// Design: this module exposes a Win32 HANDLE (an overlapped-capable named
// pipe) that behaves exactly like the \\.\vdsX device handle vdsd.cc already
// speaks Frame protocol over (see vds_io.hh: read_handle_frame /
// write_handle_frame). vdsd.cc, handle_virtual_frame(), and
// handle_bluetooth_frame() do not need to change at all. Internally, a
// background thread bridges that pipe to a real USB/IP TCP connection from
// usbip-win2, translating USBIP_CMD_SUBMIT/RET_SUBMIT <-> vds_frame_* types.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h/ws2tcpip.h must be included before windows.h in this
// translation unit, or windows.h drags in the legacy winsock.h and every
// socket symbol ends up redefined with conflicting linkage (MSVC C2375).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "uapi/vds.h"
#include "vds_win32.hh"

namespace vds::win::usbip {

// USB/IP protocol version this module implements. Confirmed against
// usbip-win2 / usbipd-win (both currently speak v1.1.1). If usbip-win2 bumps
// this, OP_REQ_IMPORT's version field comparison below needs updating.
constexpr std::uint16_t kProtocolVersion = 0x0111;

// Standard USB/IP TCP port. usbip-win2 lets the user pick a host:port when
// attaching (`usbip attach -r 127.0.0.1 -b <busid>`), so this is a default,
// not a hard requirement -- exposed so the installer/config can override it
// if multiple vDS ports need distinct listeners.
constexpr std::uint16_t kDefaultPort = 3240;

// One USB/IP-exported virtual port. Owns:
//  - a TCP listener that answers OP_REQ_DEVLIST / OP_REQ_IMPORT and then
//    speaks USBIP_CMD_SUBMIT/RET_SUBMIT for the imported device
//  - a named pipe server presenting the exact same Frame protocol vdsd.cc
//    already consumes, so the rest of the daemon is untouched
//
// NOTE: control transfers and the HID interrupt IN/OUT path are implemented
// against the public USB/IP spec and vDS's own existing descriptor tables
// (include/vds/ds5_usb.h) and are expected to work once wired up. The
// isochronous audio OUT/IN path (speaker haptics + mic) is present but
// flagged as first-pass -- iso packet timing over a loopback TCP socket in
// usermode needs real hardware iteration to avoid underrun/overrun; treat it
// as a starting point, not a finished implementation.
class VirtualPort {
public:
  VirtualPort(std::uint32_t profile, unsigned port_index,
              std::uint16_t tcp_port = kDefaultPort);
  ~VirtualPort();

  VirtualPort(const VirtualPort &) = delete;
  VirtualPort &operator=(const VirtualPort &) = delete;

  // Named pipe path vdsd.cc's open_device()/open_device_for_control() should
  // open in place of \\.\vdsX when running in USB/IP mode.
  const std::string &pipe_path() const { return pipe_path_; }

  // True once usbip-win2 has completed OP_REQ_IMPORT and CMD_SUBMIT traffic
  // is flowing. Mirrors what VDS_PORT_INFO_USB_PLUGGED meant under the old
  // driver-based transport.
  bool usb_attached() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string pipe_path_;
};

// Starts (or reuses) a VirtualPort for the given profile/port index. Mirrors
// the lifetime shape of the old VirtualPortBindingGuard: construct when a
// controller is bound, destroy when unbound.
std::unique_ptr<VirtualPort> open_usbip_virtual_port(std::uint32_t profile,
                                                     unsigned port_index);

} // namespace vds::win::usbip
