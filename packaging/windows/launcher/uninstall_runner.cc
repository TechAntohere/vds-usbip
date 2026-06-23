#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "uninstall_payload.hh"

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

std::string utf8_from_wide(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return {};
  }

  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
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

    const std::string bytes = utf8_from_wide(line);
    std::ofstream stream(installer_log_path(),
                         std::ios::binary | std::ios::app);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  } catch (...) {
  }
}

bool service_exists(const wchar_t *name) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) {
    return true;
  }

  SC_HANDLE service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    const DWORD error = GetLastError();
    CloseServiceHandle(scm);
    return error != ERROR_SERVICE_DOES_NOT_EXIST;
  }

  CloseServiceHandle(service);
  CloseServiceHandle(scm);
  return true;
}

std::filesystem::path uninstall_root() {
  std::vector<wchar_t> temp_path(MAX_PATH + 1);
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(temp_path.size()), temp_path.data());
  if (length == 0 || length >= temp_path.size()) {
    throw std::runtime_error("GetTempPathW failed");
  }

  std::filesystem::path root(temp_path.data());
  root /= L"vds-uninstall-" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void write_payloads(const std::filesystem::path &root) {
  for (const auto &payload : kVdsUninstallPayloads) {
    const std::filesystem::path path = root / payload.relative_path;
    std::filesystem::create_directories(path.parent_path());

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
      throw std::runtime_error("failed to open payload output");
    }
    stream.write(reinterpret_cast<const char *>(payload.data),
                 static_cast<std::streamsize>(payload.size));
    if (!stream) {
      throw std::runtime_error("failed to write payload output");
    }
  }
}

std::wstring read_previous_install_dir() {
  std::vector<wchar_t> value(32768);
  DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  const LSTATUS status =
      RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\vDS\\Setup", L"InstallDir",
                   RRF_RT_REG_SZ, nullptr, value.data(), &size);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return {};
  }
  if (status != ERROR_SUCCESS) {
    throw std::runtime_error("failed to read previous InstallDir");
  }
  return value.data();
}

int run_process(std::wstring command_line) {
  {
    std::wstring message = L"run uninstall script: ";
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
    std::wstring message = L"uninstall script CreateProcessW failed: ";
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

  std::wstring message = L"uninstall script exit: ";
  message += std::to_wstring(exit_code);
  append_installer_log(message);
  return static_cast<int>(exit_code);
}

std::filesystem::path powershell_path() {
  std::vector<wchar_t> system_dir(MAX_PATH + 1);
  const UINT length = GetSystemDirectoryW(system_dir.data(),
                                          static_cast<UINT>(system_dir.size()));
  if (length == 0 || length >= system_dir.size()) {
    throw std::runtime_error("GetSystemDirectoryW failed");
  }

  std::filesystem::path path(system_dir.data());
  path /= L"WindowsPowerShell\\v1.0\\powershell.exe";
  return path;
}

int run_powershell_script(const std::filesystem::path &script,
                          const std::vector<std::wstring> &arguments) {
  std::wstring command_line =
      quote_command_argument(powershell_path().wstring());
  command_line +=
      L" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ";
  command_line += quote_command_argument(script.wstring());
  for (const auto &argument : arguments) {
    command_line.push_back(L' ');
    command_line += quote_command_argument(argument);
  }
  command_line += L" *>> ";
  command_line += quote_command_argument(installer_log_path().wstring());

  return run_process(std::move(command_line));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::filesystem::path root;
  try {
    append_installer_log(L"existing vDS uninstall runner started");
    root = uninstall_root();
    write_payloads(root);

    int status = 0;
    if (service_exists(L"vdsd")) {
      status =
          run_powershell_script(root / L"uninstall_service.ps1", {L"-Stop"});
      (void)status;
    }

    const std::wstring previous_install_dir = read_previous_install_dir();
    if (!previous_install_dir.empty()) {
      status = run_powershell_script(root / L"uninstall_env.ps1",
                                     {previous_install_dir});
      (void)status;
    }

    status = run_powershell_script(root / L"windrv\\uninstall.ps1",
                                   {L"-Target", L"all"});
    (void)status;

    std::filesystem::remove_all(root);
    append_installer_log(L"existing vDS uninstall runner finished");
    return 0;
  } catch (...) {
    if (!root.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
    append_installer_log(L"existing vDS uninstall runner failed");
    return 1;
  }
}
