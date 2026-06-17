// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

#include <windows.h>

#include "vds_build_info.hh"
#include "vds_config.hh"
#include "vds_win32.hh"
#include "vdsctl_common.hh"

namespace {

constexpr const char *kDefaultControlPipe = R"(\\.\pipe\vdsd)";
constexpr const char *kVdsUsbParametersRegistryPath =
    R"(SYSTEM\CurrentControlSet\Services\vds_usb\Parameters)";

using vds::win::win32_error_message;

std::optional<std::string> request_daemon(const std::string &command,
                                          bool allow_missing) {
  std::array<char, 8192> buffer{};
  DWORD bytes_read = 0;
  if (CallNamedPipeA(kDefaultControlPipe, const_cast<char *>(command.data()),
                     static_cast<DWORD>(command.size()), buffer.data(),
                     static_cast<DWORD>(buffer.size()), &bytes_read, 2000)) {
    return std::string(buffer.data(), bytes_read);
  }

  const DWORD error = GetLastError();
  if (allow_missing &&
      (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY ||
       error == ERROR_SEM_TIMEOUT)) {
    return std::nullopt;
  }
  throw std::runtime_error("failed to connect " +
                           std::string(kDefaultControlPipe) + ": " +
                           win32_error_message(error));
}

std::optional<std::string>
request_daemon_if_running(const std::string &command) {
  return request_daemon(command, true);
}

void notify_reload_if_running() { (void)request_daemon_if_running("RELOAD\n"); }

std::optional<unsigned> read_max_port_from_registry_path(const char *path) {
  DWORD value = 0;
  DWORD value_size = sizeof(value);
  const LSTATUS status =
      RegGetValueA(HKEY_LOCAL_MACHINE, path, "MaxPort", RRF_RT_REG_DWORD,
                   nullptr, &value, &value_size);
  if (status != ERROR_SUCCESS) {
    return std::nullopt;
  }
  if (value < vds::kMinPortCount || value > vds::kMaxPortCount) {
    return vds::kMaxPortCount;
  }
  return value;
}

unsigned read_configured_max_port() {
  if (const auto max_port =
          read_max_port_from_registry_path(kVdsUsbParametersRegistryPath)) {
    return *max_port;
  }
  return vds::kMaxPortCount;
}

} // namespace

int main(int argc, char **argv) {
  return vds::run_vdsctl_app(
      argc, argv, vds::kVersion, vds::kBuildYear,
      vds::VdsctlPlatform{
          .db_path = vds::kDefaultDbPath,
          .max_port_name = "MaxPort",
          .read_max_port = read_configured_max_port,
          .notify_reload = notify_reload_if_running,
          .request_control_optional = request_daemon_if_running,
          .request_control =
              [](const std::string &command) {
                return request_daemon(command, false);
              },
      });
}
