// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_config.hh"

namespace {

constexpr mode_t kVdsFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

bool is_hex_digit(char ch) {
  return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

char lowercase_hex(char ch) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

bool is_decimal_number(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
    return std::isdigit(static_cast<unsigned char>(ch)) != 0;
  });
}

bool is_vds_device_path(std::string_view path) {
  constexpr std::string_view prefix = "/dev/vds";
  return path.rfind(prefix, 0) == 0 &&
         is_decimal_number(path.substr(prefix.size()));
}

std::vector<std::string> split_commas(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t comma = text.find(',', offset);
    const std::string_view part = text.substr(
        offset, comma == std::string_view::npos ? std::string_view::npos
                                                : comma - offset);
    if (part.empty()) {
      throw std::runtime_error("empty device in limit-dev list");
    }
    parts.emplace_back(part);
    if (comma == std::string_view::npos) {
      break;
    }
    offset = comma + 1;
  }
  return parts;
}

void ensure_db_directory(const std::filesystem::path &path) {
  const std::filesystem::path directory = path.parent_path();
  if (!directory.empty()) {
    std::filesystem::create_directories(directory);
  }
}

std::filesystem::path db_directory(const std::filesystem::path &db_path) {
  const std::filesystem::path directory = db_path.parent_path();
  if (directory.empty()) {
    return ".";
  }
  return directory;
}

void chmod_db_file_if_exists(const std::filesystem::path &db_path) {
  if (::chmod(db_path.c_str(), kVdsFileMode) < 0 && errno != ENOENT) {
    throw std::runtime_error("failed to chmod " + db_path.string() + ": " +
                             std::strerror(errno));
  }
}

vds::UniqueFd lock_db_directory(const std::filesystem::path &db_path) {
  const std::filesystem::path directory = db_directory(db_path);
  vds::UniqueFd fd(
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!fd) {
    throw std::runtime_error("failed to open DB directory " +
                             directory.string() + ": " + std::strerror(errno));
  }
  if (::flock(fd.get(), LOCK_EX) < 0) {
    throw std::runtime_error("failed to lock DB directory " +
                             directory.string() + ": " + std::strerror(errno));
  }
  chmod_db_file_if_exists(db_path);
  return fd;
}

std::string read_file_if_exists(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (!error) {
      return {};
    }
    throw std::runtime_error("failed to stat " + path.string() + ": " +
                             error.message());
  }

  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

std::string serialize_limit_devices(const std::vector<std::string> &devices) {
  std::string text;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (i > 0) {
      text += ',';
    }
    text += devices[i];
  }
  return text;
}

void skip_json_space(std::string_view text, std::size_t &offset) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
}

void require_json_char(std::string_view text, std::size_t &offset,
                       char expected, std::size_t line_number) {
  skip_json_space(text, offset);
  if (offset >= text.size() || text[offset] != expected) {
    throw std::runtime_error("invalid JSON binding record at line " +
                             std::to_string(line_number));
  }
  ++offset;
}

char parse_json_escape(char escape, std::size_t line_number) {
  switch (escape) {
  case '"':
  case '\\':
  case '/':
    return escape;
  case 'b':
    return '\b';
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  default:
    throw std::runtime_error("unsupported JSON escape at line " +
                             std::to_string(line_number));
  }
}

int parse_json_hex_digit(char ch, std::size_t line_number) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  throw std::runtime_error("invalid JSON unicode escape at line " +
                           std::to_string(line_number));
}

