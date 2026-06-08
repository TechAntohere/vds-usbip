// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uapi/vds.h"
#include "unique_fd.hh"
#include "vds/ds5.h"
#include "vds_bt.hh"
#include "vds_config.hh"
#include "vds_log.hh"
#include "vds_protocol.hh"

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";
constexpr const char *kDefaultLogPath = "/var/log/vdsd.log";
constexpr std::size_t kTraceDumpMaxBytes = 96;
constexpr std::size_t kUsbInputButtonsOffset = 8;
constexpr std::size_t kUsbInputButtonsSize = 4;
constexpr std::uint64_t kInputTraceSummaryInterval = 1000;
constexpr std::uint64_t kOutputTraceSummaryInterval = 250;
constexpr int kMaxPortFramesPerWake = 64;
constexpr int kMaxBtPacketsPerWake = 64;
constexpr int kPendingOutputPollMs = 2;
constexpr auto kInputTraceGapWarn = std::chrono::milliseconds(20);
constexpr auto kInputTraceSlowWriteWarn = std::chrono::milliseconds(5);
constexpr auto kOutputTraceSlowWarn = std::chrono::milliseconds(5);
constexpr auto kOutputTraceFeatureSlowWarn = std::chrono::milliseconds(20);
/*
 * 0x36 carries both a haptics block and a 10 ms Opus speaker block. Pace the
 * combined packet on the speaker cadence; using the 64-byte haptics duration
 * here lets speaker audio drift into periodic underruns after a few seconds.
 */
constexpr auto kAudioOutputInterval = std::chrono::milliseconds(10);
constexpr auto kHapticsOutputBlockedRetry = std::chrono::milliseconds(2);
constexpr auto kBluetoothPreemptWait = std::chrono::milliseconds(2000);
constexpr auto kBluetoothPreemptPoll = std::chrono::milliseconds(100);
constexpr auto kPortScanInterval = std::chrono::milliseconds(2000);
constexpr int kInitialFeatureReportPollMs = 5000;
constexpr std::size_t kMaxPendingAudioChunks = 8;
constexpr std::array<std::uint8_t, 3> kInitialFeatureReportIds = {0x09, 0x20,
                                                                  0x05};

volatile sig_atomic_t g_stop_requested = 0;

constexpr std::uint32_t kTraceInput = 1u << 0;
constexpr std::uint32_t kTraceOutput = 1u << 1;
constexpr std::uint32_t kTraceAll = kTraceInput | kTraceOutput;

struct DeviceId {
  std::uint16_t vendor;
  std::uint16_t product;
};

struct Options {
  std::string socket = kDefaultControlSocket;
  std::string log_path = kDefaultLogPath;
};

struct LatencyTraceStats {
  std::uint64_t count = 0;
  std::uint64_t total_us = 0;
  std::uint64_t max_us = 0;
};

struct TraceState {
  std::vector<std::uint8_t> last_hid_out;
  std::uint64_t dropped_usb_frame_count = 0;
  std::uint64_t dropped_audio_haptics_count = 0;
  std::uint64_t queue_dropped_audio_haptics_count = 0;
  std::uint64_t blocked_audio_haptics_count = 0;
  std::uint64_t skipped_silent_audio_count = 0;
  std::uint64_t deferred_bt_state_count = 0;
  std::uint64_t coalesced_bt_state_count = 0;
  std::uint64_t blocked_bt_state_count = 0;
  std::uint64_t audio_usb_frame_count = 0;
  std::uint64_t bt_input_count = 0;
  LatencyTraceStats output_hid_latency;
  LatencyTraceStats output_feature_latency;
  LatencyTraceStats output_audio_latency;
  LatencyTraceStats output_audio_extract_latency;
  LatencyTraceStats output_audio_send_latency;
  std::uint64_t input_write_count = 0;
  std::uint64_t input_write_total_us = 0;
  std::uint64_t input_write_max_us = 0;
  std::uint64_t input_gap_max_us = 0;
  Clock::time_point last_input_time{};
  bool have_last_input_time = false;
  std::array<std::uint8_t, kUsbInputButtonsSize> last_input_buttons{};
  bool have_last_input_buttons = false;
  bool audio_haptics_nonzero_seen = false;
};

struct VirtualPort {
  std::string path;
  vds::UniqueFd fd;
  vds::PcmAudioExtractor extractor;
  vds::HapticsPacketBuilder haptics_builder;
  vds::DsOutputState output_state;
  std::deque<vds::AudioChunk> pending_audio_chunks;
  std::optional<vds::DsState> last_sent_bt_state;
  std::optional<vds::DsState> pending_bt_state;
  std::optional<vds::BtStateReport> pending_bt_state_report;
  Clock::time_point next_haptics_send_time{};
  bool audio_signal_active = false;
  std::array<std::vector<std::uint8_t>, 256> feature_cache;
  std::array<bool, 256> feature_cached;
  std::vector<std::uint8_t> pending_feature_reports;
  TraceState trace_state;
};

struct ControllerBinding {
  vds::ControllerBindingConfig config;
  std::string device;
  std::optional<std::uint32_t> detected_identity;
  std::optional<vds::BtL2capBackend> backend;
  vds::UniqueFd pending_control_fd;
  vds::UniqueFd pending_interrupt_fd;
  bool virtual_connected = false;
  std::string last_error;
};

enum class EventKind : std::uint32_t {
  Control = 1,
  Port = 2,
  BtControl = 3,
  BtInterrupt = 4,
  BtAcceptControl = 5,
  BtAcceptInterrupt = 6,
};

struct EventSource {
  EventKind kind;
  std::size_t index;
};

class SocketPathGuard {
public:
  explicit SocketPathGuard(std::string path) : path_(std::move(path)) {}
  ~SocketPathGuard() { ::unlink(path_.c_str()); }

  SocketPathGuard(const SocketPathGuard &) = delete;
  SocketPathGuard &operator=(const SocketPathGuard &) = delete;

private:
  std::string path_;
};

void signal_handler(int) { g_stop_requested = 1; }

void usage() {
  std::cerr << "vdsd (" << VDS_VERSION << "): vDS daemon - Copyright (C) "
            << VDS_BUILD_YEAR << " Jihong Min\n"
            << "usage: vdsd [--socket /run/vdsd.sock] "
               "[--log /var/log/vdsd.log]\n";
}

Options parse_args(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      options.socket = argv[++i];
    } else if (arg == "--log" && i + 1 < argc) {
      options.log_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  return options;
}

int open_required(const std::string &path, int flags) {
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    throw std::runtime_error("failed to open " + path + ": " +
                             std::strerror(errno));
  }
  return fd;
}

