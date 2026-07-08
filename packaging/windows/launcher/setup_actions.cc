#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <msiquery.h>

namespace {

std::wstring quote_command_argument(std::wstring_view value) {
  std::wstring result = L"\"";
  unsigned backslash_count = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslash_count;
      continue;
    }
    if (ch == L'"') {
      result.append(backslash_count * 2 + 1, L'\\');
      result.push_back(ch);
      backslash_count = 0;
      continue;
    }
    result.append(backslash_count, L'\\');
    backslash_count = 0;
    result.push_back(ch);
  }
  result.append(backslash_count * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::filesystem::path system_executable(const wchar_t *name) {
  std::vector<wchar_t> system_dir(MAX_PATH + 1);
  const UINT length = GetSystemDirectoryW(system_dir.data(),
                                          static_cast<UINT>(system_dir.size()));
  if (length == 0 || length >= system_dir.size()) {
    throw std::runtime_error("GetSystemDirectoryW failed");
  }

  std::filesystem::path path(system_dir.data());
  path /= name;
  return path;
}

std::wstring sanitize_log_text(std::wstring value) {
  for (wchar_t &ch : value) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
      ch = L' ';
    } else if (ch < 0x20) {
      ch = L'?';
    }
  }
  return value;
}

std::wstring wide_from_code_page(UINT code_page, DWORD flags,
                                 std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int length =
      MultiByteToWideChar(code_page, flags, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }

  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(code_page, flags, value.data(),
                      static_cast<int>(value.size()), result.data(), length);
  return result;
}

std::wstring fallback_wide_from_log_bytes(std::string_view value) {
  std::wstring result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      result.push_back(L' ');
    } else if (ch >= 0x20 && ch < 0x7f) {
      result.push_back(static_cast<wchar_t>(ch));
    } else {
      result.push_back(L'?');
    }
  }
  return result;
}

std::wstring wide_from_log_bytes(std::string_view value) {
  std::wstring result =
      wide_from_code_page(CP_UTF8, MB_ERR_INVALID_CHARS, value);
  if (result.empty() && !value.empty()) {
    result = wide_from_code_page(CP_OEMCP, 0, value);
  }
  if (result.empty() && !value.empty()) {
    result = wide_from_code_page(CP_ACP, 0, value);
  }
  if (result.empty() && !value.empty()) {
    result = fallback_wide_from_log_bytes(value);
  }
  return sanitize_log_text(std::move(result));
}

std::filesystem::path installer_log_path() {
  std::vector<wchar_t> program_data(32768);
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data.data(),
                              static_cast<DWORD>(program_data.size()));
  if (length == 0 || length >= static_cast<DWORD>(program_data.size())) {
    throw std::runtime_error("ProgramData is not available");
  }

  std::filesystem::path path(program_data.data());
  path /= L"vDS";
  std::filesystem::create_directories(path);
  path /= L"installer.log";
  return path;
}

void append_installer_log(std::wstring_view message) noexcept {
  try {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t timestamp[64]{};
    swprintf_s(timestamp, L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu ",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond, time.wMilliseconds);

    std::wstring line(timestamp);
    line += message;
    line += L"\r\n";

    const std::filesystem::path log_path = installer_log_path();
    bool write_bom = true;
    std::error_code error;
    if (std::filesystem::exists(log_path, error)) {
      write_bom = std::filesystem::file_size(log_path, error) == 0;
    }

    std::ofstream stream(log_path, std::ios::binary | std::ios::app);
    if (write_bom) {
      constexpr wchar_t bom = 0xfeff;
      stream.write(reinterpret_cast<const char *>(&bom), sizeof(bom));
    }
    stream.write(reinterpret_cast<const char *>(line.data()),
                 static_cast<std::streamsize>(line.size() * sizeof(wchar_t)));
  } catch (...) {
  }
}

int run_process(std::wstring command_line) {
  {
    std::wstring message = L"run setup action: ";
    message += command_line;
    append_installer_log(message);
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESHOWWINDOW;
  startup_info.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process_info{};

  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                      &process_info)) {
    std::wstring message = L"setup action CreateProcessW failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    return 1;
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    exit_code = 1;
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  {
    std::wstring message = L"setup action exit: ";
    message += std::to_wstring(exit_code);
    append_installer_log(message);
  }
  return static_cast<int>(exit_code);
}

std::string run_process_capture_output(std::wstring command_line,
                                       int *exit_status = nullptr) {
  if (exit_status) {
    *exit_status = 1;
  }

  {
    std::wstring message = L"run setup action with output capture: ";
    message += command_line;
    append_installer_log(message);
  }

  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0)) {
    std::wstring message = L"setup action CreatePipe failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    return {};
  }
  if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    std::wstring message = L"setup action SetHandleInformation failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return {};
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup_info.wShowWindow = SW_HIDE;
  startup_info.hStdOutput = write_pipe;
  startup_info.hStdError = write_pipe;

  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                      &process_info)) {
    std::wstring message = L"setup action capture CreateProcessW failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return {};
  }

  CloseHandle(write_pipe);

  std::string output;
  char buffer[4096];
  DWORD bytes_read = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) &&
         bytes_read != 0) {
    output.append(buffer, buffer + bytes_read);
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    exit_code = 1;
  }
  if (exit_status) {
    *exit_status = static_cast<int>(exit_code);
  }
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  CloseHandle(read_pipe);
  {
    std::wstring message = L"setup action capture exit: ";
    message += std::to_wstring(exit_code);
    message += L" output_bytes=";
    message += std::to_wstring(output.size());
    append_installer_log(message);
  }
  return output;
}

