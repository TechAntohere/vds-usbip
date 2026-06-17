// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vds_common.hh"
#include "vds_config.hh"
#include "vds_fs.hh"

namespace {

struct JsonValue {
  std::string string_value;
  std::vector<unsigned> array_value;
  bool is_array = false;
};

struct JsonField {
  std::string key;
  JsonValue value;
};

bool is_hex_digit(char ch) {
  return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

char lowercase_hex(char ch) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
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
      throw std::runtime_error("empty port in ports list");
    }
    parts.emplace_back(part);
    if (comma == std::string_view::npos) {
      break;
    }
    offset = comma + 1;
  }
  return parts;
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
    throw std::runtime_error("invalid JSON config record at line " +
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

unsigned parse_json_unsigned(std::string_view text, std::size_t &offset,
                             std::size_t line_number) {
  skip_json_space(text, offset);
  const std::size_t start = offset;
  while (offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
  if (start == offset) {
    throw std::runtime_error("expected JSON unsigned integer at line " +
                             std::to_string(line_number));
  }

  unsigned long long value = 0;
  for (const char ch : text.substr(start, offset - start)) {
    value = value * 10 + static_cast<unsigned>(ch - '0');
    if (value > std::numeric_limits<unsigned>::max()) {
      throw std::runtime_error("JSON unsigned integer overflow at line " +
                               std::to_string(line_number));
    }
  }
  return static_cast<unsigned>(value);
}

std::vector<unsigned> parse_json_unsigned_array(std::string_view text,
                                                std::size_t &offset,
                                                std::size_t line_number) {
  std::vector<unsigned> values;
  require_json_char(text, offset, '[', line_number);
  skip_json_space(text, offset);
  if (offset < text.size() && text[offset] == ']') {
    ++offset;
    return values;
  }

  while (true) {
    values.push_back(parse_json_unsigned(text, offset, line_number));
    skip_json_space(text, offset);
    if (offset >= text.size()) {
      throw std::runtime_error("unterminated JSON array at line " +
                               std::to_string(line_number));
    }
    if (text[offset] == ']') {
      ++offset;
      return values;
    }
    if (text[offset] != ',') {
      throw std::runtime_error("expected JSON array separator at line " +
                               std::to_string(line_number));
    }
    ++offset;
  }
}

JsonValue parse_json_value(std::string_view line, std::size_t &offset,
                           std::size_t line_number) {
  skip_json_space(line, offset);
  if (offset >= line.size()) {
    throw std::runtime_error("missing JSON value at line " +
                             std::to_string(line_number));
  }
  if (line[offset] == '"') {
    return JsonValue{
        .string_value = parse_json_string(line, offset, line_number),
        .array_value = {},
        .is_array = false,
    };
  }
  if (line[offset] == '[') {
    return JsonValue{
        .string_value = {},
        .array_value = parse_json_unsigned_array(line, offset, line_number),
        .is_array = true,
    };
  }
  throw std::runtime_error("unsupported JSON value at line " +
                           std::to_string(line_number));
}

std::vector<JsonField> parse_json_object(std::string_view line,
                                         std::size_t line_number) {
  std::vector<JsonField> fields;
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
    fields.push_back(JsonField{
        .key = std::move(key),
        .value = parse_json_value(line, offset, line_number),
    });

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

std::string serialize_ports(std::span<const unsigned> ports) {
  std::string text = "[";
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (i > 0) {
      text += ',';
    }
    text += std::to_string(ports[i]);
  }
  text += ']';
  return text;
}

std::string serialize_config_db(const vds::ConfigDb &db) {
  std::string text;
  for (const auto &controller : db.controllers) {
    text += "{\"address\":\"";
    text += escape_json_string(controller.address);
    text += "\",\"profile\":\"";
    text +=
        escape_json_string(vds::controller_profile_name(controller.profile));
    text += "\",\"ports\":";
    text += serialize_ports(controller.ports);
    text += "}\n";
  }
  return text;
}

void require_string_value(const JsonValue &value, std::string_view key,
                          std::size_t line_number) {
  if (value.is_array) {
    throw std::runtime_error("field " + std::string(key) +
                             " must be a string at line " +
                             std::to_string(line_number));
  }
}

void require_array_value(const JsonValue &value, std::string_view key,
                         std::size_t line_number) {
  if (!value.is_array) {
    throw std::runtime_error("field " + std::string(key) +
                             " must be an array at line " +
                             std::to_string(line_number));
  }
}

void validate_ports(std::vector<unsigned> &ports) {
  std::sort(ports.begin(), ports.end());
  const auto duplicate = std::adjacent_find(ports.begin(), ports.end());
  if (duplicate != ports.end()) {
    throw std::runtime_error("duplicate port: " + std::to_string(*duplicate));
  }
  for (const unsigned port : ports) {
    if (port >= vds::kMaxPortCount) {
      throw std::runtime_error("port must be in range 0.." +
                               std::to_string(vds::kMaxPortCount - 1) + ": " +
                               std::to_string(port));
    }
  }
}

vds::ControllerConfig parse_controller_line(const std::string &line,
                                            std::size_t line_number) {
  vds::ControllerConfig config{
      .address = {},
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
  };
  bool have_address = false;
  bool have_profile = false;
  bool have_ports = false;

  for (const auto &field : parse_json_object(line, line_number)) {
    if (field.key == "address") {
      if (have_address) {
        throw std::runtime_error("duplicate address field at line " +
                                 std::to_string(line_number));
      }
      require_string_value(field.value, field.key, line_number);
      config.address =
          vds::normalize_bluetooth_address(field.value.string_value);
      have_address = true;
    } else if (field.key == "profile") {
      if (have_profile) {
        throw std::runtime_error("duplicate profile field at line " +
                                 std::to_string(line_number));
      }
      require_string_value(field.value, field.key, line_number);
      config.profile = vds::parse_controller_profile(field.value.string_value);
      have_profile = true;
    } else if (field.key == "ports") {
      if (have_ports) {
        throw std::runtime_error("duplicate ports field at line " +
                                 std::to_string(line_number));
      }
      require_array_value(field.value, field.key, line_number);
      config.ports = field.value.array_value;
      validate_ports(config.ports);
      have_ports = true;
    } else {
      throw std::runtime_error("unknown JSON config field at line " +
                               std::to_string(line_number) + ": " + field.key);
    }
  }

  if (!have_address || !have_profile || !have_ports) {
    throw std::runtime_error("missing controller field at line " +
                             std::to_string(line_number));
  }
  return config;
}

vds::ConfigDb parse_config_db(std::string_view text) {
  vds::ConfigDb db;
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
    vds::ControllerConfig config = parse_controller_line(line, line_number);
    const auto duplicate =
        std::find_if(db.controllers.begin(), db.controllers.end(),
                     [&](const vds::ControllerConfig &existing) {
                       return existing.address == config.address;
                     });
    if (duplicate != db.controllers.end()) {
      throw std::runtime_error("duplicate controller address at line " +
                               std::to_string(line_number));
    }
    db.controllers.push_back(std::move(config));
  }
  return db;
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

ControllerProfile parse_controller_profile(std::string_view profile) {
  if (profile.empty()) {
    return ControllerProfile::Unspecified;
  }
  if (profile == "ds5") {
    return ControllerProfile::Ds5;
  }
  if (profile == "dse") {
    return ControllerProfile::Dse;
  }
  throw std::runtime_error("unknown profile: " + std::string(profile));
}

std::string controller_profile_name(ControllerProfile profile) {
  switch (profile) {
  case ControllerProfile::Unspecified:
    return "";
  case ControllerProfile::Ds5:
    return "ds5";
  case ControllerProfile::Dse:
    return "dse";
  }
  throw std::runtime_error("unknown controller profile");
}

std::vector<unsigned> parse_ports(std::string_view text) {
  if (text.empty()) {
    return {};
  }

  std::vector<unsigned> ports;
  for (const auto &part : split_commas(text)) {
    if (!vds::is_decimal_number(part)) {
      throw std::runtime_error("port must be a number: " + part);
    }
    unsigned long long value = 0;
    for (const char ch : part) {
      value = value * 10 + static_cast<unsigned>(ch - '0');
      if (value > std::numeric_limits<unsigned>::max()) {
        throw std::runtime_error("port number overflow: " + part);
      }
    }
    ports.push_back(static_cast<unsigned>(value));
  }
  validate_ports(ports);
  return ports;
}

std::string format_ports(std::span<const unsigned> ports) {
  std::string text;
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (i > 0) {
      text += ',';
    }
    text += std::to_string(ports[i]);
  }
  return text;
}

std::string port_path_for_index(unsigned port) {
  if (port >= kMaxPortCount) {
    throw std::runtime_error("port must be in range 0.." +
                             std::to_string(kMaxPortCount - 1) + ": " +
                             std::to_string(port));
  }
#ifdef _WIN32
  return R"(\\.\vds)" + std::to_string(port);
#else
  return "/dev/vds" + std::to_string(port);
#endif
}

std::optional<unsigned> port_index_from_path(std::string_view path) {
#ifdef _WIN32
  constexpr std::string_view dos_prefix = R"(\\.\vds)";
  constexpr std::string_view nt_prefix = R"(\\?\vds)";
  constexpr std::string_view short_prefix = "vds";
  std::string_view port_text;
  if (path.rfind(dos_prefix, 0) == 0) {
    port_text = path.substr(dos_prefix.size());
  } else if (path.rfind(nt_prefix, 0) == 0) {
    port_text = path.substr(nt_prefix.size());
  } else if (path.rfind(short_prefix, 0) == 0) {
    port_text = path.substr(short_prefix.size());
  } else {
    return std::nullopt;
  }
#else
  constexpr std::string_view prefix = "/dev/vds";
  if (path.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  const std::string_view port_text = path.substr(prefix.size());
#endif

  if (!vds::is_decimal_number(port_text)) {
    return std::nullopt;
  }
  unsigned value = 0;
  for (const char ch : port_text) {
    value = value * 10 + static_cast<unsigned>(ch - '0');
    if (value >= kMaxPortCount) {
      return std::nullopt;
    }
  }
  return value;
}

bool controller_config_allows_port(const ControllerConfig &config,
                                   unsigned port) {
  return config.ports.empty() ||
         std::find(config.ports.begin(), config.ports.end(), port) !=
             config.ports.end();
}

bool controller_config_allows_path(const ControllerConfig &config,
                                   std::string_view path) {
  const auto port = port_index_from_path(path);
  return port && controller_config_allows_port(config, *port);
}

std::vector<unsigned>
controller_config_port_targets(const ControllerConfig &config) {
  if (!config.ports.empty()) {
    return config.ports;
  }

  std::vector<unsigned> ports;
  ports.reserve(kMaxPortCount);
  for (unsigned port = 0; port < kMaxPortCount; ++port) {
    ports.push_back(port);
  }
  return ports;
}

bool controller_config_has_candidate_port(
    const ControllerConfig &config, std::span<const unsigned> candidate_ports) {
  return select_controller_config_port(config, candidate_ports).has_value();
}

std::optional<unsigned>
select_controller_config_port(const ControllerConfig &config,
                              std::span<const unsigned> candidate_ports,
                              std::span<const unsigned> occupied_ports) {
  for (const unsigned port : controller_config_port_targets(config)) {
    if (std::find(candidate_ports.begin(), candidate_ports.end(), port) ==
        candidate_ports.end()) {
      continue;
    }
    if (std::find(occupied_ports.begin(), occupied_ports.end(), port) !=
        occupied_ports.end()) {
      continue;
    }
    return port;
  }
  return std::nullopt;
}

const ControllerConfig *
find_controller_config_by_address(const ConfigDb &db,
                                  std::string_view address) {
  const std::string normalized = normalize_bluetooth_address(address);
  for (const auto &config : db.controllers) {
    if (config.address == normalized) {
      return &config;
    }
  }
  return nullptr;
}

ConfigDb load_config_db(const std::string &path) {
  const std::filesystem::path db_path(path);
  ensure_parent_directory(db_path);
  const auto lock = lock_file(db_path);
  return parse_config_db(read_file_if_exists(db_path));
}

ConfigDb update_config_db(const std::string &path,
                          const std::function<void(ConfigDb &)> &update) {
  const std::filesystem::path db_path(path);
  ensure_parent_directory(db_path);
  const auto lock = lock_file(db_path);
  ConfigDb db = parse_config_db(read_file_if_exists(db_path));
  update(db);
  write_file_atomic(db_path, serialize_config_db(db));
  return db;
}

void upsert_controller_config(ConfigDb &db, ControllerConfig config) {
  config.address = normalize_bluetooth_address(config.address);
  validate_ports(config.ports);
  for (auto &controller : db.controllers) {
    if (controller.address == config.address) {
      controller = std::move(config);
      return;
    }
  }
  db.controllers.push_back(std::move(config));
}

bool remove_controller_config(ConfigDb &db, std::string_view address) {
  const std::string normalized = normalize_bluetooth_address(address);
  const auto old_size = db.controllers.size();
  db.controllers.erase(std::remove_if(db.controllers.begin(),
                                      db.controllers.end(),
                                      [&](const ControllerConfig &config) {
                                        return config.address == normalized;
                                      }),
                       db.controllers.end());
  return db.controllers.size() != old_size;
}

void validate_config_assignments(const ConfigDb &db,
                                 std::span<const std::string> devices) {
  if (db.controllers.empty()) {
    return;
  }
  if (devices.empty()) {
    throw std::runtime_error("no available virtual ports");
  }

  std::vector<unsigned> available_ports;
  for (const auto &device : devices) {
    const auto port = port_index_from_path(device);
    if (port) {
      available_ports.push_back(*port);
    }
  }
  std::sort(available_ports.begin(), available_ports.end());

  for (const auto &config : db.controllers) {
    if (config.ports.empty()) {
      continue;
    }
    const auto allowed_port = std::find_if(
        config.ports.begin(), config.ports.end(), [&](unsigned port) {
          return std::binary_search(available_ports.begin(),
                                    available_ports.end(), port);
        });
    if (allowed_port == config.ports.end()) {
      throw std::runtime_error("no allowed virtual port for " + config.address);
    }
  }
}

} // namespace vds
