// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "vds_log.hh"

namespace {

std::string local_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds)
          .count();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);

  std::tm local_time{};
  if (!::localtime_r(&time, &local_time)) {
    throw std::runtime_error("failed to get local time");
  }

  const long offset_seconds = local_time.tm_gmtoff;
  const char offset_sign = offset_seconds < 0 ? '-' : '+';
  const long abs_offset = std::labs(offset_seconds);
  const long offset_hours = abs_offset / 3600;
  const long offset_minutes = (abs_offset % 3600) / 60;

  std::ostringstream out;
  out << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
      << std::setfill('0') << milliseconds << offset_sign << std::setw(2)
      << std::setfill('0') << offset_hours << ':' << std::setw(2)
      << std::setfill('0') << offset_minutes;
  return out.str();
}

void write_log_message(std::ostream &out, std::string_view message) {
  for (const char ch : message) {
    if (ch == '\n' || ch == '\r' || ch == '|') {
      out << ' ';
    } else {
      out << ch;
    }
  }
}

} // namespace

namespace vds {

Logger::Logger(const std::string &path) {
  const std::filesystem::path log_path(path);
  const std::filesystem::path directory = log_path.parent_path();
  if (!directory.empty()) {
    std::filesystem::create_directories(directory);
  }
  file_.open(path, std::ios::app);
  if (!file_) {
    throw std::runtime_error("failed to open log file: " + path);
  }
}

void Logger::log(std::string_view scope, LogLevel level,
                 std::string_view message) {
  file_ << local_timestamp() << '|' << scope << '|' << log_level_name(level)
        << '|';
  write_log_message(file_, message);
  file_ << '\n';
  file_.flush();
}

const char *log_level_name(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Info:
    return "INFO";
  case LogLevel::Warn:
    return "WARN";
  case LogLevel::Error:
    return "ERROR";
  }
  return "ERROR";
}

} // namespace vds
