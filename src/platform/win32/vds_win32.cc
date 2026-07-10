// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <stdexcept>

#include "uapi/vds.h"
#include "vds_win32.hh"

namespace vds::win {

std::string win32_error_message(DWORD error) {
  LPSTR message = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageA(
      flags, nullptr, error, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
  if (length == 0 || message == nullptr) {
    return "Win32 error " + std::to_string(error);
  }

  std::string text(message, length);
  LocalFree(message);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

std::string query_vds_driver_version(HANDLE device) {
  vds_driver_info info{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(device, VDS_IOCTL_GET_DRIVER_INFO, nullptr, 0, &info,
                       sizeof(info), &bytes_returned, nullptr)) {
    throw std::runtime_error("driver version query failed: " +
                             win32_error_message(GetLastError()));
  }
  if (bytes_returned != sizeof(info) ||
      info.version != VDS_DRIVER_INFO_VERSION || info.size != sizeof(info) ||
      info.driver_version[0] == '\0' ||
      info.driver_version[VDS_DRIVER_VERSION_MAX - 1] != '\0') {
    throw std::runtime_error("invalid driver version reply");
  }
  return info.driver_version;
}

} // namespace vds::win
