// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <optional>
#include <string>

namespace vds {

std::optional<std::string> bluez_device_modalias(const std::string &address);
bool disconnect_bluez_device(const std::string &address);

} // namespace vds
