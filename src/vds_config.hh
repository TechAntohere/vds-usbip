// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vds {

constexpr const char *kDefaultBindingDbPath = "/var/lib/vds/bindings.db";

enum class BindingIdentity {
  Auto,
  Ds5,
  Dse,
};

struct ControllerBindingConfig {
  std::string address;
  BindingIdentity identity = BindingIdentity::Auto;
  std::vector<std::string> limit_devices;
};

struct BindingDb {
  std::vector<ControllerBindingConfig> controllers;
};

std::string normalize_bluetooth_address(std::string_view address);
BindingIdentity parse_binding_identity(std::string_view identity);
std::string binding_identity_name(BindingIdentity identity);
std::vector<std::string> parse_limit_devices(std::string_view text);
std::vector<std::string> discover_vds_devices();
BindingDb load_binding_db(const std::string &path);
BindingDb update_binding_db(const std::string &path,
                            const std::function<void(BindingDb &)> &update);
void upsert_binding(BindingDb &db, ControllerBindingConfig binding);
bool remove_binding(BindingDb &db, std::string_view address);
void validate_binding_assignments(const BindingDb &db,
                                  std::span<const std::string> devices);

} // namespace vds