std::wstring msi_property(MSIHANDLE install, const wchar_t *name) {
  DWORD size = 0;
  wchar_t empty_value[1]{};
  UINT status = MsiGetPropertyW(install, name, empty_value, &size);
  if (status != ERROR_MORE_DATA && status != ERROR_SUCCESS) {
    return {};
  }

  std::wstring value(size, L'\0');
  ++size;
  value.resize(size);
  status = MsiGetPropertyW(install, name, value.data(), &size);
  if (status != ERROR_SUCCESS) {
    return {};
  }
  value.resize(size);
  return value;
}

std::wstring lowercase_ascii(std::wstring value) {
  for (wchar_t &ch : value) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(std::towlower(ch));
    }
  }
  return value;
}

std::string lowercase_ascii(std::string value) {
  for (char &ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool test_signing_enabled() {
  std::wstring command_line =
      quote_command_argument(system_executable(L"bcdedit.exe").wstring());
  command_line += L" /enum {current}";

  int status = 1;
  const std::string output =
      run_process_capture_output(std::move(command_line), &status);
  {
    std::wstring message = L"Windows test signing bcdedit exit code: ";
    message += std::to_wstring(status);
    append_installer_log(message);
  }
  if (output.empty()) {
    append_installer_log(L"Windows test signing bcdedit output: <no output>");
  }

  bool found_test_signing_line = false;
  bool enabled = false;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    std::wstring message = L"Windows test signing bcdedit output: ";
    message += wide_from_log_bytes(line);
    append_installer_log(message);

    const std::string normalized_line = lowercase_ascii(line);
    if (normalized_line.find("testsigning") == std::string::npos) {
      continue;
    }

    found_test_signing_line = true;
    if (normalized_line.find("yes") != std::string::npos) {
      enabled = true;
    }
  }
  if (status != 0) {
    append_installer_log(
        L"Windows test signing bcdedit failed; treating test signing as "
        L"disabled");
    return false;
  }
  if (!found_test_signing_line) {
    append_installer_log(
        L"Windows test signing bcdedit line was not found; treating it as "
        L"disabled");
  } else if (enabled) {
    append_installer_log(
        L"Windows test signing bcdedit state parsed as enabled");
  } else {
    append_installer_log(
        L"Windows test signing bcdedit state parsed as disabled");
  }
  return enabled;
}

std::filesystem::path program_data_path() {
  std::vector<wchar_t> program_data(MAX_PATH + 1);
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data.data(),
                              static_cast<DWORD>(program_data.size()));
  if (length == 0 || length >= program_data.size()) {
    throw std::runtime_error("ProgramData is not available");
  }

  return std::filesystem::path(program_data.data());
}

std::filesystem::path program_files_path() {
  std::vector<wchar_t> program_files(MAX_PATH + 1);
  DWORD length =
      GetEnvironmentVariableW(L"ProgramW6432", program_files.data(),
                              static_cast<DWORD>(program_files.size()));
  if (length == 0) {
    length = GetEnvironmentVariableW(L"ProgramFiles", program_files.data(),
                                     static_cast<DWORD>(program_files.size()));
  }
  if (length == 0 || length >= program_files.size()) {
    throw std::runtime_error("Program Files is not available");
  }

  return std::filesystem::path(program_files.data());
}

