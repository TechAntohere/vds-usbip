// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "uapi/vds.h"
#include "unique_handle.hh"
#include "vds_io.hh"
#include "vds_log.hh"

namespace vds::win {

class BluetoothTransport {
public:
  virtual ~BluetoothTransport() = default;

  virtual Frame read_frame() = 0;
  virtual std::optional<std::vector<std::uint8_t>>
  read_feature_report(std::uint8_t report_id) {
    (void)report_id;
    return std::nullopt;
  }
  virtual void write_feature_report(std::span<const std::uint8_t> report) = 0;
  // Full HID feature-report byte length (caps.FeatureReportByteLength), 0 if
  // unknown. Needed to build fixed-length command-channel reports (0x80).
  virtual std::size_t feature_report_length() const { return 0; }
  virtual void write_interrupt_packet(std::span<const std::uint8_t> packet) = 0;
  virtual bool
  try_write_interrupt_packet(std::span<const std::uint8_t> packet) = 0;
  virtual std::optional<std::string> take_output_diagnostics(bool force) {
    (void)force;
    return std::nullopt;
  }
  virtual void cancel() = 0;
  virtual std::string description() const = 0;
};

struct HidBluetoothDevice {
  std::string path;
  std::string address;
  std::string name;
  std::uint32_t profile = VDS_PROFILE_DS5;
  bool profile_valid = false;
  bool filter_backed = false;
  bool bluetooth_connected = false;
  bool report_target = false;
  bool access_restricted = false;
};

struct HidBluetoothDeviceSnapshot {
  std::uint32_t generation = 0;
  std::vector<HidBluetoothDevice> devices;
};

class FilterBluetoothDeviceChangeWait {
public:
  FilterBluetoothDeviceChangeWait();
  ~FilterBluetoothDeviceChangeWait();

  FilterBluetoothDeviceChangeWait(const FilterBluetoothDeviceChangeWait &) =
      delete;
  FilterBluetoothDeviceChangeWait &
  operator=(const FilterBluetoothDeviceChangeWait &) = delete;

  bool arm(std::uint32_t generation);
  bool complete();
  void cancel();

  HANDLE event() const { return event_.get(); }
  bool pending() const { return pending_; }

private:
  UniqueHandle handle_;
  UniqueHandle event_;
  OVERLAPPED overlapped_{};
  vds_filter_device_change change_{};
  bool pending_ = false;
};

std::optional<HidBluetoothDevice>
find_filter_bluetooth_device(const std::string &address);
bool filter_provider_available();
std::string filter_driver_version();
HidBluetoothDeviceSnapshot list_filter_bluetooth_device_snapshot();
std::vector<HidBluetoothDevice> list_filter_bluetooth_devices();
// Plain-HID discovery, no vds_filter.sys dependency. See definition for
// current limitations (no exclusive-access tracking yet).
// `logger`, if non-null, receives a WARN when a HID Bluetooth device is
// found but its address could not be extracted (previously this failed
// completely silently -- see extract_bluetooth_address_from_hid_path /
// extract_bluetooth_address_from_devinst -- which cost real debugging
// time tracking down a device that was simply being skipped).
std::vector<HidBluetoothDevice>
list_hid_bluetooth_devices(Logger *logger = nullptr);
std::string describe_bluetooth_lookup(const std::string &address);
std::unique_ptr<BluetoothTransport>
make_filter_bluetooth_transport(const std::string &address);

} // namespace vds::win
