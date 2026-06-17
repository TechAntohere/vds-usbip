// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "vdsctl_common.hh"

namespace vds {

namespace {

void drop_trailing_cr(std::string &line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
}

bool consume_ok_response_header(std::istringstream &lines) {
  std::string line;
  return std::getline(lines, line) && line == "OK";
}

} // namespace

std::string vdsctl_usage(std::string_view version,
                         std::string_view build_year) {
  std::string text = "vdsctl (";
  text += version;
  text += "): vDS management utility - Copyright (C) ";
  text += build_year;
  text += " Jihong Min\n"
          "usage:\n"
          "  vdsctl attach <address> [--profile ds5|dse|\"\"] "
          "[--ports 0[,1...]|\"\"]\n"
          "  vdsctl detach <address>\n"
          "  vdsctl list\n"
          "  vdsctl trace on|off [--scope all|input|output]\n";
  return text;
}

VdsctlCommand parse_vdsctl_command(std::string_view command) {
  if (command == "attach") {
    return VdsctlCommand::Attach;
  }
  if (command == "detach") {
    return VdsctlCommand::Detach;
  }
  if (command == "list") {
    return VdsctlCommand::List;
  }
  if (command == "trace") {
    return VdsctlCommand::Trace;
  }
  throw std::runtime_error("unknown command: " + std::string(command));
}

int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year,
                   const VdsctlPlatform &platform) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "-h" ||
        std::string_view(argv[1]) == "--help") {
      std::cerr << vdsctl_usage(version, build_year);
      return argc < 2 ? 1 : 0;
    }

    switch (parse_vdsctl_command(argv[1])) {
    case VdsctlCommand::Attach:
      std::cout << run_vdsctl_attach(
          argc, argv, platform.db_path, platform.read_max_port(),
          platform.max_port_name, platform.notify_reload);
      break;
    case VdsctlCommand::Detach:
      std::cout << run_vdsctl_detach(argc, argv, platform.db_path,
                                     platform.notify_reload);
      break;
    case VdsctlCommand::List:
      std::cout << run_vdsctl_list(
          argc, platform.db_path,
          load_vdsctl_runtime_statuses(platform.request_control_optional));
      break;
    case VdsctlCommand::Trace:
      std::cout << run_vdsctl_trace(argc, argv, platform.request_control);
      break;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "vdsctl: " << error.what() << "\n";
    return 1;
  }
}

void require_vdsctl_arg_count(int argc, int expected) {
  if (argc != expected) {
    throw std::runtime_error("unexpected argument");
  }
}

ControllerConfig parse_vdsctl_attach(int argc, char **argv) {
  if (argc < 3) {
    throw std::runtime_error("attach requires an address");
  }

  ControllerConfig config{
      .address = normalize_bluetooth_address(argv[2]),
      .profile = ControllerProfile::Unspecified,
      .ports = {},
  };

  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next_arg_is_value = [&] {
      return i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0;
    };
    if (arg == "--profile") {
      config.profile =
          parse_controller_profile(next_arg_is_value() ? argv[++i] : "");
    } else if (arg == "--ports") {
      config.ports = parse_ports(next_arg_is_value() ? argv[++i] : "");
    } else {
      throw std::runtime_error("unknown attach option: " + std::string(arg));
    }
  }

  return config;
}

VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    throw std::runtime_error("trace requires on or off with optional --scope");
  }

  const std::string_view mode = argv[2];
  VdsctlTraceCommand command{
      .enabled = false,
      .scope = "all",
  };
  if (argc == 5) {
    if (std::string_view(argv[3]) != "--scope") {
      throw std::runtime_error("trace option must be --scope");
    }
    command.scope = argv[4];
  }

  if (mode == "on") {
    command.enabled = true;
  } else if (mode == "off") {
    command.enabled = false;
  } else {
    throw std::runtime_error("trace requires on or off");
  }

  if (command.scope != "all" && command.scope != "input" &&
      command.scope != "output") {
    throw std::runtime_error("trace scope must be all, input, or output");
  }

  return command;
}

std::string
format_vdsctl_trace_control_command(const VdsctlTraceCommand &trace) {
  std::string command = trace.enabled ? "TRACE ON " : "TRACE OFF ";
  command += trace.scope;
  command += '\n';
  return command;
}

void validate_controller_ports_against_max_port(
    const ControllerConfig &config, unsigned max_port,
    std::string_view max_port_name) {
  if (max_port < kMinPortCount || max_port > kMaxPortCount) {
    throw std::runtime_error(
        "configured " + std::string(max_port_name) + " is outside range " +
        std::to_string(kMinPortCount) + ".." + std::to_string(kMaxPortCount));
  }

  for (const unsigned port : config.ports) {
    if (port >= max_port) {
      throw std::runtime_error(
          "port " + std::to_string(port) + " is outside configured " +
          std::string(max_port_name) + "=" + std::to_string(max_port));
    }
  }
}