std::optional<unsigned int> parse_hex_token(std::string_view text,
                                            std::size_t offset,
                                            std::size_t max_digits) {
  unsigned int value = 0;
  std::size_t digits = 0;
  for (std::size_t i = offset; i < text.size() && digits < max_digits; ++i) {
    const char ch = text[i];
    unsigned int nibble;
    if (ch >= '0' && ch <= '9') {
      nibble = static_cast<unsigned int>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = static_cast<unsigned int>(10 + ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = static_cast<unsigned int>(10 + ch - 'A');
    } else {
      break;
    }
    value = (value << 4) | nibble;
    ++digits;
  }

  if (digits == 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string_view> line_with_prefix(std::string_view text,
                                                 std::string_view prefix) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t end = text.find('\n', offset);
    const std::string_view line = text.substr(
        offset,
        end == std::string_view::npos ? std::string_view::npos : end - offset);
    if (line.rfind(prefix, 0) == 0) {
      return line;
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  return std::nullopt;
}

std::optional<DeviceId> parse_hid_id_line(std::string_view text) {
  const auto line = line_with_prefix(text, "HID_ID=");
  if (!line) {
    return std::nullopt;
  }

  const std::size_t first_colon = line->find(':');
  if (first_colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t second_colon = line->find(':', first_colon + 1);
  if (second_colon == std::string_view::npos) {
    return std::nullopt;
  }

  const auto vendor = parse_hex_token(*line, first_colon + 1, 8);
  const auto product = parse_hex_token(*line, second_colon + 1, 8);
  if (!vendor || !product) {
    return std::nullopt;
  }

  return DeviceId{.vendor = static_cast<std::uint16_t>(*vendor & 0xffffu),
                  .product = static_cast<std::uint16_t>(*product & 0xffffu)};
}

std::optional<DeviceId> parse_modalias_line(std::string_view line) {
  const std::size_t vendor_marker = line.find('v');
  if (vendor_marker == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t product_marker = line.find('p', vendor_marker + 1);
  if (product_marker == std::string_view::npos) {
    return std::nullopt;
  }

  const bool usb_modalias = line.find("usb:") != std::string_view::npos;
  const std::size_t digits = usb_modalias ? 4 : 8;
  const auto vendor = parse_hex_token(line, vendor_marker + 1, digits);
  const auto product = parse_hex_token(line, product_marker + 1, digits);
  if (!vendor || !product) {
    return std::nullopt;
  }

  return DeviceId{.vendor = static_cast<std::uint16_t>(*vendor & 0xffffu),
                  .product = static_cast<std::uint16_t>(*product & 0xffffu)};
}

std::optional<DeviceId> parse_modalias(std::string_view text) {
  if (const auto line = line_with_prefix(text, "MODALIAS=")) {
    return parse_modalias_line(*line);
  }
  if (const auto line = line_with_prefix(text, "\tModalias: ")) {
    return parse_modalias_line(*line);
  }
  if (const auto line = line_with_prefix(text, "Modalias: ")) {
    return parse_modalias_line(*line);
  }
  return std::nullopt;
}

std::optional<DeviceId> parse_device_id(std::string_view text) {
  if (const auto hid_id = parse_hid_id_line(text)) {
    return hid_id;
  }
  return parse_modalias(text);
}

std::string hex16(std::uint16_t value) {
  std::array<char, 7> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "0x%04x", value);
  return buffer.data();
}

std::string hex_bytes(std::span<const std::uint8_t> bytes,
                      std::size_t max_bytes) {
  std::ostringstream out;
  const std::size_t count = std::min(bytes.size(), max_bytes);

  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) {
      out << ' ';
    }
    out << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<unsigned>(bytes[i]);
  }
  if (bytes.size() > max_bytes) {
    out << " ...";
  }
  return out.str();
}

std::string hex_byte(std::uint8_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setw(2) << std::setfill('0')
      << static_cast<unsigned>(value);
  return out.str();
}

std::uint32_t parse_trace_scope(std::string_view scope) {
  if (scope == "all") {
    return kTraceAll;
  }
  if (scope == "input") {
    return kTraceInput;
  }
  if (scope == "output") {
    return kTraceOutput;
  }
  throw std::runtime_error("trace scope must be all, input, or output");
}

std::string trace_scope_name(std::uint32_t scope) {
  if (scope == kTraceAll) {
    return "all";
  }
  if (scope == kTraceInput) {
    return "input";
  }
  if (scope == kTraceOutput) {
    return "output";
  }
  return "none";
}

std::string active_trace_name(std::uint32_t trace_flags) {
  if (trace_flags == 0) {
    return "none";
  }
  if (trace_flags == kTraceAll) {
    return "all";
  }
  if (trace_flags == kTraceInput) {
    return "input";
  }
  if (trace_flags == kTraceOutput) {
    return "output";
  }
  return "input,output";
}

bool trace_enabled(std::uint32_t trace_flags, std::uint32_t target) {
  return (trace_flags & target) != 0;
}

std::uint64_t duration_us(Clock::duration duration) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

void trace_output_latency(const std::string &device, std::string_view label,
                          LatencyTraceStats &stats, Clock::duration duration,
                          Clock::duration slow_threshold, vds::Logger &logger) {
  const std::uint64_t elapsed_us = duration_us(duration);
  ++stats.count;
  stats.total_us += elapsed_us;
  stats.max_us = std::max(stats.max_us, elapsed_us);

  if (duration >= slow_threshold) {
    logger.log("output", vds::LogLevel::Warn,
               device + " slow " + std::string(label) +
                   " latency count=" + std::to_string(stats.count) +
                   " latency_us=" + std::to_string(elapsed_us));
  }
  if (stats.count == 1 || stats.count % kOutputTraceSummaryInterval == 0) {
    logger.log("output", vds::LogLevel::Debug,
               device + " " + std::string(label) +
                   " latency summary count=" + std::to_string(stats.count) +
                   " avg_us=" + std::to_string(stats.total_us / stats.count) +
                   " max_us=" + std::to_string(stats.max_us));
  }
}

void trace_hid_out_report(const std::string &device,
                          std::span<const std::uint8_t> payload,
                          TraceState &trace_state, vds::Logger &logger) {
  std::ostringstream line;
  line << device << " hid out";
  if (!payload.empty()) {
    line << " report=" << hex_byte(payload[0]);
  }
  if (payload.size() >= 5) {
    /*
     * USB report 0x02 carries SetStateData after the report ID. These fields
     * show whether a plain rumble request exists before the HD haptics audio
     * path is involved.
     */
    line << " flags0=" << hex_byte(payload[1])
         << " flags1=" << hex_byte(payload[2])
         << " rumble_r=" << static_cast<unsigned>(payload[3])
         << " rumble_l=" << static_cast<unsigned>(payload[4]);
  }
  if (payload.size() >= 40) {
    line << " light_flags=" << hex_byte(payload[39]);
  }
  logger.log("hid", vds::LogLevel::Debug, line.str());

  const bool hid_out_changed =
      trace_state.last_hid_out.size() != payload.size() ||
      !std::equal(trace_state.last_hid_out.begin(),
                  trace_state.last_hid_out.end(), payload.begin());
  if (!hid_out_changed) {
    return;
  }

  trace_state.last_hid_out.assign(payload.begin(), payload.end());
  std::ostringstream changed;
  changed << device
          << " hid out changed raw=" << hex_bytes(payload, kTraceDumpMaxBytes);
  if (payload.size() >= 33 && payload[0] == VDS_USB_OUTPUT_REPORT_ID) {
    /*
     * SetStateData begins after the report ID. Offsets 10..20 and 21..31 are
     * the right/left adaptive trigger blocks from the DualSense USB report.
     */
    changed << " right_trigger=" << hex_bytes(payload.subspan(11, 11), 11)
            << " left_trigger=" << hex_bytes(payload.subspan(22, 11), 11);
  }
  logger.log("hid", vds::LogLevel::Debug, changed.str());
}

const char *usb_interface_kind_name(std::uint8_t kind) {
  switch (kind) {
  case VDS_USB_INTERFACE_HID:
    return "hid";
  case VDS_USB_INTERFACE_AUDIO_OUT:
    return "audio_out";
  case VDS_USB_INTERFACE_AUDIO_IN:
    return "audio_in";
  default:
    return "unknown";
  }
}

std::string run_bluetoothctl_info(const std::string &address) {
  (void)vds::parse_bluetooth_address(address);

  const std::string command = "bluetoothctl info " + address + " 2>/dev/null";
  FILE *pipe = ::popen(command.c_str(), "r");
  if (!pipe) {
    throw std::runtime_error("failed to run bluetoothctl");
  }

  std::string output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    output += buffer.data();
  }

  const int status = ::pclose(pipe);
  if (status != 0 && output.empty()) {
    throw std::runtime_error("bluetoothctl info failed for " + address);
  }
  return output;
}

std::optional<std::string> read_first_line(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }

  std::string line;
  if (!std::getline(file, line)) {
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

bool bluetooth_hid_input_present(const std::string &address) {
  const std::string normalized_address =
      vds::normalize_bluetooth_address(address);
  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator("/sys/bus/hid/devices", error)) {
    if (error) {
      return false;
    }

    const std::string name = entry.path().filename().string();
    if (name.rfind("0005:", 0) != 0) {
      continue;
    }

    const auto uniq = read_first_line(entry.path() / "uniq");
    if (!uniq) {
      continue;
    }

    try {
      if (vds::normalize_bluetooth_address(*uniq) == normalized_address) {
        return true;
      }
    } catch (const std::exception &) {
      continue;
    }
  }
  return false;
}

std::uint32_t identity_from_device_id(const DeviceId &id) {
  if (id.vendor != VDS_SONY_VENDOR_ID) {
    throw std::runtime_error("unsupported controller vendor " +
                             hex16(id.vendor));
  }
  if (id.product == VDS_DS5_PRODUCT_ID) {
    return VDS_IDENTITY_DS5;
  }
  if (id.product == VDS_DSE_PRODUCT_ID) {
    return VDS_IDENTITY_DSE;
  }
  throw std::runtime_error("unsupported Sony controller product " +
                           hex16(id.product));
}

const char *identity_name(std::uint32_t identity) {
  if (identity == VDS_IDENTITY_DSE) {
    return "dualsense-edge";
  }
  return "dualsense";
}

std::uint32_t detect_bluetooth_identity(const std::string &address) {
  const std::string info = run_bluetoothctl_info(address);
  const auto id = parse_device_id(info);
  if (!id) {
    throw std::runtime_error("failed to detect Bluetooth device ID for " +
                             address);
  }
  return identity_from_device_id(*id);
}

std::uint32_t
resolve_binding_identity(const vds::ControllerBindingConfig &binding) {
  switch (binding.identity) {
  case vds::BindingIdentity::Auto:
    return detect_bluetooth_identity(binding.address);
  case vds::BindingIdentity::Ds5:
    return VDS_IDENTITY_DS5;
  case vds::BindingIdentity::Dse:
    return VDS_IDENTITY_DSE;
  }
  throw std::runtime_error("unknown binding identity");
}

int open_control_socket(const std::string &path) {
  vds::UniqueFd fd(
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw std::runtime_error("failed to open control socket: " +
                             std::string(std::strerror(errno)));
  }

  ::unlink(path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("control socket path is too long: " + path);
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  if (::bind(fd.get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) < 0) {
    const int error = errno;
    throw std::runtime_error("failed to bind control socket " + path + ": " +
                             std::strerror(error));
  }
  (void)::chmod(path.c_str(), 0600);

  if (::listen(fd.get(), 4) < 0) {
    const int error = errno;
    throw std::runtime_error("failed to listen on control socket " + path +
                             ": " + std::strerror(error));
  }
  return fd.release();
}

void write_full(int fd, std::string_view text) {
  const char *data = text.data();
  std::size_t size = text.size();
  while (size > 0) {
    const ssize_t written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("write failed: " +
                               std::string(std::strerror(errno)));
    }
    if (written == 0) {
      throw std::runtime_error("write returned zero bytes");
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

bool write_vds_frame(VirtualPort &port, std::span<const std::uint8_t> bytes,
                     bool trace, vds::Logger &logger) {
  while (true) {
    const ssize_t written = ::write(port.fd.get(), bytes.data(), bytes.size());
    if (written == static_cast<ssize_t>(bytes.size())) {
      return true;
    }
    if (written >= 0) {
      throw std::runtime_error("short " + port.path + " frame write");
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOSPC) {
      ++port.trace_state.dropped_usb_frame_count;
      if (trace) {
        if (port.trace_state.dropped_usb_frame_count == 1 ||
            port.trace_state.dropped_usb_frame_count % 1000 == 0) {
          logger.log(
              "port", vds::LogLevel::Warn,
              port.path + " USB frame queue full count=" +
                  std::to_string(port.trace_state.dropped_usb_frame_count));
        }
      }
      return false;
    }
    throw std::runtime_error("write failed: " +
                             std::string(std::strerror(errno)));
  }
}

bool flush_pending_bt_state_report(VirtualPort &port,
                                   vds::BtL2capBackend &bt_backend,
                                   std::uint32_t trace_flags,
                                   vds::Logger &logger) {
  if (!port.pending_bt_state_report || !port.pending_bt_state) {
    return true;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (bt_backend.try_send_output_report(*port.pending_bt_state_report)) {
    port.last_sent_bt_state = *port.pending_bt_state;
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
    if (output_trace) {
      logger.log("hid", vds::LogLevel::Debug,
                 port.path + " flushed pending BT 0x31 state report");
    }
    return true;
  }

  ++port.trace_state.blocked_bt_state_count;
  if (output_trace && (port.trace_state.blocked_bt_state_count == 1 ||
                       port.trace_state.blocked_bt_state_count % 1000 == 0)) {
    logger.log("hid", vds::LogLevel::Warn,
               port.path +
                   " pending BT 0x31 state report still blocked "
                   "count=" +
                   std::to_string(port.trace_state.blocked_bt_state_count));
  }
  return false;
}

void ioctl_noarg(int fd, unsigned long request, const char *name) {
  if (::ioctl(fd, request) < 0) {
    throw std::runtime_error(std::string(name) +
                             " failed: " + std::strerror(errno));
  }
}

void ioctl_set_identity(int fd, std::uint32_t identity) {
  vds_identity_config config{
      .identity = identity,
      .polling_rate_mode = 0,
  };
  if (::ioctl(fd, VDS_IOC_SET_IDENTITY, &config) < 0) {
    throw std::runtime_error("VDS_IOC_SET_IDENTITY failed: " +
                             std::string(std::strerror(errno)));
  }
}

void reset_virtual_port(VirtualPort &port) {
  port.extractor = vds::PcmAudioExtractor{};
  port.haptics_builder = vds::HapticsPacketBuilder{};
  port.output_state = vds::DsOutputState{};
  port.pending_audio_chunks.clear();
  port.last_sent_bt_state.reset();
  port.pending_bt_state.reset();
  port.pending_bt_state_report.reset();
  port.next_haptics_send_time = {};
  port.audio_signal_active = false;
  port.feature_cache = {};
  port.feature_cached = {};
  port.pending_feature_reports.clear();
  port.trace_state = TraceState{};
}

void disconnect_virtual_port(VirtualPort &port, vds::Logger &logger) {
  port.pending_audio_chunks.clear();
  port.next_haptics_send_time = {};
  port.audio_signal_active = false;
  try {
    ioctl_noarg(port.fd.get(), VDS_IOC_DISCONNECT, "VDS_IOC_DISCONNECT");
    logger.log("usb", vds::LogLevel::Info,
               port.path + " virtual USB disconnected");
  } catch (const std::exception &error) {
    logger.log("usb", vds::LogLevel::Warn,
               port.path + " disconnect failed: " + error.what());
  }
}

void handle_frame(const vds_frame_header &header,
                  std::span<const std::uint8_t> payload,
                  vds::BtL2capBackend *bt_backend, std::uint32_t trace_flags,
                  VirtualPort &port, vds::Logger &logger) {
  if (header.type == VDS_FRAME_USB_INTERFACE) {
    if (payload.size() != sizeof(vds_usb_interface_event)) {
      logger.log("usb", vds::LogLevel::Warn,
                 port.path + " malformed USB interface event len=" +
                     std::to_string(payload.size()));
      return;
    }

    vds_usb_interface_event event{};
    std::memcpy(&event, payload.data(), sizeof(event));

    std::ostringstream line;
    line << port.path << " USB set_interface kind="
         << usb_interface_kind_name(event.interface_kind)
         << " number=" << static_cast<unsigned>(event.interface_number)
         << " alt=" << static_cast<unsigned>(event.altsetting);
    if (event.interface_kind == VDS_USB_INTERFACE_AUDIO_OUT ||
        event.interface_kind == VDS_USB_INTERFACE_AUDIO_IN) {
      line << " stream=" << (event.altsetting != 0 ? "on" : "off");
    }
    logger.log("usb", vds::LogLevel::Info, line.str());
    return;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (output_trace) {
    std::ostringstream line;
    line << port.path << " frame " << vds::frame_type_name(header.type)
         << " len=" << header.length << " seq=" << header.sequence;

    bool emit_trace = true;
    if (header.type == VDS_FRAME_USB_AUDIO_OUT &&
        payload.size() >= VDS_AUDIO_CHANNELS * sizeof(std::int16_t)) {
      std::array<int, VDS_AUDIO_CHANNELS> peaks{};
      const std::size_t frame_size = VDS_AUDIO_CHANNELS * sizeof(std::int16_t);
      const std::size_t frames = payload.size() / frame_size;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t *base = payload.data() + frame * frame_size;
        for (std::size_t channel = 0; channel < VDS_AUDIO_CHANNELS; ++channel) {
          const auto low =
              static_cast<std::uint16_t>(base[channel * sizeof(std::int16_t)]);
          const auto high = static_cast<std::uint16_t>(
                                base[channel * sizeof(std::int16_t) + 1])
                            << 8;
          const auto sample = static_cast<std::int16_t>(low | high);
          peaks[channel] =
              std::max(peaks[channel], std::abs(static_cast<int>(sample)));
        }
      }

      ++port.trace_state.audio_usb_frame_count;
      const bool haptics_nonzero = peaks[2] != 0 || peaks[3] != 0;
      emit_trace =
          port.trace_state.audio_usb_frame_count == 1 ||
          port.trace_state.audio_usb_frame_count % 250 == 0 ||
          (haptics_nonzero && !port.trace_state.audio_haptics_nonzero_seen);
      port.trace_state.audio_haptics_nonzero_seen |= haptics_nonzero;
      line << " pcm_peak_ch0=" << peaks[0] << " pcm_peak_ch1=" << peaks[1]
           << " pcm_peak_ch2=" << peaks[2] << " pcm_peak_ch3=" << peaks[3];
    }

    if (emit_trace) {
      logger.log("usb", vds::LogLevel::Debug, line.str());
    }
  }

  if (header.type == VDS_FRAME_USB_HID_OUT) {
    const auto start = Clock::now();
    if (output_trace) {
      trace_hid_out_report(port.path, payload, port.trace_state, logger);
    }
    if (!port.output_state.apply_usb_output_report(payload)) {
      if (output_trace) {
        logger.log("hid", vds::LogLevel::Debug,
                   port.path + " hid out ignored: unsupported output report");
      }
      return;
    }
    if (output_trace) {
      logger.log("hid", vds::LogLevel::Debug,
                 port.path + " hid out accepted for BT state update");
    }

    if (bt_backend) {
      (void)flush_pending_bt_state_report(port, *bt_backend, trace_flags,
                                          logger);

      const vds::DsState state = port.output_state.state();
      if (port.last_sent_bt_state && *port.last_sent_bt_state == state) {
        if (port.pending_bt_state) {
          port.pending_bt_state.reset();
          port.pending_bt_state_report.reset();
          ++port.trace_state.coalesced_bt_state_count;
        }
        if (output_trace) {
          logger.log("hid", vds::LogLevel::Debug,
                     port.path + " hid out skipped: BT state unchanged");
        }
      } else if (port.pending_bt_state && *port.pending_bt_state == state) {
        if (output_trace) {
          logger.log("hid", vds::LogLevel::Debug,
                     port.path + " hid out skipped: BT state already pending");
        }
      } else {
        const auto packet = port.output_state.build_bt_state_report();
        if (bt_backend->try_send_output_report(packet)) {
          port.last_sent_bt_state = state;
          port.pending_bt_state.reset();
          port.pending_bt_state_report.reset();
          if (output_trace) {
            logger.log("hid", vds::LogLevel::Debug,
                       port.path +
                           " hid out forwarded as BT 0x31 state report");
          }
        } else {
          if (port.pending_bt_state) {
            ++port.trace_state.coalesced_bt_state_count;
          }
          ++port.trace_state.deferred_bt_state_count;
          port.pending_bt_state = state;
          port.pending_bt_state_report = packet;
          if (output_trace &&
              (port.trace_state.deferred_bt_state_count == 1 ||
               port.trace_state.deferred_bt_state_count % 1000 == 0)) {
            logger.log(
                "hid", vds::LogLevel::Warn,
                port.path + " deferred BT 0x31 state report count=" +
                    std::to_string(port.trace_state.deferred_bt_state_count) +
                    " coalesced=" +
                    std::to_string(port.trace_state.coalesced_bt_state_count));
          }
        }
      }
    }

    if (output_trace) {
      trace_output_latency(port.path, "hid_out",
                           port.trace_state.output_hid_latency,
                           Clock::now() - start, kOutputTraceSlowWarn, logger);
    }
    return;
  }

  if (header.type == VDS_FRAME_USB_FEATURE_GET) {
    const auto start = Clock::now();
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    const unsigned requested_length =
        payload.size() >= 3
            ? static_cast<unsigned>(payload[1] |
                                    (static_cast<unsigned>(payload[2]) << 8))
            : (payload.size() >= 2 ? static_cast<unsigned>(payload[1]) : 0);
    const bool cache_hit = port.feature_cached[report_id];
    if (output_trace) {
      logger.log("hid", vds::LogLevel::Debug,
                 port.path + " feature get report=" + hex_byte(report_id) +
                     " request_len=" + std::to_string(requested_length) +
                     " cache=" + (cache_hit ? "hit" : "miss") + " forwarded=" +
                     ((bt_backend && !cache_hit) ? "yes" : "no"));
    }
    if (cache_hit) {
      const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                          port.feature_cache[report_id]);
      (void)write_vds_frame(port, frame, output_trace, logger);
      if (output_trace) {
        trace_output_latency(
            port.path, "feature_get", port.trace_state.output_feature_latency,
            Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
      }
      return;
    }
    if (bt_backend) {
      port.pending_feature_reports.push_back(report_id);
      bt_backend->send_feature_get(report_id);
    }
    if (output_trace) {
      trace_output_latency(
          port.path, "feature_get", port.trace_state.output_feature_latency,
          Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
    }
    return;
  }

  if (header.type == VDS_FRAME_USB_FEATURE_SET) {
    const auto start = Clock::now();
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    if (output_trace) {
      logger.log("hid", vds::LogLevel::Debug,
                 port.path + " feature set report=" + hex_byte(report_id) +
                     " len=" + std::to_string(payload.size()) +
                     " forwarded=" + (bt_backend ? "yes" : "no"));
      logger.log("hid", vds::LogLevel::Debug,
                 port.path + " feature set payload=" +
                     hex_bytes(payload, kTraceDumpMaxBytes));
    }
    if (bt_backend) {
      bt_backend->send_feature_set(payload);
    }
    if (output_trace) {
      trace_output_latency(
          port.path, "feature_set", port.trace_state.output_feature_latency,
          Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
    }
    return;
  }

  if (header.type != VDS_FRAME_USB_AUDIO_OUT) {
    return;
  }

  if (!bt_backend) {
    return;
  }

  std::size_t queued_chunks = 0;
  std::size_t skipped_silence_chunks = 0;
  std::size_t dropped_chunks = 0;
  const auto audio_start = Clock::now();
  const auto extract_start = Clock::now();
  const auto chunks = port.extractor.push_usb_audio(payload);
  const auto extract_duration = Clock::now() - extract_start;
  for (const auto &chunk : chunks) {
    if (!chunk.has_signal && !port.audio_signal_active) {
      ++skipped_silence_chunks;
      continue;
    }

    /*
     * USB isochronous URBs can arrive in bursts. Queue completed 0x36 audio
     * chunks here and let flush_pending_outputs() send them at the controller
     * cadence, otherwise speaker/haptics audio turns into audible bursts.
     */
    if (port.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
      port.pending_audio_chunks.pop_front();
      ++dropped_chunks;
      ++port.trace_state.dropped_audio_haptics_count;
      ++port.trace_state.queue_dropped_audio_haptics_count;
    }

    port.pending_audio_chunks.push_back(chunk);
    ++queued_chunks;
  }
  if (dropped_chunks > 0 &&
      (port.trace_state.dropped_audio_haptics_count == dropped_chunks ||
       port.trace_state.dropped_audio_haptics_count % 1000 == 0)) {
    logger.log(
        "audio", vds::LogLevel::Warn,
        port.path + " dropped BT 0x36 haptics packets count=" +
            std::to_string(port.trace_state.dropped_audio_haptics_count) +
            " queue=" +
            std::to_string(port.trace_state.queue_dropped_audio_haptics_count) +
            " blocked=" +
            std::to_string(port.trace_state.blocked_audio_haptics_count));
  }
  if (output_trace && queued_chunks > 0) {
    logger.log(
        "audio", vds::LogLevel::Debug,
        port.path + " audio out queued " + std::to_string(queued_chunks) +
            " BT 0x36 haptics packets pending=" +
            std::to_string(port.pending_audio_chunks.size()) +
            " extract_encode_us=" +
            std::to_string(duration_us(extract_duration)) + " total_us=" +
            std::to_string(duration_us(Clock::now() - audio_start)));
  }
  if (skipped_silence_chunks > 0) {
    port.trace_state.skipped_silent_audio_count += skipped_silence_chunks;
    if (output_trace &&
        (port.trace_state.skipped_silent_audio_count ==
             skipped_silence_chunks ||
         port.trace_state.skipped_silent_audio_count % 1000 == 0)) {
      logger.log(
          "audio", vds::LogLevel::Debug,
          port.path + " skipped silent BT 0x36 haptics packets count=" +
              std::to_string(port.trace_state.skipped_silent_audio_count));
    }
  }
  if (output_trace) {
    trace_output_latency(
        port.path, "audio_out", port.trace_state.output_audio_latency,
        Clock::now() - audio_start, kOutputTraceSlowWarn, logger);
    trace_output_latency(port.path, "audio_extract_encode",
                         port.trace_state.output_audio_extract_latency,
                         extract_duration, kOutputTraceSlowWarn, logger);
  }
}

bool handle_vds_frame(VirtualPort &port, vds::BtL2capBackend *bt_backend,
                      std::uint32_t trace_flags, vds::Logger &logger) {
  std::array<std::uint8_t, sizeof(vds_frame_header) + VDS_FRAME_MAX_PAYLOAD>
      frame;
  const ssize_t got = ::read(port.fd.get(), frame.data(), frame.size());
  if (got == 0) {
    throw std::runtime_error(port.path + " closed");
  }
  if (got < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return false;
    }
    throw std::runtime_error(
        port.path + " read failed: " + std::string(std::strerror(errno)));
  }
  if (static_cast<std::size_t>(got) < sizeof(vds_frame_header)) {
    throw std::runtime_error(port.path + " short VDS frame header");
  }
  vds_frame_header header{};
  std::memcpy(&header, frame.data(), sizeof(header));
  if (header.length > VDS_FRAME_MAX_PAYLOAD) {
    throw std::runtime_error(port.path + " oversized VDS frame");
  }
  if (static_cast<std::size_t>(got) != sizeof(header) + header.length) {
    throw std::runtime_error(port.path + " short VDS frame payload");
  }
  const std::span payload(frame.data() + sizeof(header), header.length);
  handle_frame(header, payload, bt_backend, trace_flags, port, logger);
  return true;
}

bool handle_bt_input(VirtualPort &port, vds::BtL2capBackend &bt_backend,
                     std::uint32_t trace_flags, vds::Logger &logger) {
  const auto read_time = Clock::now();
  const auto report = bt_backend.read_input_report();
  if (!report) {
    return false;
  }
  const bool input_trace = trace_enabled(trace_flags, kTraceInput);
  if (input_trace) {
    ++port.trace_state.bt_input_count;
    if (port.trace_state.have_last_input_time) {
      const auto gap = read_time - port.trace_state.last_input_time;
      const auto gap_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(gap).count());
      port.trace_state.input_gap_max_us =
          std::max(port.trace_state.input_gap_max_us, gap_us);
      if (gap >= kInputTraceGapWarn) {
        logger.log("input", vds::LogLevel::Warn,
                   port.path + " input gap count=" +
                       std::to_string(port.trace_state.bt_input_count) +
                       " gap_us=" + std::to_string(gap_us));
      }
    }
    port.trace_state.last_input_time = read_time;
    port.trace_state.have_last_input_time = true;

    const std::span buttons(report->data() + kUsbInputButtonsOffset,
                            kUsbInputButtonsSize);
    const bool buttons_changed =
        !port.trace_state.have_last_input_buttons ||
        !std::equal(buttons.begin(), buttons.end(),
                    port.trace_state.last_input_buttons.begin());
    if (buttons_changed) {
      std::copy(buttons.begin(), buttons.end(),
                port.trace_state.last_input_buttons.begin());
      port.trace_state.have_last_input_buttons = true;
      logger.log("input", vds::LogLevel::Debug,
                 port.path + " buttons changed raw=" +
                     hex_bytes(buttons, kUsbInputButtonsSize));
    }
  }

  vds_frame_header header{};
  header.type = VDS_FRAME_USB_HID_IN;
  header.length = static_cast<std::uint32_t>(report->size());

  std::array<std::uint8_t, sizeof(header) + vds::kUsbInputReportSize> frame{};
  std::memcpy(frame.data(), &header, sizeof(header));
  std::memcpy(frame.data() + sizeof(header), report->data(), report->size());
  const std::span bytes(frame.data(), sizeof(header) + report->size());

  const auto write_start = Clock::now();
  const bool wrote = write_vds_frame(port, bytes, input_trace, logger);
  const auto write_duration = Clock::now() - write_start;
  if (input_trace) {
    const auto write_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(write_duration)
            .count());
    ++port.trace_state.input_write_count;
    port.trace_state.input_write_total_us += write_us;
    port.trace_state.input_write_max_us =
        std::max(port.trace_state.input_write_max_us, write_us);

    if (write_duration >= kInputTraceSlowWriteWarn) {
      logger.log("input", vds::LogLevel::Warn,
                 port.path + " slow input write count=" +
                     std::to_string(port.trace_state.bt_input_count) +
                     " write_us=" + std::to_string(write_us) +
                     " written=" + (wrote ? "yes" : "no"));
    }
    if (port.trace_state.bt_input_count == 1 ||
        port.trace_state.bt_input_count % kInputTraceSummaryInterval == 0) {
      const std::uint64_t avg_write_us =
          port.trace_state.input_write_count == 0
              ? 0
              : port.trace_state.input_write_total_us /
                    port.trace_state.input_write_count;
      logger.log("input", vds::LogLevel::Debug,
                 port.path + " input summary count=" +
                     std::to_string(port.trace_state.bt_input_count) +
                     " len=" + std::to_string(report->size()) +
                     " avg_write_us=" + std::to_string(avg_write_us) +
                     " max_write_us=" +
                     std::to_string(port.trace_state.input_write_max_us) +
                     " max_gap_us=" +
                     std::to_string(port.trace_state.input_gap_max_us));
    }
  }
  return true;
}

bool handle_bt_control(VirtualPort &port, vds::BtL2capBackend &bt_backend,
                       std::uint32_t trace_flags, vds::Logger &logger) {
  const auto report = bt_backend.read_feature_report();
  if (!report) {
    return false;
  }
  if (report->empty()) {
    return true;
  }

  const std::uint8_t report_id = (*report)[0];
  port.feature_cache[report_id] = *report;
  port.feature_cached[report_id] = true;

  const auto pending = std::find(port.pending_feature_reports.begin(),
                                 port.pending_feature_reports.end(), report_id);
  const bool usb_waiting = pending != port.pending_feature_reports.end();
  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (output_trace) {
    logger.log("hid", vds::LogLevel::Debug,
               port.path + " bt feature cached report=" + hex_byte(report_id) +
                   " len=" + std::to_string(report->size()) +
                   " usb_waiting=" + (usb_waiting ? "yes" : "no"));
  }
  if (!usb_waiting) {
    return true;
  }

  port.pending_feature_reports.erase(pending);
  const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY, *report);
  (void)write_vds_frame(port, frame, output_trace, logger);
  return true;
}

bool is_bluetooth_error(const std::exception &error) {
  return std::string_view(error.what()).rfind("Bluetooth L2CAP", 0) == 0 ||
         std::string_view(error.what())
                 .rfind("failed to connect Bluetooth", 0) == 0;
}

void cache_feature_report(VirtualPort &port,
                          std::span<const std::uint8_t> report, bool trace,
                          vds::Logger &logger) {
  if (report.empty()) {
    return;
  }

  const std::uint8_t report_id = report[0];
  port.feature_cache[report_id] =
      std::vector<std::uint8_t>(report.begin(), report.end());
  port.feature_cached[report_id] = true;

  const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                      port.feature_cache[report_id]);
  (void)write_vds_frame(port, frame, trace, logger);
}

void initialize_bt_controller(vds::BtL2capBackend &bt_backend,
                              VirtualPort &port, vds::Logger &logger) {
  for (const std::uint8_t report_id : kInitialFeatureReportIds) {
    bt_backend.send_feature_get(report_id);

    while (true) {
      pollfd pfd{.fd = bt_backend.control_fd(), .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, kInitialFeatureReportPollMs);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        throw std::runtime_error("timed out waiting for initial Bluetooth "
                                 "feature report " +
                                 hex_byte(report_id));
      }
      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("Bluetooth control channel closed during "
                                 "initial feature report fetch");
      }
      if ((pfd.revents & POLLIN) == 0) {
        continue;
      }

      const auto report = bt_backend.read_feature_report();
      if (!report || report->empty()) {
        continue;
      }

      cache_feature_report(port, *report, false, logger);
      if ((*report)[0] == report_id) {
        break;
      }
    }
  }

  logger.log("hid", vds::LogLevel::Info,
             port.path + " primed initial Bluetooth feature cache");
}

