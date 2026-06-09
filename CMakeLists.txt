cmake_minimum_required(VERSION 3.20)

project(vds LANGUAGES CXX)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VDS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VDS_BUILD_INFO_HEADER ${VDS_GENERATED_DIR}/vds_build_info.hh)
set(VDS_BUILD_INFO_SCRIPT
    ${CMAKE_CURRENT_BINARY_DIR}/generate_vds_build_info.sh)
file(
  WRITE ${VDS_BUILD_INFO_SCRIPT}
  [=[
#!/bin/sh
set -eu
source_dir=$1
header=$2
mkdir -p "$(dirname "$header")"
version=$(git -C "$source_dir" describe --always --abbrev=7 --dirty=+ --tags 2>/dev/null || printf unknown)
year=$(date +%Y)
tmp="${header}.tmp"
{
  printf '#pragma once\n\n'
  printf 'namespace vds {\n\n'
  printf 'inline constexpr const char *kVersion = "%s";\n' "$version"
  printf 'inline constexpr const char *kBuildYear = "%s";\n\n' "$year"
  printf '} // namespace vds\n'
} > "$tmp"
if ! test -f "$header" || ! cmp -s "$tmp" "$header"; then
  mv "$tmp" "$header"
else
  rm "$tmp"
fi
]=])

add_custom_target(
  vds_build_info_header ALL
  COMMAND /bin/sh ${VDS_BUILD_INFO_SCRIPT} ${CMAKE_CURRENT_SOURCE_DIR}
          ${VDS_BUILD_INFO_HEADER}
  BYPRODUCTS ${VDS_BUILD_INFO_HEADER}
  COMMENT "Generating vDS build info header"
  VERBATIM)

add_library(vds_protocol OBJECT src/vds_protocol.cc)
target_include_directories(
  vds_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                      ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_protocol PRIVATE PkgConfig::OPUS)

add_library(vds_bt OBJECT src/vds_bt.cc)
target_link_libraries(vds_bt PUBLIC vds_protocol PkgConfig::BLUEZ)

add_library(vds_bluez OBJECT src/vds_bluez.cc)
target_include_directories(vds_bluez PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_bluez PUBLIC vds_config PkgConfig::DBUS)

add_library(vds_config OBJECT src/vds_config.cc)
target_include_directories(vds_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vds_log OBJECT src/vds_log.cc)
target_include_directories(vds_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vds_udev OBJECT src/vds_udev.cc)
target_include_directories(vds_udev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_udev PUBLIC vds_config PkgConfig::UDEV)

add_executable(vdsd src/vdsd.cc)
add_dependencies(vdsd vds_build_info_header)
target_include_directories(vdsd PRIVATE ${VDS_GENERATED_DIR})
target_link_libraries(vdsd PRIVATE vds_protocol vds_bt vds_bluez vds_config
                                   vds_log vds_udev)

add_executable(vdsctl src/vdsctl.cc)
add_dependencies(vdsctl vds_build_info_header)
target_include_directories(vdsctl PRIVATE ${VDS_GENERATED_DIR})
target_link_libraries(vdsctl PRIVATE vds_config vds_udev)

install(TARGETS vdsd vdsctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_custom_target(
  uninstall COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_INSTALL_FULL_BINDIR}/vdsd
                    ${CMAKE_INSTALL_FULL_BINDIR}/vdsctl)
