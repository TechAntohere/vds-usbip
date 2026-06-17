// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_build_info.hh"
#include "vds_config.hh"
#include "vds_io.hh"
#include "vdsctl_common.hh"

namespace {

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";
constexpr const char *kModuleMaxPortPath =
    "/sys/module/vds_hcd/parameters/max_port";
constexpr const char *kModprobeConfigPath = "/etc/modprobe.d/vds_hcd.conf";

std::optional<unsigned> parse_configured_max_port(std::string_view text) {
  if (text.empty() ||
      !std::all_of(text.begin(), text.end(),
                   [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
    return std::nullopt;
  }

  unsigned long long value = 0;
  for (const char ch : text) {
    value = value * 10 + static_cast<unsigned>(ch - '0');
    if (value > std::numeric_limits<unsigned>::max()) {
      return std::nullopt;
    }
  }
  if (value < vds::kMinPortCount || value > vds::kMaxPortCount) {
    return std::nullopt;
  }
  return static_cast<unsigned>(value);
}

std::optional<std::string> request_daemon(const std::string &socket_path,
                                          const std::string &command,
                                          bool allow_missing) {
  vds::UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw std::runtime_error("failed to open daemon control socket: " +
                             std::string(std::strerror(errno)));
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("control socket path is too long: " + socket_path);
  }
  std::strncpy(address.sun_path, socket_path.c_str(),
               sizeof(address.sun_path) - 1);

  if (::connect(fd.get(), reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) < 0) {
    const int error = errno;
    if (allow_missing && (error == ENOENT || error == ECONNREFUSED)) {
      return std::nullopt;
    }
    throw std::runtime_error("failed to connect " + socket_path + ": " +
                             std::strerror(error));
  }

  vds::write_full(fd.get(), command);
  (void)::shutdown(fd.get(), SHUT_WR);

  std::string response;
  std::array<char, 512> buffer{};
  while (true) {
    const ssize_t got = ::read(fd.get(), buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("failed to read daemon response: " +
                               std::string(std::strerror(errno)));
    }
    if (got == 0) {
      break;
    }
    response.append(buffer.data(), static_cast<std::size_t>(got));
  }
  return response;
}

std::optional<std::string>
request_daemon_if_running(const std::string &command) {
  return request_daemon(kDefaultControlSocket, command, true);
}

void notify_reload_if_running() { (void)request_daemon_if_running("RELOAD\n"); }

std::optional<unsigned> read_max_port_from_module() {
  std::ifstream file(kModuleMaxPortPath);
  if (!file) {
    return std::nullopt;
  }
  unsigned max_port = 0;
  file >> max_port;
  if (!file || max_port < vds::kMinPortCount || max_port > vds::kMaxPortCount) {
    return std::nullopt;
  }
  return max_port;
}

std::optional<unsigned> read_max_port_from_modprobe_config() {
  std::ifstream file(kModprobeConfigPath);
  if (!file) {
    return std::nullopt;
  }

  std::string token;
  while (file >> token) {
    constexpr std::string_view prefix = "max_port=";
    if (token.rfind(prefix, 0) != 0) {
      continue;
    }
    return parse_configured_max_port(token.substr(prefix.size()));
  }
  return std::nullopt;
}

unsigned read_configured_max_port() {
  if (const auto max_port = read_max_port_from_module()) {
    return *max_port;
  }
  if (const auto max_port = read_max_port_from_modprobe_config()) {
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
          .max_port_name = "max_port",
          .read_max_port = read_configured_max_port,
          .notify_reload = notify_reload_if_running,
          .request_control_optional = request_daemon_if_running,
          .request_control =
              [](const std::string &command) {
                return request_daemon(kDefaultControlSocket, command, false);
              },
      });
}