bool binding_allows_device(const vds::ControllerBindingConfig &binding,
                           const std::string &device) {
  if (binding.limit_devices.empty()) {
    return true;
  }
  return std::find(binding.limit_devices.begin(), binding.limit_devices.end(),
                   device) != binding.limit_devices.end();
}

std::optional<std::size_t> find_port_index(std::span<const VirtualPort> ports,
                                           const std::string &path) {
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (ports[i].path == path) {
      return i;
    }
  }
  return std::nullopt;
}

bool binding_uses_port(const ControllerBinding &binding) {
  return binding.backend || binding.pending_control_fd ||
         binding.pending_interrupt_fd || binding.virtual_connected;
}

bool port_used_by_other_binding(std::span<const ControllerBinding> bindings,
                                const ControllerBinding &self,
                                const std::string &device) {
  for (const auto &binding : bindings) {
    if (&binding == &self) {
      continue;
    }
    if (binding.device == device && binding_uses_port(binding)) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t>
available_port_index(std::span<const VirtualPort> ports,
                     std::span<const ControllerBinding> bindings,
                     const ControllerBinding &binding) {
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (!binding_allows_device(binding.config, ports[i].path)) {
      continue;
    }
    if (port_used_by_other_binding(bindings, binding, ports[i].path)) {
      continue;
    }
    return i;
  }
  return std::nullopt;
}

ControllerBinding *binding_for_port(std::vector<ControllerBinding> &bindings,
                                    const std::string &device) {
  for (auto &binding : bindings) {
    if (binding.device == device && binding_uses_port(binding)) {
      return &binding;
    }
  }
  return nullptr;
}

ControllerBinding *binding_for_address(std::vector<ControllerBinding> &bindings,
                                       const std::string &address) {
  for (auto &binding : bindings) {
    if (binding.config.address == address) {
      return &binding;
    }
  }
  return nullptr;
}

void drop_bt_backend(ControllerBinding &binding, VirtualPort &port,
                     const std::string &reason, vds::Logger &logger) {
  if (!binding.backend && !binding.virtual_connected) {
    return;
  }

  port.pending_bt_state.reset();
  port.pending_bt_state_report.reset();
  logger.log("bluetooth", vds::LogLevel::Warn,
             "backend disconnected address=" + binding.config.address +
                 " device=" + binding.device + " reason=" + reason);
  binding.backend.reset();
  binding.pending_control_fd.reset();
  binding.pending_interrupt_fd.reset();
  binding.virtual_connected = false;
  binding.device.clear();
  disconnect_virtual_port(port, logger);
}

void sync_virtual_ports(std::vector<VirtualPort> &ports,
                        std::span<const std::string> devices,
                        vds::Logger &logger) {
  std::vector<bool> moved(ports.size(), false);
  std::vector<VirtualPort> updated;
  updated.reserve(devices.size());

  for (const auto &device : devices) {
    auto it =
        std::find_if(ports.begin(), ports.end(), [&](const VirtualPort &port) {
          return port.path == device;
        });
    if (it != ports.end()) {
      const std::size_t old_index =
          static_cast<std::size_t>(std::distance(ports.begin(), it));
      moved[old_index] = true;
      updated.push_back(std::move(*it));
      continue;
    }

    vds::UniqueFd fd(open_required(device, O_RDWR | O_NONBLOCK | O_CLOEXEC));
    logger.log("port", vds::LogLevel::Info, "opened " + device);
    updated.push_back(VirtualPort{
        .path = device,
        .fd = std::move(fd),
        .extractor = {},
        .haptics_builder = {},
        .output_state = {},
        .pending_audio_chunks = {},
        .last_sent_bt_state = std::nullopt,
        .pending_bt_state = std::nullopt,
        .pending_bt_state_report = std::nullopt,
        .next_haptics_send_time = {},
        .audio_signal_active = false,
        .feature_cache = {},
        .feature_cached = {},
        .pending_feature_reports = {},
        .trace_state = {},
    });
  }

  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (!moved[i]) {
      logger.log("port", vds::LogLevel::Info, "closed " + ports[i].path);
    }
  }
  if (updated.empty()) {
    logger.log("port", vds::LogLevel::Warn, "no /dev/vds* endpoints found");
  }
  ports = std::move(updated);
}