std::string parse_json_string(std::string_view text, std::size_t &offset,
                              std::size_t line_number) {
  skip_json_space(text, offset);
  if (offset >= text.size() || text[offset] != '"') {
    throw std::runtime_error("expected JSON string at line " +
                             std::to_string(line_number));
  }
  ++offset;

  std::string value;
  while (offset < text.size()) {
    const char ch = text[offset++];
    if (ch == '"') {
      return value;
    }
    if (ch == '\\') {
      if (offset >= text.size()) {
        throw std::runtime_error("unterminated JSON escape at line " +
                                 std::to_string(line_number));
      }
      const char escape = text[offset++];
      if (escape == 'u') {
        if (offset + 4 > text.size()) {
          throw std::runtime_error("unterminated JSON unicode escape at line " +
                                   std::to_string(line_number));
        }
        unsigned int code_point = 0;
        for (int i = 0; i < 4; ++i) {
          code_point = (code_point << 4) |
                       parse_json_hex_digit(text[offset++], line_number);
        }
        if (code_point > 0x7f) {
          throw std::runtime_error("unsupported JSON unicode escape at line " +
                                   std::to_string(line_number));
        }
        value.push_back(static_cast<char>(code_point));
      } else {
        value.push_back(parse_json_escape(escape, line_number));
      }
      continue;
    }
    if (static_cast<unsigned char>(ch) < 0x20) {
      throw std::runtime_error("invalid control character at line " +
                               std::to_string(line_number));
    }
    value.push_back(ch);
  }

  throw std::runtime_error("unterminated JSON string at line " +
                           std::to_string(line_number));
}

std::vector<std::pair<std::string, std::string>>
parse_json_object(std::string_view line, std::size_t line_number) {
  std::vector<std::pair<std::string, std::string>> fields;
  std::size_t offset = 0;

  require_json_char(line, offset, '{', line_number);
  skip_json_space(line, offset);
  if (offset < line.size() && line[offset] == '}') {
    ++offset;
    skip_json_space(line, offset);
    if (offset != line.size()) {
      throw std::runtime_error("trailing data after JSON object at line " +
                               std::to_string(line_number));
    }
    return fields;
  }

  while (true) {
    std::string key = parse_json_string(line, offset, line_number);
    require_json_char(line, offset, ':', line_number);
    std::string value = parse_json_string(line, offset, line_number);
    fields.emplace_back(std::move(key), std::move(value));

    skip_json_space(line, offset);
    if (offset >= line.size()) {
      throw std::runtime_error("unterminated JSON object at line " +
                               std::to_string(line_number));
    }
    if (line[offset] == '}') {
      ++offset;
      skip_json_space(line, offset);
      if (offset != line.size()) {
        throw std::runtime_error("trailing data after JSON object at line " +
                                 std::to_string(line_number));
      }
      return fields;
    }
    if (line[offset] != ',') {
      throw std::runtime_error("expected JSON field separator at line " +
                               std::to_string(line_number));
    }
    ++offset;
  }
}

std::string escape_json_string(std::string_view text) {
  std::string escaped;
  for (const char ch : text) {
    switch (ch) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char code[7];
        std::snprintf(code, sizeof(code), "\\u%04x",
                      static_cast<unsigned char>(ch));
        escaped += code;
      } else {
        escaped.push_back(ch);
      }
      break;
    }
  }
  return escaped;
}

std::string serialize_binding_db(const vds::BindingDb &db) {
  std::string text;
  for (const auto &controller : db.controllers) {
    text += "{\"address\": \"";
    text += escape_json_string(controller.address);
    text += "\", \"identity\": \"";
    text += escape_json_string(vds::binding_identity_name(controller.identity));
    text += "\", \"limit\": \"";
    text +=
        escape_json_string(serialize_limit_devices(controller.limit_devices));
    text += "\"}\n";
  }
  return text;
}

