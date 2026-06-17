// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "uapi/vds.h"
#include "vds_config.hh"

namespace vds {

inline std::uint32_t
usb_profile_from_controller_profile(ControllerProfile profile) {
  switch (profile) {
  case ControllerProfile::Ds5:
    return VDS_PROFILE_DS5;
  case ControllerProfile::Dse:
    return VDS_PROFILE_DSE;
  case ControllerProfile::Unspecified:
    break;
  }
  throw std::runtime_error("empty profile has no fixed USB profile");
}

inline std::string usb_profile_name(std::uint32_t profile) {
  return profile == VDS_PROFILE_DSE ? "dse" : "ds5";
}

} // namespace vds