void preempt_default_bluetooth_owner(const std::string &address,
                                     vds::Logger &logger) {
  if (!bluetooth_hid_input_present(address)) {
    return;
  }

  logger.log("bluetooth", vds::LogLevel::Warn,
             "preempting default Bluetooth HID owner address=" + address);
  const std::string command =
      "bluetoothctl disconnect " + address + " >/dev/null 2>&1";
  if (std::system(command.c_str()) != 0) {
    logger.log("bluetooth", vds::LogLevel::Warn,
               "bluetoothctl disconnect failed address=" + address);
  }

  const auto deadline = Clock::now() + kBluetoothPreemptWait;
  while (Clock::now() < deadline) {
    if (!bluetooth_hid_input_present(address)) {
      logger.log("bluetooth", vds::LogLevel::Info,
                 "default stack released address=" + address);
      return;
    }
    std::this_thread::sleep_for(kBluetoothPreemptPoll);
  }

  logger.log("bluetooth", vds::LogLevel::Warn,
             "default Bluetooth HID owner still present address=" + address);
}

void complete_pending_binding(ControllerBinding &binding, VirtualPort &port,
                              vds::Logger &logger) {
  const std::uint32_t identity = resolve_binding_identity(binding.config);
  preempt_default_bluetooth_owner(binding.config.address, logger);

  vds::BtL2capBackend candidate(binding.config.address,
                                std::move(binding.pending_control_fd),
                                std::move(binding.pending_interrupt_fd));

  reset_virtual_port(port);
  ioctl_noarg(port.fd.get(), VDS_IOC_DISCONNECT, "VDS_IOC_DISCONNECT");
  ioctl_set_identity(port.fd.get(), identity);
  initialize_bt_controller(candidate, port, logger);
  candidate.send_output_report(port.output_state.build_bt_init_report());
  port.last_sent_bt_state = port.output_state.state();
  logger.log("hid", vds::LogLevel::Info,
             port.path + " sent initial Bluetooth state report");

  binding.backend = std::move(candidate);
  binding.detected_identity = identity;
  ioctl_noarg(port.fd.get(), VDS_IOC_CONNECT, "VDS_IOC_CONNECT");
  binding.virtual_connected = true;
  binding.last_error.clear();

  logger.log("bluetooth", vds::LogLevel::Info,
             "raw L2CAP backend connected address=" + binding.config.address +
                 " device=" + port.path +
                 " identity=" + identity_name(identity));
}

