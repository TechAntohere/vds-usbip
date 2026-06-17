// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vds_config.hh"
#include "vds_log.hh"
#include "vdsd_common.hh"

namespace vds {

VdsdCommonOptions default_vdsd_common_options() {
  return VdsdCommonOptions{
      .db_path = kDefaultDbPath,
      .log_path = kDefaultLogPath,
      .help_requested = false,
  };
}

std::string vdsd_usage(std::string_view version, std::string_view build_year,
                       std::string_view platform_options) {
  std::string text = "vdsd (";
  text += version;
  text += "): vDS daemon - Copyright (C) ";
  text += build_year;
  text += " Jihong Min\n"
          "usage:\n"
          "  vdsd [--db-path ";
  text += kDefaultDbPath;
  text += "] [--log ";
  text += kDefaultLogPath;
  text += "]";
  if (!platform_options.empty()) {
    text += " ";
    text += platform_options;
  }
  text += "\n";
  return text;
}

void print_vdsd_usage(std::ostream &out, std::string_view version,
                      std::string_view build_year,
                      std::string_view platform_options) {
  out << vdsd_usage(version, build_year, platform_options);
}

bool parse_vdsd_common_option(int argc, char **argv, int &index,
                              VdsdCommonOptions &options) {
  const std::string_view arg = argv[index];
  if (arg == "--db-path") {
    if (index + 1 >= argc) {
      throw std::runtime_error("--db-path requires a path");
    }
    options.db_path = argv[++index];
    return true;
  }
  if (arg == "--log") {
    if (index + 1 >= argc) {
      throw std::runtime_error("--log requires a path");
    }
    options.log_path = argv[++index];
    return true;
  }
  if (arg == "--help" || arg == "-h") {
    options.help_requested = true;
    return true;
  }
  return false;
}

std::string trim_command(std::string command) {
  while (!command.empty() &&
         (command.back() == '\n' || command.back() == '\r' ||
          command.back() == ' ' || command.back() == '\t')) {
    command.pop_back();
  }
  return command;
}

std::uint32_t parse_trace_scope(std::string_view scope) {
  if (scope == "all") {
    return kTraceAll;
  }
  if (scope == "input") {
    return kTraceInput;
  }
  if (scope == "output") {
    return kTraceOutput;
  }
  throw std::runtime_error("trace scope must be all, input, or output");
}

std::string trace_scope_name(std::uint32_t scope) {
  if (scope == kTraceAll) {
    return "all";
  }
  if (scope == kTraceInput) {
    return "input";
  }
  if (scope == kTraceOutput) {
    return "output";
  }
  return "none";
}

std::string active_trace_name(std::uint32_t trace_flags) {
  trace_flags &= kTraceAll;
  if (trace_flags == 0) {
    return "none";
  }
  return trace_scope_name(trace_flags);
}

bool trace_enabled(std::uint32_t trace_flags, std::uint32_t target) {
  return (trace_flags & target) != 0;
}

bool remember_vdsd_warning(std::vector<std::string> &warnings,
                           std::string_view key) {
  const auto existing =
      std::find_if(warnings.begin(), warnings.end(),
                   [&](std::string_view item) { return item == key; });
  if (existing != warnings.end()) {
    return false;
  }
  warnings.emplace_back(key);
  return true;
}

void forget_vdsd_warning(std::vector<std::string> &warnings,
                         std::string_view key) {
  warnings.erase(
      std::remove_if(warnings.begin(), warnings.end(),
                     [&](std::string_view item) { return item == key; }),
      warnings.end());
}

bool remember_vdsd_warning_state(std::vector<std::string> &warnings,
                                 std::string_view key, std::string_view state) {
  std::string prefix{key};
  prefix += '\n';
  std::string entry = prefix;
  entry += state;
  if (std::find(warnings.begin(), warnings.end(), entry) != warnings.end()) {
    return false;
  }

  warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                                [&](std::string_view existing) {
                                  return existing.rfind(prefix, 0) == 0;
                                }),
                 warnings.end());
  warnings.push_back(std::move(entry));
  return true;
}

void forget_vdsd_warning_state(std::vector<std::string> &warnings,
                               std::string_view key) {
  std::string prefix{key};
  prefix += '\n';
  warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                                [&](std::string_view existing) {
                                  return existing.rfind(prefix, 0) == 0;
                                }),
                 warnings.end());
}

std::vector<VdsdControlPortStatus> build_vdsd_control_port_statuses(
    std::span<const VdsdControlPortCandidate> candidates,
    std::span<const VdsdControlPortBinding> bindings) {
  std::vector<VdsdControlPortStatus> statuses;
  statuses.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    const auto binding =
        std::find_if(bindings.begin(), bindings.end(), [&](const auto &item) {
          return item.port == candidate.port;
        });
    statuses.push_back(VdsdControlPortStatus{
        .port = candidate.port,
        .path = candidate.path,
        .occupied = binding != bindings.end(),
        .address = binding != bindings.end() ? binding->address : "",
        .device_address =
            binding != bindings.end() ? binding->device_address : "",
    });
  }
  return statuses;
}

