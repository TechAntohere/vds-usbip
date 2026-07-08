#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <newdev.h>
#include <setupapi.h>

namespace {

std::wstring win32_error_message(DWORD error) {
  wchar_t *message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
  if (length == 0 || !message) {
    return L"Win32 error " + std::to_wstring(error);
  }

  std::wstring result(message, message + length);
  LocalFree(message);
  while (!result.empty() &&
         (result.back() == L'\r' || result.back() == L'\n')) {
    result.pop_back();
  }
  return result;
}

[[noreturn]] void throw_last_error(const wchar_t *operation) {
  const DWORD error = GetLastError();
  throw std::runtime_error(
      std::string("failed to ") + std::filesystem::path(operation).string() +
      ": " + std::to_string(error) + ": " +
      std::filesystem::path(win32_error_message(error)).string());
}

std::wstring full_path_from_arg(const wchar_t *value) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetFullPathNameW(
      value, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  if (length == 0 || length >= static_cast<DWORD>(buffer.size())) {
    throw_last_error(L"resolve INF path");
  }
  return std::wstring(buffer.data(), buffer.data() + length);
}

void create_vds_usb_root_device() {
  GUID class_guid{0x36fc9e60,
                  0xc465,
                  0x11cf,
                  {0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
  HDEVINFO device_info_set = SetupDiCreateDeviceInfoList(&class_guid, nullptr);
  if (device_info_set == INVALID_HANDLE_VALUE) {
    throw_last_error(L"create device info list");
  }

  SP_DEVINFO_DATA device_info_data{};
  device_info_data.cbSize = sizeof(device_info_data);
  if (!SetupDiCreateDeviceInfoW(device_info_set, L"USB", &class_guid,
                                L"vDS USB Root Hub", nullptr, DICD_GENERATE_ID,
                                &device_info_data)) {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(device_info_set);
    SetLastError(error);
    throw_last_error(L"create vDS USB root device info");
  }

  const wchar_t hardware_ids[] = L"Root\\VDSUSB\0";
  if (!SetupDiSetDeviceRegistryPropertyW(
          device_info_set, &device_info_data, SPDRP_HARDWAREID,
          reinterpret_cast<const BYTE *>(hardware_ids), sizeof(hardware_ids))) {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(device_info_set);
    SetLastError(error);
    throw_last_error(L"set vDS USB root hardware ID");
  }

  if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, device_info_set,
                                 &device_info_data)) {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(device_info_set);
    SetLastError(error);
    throw_last_error(L"register vDS USB root device");
  }

  SetupDiDestroyDeviceInfoList(device_info_set);
}

void update_vds_usb_driver(const std::wstring &inf_path) {
  BOOL reboot_required = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(
          nullptr, L"Root\\VDSUSB", inf_path.c_str(),
          INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot_required)) {
    throw_last_error(L"bind vDS USB root device to vds_usb.inf");
  }
  if (reboot_required) {
    std::wcout << L"vDS USB root driver update requested a reboot\n";
  }
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    std::wcerr << L"usage: vds-root-device-installer.exe <vds_usb.inf>\n";
    return 2;
  }

  try {
    const std::wstring inf_path = full_path_from_arg(argv[1]);
    std::wcout << L"Creating vDS USB root device for " << inf_path << L"\n";
    create_vds_usb_root_device();
    update_vds_usb_driver(inf_path);
    std::wcout << L"vDS USB root device is ready\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