void handle_bt_accept(std::vector<VirtualPort> &ports,
                      std::vector<ControllerBinding> &bindings,
                      vds::BtL2capAcceptor &acceptor, bool control_channel,
                      vds::Logger &logger, bool &epoll_dirty) {
  while (true) {
    auto accepted = control_channel ? acceptor.accept_control()
                                    : acceptor.accept_interrupt();
    if (!accepted) {
      return;
    }

    ControllerBinding *binding =
        binding_for_address(bindings, accepted->address);
    if (!binding) {
      logger.log("bluetooth", vds::LogLevel::Warn,
                 "rejected raw HID channel from unregistered address=" +
                     accepted->address);
      continue;
    }
    if (binding->backend) {
      logger.log("bluetooth", vds::LogLevel::Warn,
                 "rejected duplicate raw HID channel address=" +
                     accepted->address);
      continue;
    }

    std::optional<std::size_t> port_index;
    if (!binding->device.empty()) {
      port_index = find_port_index(ports, binding->device);
      if (port_index &&
          port_used_by_other_binding(bindings, *binding, binding->device)) {
        port_index.reset();
      }
    }
    if (!port_index) {
      port_index = available_port_index(ports, bindings, *binding);
      if (!port_index) {
        binding->last_error = "no available virtual port";
        logger.log("port", vds::LogLevel::Warn,
                   "rejected raw HID channel address=" + accepted->address +
                       " reason=no available virtual port");
        continue;
      }
      binding->device = ports[*port_index].path;
      logger.log("config", vds::LogLevel::Info,
                 "binding assigned address=" + binding->config.address +
                     " device=" + binding->device + " identity=" +
                     vds::binding_identity_name(binding->config.identity));
    }

    if (control_channel) {
      binding->pending_control_fd = std::move(accepted->fd);
      logger.log("bluetooth", vds::LogLevel::Info,
                 "accepted raw HID control channel address=" +
                     accepted->address);
    } else {
      binding->pending_interrupt_fd = std::move(accepted->fd);
      logger.log("bluetooth", vds::LogLevel::Info,
                 "accepted raw HID interrupt channel address=" +
                     accepted->address);
    }

    if (!binding->pending_control_fd || !binding->pending_interrupt_fd) {
      continue;
    }

    port_index = find_port_index(ports, binding->device);
    if (!port_index) {
      binding->pending_control_fd.reset();
      binding->pending_interrupt_fd.reset();
      binding->device.clear();
      binding->last_error = "assigned virtual port disappeared";
      logger.log("port", vds::LogLevel::Error,
                 "binding inactive address=" + binding->config.address +
                     " reason=assigned port disappeared");
      continue;
    }

    try {
      complete_pending_binding(*binding, ports[*port_index], logger);
      epoll_dirty = true;
    } catch (const std::exception &error) {
      binding->backend.reset();
      binding->pending_control_fd.reset();
      binding->pending_interrupt_fd.reset();
      binding->virtual_connected = false;
      binding->device.clear();
      binding->last_error = error.what();
      logger.log("bluetooth", vds::LogLevel::Error,
                 "connect failed address=" + binding->config.address +
                     " device=" + ports[*port_index].path +
                     " error=" + error.what());
    }
  }
}

