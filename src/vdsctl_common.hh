// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vds_config.hh"

namespace vds {

enum class VdsctlCommand {
  Attach,
  Detach,
  List,
  Trace,
};

struct VdsctlTraceCommand {
  bool enabled = false;
  std::string scope;
};

struct VdsctlRuntimeStatus {
  std::string address;
  bool connected = false;
  std::string path;
};

struct VdsctlPlatform {
  std::string db_path;
  std::string max_port_name;
  std::function<unsigned()> read_max_port;
  std::function<void()> notify_reload;
  std::function<std::optional<std::string>(const std::string &)>
      request_control_optional;
  std::function<std::optional<std::string>(const std::string &)>
      request_control;
};

std::string vdsctl_usage(std::string_view version, std::string_view build_year);
int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year, const VdsctlPlatform &platform);
VdsctlCommand parse_vdsctl_command(std::string_view command);
void require_vdsctl_arg_count(int argc, int expected);
ControllerConfig parse_vdsctl_attach(int argc, char **argv);
VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv);
std::string
format_vdsctl_trace_control_command(const VdsctlTraceCommand &trace);
void validate_controller_ports_against_max_port(const ControllerConfig &config,
                                                unsigned max_port,
                                                std::string_view max_port_name);
std::vector<std::string> port_paths_for_max_port(unsigned max_port);
std::string run_vdsctl_attach(int argc, char **argv, const std::string &db_path,
                              unsigned max_port, std::string_view max_port_name,
                              const std::function<void()> &notify_reload);
std::string run_vdsctl_detach(int argc, char **argv, const std::string &db_path,
                              const std::function<void()> &notify_reload);
std::string
run_vdsctl_list(int argc, const std::string &db_path,
                std::span<const VdsctlRuntimeStatus> runtime_statuses);
std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::optional<std::string>(const std::string &)>
        &request_control);
void update_config_db_for_attach(const std::string &path,
                                 const ControllerConfig &config,
                                 unsigned max_port);
bool update_config_db_for_detach(const std::string &path,
                                 std::string_view address);
std::vector<VdsctlRuntimeStatus>
parse_vdsctl_runtime_statuses(std::string_view response);
std::vector<VdsctlRuntimeStatus> load_vdsctl_runtime_statuses(
    const std::function<std::optional<std::string>(const std::string &)>
        &request_control);
std::string format_controller_config(const ControllerConfig &config);
std::string
format_controller_config_list(const ConfigDb &db,
                              std::span<const VdsctlRuntimeStatus> statuses);

} // namespace vds