std::filesystem::path temp_path() {
  std::vector<wchar_t> temp_path(MAX_PATH + 1);
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(temp_path.size()), temp_path.data());
  if (length == 0 || length >= temp_path.size()) {
    throw std::runtime_error("GetTempPathW failed");
  }

  return std::filesystem::path(temp_path.data());
}

std::filesystem::path installer_cache_dir() {
  std::filesystem::path path = temp_path();
  path /= L"vDSSetup";
  return path;
}

std::filesystem::path installer_cache_path() {
  std::filesystem::path path = installer_cache_dir();
  path /= L"vDSSetup.exe";
  return path;
}

std::filesystem::path prepare_installer_cache_path() {
  std::filesystem::create_directories(installer_cache_dir());
  return installer_cache_path();
}

void delete_resume_task() {
  std::wstring command_line =
      quote_command_argument(system_executable(L"schtasks.exe").wstring());
  command_line += L" /Delete /TN ";
  command_line += quote_command_argument(L"vDS Setup Resume");
  command_line += L" /F";
  run_process(std::move(command_line));
}

void delete_resume_cache() {
  try {
    std::error_code ignored;
    std::filesystem::remove(installer_cache_path(), ignored);
    std::filesystem::remove(installer_cache_dir(), ignored);
  } catch (...) {
  }
}

bool launched_from_resume_cache(MSIHANDLE install) {
  const std::wstring source = msi_property(install, L"VDS_SETUP_SOURCE");
  if (source.empty()) {
    return false;
  }

  try {
    const std::wstring source_path = lowercase_ascii(
        std::filesystem::path(source).lexically_normal().wstring());
    const std::wstring cache_path =
        lowercase_ascii(installer_cache_path().lexically_normal().wstring());
    return source_path == cache_path;
  } catch (...) {
    return false;
  }
}

void create_resume_task(const std::filesystem::path &cached_installer) {
  std::wstring task_command = L"\"" + cached_installer.wstring() + L"\"";
  std::wstring command_line =
      quote_command_argument(system_executable(L"schtasks.exe").wstring());
  command_line += L" /Create /TN ";
  command_line += quote_command_argument(L"vDS Setup Resume");
  command_line += L" /SC ONLOGON /RL HIGHEST /TR ";
  command_line += quote_command_argument(task_command);
  command_line += L" /F";

  if (run_process(std::move(command_line)) != 0) {
    throw std::runtime_error("failed to create resume scheduled task");
  }
}

void enable_test_signing() {
  append_installer_log(L"enabling Windows test signing");
  std::wstring command_line =
      quote_command_argument(system_executable(L"bcdedit.exe").wstring());
  command_line += L" /set testsigning on";

  int status = 1;
  const std::string output =
      run_process_capture_output(std::move(command_line), &status);
  if (output.empty()) {
    append_installer_log(L"Windows test signing enable output: <no output>");
  } else {
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
      std::wstring message = L"Windows test signing enable output: ";
      message += wide_from_log_bytes(line);
      append_installer_log(message);
    }
  }
  if (status != 0) {
    append_installer_log(L"failed to enable Windows test signing");
    throw std::runtime_error("failed to enable Windows test signing with exit "
                             "code " +
                             std::to_string(status));
  }
  append_installer_log(
      L"Windows test signing enable command succeeded; reboot is required");
}

void reboot_now() {
  std::wstring command_line =
      quote_command_argument(system_executable(L"shutdown.exe").wstring());
  command_line += L" /r /t 0";
  run_process(std::move(command_line));
}

void delete_hklm_vds_setup_value(const wchar_t *name) {
  HKEY setup_key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\vDS\\Setup", 0,
                    KEY_SET_VALUE, &setup_key) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteValueW(setup_key, name);
  RegCloseKey(setup_key);
}

void set_registry_string(HKEY key, const wchar_t *name,
                         const std::wstring &value) {
  const DWORD byte_count =
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  RegSetValueExW(key, name, 0, REG_SZ,
                 reinterpret_cast<const BYTE *>(value.data()), byte_count);
}

void set_registry_dword(HKEY key, const wchar_t *name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD,
                 reinterpret_cast<const BYTE *>(&value), sizeof(value));
}

