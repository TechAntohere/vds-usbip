cmake_minimum_required(VERSION 3.20)

project(vds LANGUAGES CXX)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

execute_process(
  COMMAND git describe --always --abbrev=7 --dirty=+ --tags
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE VDS_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(VDS_VERSION STREQUAL "")
  set(VDS_VERSION "unknown")
endif()
string(TIMESTAMP VDS_BUILD_YEAR "%Y")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(VDS_VERSION="${VDS_VERSION}"
                        VDS_BUILD_YEAR="${VDS_BUILD_YEAR}")

add_library(vds_protocol src/vds_protocol.cc)
target_include_directories(
  vds_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                      ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_protocol PRIVATE PkgConfig::OPUS)

add_library(vds_bt src/vds_bt.cc)
target_link_libraries(vds_bt PUBLIC vds_protocol)

add_library(vds_config src/vds_config.cc)
target_include_directories(vds_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vds_log src/vds_log.cc)
target_include_directories(vds_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(vdsd src/vdsd.cc)
target_link_libraries(vdsd PRIVATE vds_protocol vds_bt vds_config vds_log)

add_executable(vdsctl src/vdsctl.cc)
target_link_libraries(vdsctl PRIVATE vds_config)

install(TARGETS vdsd vdsctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_custom_target(
  uninstall COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_INSTALL_FULL_BINDIR}/vdsd
                    ${CMAKE_INSTALL_FULL_BINDIR}/vdsctl)
