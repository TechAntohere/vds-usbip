#include <cstdio>
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

std::filesystem::path powershell_path() {
  std::vector<wchar_t> system_dir(MAX_PATH + 1);
  const UINT length = GetSystemDirectoryW(system_dir.data(),
                                          static_cast<UINT>(system_dir.size()));
  if (length == 0 || length >= system_dir.size()) {
    return L"powershell.exe";
  }

  std::filesystem::path path(system_dir.data());
  path /= L"WindowsPowerShell\\v1.0\\powershell.exe";
  return path;
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

void append_process_output(std::wstring_view prefix, std::string_view output) {
  if (output.empty()) {
    std::wstring message(prefix);
    message += L"<no output>";
    append_installer_log(message);
    return;
  }

  std::istringstream stream{std::string(output)};
  std::string line;
  while (std::getline(stream, line)) {
    std::wstring message(prefix);
    message += wide_from_log_bytes(line);
    append_installer_log(message);
  }
}

int run_process(std::wstring command_line) {
  {
    std::wstring message = L"run driver script: ";
    message += command_line;
    append_installer_log(message);
  }

  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0)) {
    std::wstring message = L"driver script CreatePipe failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    return 1;
  }
  if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    std::wstring message = L"driver script SetHandleInformation failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 1;
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
    std::wstring message = L"driver script CreateProcessW failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 1;
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

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  CloseHandle(read_pipe);

  append_process_output(L"driver script output: ", output);

  std::wstring message = L"driver script exit: ";
  message += std::to_wstring(exit_code);
  message += L" output_bytes=";
  message += std::to_wstring(output.size());
  append_installer_log(message);
  return static_cast<int>(exit_code);
}

bool is_success_exit_code(int status) {
  return status == 0 || status == 3010 || status == 1641;
}

int run_powershell_script(const std::filesystem::path &script,
                          const std::vector<std::wstring> &arguments) {
  std::wstring command_line =
      quote_command_argument(powershell_path().wstring());
  command_line += L" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                  L"-WindowStyle Hidden -File ";
  command_line += quote_command_argument(script.wstring());
  for (const auto &argument : arguments) {
    command_line.push_back(L' ');
    command_line += quote_command_argument(argument);
  }

  return run_process(std::move(command_line));
}

} // namespace

extern "C" {
extern int __argc;
extern wchar_t **__wargv;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  if (__argc != 4) {
    return 2;
  }

  const std::wstring_view action = __wargv[1];
  const std::wstring target = __wargv[2];
  const std::filesystem::path script = __wargv[3];

  int status = 1;
  if (action == L"install") {
    status = run_powershell_script(
        script, {L"-Target", target, L"-SkipRemovePrevious"});
  } else if (action == L"uninstall") {
    status = run_powershell_script(script, {L"-Target", target});
  } else {
    return 2;
  }

  return is_success_exit_code(status) ? 0 : status;
}