std::wstring cache_setup_launcher(MSIHANDLE install) {
  const std::wstring source = msi_property(install, L"VDS_SETUP_SOURCE");
  if (source.empty() || !std::filesystem::exists(source)) {
    return {};
  }

  const std::filesystem::path cached_installer = prepare_installer_cache_path();
  std::filesystem::copy_file(source, cached_installer,
                             std::filesystem::copy_options::overwrite_existing);
  return cached_installer.wstring();
}

void update_control_panel_entry(MSIHANDLE install,
                                const std::wstring &cached_installer) {
  if (cached_installer.empty()) {
    return;
  }

  HKEY uninstall_key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                      L"Uninstall\\vDS",
                      0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                      &uninstall_key, nullptr) != ERROR_SUCCESS) {
    return;
  }

  const std::wstring install_folder = msi_property(install, L"INSTALLFOLDER");
  const std::wstring display_version =
      msi_property(install, L"VDS_DISPLAY_VERSION");
  const std::wstring uninstall_string =
      quote_command_argument(cached_installer) + L" --uninstall";

  set_registry_string(uninstall_key, L"DisplayName", L"vDS");
  set_registry_string(uninstall_key, L"DisplayVersion", display_version);
  set_registry_string(uninstall_key, L"DisplayIcon", cached_installer);
  set_registry_string(uninstall_key, L"InstallLocation", install_folder);
  set_registry_string(uninstall_key, L"Publisher", L"Jihong Min");
  set_registry_string(uninstall_key, L"UninstallString", uninstall_string);
  set_registry_dword(uninstall_key, L"NoModify", 1);
  set_registry_dword(uninstall_key, L"NoRepair", 1);

  RegCloseKey(uninstall_key);
}

void delete_control_panel_entry(REGSAM registry_view) {
  HKEY uninstall_root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0, KEY_WRITE | registry_view,
                    &uninstall_root) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteTreeW(uninstall_root, L"vDS");
  RegCloseKey(uninstall_root);
}

std::wstring registry_string(HKEY key, const wchar_t *name) {
  DWORD type = 0;
  DWORD byte_count = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &byte_count) !=
          ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return {};
  }

  std::wstring value(byte_count / sizeof(wchar_t), L'\0');
  if (value.empty()) {
    return {};
  }
  if (RegQueryValueExW(key, name, nullptr, nullptr,
                       reinterpret_cast<BYTE *>(value.data()),
                       &byte_count) != ERROR_SUCCESS) {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

bool is_vds_windows_installer_entry(HKEY uninstall_key) {
  if (registry_string(uninstall_key, L"Publisher") != L"Jihong Min") {
    return false;
  }

  const std::wstring display_name =
      registry_string(uninstall_key, L"DisplayName");
  return display_name == L"vDS" || display_name == L"vDS USB Driver" ||
         display_name == L"vDS Filter Driver";
}

void delete_windows_installer_entries(REGSAM registry_view) {
  HKEY uninstall_root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0,
                    KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE |
                        DELETE | registry_view,
                    &uninstall_root) != ERROR_SUCCESS) {
    return;
  }

  DWORD index = 0;
  while (true) {
    wchar_t subkey_name[256]{};
    DWORD subkey_name_length =
        static_cast<DWORD>(sizeof(subkey_name) / sizeof(subkey_name[0]));
    const LSTATUS enum_status =
        RegEnumKeyExW(uninstall_root, index, subkey_name, &subkey_name_length,
                      nullptr, nullptr, nullptr, nullptr);
    if (enum_status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (enum_status != ERROR_SUCCESS) {
      ++index;
      continue;
    }

    bool remove_entry = false;
    HKEY uninstall_key = nullptr;
    if (RegOpenKeyExW(uninstall_root, subkey_name, 0, KEY_QUERY_VALUE,
                      &uninstall_key) == ERROR_SUCCESS) {
      remove_entry = is_vds_windows_installer_entry(uninstall_key);
      RegCloseKey(uninstall_key);
    }

    if (remove_entry) {
      std::wstring message = L"remove stale vDS Windows Installer entry: ";
      message += subkey_name;
      append_installer_log(message);
      RegDeleteTreeW(uninstall_root, subkey_name);
      continue;
    }

    ++index;
  }

  RegCloseKey(uninstall_root);
}

void remove_userspace_files(const std::filesystem::path &path) {
  if (path.empty()) {
    return;
  }

  std::error_code ignored;
  std::filesystem::remove(path / L"vdsd.exe", ignored);
  std::filesystem::remove(path / L"vdsctl.exe", ignored);
  std::filesystem::remove(path / L"opus.dll", ignored);
  std::filesystem::remove(path, ignored);
}

} // namespace