std::vector<std::string> port_paths_for_max_port(unsigned max_port) {
  if (max_port < kMinPortCount || max_port > kMaxPortCount) {
    throw std::runtime_error("max_port must be in range " +
                             std::to_string(kMinPortCount) + ".." +
                             std::to_string(kMaxPortCount));
  }

  std::vector<std::string> devices;
  devices.reserve(max_port);
  for (unsigned port = 0; port < max_port; ++port) {
    devices.push_back(port_path_for_index(port));
  }
  return devices;
}

std::string run_vdsctl_attach(int argc, char **argv, const std::string &db_path,
                              unsigned max_port, std::string_view max_port_name,
                              const std::function<void()> &notify_reload) {
  ControllerConfig config = parse_vdsctl_attach(argc, argv);
  validate_controller_ports_against_max_port(config, max_port, max_port_name);
  update_config_db_for_attach(db_path, config, max_port);
  notify_reload();

  std::string text = "OK configured ";
  text += format_controller_config(config);
  text += '\n';
  return text;
}

std::string run_vdsctl_detach(int argc, char **argv, const std::string &db_path,
                              const std::function<void()> &notify_reload) {
  require_vdsctl_arg_count(argc, 3);
  const std::string address = normalize_bluetooth_address(argv[2]);
  const bool removed = update_config_db_for_detach(db_path, address);
  notify_reload();

  std::string text = removed ? "OK removed " : "OK not-configured ";
  text += address;
  text += '\n';
  return text;
}

std::string
run_vdsctl_list(int argc, const std::string &db_path,
                std::span<const VdsctlRuntimeStatus> runtime_statuses) {
  require_vdsctl_arg_count(argc, 2);
  return format_controller_config_list(load_config_db(db_path),
                                       runtime_statuses);
}

std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::optional<std::string>(const std::string &)>
        &request_control) {
  const VdsctlTraceCommand trace = parse_vdsctl_trace(argc, argv);
  const auto response =
      request_control(format_vdsctl_trace_control_command(trace));
  return response.value_or("");
}

void update_config_db_for_attach(const std::string &path,
                                 const ControllerConfig &config,
                                 unsigned max_port) {
  const std::vector<std::string> devices = port_paths_for_max_port(max_port);
  update_config_db(path, [&](ConfigDb &db) {
    upsert_controller_config(db, config);
    validate_config_assignments(db, devices);
  });
}

bool update_config_db_for_detach(const std::string &path,
                                 std::string_view address) {
  const std::string normalized = normalize_bluetooth_address(address);
  bool removed = false;
  update_config_db(path, [&](ConfigDb &db) {
    removed = remove_controller_config(db, normalized);
  });
  return removed;
}

std::vector<VdsctlRuntimeStatus>
parse_vdsctl_runtime_statuses(std::string_view response) {
  std::istringstream lines{std::string(response)};
  if (!consume_ok_response_header(lines)) {
    return {};
  }

  std::vector<VdsctlRuntimeStatus> statuses;
  std::string line;
  while (std::getline(lines, line)) {
    drop_trailing_cr(line);
    if (line.empty()) {
      continue;
    }

    std::istringstream fields(line);
    std::string address;
    std::string state;
    std::string path;
    std::string extra;
    if (!(fields >> address >> state)) {
      throw std::runtime_error("malformed daemon LIST response");
    }
    if (state != "connected" && state != "disconnected") {
      throw std::runtime_error("unknown daemon LIST state: " + state);
    }
    if (fields >> path && path == "-") {
      path.clear();
    }
    if (fields >> extra) {
      throw std::runtime_error("malformed daemon LIST response");
    }
    statuses.push_back(VdsctlRuntimeStatus{
        .address = normalize_bluetooth_address(address),
        .connected = state == "connected",
        .path = std::move(path),
    });
  }
  return statuses;
}

std::vector<VdsctlRuntimeStatus> load_vdsctl_runtime_statuses(
    const std::function<std::optional<std::string>(const std::string &)>
        &request_control) {
  const auto response = request_control("LIST\n");
  if (!response) {
    return {};
  }
  return parse_vdsctl_runtime_statuses(*response);
}

std::string format_controller_config(const ControllerConfig &config) {
  std::string text = config.address;
  text += " profile=\"";
  text += controller_profile_name(config.profile);
  text += "\" ports=";
  if (config.ports.empty()) {
    text += "[]";
  } else {
    text += '[';
    text += format_ports(config.ports);
    text += ']';
  }
  return text;
}

std::string
format_controller_config_list(const ConfigDb &db,
                              std::span<const VdsctlRuntimeStatus> statuses) {
  const auto runtime_status = [&](std::string_view address) {
    for (const auto &status : statuses) {
      if (status.address == address) {
        return &status;
      }
    }
    return static_cast<const VdsctlRuntimeStatus *>(nullptr);
  };

  std::string text;
  for (const auto &controller : db.controllers) {
    const VdsctlRuntimeStatus *status = runtime_status(controller.address);
    if (status && status->connected && !status->path.empty()) {
      text += '[';
      text += status->path;
      text += "] ";
    } else {
      text += status && status->connected ? "[connected] " : "[disconnected] ";
    }
    text += format_controller_config(controller);
    text += '\n';
  }
  return text;
}

} // namespace vds