bool has_pending_bt_output(std::span<const VirtualPort> ports,
                           std::span<const ControllerBinding> bindings) {
  for (const auto &binding : bindings) {
    if (!binding.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, binding.device);
    if (port_index && (ports[*port_index].pending_bt_state_report ||
                       !ports[*port_index].pending_audio_chunks.empty())) {
      return true;
    }
  }
  return false;
}

int next_wakeup_timeout_ms(Clock::time_point next_port_scan,
                           std::span<const VirtualPort> ports,
                           std::span<const ControllerBinding> bindings) {
  const auto now = Clock::now();
  if (next_port_scan <= now) {
    return 0;
  }
  const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
      next_port_scan - now);
  int timeout_ms =
      static_cast<int>(std::min<std::int64_t>(wait.count(), 60000));
  if (has_pending_bt_output(ports, bindings)) {
    timeout_ms = std::min(timeout_ms, kPendingOutputPollMs);
  }
  for (const auto &binding : bindings) {
    if (!binding.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, binding.device);
    if (!port_index || ports[*port_index].pending_audio_chunks.empty()) {
      continue;
    }

    const auto next_time = ports[*port_index].next_haptics_send_time;
    if (next_time == Clock::time_point{} || next_time <= now) {
      return 0;
    }
    const auto audio_wait =
        std::chrono::ceil<std::chrono::milliseconds>(next_time - now);
    timeout_ms = std::min(timeout_ms, static_cast<int>(audio_wait.count()));
  }
  return timeout_ms;
}

