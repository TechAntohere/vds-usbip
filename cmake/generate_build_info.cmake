if(NOT DEFINED VDS_SOURCE_DIR OR NOT DEFINED VDS_BUILD_INFO_HEADER)
  message(FATAL_ERROR "missing vDS build info arguments")
endif()

if(WIN32)
  set(VDS_VERSION_SCRIPT "${VDS_SOURCE_DIR}/generate_version.ps1")
  find_program(VDS_VERSION_SCRIPT_SHELL pwsh powershell)
  if(VDS_VERSION_SCRIPT_SHELL)
    set(VDS_VERSION_COMMAND
        "${VDS_VERSION_SCRIPT_SHELL}" -NoProfile -ExecutionPolicy Bypass -File
        "${VDS_VERSION_SCRIPT}" -Type Tool)
  else()
    set(VDS_VERSION_COMMAND "${VDS_VERSION_SCRIPT}" -Type Tool)
  endif()
else()
  set(VDS_VERSION_SCRIPT "${VDS_SOURCE_DIR}/generate-version.sh")
  set(VDS_VERSION_COMMAND "${VDS_VERSION_SCRIPT}")
endif()

execute_process(
  COMMAND ${VDS_VERSION_COMMAND}
  OUTPUT_VARIABLE VDS_VERSION
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(VDS_VERSION STREQUAL "")
  set(VDS_VERSION unknown)
endif()

# Numeric 4-part version for Win32 VERSIONINFO (FILEVERSION/PRODUCTVERSION
# need plain comma-separated integers -- reuse the same driver-version
# derivation already used for the kernel INF DriverVer= line, since that
# logic is already tested elsewhere and gives a monotonic W.X.Y.Z from
# tag + commit-distance).
set(VDS_FILEVERSION_CSV "0,1,0,0")
if(WIN32)
  execute_process(
    COMMAND ${VDS_VERSION_COMMAND} -Type Kernel
    OUTPUT_VARIABLE VDS_KERNEL_VERSION_LINE
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  # Expected form: DriverVer=MM/dd/yyyy,W.X.Y.Z
  if(VDS_KERNEL_VERSION_LINE MATCHES "DriverVer=[^,]+,([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(VDS_FILEVERSION_CSV "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},${CMAKE_MATCH_4}")
  endif()
endif()

string(TIMESTAMP VDS_BUILD_YEAR "%Y")
get_filename_component(VDS_BUILD_INFO_DIR "${VDS_BUILD_INFO_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${VDS_BUILD_INFO_DIR}")

set(VDS_BUILD_INFO_TMP "${VDS_BUILD_INFO_HEADER}.tmp")
file(
  WRITE "${VDS_BUILD_INFO_TMP}"
  "#pragma once\n\n"
  "namespace vds {\n\n"
  "inline constexpr const char *kVersion = \"${VDS_VERSION}\";\n"
  "inline constexpr const char *kBuildYear = \"${VDS_BUILD_YEAR}\";\n\n"
  "} // namespace vds\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${VDS_BUILD_INFO_TMP}" "${VDS_BUILD_INFO_HEADER}")
file(REMOVE "${VDS_BUILD_INFO_TMP}")

# Real VERSIONINFO resources for vdsd.exe/vdsctl.exe (previously 0.0.0.0,
# blank everywhere -- made it impossible to tell a deployed binary apart
# from source). Windows-only; generated alongside the header so both stay
# in sync with the same git-derived version on every build.
if(WIN32)
  set(VDS_VDSD_RC "${VDS_BUILD_INFO_DIR}/vdsd_version.rc")
  set(VDS_VDSCTL_RC "${VDS_BUILD_INFO_DIR}/vdsctl_version.rc")

  function(vds_write_version_rc OUT_PATH FILE_DESC INTERNAL_NAME ORIG_FILENAME)
    set(TMP "${OUT_PATH}.tmp")
    file(WRITE "${TMP}"
      "#include <winresrc.h>\n\n"
      "VS_VERSION_INFO VERSIONINFO\n"
      "FILEVERSION ${VDS_FILEVERSION_CSV}\n"
      "PRODUCTVERSION ${VDS_FILEVERSION_CSV}\n"
      "FILEFLAGSMASK VS_FFI_FILEFLAGSMASK\n"
      "FILEFLAGS 0x0L\n"
      "FILEOS VOS_NT_WINDOWS32\n"
      "FILETYPE VFT_APP\n"
      "FILESUBTYPE VFT2_UNKNOWN\n"
      "BEGIN\n"
      "  BLOCK \"StringFileInfo\"\n"
      "  BEGIN\n"
      "    BLOCK \"040904b0\"\n"
      "    BEGIN\n"
      "      VALUE \"CompanyName\", \"vds-usbip contributors\\0\"\n"
      "      VALUE \"FileDescription\", \"${FILE_DESC}\\0\"\n"
      "      VALUE \"FileVersion\", \"${VDS_VERSION}\\0\"\n"
      "      VALUE \"InternalName\", \"${INTERNAL_NAME}\\0\"\n"
      "      VALUE \"LegalCopyright\", \"Copyright (C) ${VDS_BUILD_YEAR}\\0\"\n"
      "      VALUE \"OriginalFilename\", \"${ORIG_FILENAME}\\0\"\n"
      "      VALUE \"ProductName\", \"vDS (Virtual DualSense over USB/IP)\\0\"\n"
      "      VALUE \"ProductVersion\", \"${VDS_VERSION}\\0\"\n"
      "    END\n"
      "  END\n"
      "  BLOCK \"VarFileInfo\"\n"
      "  BEGIN\n"
      "    VALUE \"Translation\", 0x409, 1200\n"
      "  END\n"
      "END\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TMP}" "${OUT_PATH}")
    file(REMOVE "${TMP}")
  endfunction()

  vds_write_version_rc("${VDS_VDSD_RC}" "vDS Daemon" "vdsd.exe" "vdsd.exe")
  vds_write_version_rc("${VDS_VDSCTL_RC}" "vDS Control CLI" "vdsctl.exe" "vdsctl.exe")
endif()