extern "C" __declspec(dllexport) UINT __stdcall
CheckTestSigning(MSIHANDLE install) {
  const bool enabled = test_signing_enabled();
  const bool resumed = launched_from_resume_cache(install);
  MsiSetPropertyW(install, L"VDS_TESTSIGNING_ENABLED", enabled ? L"1" : L"0");
  MsiSetPropertyW(install, L"VDS_TESTSIGNING_RESUME_FAILED",
                  (!enabled && resumed) ? L"1" : L"0");
  if (enabled) {
    append_installer_log(L"Windows test signing is enabled");
  } else if (resumed) {
    append_installer_log(
        L"Windows test signing is still disabled after setup resume");
  } else {
    append_installer_log(L"Windows test signing is disabled");
  }
  if (enabled || resumed) {
    delete_resume_task();
    delete_resume_cache();
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall
EnableTestSigningAndReboot(MSIHANDLE install) {
  try {
    append_installer_log(L"EnableTestSigningAndReboot started");
    delete_resume_task();
    delete_resume_cache();
    enable_test_signing();

    const std::wstring source = msi_property(install, L"VDS_SETUP_SOURCE");
    if (!source.empty() && std::filesystem::exists(source)) {
      const std::filesystem::path cached_installer =
          prepare_installer_cache_path();
      std::filesystem::copy_file(
          source, cached_installer,
          std::filesystem::copy_options::overwrite_existing);
      create_resume_task(cached_installer);
      std::wstring message = L"cached setup launcher for resume: ";
      message += cached_installer.wstring();
      append_installer_log(message);
    } else {
      append_installer_log(
          L"setup source was unavailable; resume cache was not created");
    }

    append_installer_log(L"rebooting Windows after enabling test signing");
    reboot_now();
    return ERROR_SUCCESS;
  } catch (const std::exception &error) {
    std::wstring message = L"EnableTestSigningAndReboot failed: ";
    message += wide_from_log_bytes(error.what());
    append_installer_log(message);
    delete_resume_task();
    delete_resume_cache();
    return ERROR_INSTALL_FAILURE;
  } catch (...) {
    append_installer_log(L"EnableTestSigningAndReboot failed");
    delete_resume_task();
    delete_resume_cache();
    return ERROR_INSTALL_FAILURE;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
PromptRebootAfterUninstall(MSIHANDLE) {
  const int choice = MessageBoxW(
      nullptr,
      L"vDS has been removed from this computer.\n\n"
      L"A Windows reboot is recommended before reinstalling vDS or using "
      L"Bluetooth controller devices normally.\n\n"
      L"Reboot now?",
      L"vDS Setup", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2);
  if (choice == IDYES) {
    reboot_now();
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall
UpdateDisplayVersion(MSIHANDLE install) {
  const std::wstring cached_installer = cache_setup_launcher(install);
  update_control_panel_entry(install, cached_installer);
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall ClearPathMarker(MSIHANDLE) {
  delete_hklm_vds_setup_value(L"PathEntry");
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall
RemoveProgramData(MSIHANDLE install) {
  if (msi_property(install, L"CustomActionData") != L"1") {
    return ERROR_SUCCESS;
  }

  try {
    std::error_code ignored;
    std::filesystem::remove_all(program_data_path() / L"vDS", ignored);
  } catch (...) {
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall
RemoveInstallDirectories(MSIHANDLE install) {
  try {
    remove_userspace_files(
        std::filesystem::path(msi_property(install, L"CustomActionData")));
    remove_userspace_files(program_files_path() / L"vDS");
  } catch (...) {
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RemoveVdsRegistry(MSIHANDLE) {
  delete_control_panel_entry(KEY_WOW64_64KEY);
  delete_control_panel_entry(KEY_WOW64_32KEY);
  delete_control_panel_entry(0);
  delete_windows_installer_entries(KEY_WOW64_64KEY);
  delete_windows_installer_entries(KEY_WOW64_32KEY);
  delete_windows_installer_entries(0);

  HKEY software_key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_WRITE,
                    &software_key) == ERROR_SUCCESS) {
    RegDeleteTreeW(software_key, L"vDS");
    RegCloseKey(software_key);
  }
  return ERROR_SUCCESS;
}