bool flush_pending_audio_chunk(VirtualPort &port,
                               vds::BtL2capBackend &bt_backend,
                               std::uint32_t trace_flags, vds::Logger &logger) {
  if (port.pending_audio_chunks.empty()) {
    return true;
  }

  const auto now = Clock::now();
  if (port.next_haptics_send_time != Clock::time_point{} &&
      now < port.next_haptics_send_time) {
    return true;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  const auto &chunk = port.pending_audio_chunks.front();
  const auto packet = port.haptics_builder.build_packet(
      chunk.haptics, chunk.speaker, port.output_state.state());
  const auto send_start = Clock::now();
  if (!bt_backend.try_send_output_report(packet)) {
    ++port.trace_state.dropped_audio_haptics_count;
    ++port.trace_state.blocked_audio_haptics_count;
    port.next_haptics_send_time = Clock::now() + kHapticsOutputBlockedRetry;
    if (output_trace &&
        (port.trace_state.blocked_audio_haptics_count == 1 ||
         port.trace_state.blocked_audio_haptics_count % 1000 == 0)) {
      logger.log(
          "audio", vds::LogLevel::Warn,
          port.path + " pending BT 0x36 haptics packet still blocked count=" +
              std::to_string(port.trace_state.blocked_audio_haptics_count));
    }
    return false;
  }

  const auto send_duration = Clock::now() - send_start;
  port.pending_audio_chunks.pop_front();
  port.audio_signal_active = chunk.has_signal;
  port.last_sent_bt_state = port.output_state.state();
  if (port.pending_bt_state &&
      *port.pending_bt_state == *port.last_sent_bt_state) {
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
  }
  port.next_haptics_send_time = now + kAudioOutputInterval;

  if (output_trace) {
    trace_output_latency(port.path, "audio_bt_send",
                         port.trace_state.output_audio_send_latency,
                         send_duration, kOutputTraceSlowWarn, logger);
  }
  return true;
}

void flush_pending_outputs(std::vector<VirtualPort> &ports,
                           std::vector<ControllerBinding> &bindings,
                           std::uint32_t trace_flags, vds::Logger &logger,
                           bool &epoll_dirty) {
  for (auto &binding : bindings) {
    if (!binding.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, binding.device);
    if (!port_index) {
      binding.backend.reset();
      binding.pending_control_fd.reset();
      binding.pending_interrupt_fd.reset();
      binding.virtual_connected = false;
      binding.device.clear();
      epoll_dirty = true;
      continue;
    }

    try {
      auto &port = ports[*port_index];
      if (!flush_pending_bt_state_report(port, *binding.backend, trace_flags,
                                         logger)) {
        continue;
      }
      (void)flush_pending_audio_chunk(port, *binding.backend, trace_flags,
                                      logger);
    } catch (const std::exception &error) {
      if (!is_bluetooth_error(error)) {
        throw;
      }
      drop_bt_backend(binding, ports[*port_index], error.what(), logger);
      epoll_dirty = true;
    }
  }
}

void reconcile_bindings(std::vector<VirtualPort> &ports,
                        std::vector<ControllerBinding> &bindings,
                        vds::Logger &logger) {
  const std::vector<std::string> devices = vds::discover_vds_devices();
  for (auto &binding : bindings) {
    if (!binding_uses_port(binding) ||
        std::binary_search(devices.begin(), devices.end(), binding.device)) {
      continue;
    }
    if (const auto port_index = find_port_index(ports, binding.device)) {
      disconnect_virtual_port(ports[*port_index], logger);
    }
    binding.backend.reset();
    binding.pending_control_fd.reset();
    binding.pending_interrupt_fd.reset();
    binding.virtual_connected = false;
    binding.device.clear();
  }

  sync_virtual_ports(ports, devices, logger);
  const vds::BindingDb db = vds::load_binding_db(vds::kDefaultBindingDbPath);
  logger.log("config", vds::LogLevel::Info,
             "loaded bindings count=" + std::to_string(db.controllers.size()));

  std::vector<bool> preserved(bindings.size(), false);
  std::vector<std::string> active_devices;
  std::vector<ControllerBinding> next_bindings;
  next_bindings.reserve(db.controllers.size());

  for (const auto &config : db.controllers) {
    preempt_default_bluetooth_owner(config.address, logger);
    ControllerBinding next{
        .config = config,
        .device = {},
        .detected_identity = std::nullopt,
        .backend = std::nullopt,
        .pending_control_fd = {},
        .pending_interrupt_fd = {},
        .virtual_connected = false,
        .last_error = {},
    };

    for (std::size_t old_index = 0; old_index < bindings.size(); ++old_index) {
      auto &old = bindings[old_index];
      if (preserved[old_index] || old.config.address != config.address ||
          old.config.identity != config.identity) {
        continue;
      }
      const bool old_uses_port = binding_uses_port(old);
      const bool old_device_present =
          !old.device.empty() && find_port_index(ports, old.device);
      const bool old_device_allowed =
          old_device_present && binding_allows_device(config, old.device);
      const bool old_device_available =
          old_device_allowed &&
          std::find(active_devices.begin(), active_devices.end(), old.device) ==
              active_devices.end();

      next.detected_identity = old.detected_identity;
      next.last_error = std::move(old.last_error);
      if (old_uses_port && old_device_available) {
        next.device = std::move(old.device);
        next.backend = std::move(old.backend);
        next.pending_control_fd = std::move(old.pending_control_fd);
        next.pending_interrupt_fd = std::move(old.pending_interrupt_fd);
        next.virtual_connected = old.virtual_connected;
        active_devices.push_back(next.device);
      } else if (old_uses_port) {
        if (old.virtual_connected && old_device_present) {
          const auto old_port_index = find_port_index(ports, old.device);
          disconnect_virtual_port(ports[*old_port_index], logger);
        }
        if (old.backend) {
          logger.log("bluetooth", vds::LogLevel::Info,
                     "raw backend removed address=" + old.config.address +
                         " device=" + old.device);
        }
        old.backend.reset();
        old.pending_control_fd.reset();
        old.pending_interrupt_fd.reset();
        old.virtual_connected = false;
        old.device.clear();
      }
      preserved[old_index] = true;
      break;
    }

    logger.log("config", vds::LogLevel::Info,
               "binding registered address=" + config.address +
                   " identity=" + vds::binding_identity_name(config.identity));
    next_bindings.push_back(std::move(next));
  }

  for (std::size_t i = 0; i < bindings.size(); ++i) {
    if (preserved[i]) {
      continue;
    }
    if (bindings[i].virtual_connected) {
      if (const auto port_index = find_port_index(ports, bindings[i].device)) {
        disconnect_virtual_port(ports[*port_index], logger);
      }
    }
    if (bindings[i].backend) {
      logger.log("bluetooth", vds::LogLevel::Info,
                 "raw backend removed address=" + bindings[i].config.address +
                     " device=" + bindings[i].device);
    }
  }

  bindings = std::move(next_bindings);
}

std::string trim_command(std::string command) {
  while (!command.empty() &&
         (command.back() == '\n' || command.back() == '\r' ||
          command.back() == ' ' || command.back() == '\t')) {
    command.pop_back();
  }
  return command;
}

void handle_control_client(int control_fd,
                           std::span<const ControllerBinding> bindings,
                           std::uint32_t &trace_flags, bool &reload_requested,
                           vds::Logger &logger) {
  vds::UniqueFd client_fd(
      ::accept4(control_fd, nullptr, nullptr, SOCK_CLOEXEC));
  if (!client_fd) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return;
    }
    throw std::runtime_error("accept control client failed: " +
                             std::string(std::strerror(errno)));
  }

  std::array<char, 256> buffer{};
  ssize_t got = 0;
  do {
    got = ::read(client_fd.get(), buffer.data(), buffer.size() - 1);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    throw std::runtime_error("read control client failed: " +
                             std::string(std::strerror(errno)));
  }
  if (got <= 0) {
    return;
  }

  std::string reply;
  const std::string command =
      trim_command(std::string(buffer.data(), static_cast<std::size_t>(got)));
  try {
    if (command == "RELOAD") {
      reload_requested = true;
      logger.log("config", vds::LogLevel::Info, "reload requested");
      reply = "OK reloaded\n";
    } else if (command == "LIST") {
      reply = "OK\n";
      for (const auto &binding : bindings) {
        reply += binding.config.address;
        reply += binding.virtual_connected ? " connected\n" : " disconnected\n";
      }
    } else if (command.rfind("TRACE ", 0) == 0) {
      std::istringstream fields(command);
      std::string verb;
      std::string action;
      std::string scope = "all";
      std::string extra;
      if (!(fields >> verb >> action) || verb != "TRACE") {
        throw std::runtime_error("malformed trace command");
      }
      if (fields >> scope) {
        if (fields >> extra) {
          throw std::runtime_error("malformed trace command");
        }
      }

      const std::uint32_t scope_mask = parse_trace_scope(scope);
      if (action == "ON") {
        trace_flags |= scope_mask;
      } else if (action == "OFF") {
        trace_flags &= ~scope_mask;
      } else {
        throw std::runtime_error("trace requires ON or OFF");
      }

      logger.log("control", vds::LogLevel::Info,
                 "trace " + trace_scope_name(scope_mask) + " " +
                     (action == "ON" ? "enabled" : "disabled") +
                     " active=" + active_trace_name(trace_flags));
      reply = "OK trace=" + std::string(action == "ON" ? "on" : "off") +
              " scope=" + trace_scope_name(scope_mask) +
              " active=" + active_trace_name(trace_flags) + "\n";
    } else {
      logger.log("control", vds::LogLevel::Warn, "unknown command: " + command);
      reply = "ERR unknown command\n";
    }
  } catch (const std::exception &error) {
    reply = std::string("ERR ") + error.what() + "\n";
  }

  try {
    write_full(client_fd.get(), reply);
  } catch (const std::exception &error) {
    if (trace_flags != 0) {
      logger.log("control", vds::LogLevel::Error,
                 std::string("reply failed: ") + error.what());
    }
  }
}

