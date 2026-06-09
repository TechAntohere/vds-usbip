// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_build_info.hh"
#include "vds_config.hh"
#include "vds_udev.hh"

namespace {

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";

void usage() {
  std::cerr << "vdsctl (" << vds::kVersion
            << "): vDS management utility - Copyright (C) " << vds::kBuildYear
            << " Jihong Min\n"
            << "usage:\n"
            << "  vdsctl attach <address> [--identity ds5|dse] "
               "[--limit-dev /dev/vds0[,/dev/vds1...]]\n"
            << "  vdsctl detach <address>\n"
            << "  vdsctl list\n"
            << "  vdsctl trace on|off [--scope all|input|output]\n";
}

void write_full(int fd, std::string_view text) {
  const char *data = text.data();
  std::size_t size = text.size();
  while (size > 0) {
    const ssize_t written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("write failed: " +
                               std::string(std::strerror(errno)));
    }
    if (written == 0) {
      throw std::runtime_error("write returned zero bytes");
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
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

  write_full(fd.get(), command);
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

void notify_reload_if_running() {
  (void)request_daemon(kDefaultControlSocket, "RELOAD\n", true);
}

void require_no_extra_args(int argc, int expected) {
  if (argc != expected) {
    throw std::runtime_error("unexpected argument");
  }
}

using RuntimeStatus = std::pair<std::string, bool>;

std::vector<RuntimeStatus> load_runtime_statuses() {
  const auto response = request_daemon(kDefaultControlSocket, "LIST\n", true);
  if (!response) {
    return {};
  }

  std::istringstream lines(*response);
  std::string line;
  if (!std::getline(lines, line) || line != "OK") {
    return {};
  }

  std::vector<RuntimeStatus> statuses;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }

    std::istringstream fields(line);
    std::string address;
    std::string state;
    std::string extra;
    if (!(fields >> address >> state) || (fields >> extra)) {
      throw std::runtime_error("malformed daemon LIST response");
    }
    if (state != "connected" && state != "disconnected") {
      throw std::runtime_error("unknown daemon LIST state: " + state);
    }
    statuses.emplace_back(vds::normalize_bluetooth_address(address),
                          state == "connected");
  }
  return statuses;
}

bool runtime_connected(std::string_view address,
                       const std::vector<RuntimeStatus> &statuses) {
  for (const auto &[status_address, connected] : statuses) {
    if (status_address == address) {
      return connected;
    }
  }
  return false;
}

void print_db(const vds::BindingDb &db,
              const std::vector<RuntimeStatus> &statuses) {
  for (const auto &controller : db.controllers) {
    std::cout << (runtime_connected(controller.address, statuses)
                      ? "[connected] "
                      : "[disconnected] ")
              << controller.address
              << " identity=" << vds::binding_identity_name(controller.identity)
              << " limit-dev=";
    for (std::size_t i = 0; i < controller.limit_devices.size(); ++i) {
      if (i > 0) {
        std::cout << ',';
      }
      std::cout << controller.limit_devices[i];
    }
    std::cout << "\n";
  }
}

void command_attach(int argc, char **argv) {
  if (argc < 3) {
    throw std::runtime_error("attach requires an address");
  }

  vds::ControllerBindingConfig binding{
      .address = vds::normalize_bluetooth_address(argv[2]),
      .identity = vds::BindingIdentity::Auto,
      .limit_devices = {},
  };

  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--identity" && i + 1 < argc) {
      binding.identity = vds::parse_binding_identity(argv[++i]);
    } else if (arg == "--limit-dev" && i + 1 < argc) {
      binding.limit_devices = vds::parse_limit_devices(argv[++i]);
    } else {
      throw std::runtime_error("unknown attach option: " + std::string(arg));
    }
  }

  const std::vector<std::string> devices = vds::discover_vds_devices();
  vds::update_binding_db(vds::kDefaultBindingDbPath, [&](vds::BindingDb &db) {
    vds::upsert_binding(db, binding);
    vds::validate_binding_assignments(db, devices);
  });
  notify_reload_if_running();

  std::cout << "OK attached " << binding.address
            << " identity=" << vds::binding_identity_name(binding.identity);
  if (!binding.limit_devices.empty()) {
    std::cout << " limit-dev=";
    for (std::size_t i = 0; i < binding.limit_devices.size(); ++i) {
      if (i > 0) {
        std::cout << ',';
      }
      std::cout << binding.limit_devices[i];
    }
  }
  std::cout << "\n";
}

void command_detach(int argc, char **argv) {
  require_no_extra_args(argc, 3);
  const std::string address = vds::normalize_bluetooth_address(argv[2]);
  vds::update_binding_db(vds::kDefaultBindingDbPath, [&](vds::BindingDb &db) {
    (void)vds::remove_binding(db, address);
  });
  notify_reload_if_running();
  std::cout << "OK detached " << address << "\n";
}

void command_list(int argc) {
  require_no_extra_args(argc, 2);
  print_db(vds::load_binding_db(vds::kDefaultBindingDbPath),
           load_runtime_statuses());
}

void command_trace(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    throw std::runtime_error("trace requires on or off with optional --scope");
  }

  const std::string_view mode = argv[2];
  std::string_view scope = "all";
  if (argc == 5) {
    if (std::string_view(argv[3]) != "--scope") {
      throw std::runtime_error("trace option must be --scope");
    }
    scope = argv[4];
  }
  if (scope != "all" && scope != "input" && scope != "output") {
    throw std::runtime_error("trace scope must be all, input, or output");
  }

  std::string command;
  if (mode == "on") {
    command = "TRACE ON " + std::string(scope) + "\n";
  } else if (mode == "off") {
    command = "TRACE OFF " + std::string(scope) + "\n";
  } else {
    throw std::runtime_error("trace requires on or off");
  }

  const auto response = request_daemon(kDefaultControlSocket, command, false);
  if (response) {
    std::cout << *response;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || std::string(argv[1]) == "--help" ||
        std::string(argv[1]) == "-h") {
      usage();
      return argc < 2 ? 1 : 0;
    }

    const std::string command = argv[1];
    if (command == "attach") {
      command_attach(argc, argv);
    } else if (command == "detach") {
      command_detach(argc, argv);
    } else if (command == "list") {
      command_list(argc);
    } else if (command == "trace") {
      command_trace(argc, argv);
    } else {
      throw std::runtime_error("unknown command: " + command);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "vdsctl: " << error.what() << "\n";
    return 1;
  }
}