vds::ControllerBindingConfig parse_controller_line(const std::string &line,
                                                   std::size_t line_number) {
  vds::ControllerBindingConfig binding{
      .address = {},
      .identity = vds::BindingIdentity::Auto,
      .limit_devices = {},
  };
  bool have_address = false;
  bool have_identity = false;
  bool have_limit = false;

  for (const auto &[key, value] : parse_json_object(line, line_number)) {
    if (key == "address") {
      if (have_address) {
        throw std::runtime_error("duplicate address field at line " +
                                 std::to_string(line_number));
      }
      binding.address = vds::normalize_bluetooth_address(value);
      have_address = true;
    } else if (key == "identity") {
      if (have_identity) {
        throw std::runtime_error("duplicate identity field at line " +
                                 std::to_string(line_number));
      }
      binding.identity = vds::parse_binding_identity(value);
      have_identity = true;
    } else if (key == "limit") {
      if (have_limit) {
        throw std::runtime_error("duplicate limit field at line " +
                                 std::to_string(line_number));
      }
      binding.limit_devices = vds::parse_limit_devices(value);
      have_limit = true;
    } else {
      throw std::runtime_error("unknown JSON binding field at line " +
                               std::to_string(line_number) + ": " + key);
    }
  }

  if (!have_address || !have_identity || !have_limit) {
    throw std::runtime_error("missing controller field at line " +
                             std::to_string(line_number));
  }
  return binding;
}

vds::BindingDb parse_binding_db(std::string_view text) {
  vds::BindingDb db;
  std::istringstream stream{std::string(text)};
  std::string line;
  std::size_t line_number = 0;

  while (std::getline(stream, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    vds::ControllerBindingConfig binding =
        parse_controller_line(line, line_number);
    const auto duplicate =
        std::find_if(db.controllers.begin(), db.controllers.end(),
                     [&](const vds::ControllerBindingConfig &existing) {
                       return existing.address == binding.address;
                     });
    if (duplicate != db.controllers.end()) {
      throw std::runtime_error("duplicate controller address at line " +
                               std::to_string(line_number));
    }
    db.controllers.push_back(std::move(binding));
  }
  return db;
}

void write_file_atomic(const std::filesystem::path &path,
                       const std::string &text) {
  const std::filesystem::path tmp_path = path.string() + ".tmp";
  {
    vds::UniqueFd fd(::open(tmp_path.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                            kVdsFileMode));
    if (!fd) {
      throw std::runtime_error("failed to open " + tmp_path.string() + ": " +
                               std::strerror(errno));
    }
    if (::fchmod(fd.get(), kVdsFileMode) < 0) {
      throw std::runtime_error("failed to chmod " + tmp_path.string() + ": " +
                               std::strerror(errno));
    }
    const char *data = text.data();
    std::size_t size = text.size();
    while (size > 0) {
      const ssize_t written = ::write(fd.get(), data, size);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("failed to write " + tmp_path.string() + ": " +
                                 std::strerror(errno));
      }
      if (written == 0) {
        throw std::runtime_error("write returned zero for " +
                                 tmp_path.string());
      }
      data += written;
      size -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd.get()) < 0) {
      throw std::runtime_error("failed to fsync " + tmp_path.string() + ": " +
                               std::strerror(errno));
    }
  }
  if (::rename(tmp_path.c_str(), path.c_str()) < 0) {
    throw std::runtime_error("failed to rename " + tmp_path.string() + " to " +
                             path.string() + ": " + std::strerror(errno));
  }
  const std::filesystem::path directory = db_directory(path);
  const vds::UniqueFd directory_fd(
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd) {
    throw std::runtime_error(std::string("failed to open DB directory for "
                                         "fsync: ") +
                             std::strerror(errno));
  }
  if (::fsync(directory_fd.get()) < 0) {
    throw std::runtime_error(std::string("failed to fsync DB directory: ") +
                             std::strerror(errno));
  }
}

} // namespace