VdsdWorkerLaunchDecision select_vdsd_worker_launch_decision(
    const ControllerConfig &config, std::span<const unsigned> available_ports,
    std::span<const unsigned> reserved_ports, std::string_view device_address,
    std::span<const std::string> reserved_device_addresses) {
  if (std::find(reserved_device_addresses.begin(),
                reserved_device_addresses.end(),
                device_address) != reserved_device_addresses.end()) {
    return VdsdWorkerLaunchDecision{
        .status = VdsdWorkerLaunchStatus::DeviceAddressReserved,
        .config = config,
        .port = 0,
        .device_address = std::string(device_address),
    };
  }

  const auto port =
      select_controller_config_port(config, available_ports, reserved_ports);
  if (!port) {
    return VdsdWorkerLaunchDecision{
        .status = VdsdWorkerLaunchStatus::NoAvailablePort,
        .config = config,
        .port = 0,
        .device_address = std::string(device_address),
    };
  }

  ControllerConfig worker_config = config;
  worker_config.ports = {*port};
  return VdsdWorkerLaunchDecision{
      .status = VdsdWorkerLaunchStatus::Ready,
      .config = std::move(worker_config),
      .port = *port,
      .device_address = std::string(device_address),
  };
}

bool vdsd_worker_config_is_stale(const ConfigDb &db, std::string_view address,
                                 ControllerProfile profile, unsigned port,
                                 bool port_enabled) {
  const ControllerConfig *config =
      find_controller_config_by_address(db, address);
  return config == nullptr || config->profile != profile ||
         !controller_config_allows_port(*config, port) || !port_enabled;
}

void record_vdsd_worker_failure(
    std::vector<VdsdWorkerFailureBackoff> &backoffs, std::string_view address,
    std::chrono::steady_clock::time_point retry_after) {
  const auto existing =
      std::find_if(backoffs.begin(), backoffs.end(),
                   [&](const auto &entry) { return entry.address == address; });
  if (existing == backoffs.end()) {
    backoffs.push_back(VdsdWorkerFailureBackoff{
        .address = std::string(address),
        .retry_after = retry_after,
    });
    return;
  }
  existing->retry_after = retry_after;
}

bool consume_vdsd_worker_retry(std::vector<VdsdWorkerFailureBackoff> &backoffs,
                               std::string_view address,
                               std::chrono::steady_clock::time_point now) {
  const auto existing =
      std::find_if(backoffs.begin(), backoffs.end(),
                   [&](const auto &entry) { return entry.address == address; });
  if (existing == backoffs.end()) {
    return true;
  }
  if (now < existing->retry_after) {
    return false;
  }
  backoffs.erase(existing);
  return true;
}

std::string format_control_list_reply(
    std::span<const VdsdControlControllerStatus> controllers) {
  std::string reply = "OK\n";
  for (const auto &controller : controllers) {
    reply += controller.address;
    reply += controller.connected ? " connected " : " disconnected ";
    reply += controller.connected && !controller.path.empty() ? controller.path
                                                              : "-";
    reply += '\n';
  }
  return reply;
}

std::string
format_control_ports_reply(std::span<const VdsdControlPortStatus> ports) {
  std::string reply = "OK\n";
  for (const auto &port : ports) {
    reply += std::to_string(port.port);
    reply += ' ';
    reply += port.path;
    reply += port.occupied ? " occupied " : " idle ";
    reply += port.occupied ? port.address : "-";
    reply += ' ';
    reply += port.device_address.empty() ? "-" : port.device_address;
    reply += '\n';
  }
  return reply;
}

VdsdTraceControlResult apply_trace_control_command(std::string_view command,
                                                   std::uint32_t &trace_flags) {
  std::istringstream fields{std::string(command)};
  std::string verb;
  std::string action;
  std::string scope_text = "all";
  std::string extra;
  if (!(fields >> verb >> action) || verb != "TRACE") {
    throw std::runtime_error("malformed trace command");
  }
  if (fields >> scope_text && (fields >> extra)) {
    throw std::runtime_error("malformed trace command");
  }

  const std::uint32_t scope = parse_trace_scope(scope_text);
  bool enabled = false;
  if (action == "ON") {
    trace_flags |= scope;
    enabled = true;
  } else if (action == "OFF") {
    trace_flags &= ~scope;
  } else {
    throw std::runtime_error("trace requires ON or OFF");
  }

  std::string reply = "OK trace=";
  reply += enabled ? "on" : "off";
  reply += " scope=";
  reply += trace_scope_name(scope);
  reply += " active=";
  reply += active_trace_name(trace_flags);
  reply += '\n';
  return VdsdTraceControlResult{
      .enabled = enabled,
      .scope = scope,
      .reply = std::move(reply),
  };
}

std::string handle_vdsd_control_command(
    std::string_view command,
    std::span<const VdsdControlControllerStatus> controllers,
    std::span<const VdsdControlPortStatus> ports, std::uint32_t &trace_flags,
    bool &reload_requested, Logger &logger) {
  try {
    if (command == "RELOAD") {
      reload_requested = true;
      logger.log("control", LogLevel::Info, "reload requested");
      return "OK reloaded\n";
    }

    if (command == "LIST") {
      return format_control_list_reply(controllers);
    }

    if (command == "PORTS") {
      return format_control_ports_reply(ports);
    }

    if (command.rfind("TRACE ", 0) == 0) {
      const VdsdTraceControlResult trace =
          apply_trace_control_command(command, trace_flags);
      logger.log("control", LogLevel::Info,
                 "trace " + trace_scope_name(trace.scope) + " " +
                     (trace.enabled ? "enabled" : "disabled") +
                     " active=" + active_trace_name(trace_flags));
      return trace.reply;
    }

    logger.log("control", LogLevel::Warn,
               "unknown command: " + std::string(command));
    return "ERR unknown command\n";
  } catch (const std::exception &error) {
    return std::string("ERR ") + error.what() + "\n";
  }
}

} // namespace vds