std::uint64_t encode_event(EventKind kind, std::size_t index) {
  return (static_cast<std::uint64_t>(kind) << 32) |
         static_cast<std::uint32_t>(index);
}

EventSource decode_event(std::uint64_t data) {
  return EventSource{
      .kind = static_cast<EventKind>(data >> 32),
      .index = static_cast<std::uint32_t>(data),
  };
}

void add_epoll_fd(int epoll_fd, int fd, EventKind kind, std::size_t index) {
  epoll_event event{};
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  event.data.u64 = encode_event(kind, index);
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    throw std::runtime_error("epoll add failed: " +
                             std::string(std::strerror(errno)));
  }
}

vds::UniqueFd rebuild_epoll(int control_fd, vds::BtL2capAcceptor &bt_acceptor,
                            std::span<VirtualPort> ports,
                            std::span<ControllerBinding> bindings) {
  vds::UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd) {
    throw std::runtime_error("epoll_create1 failed: " +
                             std::string(std::strerror(errno)));
  }

  add_epoll_fd(epoll_fd.get(), control_fd, EventKind::Control, 0);
  add_epoll_fd(epoll_fd.get(), bt_acceptor.control_listener_fd(),
               EventKind::BtAcceptControl, 0);
  add_epoll_fd(epoll_fd.get(), bt_acceptor.interrupt_listener_fd(),
               EventKind::BtAcceptInterrupt, 0);
  for (std::size_t i = 0; i < ports.size(); ++i) {
    add_epoll_fd(epoll_fd.get(), ports[i].fd.get(), EventKind::Port, i);
  }
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    if (!bindings[i].backend) {
      continue;
    }
    add_epoll_fd(epoll_fd.get(), bindings[i].backend->control_fd(),
                 EventKind::BtControl, i);
    add_epoll_fd(epoll_fd.get(), bindings[i].backend->interrupt_fd(),
                 EventKind::BtInterrupt, i);
  }
  return epoll_fd;
}

void disconnect_all(std::vector<VirtualPort> &ports,
                    std::vector<ControllerBinding> &bindings,
                    vds::Logger &logger) {
  for (auto &binding : bindings) {
    if (!binding.virtual_connected) {
      continue;
    }
    if (const auto port_index = find_port_index(ports, binding.device)) {
      disconnect_virtual_port(ports[*port_index], logger);
    }
    binding.virtual_connected = false;
  }
  bindings.clear();
}

} // namespace

int main(int argc, char **argv) {
  try {
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);

    const Options options = parse_args(argc, argv);
    vds::Logger logger(options.log_path);
    logger.log("daemon", vds::LogLevel::Info,
               "started socket=" + options.socket + " log=" + options.log_path);

    vds::UniqueFd control_fd(open_control_socket(options.socket));
    vds::BtL2capAcceptor bt_acceptor;
    logger.log("bluetooth", vds::LogLevel::Info,
               "listening for controller-initiated raw HID channels");
    SocketPathGuard control_socket_path(options.socket);
    std::vector<VirtualPort> ports;
    std::vector<ControllerBinding> bindings;
    vds::UniqueFd epoll_fd;
    std::uint32_t trace_flags = 0;
    bool reload_requested = true;
    bool epoll_dirty = true;
    auto next_port_scan = Clock::now() + kPortScanInterval;

    while (g_stop_requested == 0) {
      if (!reload_requested && Clock::now() >= next_port_scan) {
        const std::vector<std::string> discovered = vds::discover_vds_devices();
        std::vector<std::string> opened;
        opened.reserve(ports.size());
        for (const auto &port : ports) {
          opened.push_back(port.path);
        }
        if (discovered != opened) {
          logger.log("port", vds::LogLevel::Info,
                     "virtual endpoint set changed; reloading");
          reload_requested = true;
        }
        next_port_scan = Clock::now() + kPortScanInterval;
      }

      if (reload_requested) {
        try {
          reconcile_bindings(ports, bindings, logger);
          epoll_dirty = true;
          next_port_scan = Clock::now() + kPortScanInterval;
        } catch (const std::exception &error) {
          logger.log("config", vds::LogLevel::Error,
                     std::string("reload failed: ") + error.what());
        }
        reload_requested = false;
      }

      if (epoll_dirty || !epoll_fd) {
        epoll_fd =
            rebuild_epoll(control_fd.get(), bt_acceptor, ports, bindings);
        epoll_dirty = false;
      }

      std::array<epoll_event, 64> events{};
      const int timeout_ms =
          next_wakeup_timeout_ms(next_port_scan, ports, bindings);
      const int ready =
          ::epoll_wait(epoll_fd.get(), events.data(),
                       static_cast<int>(events.size()), timeout_ms);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("epoll_wait failed: " +
                                 std::string(std::strerror(errno)));
      }
      if (ready == 0) {
        flush_pending_outputs(ports, bindings, trace_flags, logger,
                              epoll_dirty);
        continue;
      }

      for (int i = 0; i < ready; ++i) {
        const EventSource source = decode_event(events[i].data.u64);
        const std::uint32_t revents = events[i].events;

        if (source.kind == EventKind::Control) {
          if ((revents & EPOLLIN) != 0) {
            handle_control_client(control_fd.get(), bindings, trace_flags,
                                  reload_requested, logger);
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            throw std::runtime_error("control socket epoll error");
          }
          continue;
        }

        if (source.kind == EventKind::BtAcceptControl ||
            source.kind == EventKind::BtAcceptInterrupt) {
          if ((revents & EPOLLIN) != 0) {
            handle_bt_accept(ports, bindings, bt_acceptor,
                             source.kind == EventKind::BtAcceptControl, logger,
                             epoll_dirty);
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            throw std::runtime_error("Bluetooth listener epoll error");
          }
          continue;
        }

        if (source.kind == EventKind::Port) {
          if (source.index >= ports.size()) {
            continue;
          }
          auto &port = ports[source.index];
          ControllerBinding *binding = binding_for_port(bindings, port.path);
          vds::BtL2capBackend *backend =
              binding && binding->backend ? &*binding->backend : nullptr;
          if ((revents & EPOLLIN) != 0) {
            for (int frame = 0; frame < kMaxPortFramesPerWake; ++frame) {
              try {
                if (!handle_vds_frame(port, backend, trace_flags, logger)) {
                  break;
                }
              } catch (const std::exception &error) {
                if (!is_bluetooth_error(error) || !binding) {
                  throw;
                }
                drop_bt_backend(*binding, port, error.what(), logger);
                epoll_dirty = true;
                break;
              }
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            logger.log("port", vds::LogLevel::Error,
                       port.path + " epoll error; reloading ports");
            reload_requested = true;
            epoll_dirty = true;
          }
          continue;
        }

        if (source.index >= bindings.size() ||
            !bindings[source.index].backend) {
          continue;
        }
        auto &binding = bindings[source.index];
        const auto port_index = find_port_index(ports, binding.device);
        if (!port_index) {
          binding.backend.reset();
          binding.virtual_connected = false;
          epoll_dirty = true;
          continue;
        }
        auto &port = ports[*port_index];

        if (source.kind == EventKind::BtControl) {
          if ((revents & EPOLLIN) != 0) {
            for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
              try {
                if (!handle_bt_control(port, *binding.backend, trace_flags,
                                       logger)) {
                  break;
                }
              } catch (const std::exception &error) {
                if (!is_bluetooth_error(error)) {
                  throw;
                }
                drop_bt_backend(binding, port, error.what(), logger);
                epoll_dirty = true;
                break;
              }
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            drop_bt_backend(binding, port,
                            "Bluetooth L2CAP control epoll error", logger);
            epoll_dirty = true;
          }
          continue;
        }

        if (source.kind == EventKind::BtInterrupt) {
          if ((revents & EPOLLIN) != 0) {
            for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
              try {
                if (!handle_bt_input(port, *binding.backend, trace_flags,
                                     logger)) {
                  break;
                }
              } catch (const std::exception &error) {
                if (!is_bluetooth_error(error)) {
                  throw;
                }
                drop_bt_backend(binding, port, error.what(), logger);
                epoll_dirty = true;
                break;
              }
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            drop_bt_backend(binding, port,
                            "Bluetooth L2CAP interrupt epoll error", logger);
            epoll_dirty = true;
          }
        }
      }

      flush_pending_outputs(ports, bindings, trace_flags, logger, epoll_dirty);
    }

    logger.log("daemon", vds::LogLevel::Info, "stopping");
    disconnect_all(ports, bindings, logger);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "vdsd: " << error.what() << "\n";
    return 1;
  }
}