namespace vds {

std::string normalize_bluetooth_address(std::string_view address) {
  if (address.size() != 17) {
    throw std::runtime_error("invalid Bluetooth address: " +
                             std::string(address));
  }

  std::string normalized;
  normalized.reserve(address.size());
  for (std::size_t i = 0; i < address.size(); ++i) {
    const bool separator = i == 2 || i == 5 || i == 8 || i == 11 || i == 14;
    if (separator) {
      if (address[i] != ':') {
        throw std::runtime_error("invalid Bluetooth address: " +
                                 std::string(address));
      }
      normalized.push_back(':');
      continue;
    }
    if (!is_hex_digit(address[i])) {
      throw std::runtime_error("invalid Bluetooth address: " +
                               std::string(address));
    }
    normalized.push_back(lowercase_hex(address[i]));
  }
  return normalized;
}

BindingIdentity parse_binding_identity(std::string_view identity) {
  if (identity == "auto") {
    return BindingIdentity::Auto;
  }
  if (identity == "ds5" || identity == "dualsense") {
    return BindingIdentity::Ds5;
  }
  if (identity == "dse" || identity == "dualsense-edge") {
    return BindingIdentity::Dse;
  }
  throw std::runtime_error("unknown identity: " + std::string(identity));
}

std::string binding_identity_name(BindingIdentity identity) {
  switch (identity) {
  case BindingIdentity::Auto:
    return "auto";
  case BindingIdentity::Ds5:
    return "ds5";
  case BindingIdentity::Dse:
    return "dse";
  }
  throw std::runtime_error("unknown binding identity");
}

std::vector<std::string> parse_limit_devices(std::string_view text) {
  if (text.empty()) {
    return {};
  }

  std::vector<std::string> devices = split_commas(text);
  std::sort(devices.begin(), devices.end());
  const auto duplicate = std::adjacent_find(devices.begin(), devices.end());
  if (duplicate != devices.end()) {
    throw std::runtime_error("duplicate limit device: " + *duplicate);
  }
  for (const auto &device : devices) {
    if (!is_vds_device_path(device)) {
      throw std::runtime_error("limit device must be /dev/vdsN: " + device);
    }
  }
  return devices;
}

BindingDb load_binding_db(const std::string &path) {
  const std::filesystem::path db_path(path);
  ensure_db_directory(db_path);
  const UniqueFd lock = lock_db_directory(db_path);
  (void)lock;
  return parse_binding_db(read_file_if_exists(db_path));
}

BindingDb update_binding_db(const std::string &path,
                            const std::function<void(BindingDb &)> &update) {
  const std::filesystem::path db_path(path);
  ensure_db_directory(db_path);
  const UniqueFd lock = lock_db_directory(db_path);
  (void)lock;
  BindingDb db = parse_binding_db(read_file_if_exists(db_path));
  update(db);
  write_file_atomic(db_path, serialize_binding_db(db));
  return db;
}

void upsert_binding(BindingDb &db, ControllerBindingConfig binding) {
  binding.address = normalize_bluetooth_address(binding.address);
  for (auto &controller : db.controllers) {
    if (controller.address == binding.address) {
      controller = std::move(binding);
      return;
    }
  }
  db.controllers.push_back(std::move(binding));
}

bool remove_binding(BindingDb &db, std::string_view address) {
  const std::string normalized = normalize_bluetooth_address(address);
  const auto old_size = db.controllers.size();
  db.controllers.erase(
      std::remove_if(db.controllers.begin(), db.controllers.end(),
                     [&](const ControllerBindingConfig &binding) {
                       return binding.address == normalized;
                     }),
      db.controllers.end());
  return db.controllers.size() != old_size;
}

void validate_binding_assignments(const BindingDb &db,
                                  std::span<const std::string> devices) {
  if (db.controllers.empty()) {
    return;
  }
  if (devices.empty()) {
    throw std::runtime_error("no available virtual ports");
  }

  std::vector<std::string> sorted_devices(devices.begin(), devices.end());
  std::sort(sorted_devices.begin(), sorted_devices.end());

  for (const auto &binding : db.controllers) {
    if (binding.limit_devices.empty()) {
      continue;
    }

    const auto allowed_device =
        std::find_if(binding.limit_devices.begin(), binding.limit_devices.end(),
                     [&](const std::string &device) {
                       return std::binary_search(sorted_devices.begin(),
                                                 sorted_devices.end(), device);
                     });
    if (allowed_device == binding.limit_devices.end()) {
      throw std::runtime_error("no allowed virtual port for " +
                               binding.address);
    }
  }
}

} // namespace vds
