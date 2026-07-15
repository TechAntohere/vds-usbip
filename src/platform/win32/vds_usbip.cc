// SPDX-License-Identifier: MIT
// USBIP virtual device transport for vDS on Windows -- see vds_usbip.hh.

#include "vds_usbip.hh"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <timeapi.h> // timeBeginPeriod/timeEndPeriod (link winmm)

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>

#include "unique_handle.hh"
#include "vds/ds5_usb.h"
#include "vds_io.hh"
#include "vds_log.hh"

#pragma comment(lib, "ws2_32.lib")

namespace vds::win::usbip {

namespace {

// ---------------------------------------------------------------------
// USB/IP wire protocol (v1.1.1). All fields big-endian on the wire; the
// helpers below convert. Field layouts follow the Linux kernel's
// Documentation/usb/usbip_protocol.rst and match what usbip-win2 (a
// spec-compliant client) expects.
// ---------------------------------------------------------------------

constexpr std::uint16_t kOpReqDevlist = 0x8005;
constexpr std::uint16_t kOpRepDevlist = 0x0005;
constexpr std::uint16_t kOpReqImport = 0x8003;
constexpr std::uint16_t kOpRepImport = 0x0003;

constexpr std::uint32_t kCmdSubmit = 0x00000001;
constexpr std::uint32_t kRetSubmit = 0x00000003;
constexpr std::uint32_t kCmdUnlink = 0x00000002;
constexpr std::uint32_t kRetUnlink = 0x00000004;

constexpr std::uint32_t kDirOut = 0;
constexpr std::uint32_t kDirIn = 1;

constexpr char kBusId[32] = "1-1";
constexpr char kPath[256] = "/vds/virtual/dualsense";

// Mic jitter buffer sizing. Mic endpoint is 48kHz / 2ch / 16-bit =
// 192 bytes per millisecond of audio.
constexpr std::size_t kMicBytesPerMs = 192;
// Cushion accumulated before delivering real audio. Bluetooth mic delivery
// jitters by roughly a report interval; ~40ms comfortably absorbs the
// 10ms burst granularity plus radio jitter without adding much latency.
constexpr std::size_t kMicPrimeBytes = 40 * kMicBytesPerMs;
// Hard cap on buffered mic audio, to bound added latency if production
// ever briefly outpaces consumption (oldest bytes dropped past this).
constexpr std::size_t kMicMaxBufferBytes = 120 * kMicBytesPerMs;

// Per-URB debug logging opens/writes/closes a file on the hot session
// thread for every URB (~hundreds/sec). That synchronous disk I/O competes
// with the very audio/mic timing we care about, so it is off unless
// VDS_USBIP_DEBUG is set in the environment. Evaluated once.
bool usbip_debug_logging() {
  static const bool enabled = [] {
    char buf[8]{};
    return GetEnvironmentVariableA("VDS_USBIP_DEBUG", buf, sizeof(buf)) > 0;
  }();
  return enabled;
}

// Digital boost applied to mic capture. The DualSense mic is quiet at
// distance; a modest linear boost makes normal speech usable. Tunable via
// VDS_MIC_GAIN (linear multiplier, e.g. 4.0 ~= +12dB). Evaluated once.
float mic_capture_gain() {
  static const float gain = [] {
    char buf[16]{};
    const DWORD n = GetEnvironmentVariableA("VDS_MIC_GAIN", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
      const float v = static_cast<float>(std::atof(buf));
      if (v > 0.0f && v <= 64.0f) return v;
    }
    return 50.0f; // DualSense mic is extremely quiet; heavy boost needed
                  // (user-tuned on real hardware). Override with VDS_MIC_GAIN.
  }();
  return gain;
}

// Apply a linear gain to interleaved 16-bit little-endian PCM in place,
// clamping to int16 range.
void apply_pcm_gain_i16(std::vector<std::uint8_t> &pcm, float gain) {
  if (gain == 1.0f) return;
  for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
    const auto s = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(pcm[i]) |
        (static_cast<std::uint16_t>(pcm[i + 1]) << 8));
    float v = static_cast<float>(s) * gain;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    const auto o = static_cast<std::int16_t>(v);
    pcm[i] = static_cast<std::uint8_t>(o & 0xff);
    pcm[i + 1] = static_cast<std::uint8_t>((o >> 8) & 0xff);
  }
}

std::uint16_t hton16(std::uint16_t v) { return htons(v); }
std::uint32_t hton32(std::uint32_t v) { return htonl(v); }
std::uint16_t ntoh16(std::uint16_t v) { return ntohs(v); }
std::uint32_t ntoh32(std::uint32_t v) { return ntohl(v); }

#pragma pack(push, 1)
struct OpCommonHeader {
  std::uint16_t version;
  std::uint16_t code;
  std::uint32_t status;
};

// usbip_usb_device, as sent in OP_REP_IMPORT / OP_REP_DEVLIST.
struct WireUsbDevice {
  char path[256];
  char busid[32];
  std::uint32_t busnum;
  std::uint32_t devnum;
  std::uint32_t speed; // 2 = USB_SPEED_HIGH
  std::uint16_t idVendor;
  std::uint16_t idProduct;
  std::uint16_t bcdDevice;
  std::uint8_t bDeviceClass;
  std::uint8_t bDeviceSubClass;
  std::uint8_t bDeviceProtocol;
  std::uint8_t bConfigurationValue;
  std::uint8_t bNumConfigurations;
  std::uint8_t bNumInterfaces;
};

struct WireUsbInterface {
  std::uint8_t bInterfaceClass;
  std::uint8_t bInterfaceSubClass;
  std::uint8_t bInterfaceProtocol;
  std::uint8_t padding;
};

// Common header for USBIP_CMD_SUBMIT / RET_SUBMIT / CMD_UNLINK / RET_UNLINK.
struct UsbipHeaderBasic {
  std::uint32_t command;
  std::uint32_t seqnum;
  std::uint32_t devid;
  std::uint32_t direction;
  std::uint32_t ep;
};

struct UsbipCmdSubmit {
  UsbipHeaderBasic base;
  std::uint32_t transfer_flags;
  std::int32_t transfer_buffer_length;
  std::int32_t start_frame;
  std::int32_t number_of_packets;
  std::int32_t interval;
  std::uint8_t setup[8];
};

struct UsbipRetSubmit {
  UsbipHeaderBasic base;
  std::int32_t status;
  std::int32_t actual_length;
  std::int32_t start_frame;
  std::int32_t number_of_packets;
  std::int32_t error_count;
  std::uint8_t padding[8];
};

struct UsbipIsoPacketDescriptor {
  std::uint32_t offset;
  std::uint32_t length;
  std::int32_t actual_length;
  std::int32_t status;
};

struct UsbipCmdUnlink {
  UsbipHeaderBasic base;
  std::uint32_t unlink_seqnum;
  std::uint8_t padding[24];
};

struct UsbipRetUnlink {
  UsbipHeaderBasic base;
  std::int32_t status;
  std::uint8_t padding[24];
};
#pragma pack(pop)

// Endpoint numbers, sourced directly from include/vds/ds5_usb.h -- no
// guessing, these are the byte-exact values already used by vds_usb.sys.
constexpr std::uint8_t kEpAudioOut = VDS_USB_AUDIO_OUT_ENDPOINT & 0x0f;
constexpr std::uint8_t kEpAudioIn = VDS_USB_AUDIO_IN_ENDPOINT & 0x0f;
constexpr std::uint8_t kEpHidIn = VDS_USB_HID_IN_ENDPOINT & 0x0f;
constexpr std::uint8_t kEpHidOut = VDS_USB_HID_OUT_ENDPOINT & 0x0f;

struct WinsockGuard {
  WinsockGuard() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }
  ~WinsockGuard() { WSACleanup(); }
};

class Socket {
public:
  explicit Socket(SOCKET s = INVALID_SOCKET) : s_(s) {}
  ~Socket() { reset(); }
  Socket(Socket &&other) noexcept : s_(other.s_) { other.s_ = INVALID_SOCKET; }
  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) {
      reset();
      s_ = other.s_;
      other.s_ = INVALID_SOCKET;
    }
    return *this;
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  SOCKET get() const { return s_; }
  bool valid() const { return s_ != INVALID_SOCKET; }
  void reset(SOCKET s = INVALID_SOCKET) {
    if (s_ != INVALID_SOCKET) {
      closesocket(s_);
    }
    s_ = s;
  }

private:
  SOCKET s_;
};

bool recv_exact(SOCKET s, void *buffer, std::size_t size) {
  auto *data = static_cast<char *>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    const int got = recv(s, data, static_cast<int>(remaining), 0);
    if (got <= 0) {
      return false;
    }
    data += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

bool send_exact(SOCKET s, const void *buffer, std::size_t size) {
  const auto *data = static_cast<const char *>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    const int sent = send(s, data, static_cast<int>(remaining), 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  return true;
}

// Picks the correct descriptor tables/endpoint-interval info for the
// configured profile (DS5 vs DSE). All byte content comes straight from
// include/vds/ds5_usb.h -- this module does not fabricate any descriptor
// bytes of its own.
struct ProfileDescriptors {
  const std::uint8_t *device;
  std::size_t device_len;
  const std::uint8_t *config;
  std::size_t config_len;
  const std::uint8_t *hid;
  std::size_t hid_len;
  const std::uint8_t *hid_report;
  std::size_t hid_report_len;
  std::uint16_t idVendor;
  std::uint16_t idProduct;
};

// Builds a USB STRING descriptor (bLength, bDescriptorType=0x03, then
// UTF-16LE code units -- ASCII input only, which covers all vDS string
// constants) for the given ASCII text. Returns the raw descriptor bytes.
std::vector<std::uint8_t> build_string_descriptor(std::string_view text) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(2 + text.size() * 2);
  bytes.push_back(0); // bLength, patched below
  bytes.push_back(0x03); // bDescriptorType = STRING
  for (const char c : text) {
    bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(0x00);
  }
  bytes[0] = static_cast<std::uint8_t>(bytes.size());
  return bytes;
}

// String descriptor index 0 is always the supported-languages list, not
// text: bLength=4, bDescriptorType=0x03, wLANGID[0] = 0x0409 (English/US).
std::vector<std::uint8_t> build_langid_descriptor() {
  return {0x04, 0x03, 0x09, 0x04};
}

ProfileDescriptors descriptors_for_profile(std::uint32_t profile) {
  if (profile == VDS_PROFILE_DSE) {
    return ProfileDescriptors{
        vds_dse_usb_device_descriptor,
        sizeof(vds_dse_usb_device_descriptor),
        vds_dse_usb_configuration_descriptor,
        sizeof(vds_dse_usb_configuration_descriptor),
        vds_dse_usb_hid_descriptor,
        sizeof(vds_dse_usb_hid_descriptor),
        vds_dse_usb_hid_report_descriptor,
        sizeof(vds_dse_usb_hid_report_descriptor),
        0x054c,
        0x0df2,
    };
  }
  return ProfileDescriptors{
      vds_ds5_usb_device_descriptor,
      sizeof(vds_ds5_usb_device_descriptor),
      vds_ds5_usb_configuration_descriptor,
      sizeof(vds_ds5_usb_configuration_descriptor),
      vds_ds5_usb_hid_descriptor,
      sizeof(vds_ds5_usb_hid_descriptor),
      vds_ds5_usb_hid_report_descriptor,
      sizeof(vds_ds5_usb_hid_report_descriptor),
      0x054c,
      0x0ce6,
  };
}

} // namespace

struct VirtualPort::Impl {
  Impl(std::uint32_t profile_in, unsigned port_index_in, std::uint16_t tcp_port_in)
      : profile(profile_in), port_index(port_index_in), tcp_port(tcp_port_in) {}

  std::uint32_t profile;
  unsigned port_index;
  std::uint16_t tcp_port;
  std::string pipe_path;

  WinsockGuard winsock;
  UniqueHandle pipe_server; // our end of the internal Frame-protocol pipe
  std::thread tcp_thread;
  std::thread pipe_accept_thread;
  std::atomic_bool stop_requested{false};
  std::atomic_bool attached{false};

  // Published raw sockets so stop() can unblock the TCP thread. Without
  // these, stop() deadlocks on tcp_thread.join(): tcp_server_loop() sits
  // in a blocking accept()/recv() that nothing ever interrupts (the
  // CancelIoEx in stop() only affects the pipe, not sockets). This is the
  // root cause of "worker never relaunches on live re-attach": the old
  // bridge session's ~VirtualPort never returns, so the supervisor never
  // frees the port. stop() shuts down the client socket (unblocking recv)
  // and pokes the listener with a throwaway loopback connect (unblocking
  // accept) rather than closing sockets it doesn't own.
  std::atomic<SOCKET> client_socket{INVALID_SOCKET};
  // Set by pipe_reader_loop() on exit so stop() can re-issue CancelIoEx
  // until the cancel actually lands (a single cancel races with the reader
  // being between reads, in which case it is consumed by nothing and the
  // next read blocks forever anyway).
  std::atomic_bool reader_exited{false};

  // Guards internal_pipe_client, which the TCP session thread uses to
  // forward Frame traffic to/from vdsd (the real consumer, connected via
  // pipe_path on the other end).
  std::mutex pipe_mutex;

  void start();
  void stop();
  void tcp_server_loop();
  bool handle_op_common(SOCKET client, std::uint16_t code);
  void handle_import_session(SOCKET client);
  bool handle_cmd_submit(SOCKET client, const UsbipCmdSubmit &cmd,
                        std::span<const std::uint8_t> out_data,
                        std::vector<std::uint8_t> &iso_in_scratch,
                        const std::vector<UsbipIsoPacketDescriptor> &iso_descriptors);

  // Bridges a single Frame to/from vdsd over the internal named pipe. These
  // reuse vds_io.hh's read_handle_frame/write_handle_frame verbatim -- they
  // already operate on any Win32 HANDLE, overlapped or not.
  Frame read_daemon_frame(HANDLE pipe);
  void write_daemon_frame(HANDLE pipe, std::uint16_t type,
                          std::span<const std::uint8_t> payload);

  // Demuxes daemon->host Frame traffic by type. The pipe carries HID_IN,
  // FEATURE_REPLY, and AUDIO_IN frames interleaved with no framing-level
  // routing; without this, whichever endpoint handler happens to call
  // read_daemon_frame() next steals and silently drops the next frame on
  // the wire regardless of its real type, starving the intended consumer.
  // This is the read-side half of the pipe-demux fix; the write side
  // (virtual_write_mutex in vdsd.cc) only protects against corrupted
  // interleaved writes and does not address this.
  std::thread pipe_reader_thread;
  std::mutex demux_mutex;
  std::condition_variable demux_cv;
  std::deque<Frame> hid_in_queue;
  std::deque<Frame> feature_reply_queue;
  // Raw PCM bytes forwarded from vdsd for AUDIO_IN (mic). Buffered because
  // vdsd's per-Opus-packet decode chunk size has no relation to the byte
  // count a given isoc URB actually requests -- forwarding a vdsd frame's
  // raw size directly as the URB's actual_length (the previous behavior)
  // can exceed the URB's declared transfer_buffer_length, which is a wire
  // protocol violation that makes usbip-win2 abort the TCP connection.
  std::vector<std::uint8_t> mic_ring_buffer;
  // Mic jitter-buffer state (guarded by demux_mutex, like mic_ring_buffer).
  // vdsd forwards mic PCM in 10ms (1920-byte) bursts tied to the Bluetooth
  // input-report cadence, but Windows pulls the isoc IN endpoint every 1ms
  // at a fixed packet size. Returning whatever happens to be buffered on
  // each pull (the old behavior) means most pulls hit an empty buffer and
  // get zero-padded -> the audible crackle. We instead accumulate a small
  // cushion before delivering, then stream continuously; on underrun we
  // re-prime rather than glitch on every subsequent pull. Rates match on
  // average (192 bytes/ms both sides) so the cushion only absorbs burst.
  bool mic_primed_ = false;
  std::uint64_t mic_pull_count_ = 0;
  std::uint64_t mic_underrun_count_ = 0;
  std::uint64_t mic_zero_pad_bytes_ = 0;
  std::uint64_t mic_received_bytes_ = 0; // total PCM bytes vdsd forwarded
  std::uint64_t mic_requested_bytes_ = 0; // total bytes Windows pulled

  void pipe_reader_loop();
  bool wait_for_hid_in_frame(Frame &out, DWORD timeout_ms);
  bool wait_for_feature_reply(Frame &out, DWORD timeout_ms);
  std::vector<std::uint8_t> take_mic_bytes(std::size_t count);

  // --- ISO OUT completion pacing -------------------------------------
  // usbaudio.sys uses URB completion as its transfer clock: completing an
  // isochronous OUT URB tells it "the bus consumed these packets," so
  // completing instantly (as this server naturally does -- there is no
  // real bus) makes Windows stream PCM as fast as it can generate it.
  // Measured: ~96 seconds of audio delivered in a 20 second tone (~5x
  // real time), forcing vdsd to drop ~60% of chunks as stale = heavy
  // stutter + crackle. vds_usb.sys solved this with a deliberate ~1ms per
  // packet completion delay (VdsCompleteUrbAfterDelay); this is the
  // USB/IP equivalent: audio OUT replies are serialized and handed to a
  // dedicated pacer thread that sends each RET_SUBMIT at its real-time
  // due moment (number_of_packets x 1ms per URB), so the session thread
  // never blocks and HID replies are unaffected.
  struct PacedReply {
    std::chrono::steady_clock::time_point due;
    SOCKET client;
    std::vector<std::uint8_t> bytes; // complete serialized RET_SUBMIT
  };
  std::thread iso_pacer_thread;
  std::mutex pacer_mutex; // guards paced_replies + iso_out_next_due
  std::condition_variable pacer_cv;
  std::deque<PacedReply> paced_replies;
  std::chrono::steady_clock::time_point iso_out_next_due{};
  std::chrono::steady_clock::time_point iso_in_next_due{}; // mic (audio IN)
  // Both the session thread and the pacer thread write complete wire
  // messages to the same socket; this keeps them from interleaving
  // mid-message.
  std::mutex socket_send_mutex;
  void iso_pacer_loop();

  // --- USB Audio Class Feature Unit volume ---------------------------
  // Windows defers master volume to the device's Feature Unit when the
  // descriptor advertises one (ours does). Previously SET_CUR was
  // accepted and discarded, so the volume slider did nothing and audio
  // always ran at full scale ("kinda loud"). Store mute/volume and apply
  // the gain to the speaker channels (ch0/1) before forwarding PCM to
  // vdsd; haptics (ch2/3) are intentionally not scaled -- system volume
  // should not change game haptics intensity.
  std::atomic<int> speaker_gain_q15{1 << 15};
  std::atomic<int> fu_volume_db256{0}; // s16, 1/256 dB units
  std::atomic<bool> fu_muted{false};
  void update_speaker_gain();
};

void VirtualPort::Impl::start() {
  pipe_path = R"(\\.\pipe\vds_usbip_port)" + std::to_string(port_index);

  // Server end of the pipe: OVERLAPPED so it composes with the same
  // read_handle_frame/write_handle_frame helpers vdsd.cc already uses on
  // the client end via open_device().
  pipe_server.reset(CreateNamedPipeA(
      pipe_path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
      64 * 1024, 64 * 1024, 0, nullptr));
  if (!pipe_server) {
    throw std::runtime_error("failed to create " + pipe_path);
  }

  tcp_thread = std::thread([this] { tcp_server_loop(); });
  iso_pacer_thread = std::thread([this] { iso_pacer_loop(); });
}

void VirtualPort::Impl::stop() {
  stop_requested = true;
  pacer_cv.notify_all();

  // Unblock an in-flight import session: shutdown (not close -- the Socket
  // RAII in tcp_server_loop still owns and closes it) makes any blocking
  // recv() on the session socket return immediately.
  const SOCKET session_socket = client_socket.exchange(INVALID_SOCKET);
  if (session_socket != INVALID_SOCKET) {
    shutdown(session_socket, SD_BOTH);
  }

  // Unblock accept(): connect a throwaway loopback socket to our own
  // listener and immediately close it. accept() returns, recv_exact on the
  // dead connection fails fast, and the loop exits on stop_requested. This
  // avoids closing the listener from a thread that doesn't own it.
  {
    const SOCKET poke = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (poke != INVALID_SOCKET) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = inet_addr("127.0.0.1");
      addr.sin_port = hton16(tcp_port);
      connect(poke, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
      closesocket(poke);
    }
  }

  if (tcp_thread.joinable()) {
    tcp_thread.join();
  }

  // Wake the demux reader, re-cancelling until it confirms exit (see the
  // reader_exited member comment for why one CancelIoEx is not enough).
  if (pipe_reader_thread.joinable()) {
    for (int attempt = 0; attempt < 200 && !reader_exited.load(); ++attempt) {
      if (pipe_server) {
        CancelIoEx(pipe_server.get(), nullptr);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pipe_reader_thread.join();
  }

  if (iso_pacer_thread.joinable()) {
    pacer_cv.notify_all();
    iso_pacer_thread.join();
  }
}

void VirtualPort::Impl::iso_pacer_loop() {
  // Without 1ms timer resolution, wait_until quantizes to the system tick
  // (up to ~15.6ms), turning the 10ms completion cadence into lumpy
  // clumps -- the same reason vdsd's audio flush loop uses its
  // HighResolutionSleeper. Scoped to this thread's lifetime.
  const bool high_res = timeBeginPeriod(1) == TIMERR_NOERROR;
  std::unique_lock<std::mutex> lk(pacer_mutex);
  while (!stop_requested.load()) {
    if (paced_replies.empty()) {
      pacer_cv.wait(lk);
      continue;
    }
    const auto due = paced_replies.front().due;
    const auto now = std::chrono::steady_clock::now();
    if (now < due) {
      pacer_cv.wait_until(lk, due);
      continue; // re-check front/stop after any wakeup
    }
    PacedReply reply = std::move(paced_replies.front());
    paced_replies.pop_front();
    lk.unlock();
    {
      std::lock_guard<std::mutex> send_guard(socket_send_mutex);
      // Failure just means the client went away; the session loop notices
      // on its own recv path, nothing to do here.
      send_exact(reply.client, reply.bytes.data(), reply.bytes.size());
    }
    lk.lock();
  }
  if (high_res) {
    timeEndPeriod(1);
  }
}

void VirtualPort::Impl::update_speaker_gain() {
  if (fu_muted.load()) {
    speaker_gain_q15 = 0;
    return;
  }
  const double db = static_cast<double>(fu_volume_db256.load()) / 256.0;
  double linear = std::pow(10.0, db / 20.0);
  if (linear > 1.0) linear = 1.0;
  if (linear < 0.0) linear = 0.0;
  speaker_gain_q15 = static_cast<int>(linear * (1 << 15) + 0.5);
}

void VirtualPort::Impl::tcp_server_loop() {
  Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (!listener.valid()) {
    return;
  }
  const BOOL reuse = TRUE;
  setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char *>(&reuse), sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = hton16(tcp_port);
  if (bind(listener.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    return;
  }
  if (listen(listener.get(), 1) != 0) {
    return;
  }

  while (!stop_requested.load()) {
    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);
    const SOCKET raw_client =
        accept(listener.get(), reinterpret_cast<sockaddr *>(&client_addr),
              &client_addr_len);
    if (raw_client == INVALID_SOCKET) {
      if (stop_requested.load()) {
        break;
      }
      continue;
    }
    Socket client(raw_client);
    // Publish the live connection so stop() can shutdown() it to unblock
    // any blocking recv below; cleared automatically when this iteration's
    // scope ends, whichever continue-path takes us there.
    client_socket = client.get();
    struct ClientSocketGuard {
      std::atomic<SOCKET> &slot;
      ~ClientSocketGuard() { slot = INVALID_SOCKET; }
    } client_socket_guard{client_socket};
    // usbip-win2 makes a short-lived control connection for OP_REQ_DEVLIST
    // (device discovery) and a separate long-lived connection for
    // OP_REQ_IMPORT (the actual attach). Handle whichever comes in.
    OpCommonHeader header{};
    if (!recv_exact(client.get(), &header, sizeof(header))) {
      continue;
    }
    const std::uint16_t code = ntoh16(header.code);
    if (code == kOpReqDevlist) {
      OpCommonHeader reply{hton16(kProtocolVersion), hton16(kOpRepDevlist), 0};
      const std::uint32_t device_count = hton32(1);
      const ProfileDescriptors desc = descriptors_for_profile(profile);
      WireUsbDevice wire_dev{};
      std::strncpy(wire_dev.path, kPath, sizeof(wire_dev.path) - 1);
      std::strncpy(wire_dev.busid, kBusId, sizeof(wire_dev.busid) - 1);
      wire_dev.busnum = hton32(1);
      wire_dev.devnum = hton32(1);
      wire_dev.speed = hton32(3); // USB_SPEED_HIGH (protocol value 3, not 2 -- 2 is USB_SPEED_FULL)
      wire_dev.idVendor = hton16(desc.idVendor);
      wire_dev.idProduct = hton16(desc.idProduct);
      wire_dev.bcdDevice = hton16(VDS_USB_DEVICE_BCD);
      wire_dev.bDeviceClass = desc.device[4];
      wire_dev.bDeviceSubClass = desc.device[5];
      wire_dev.bDeviceProtocol = desc.device[6];
      wire_dev.bConfigurationValue = VDS_USB_CONFIGURATION_VALUE;
      wire_dev.bNumConfigurations = VDS_USB_NUM_CONFIGURATIONS;
      wire_dev.bNumInterfaces = 4;
      send_exact(client.get(), &reply, sizeof(reply));
      send_exact(client.get(), &device_count, sizeof(device_count));
      send_exact(client.get(), &wire_dev, sizeof(wire_dev));
      // Interface class table: audio-control, audio-out, audio-in, HID.
      const WireUsbInterface interfaces[4] = {
          {0x01, 0x01, 0x00, 0}, // audio control
          {0x01, 0x02, 0x00, 0}, // audio streaming OUT
          {0x01, 0x02, 0x00, 0}, // audio streaming IN
          {0x03, 0x00, 0x00, 0}, // HID
      };
      send_exact(client.get(), interfaces, sizeof(interfaces));
      continue;
    }
    if (code == kOpReqImport) {
      char busid[32]{};
      if (!recv_exact(client.get(), busid, sizeof(busid))) {
        continue;
      }
      handle_import_session(client.get());
      // handle_import_session owns the connection until the client
      // disconnects or we stop; loop back to accept() for reattachment.
      continue;
    }
    // Unknown opcode: drop the connection.
  }
}

void VirtualPort::Impl::handle_import_session(SOCKET client) {
  const ProfileDescriptors desc = descriptors_for_profile(profile);

  OpCommonHeader reply{hton16(kProtocolVersion), hton16(kOpRepImport), 0};
  send_exact(client, &reply, sizeof(reply));

  WireUsbDevice wire_dev{};
  std::strncpy(wire_dev.path, kPath, sizeof(wire_dev.path) - 1);
  std::strncpy(wire_dev.busid, kBusId, sizeof(wire_dev.busid) - 1);
  wire_dev.busnum = hton32(1);
  wire_dev.devnum = hton32(1);
  wire_dev.speed = hton32(3); // USB_SPEED_HIGH
  wire_dev.idVendor = hton16(desc.idVendor);
  wire_dev.idProduct = hton16(desc.idProduct);
  wire_dev.bcdDevice = hton16(VDS_USB_DEVICE_BCD);
  wire_dev.bDeviceClass = desc.device[4];
  wire_dev.bDeviceSubClass = desc.device[5];
  wire_dev.bDeviceProtocol = desc.device[6];
  wire_dev.bConfigurationValue = VDS_USB_CONFIGURATION_VALUE;
  wire_dev.bNumConfigurations = VDS_USB_NUM_CONFIGURATIONS;
  wire_dev.bNumInterfaces = 4;
  send_exact(client, &wire_dev, sizeof(wire_dev));

  attached = true;

  // Connect to vdsd's side of the internal pipe. vdsd's open_device() is
  // expected to have already connected by the time real traffic starts,
  // since VirtualPortBindingGuard opens the pipe right after this port is
  // constructed. ConnectNamedPipe here completes that handshake.
  BOOL connected = ConnectNamedPipe(pipe_server.get(), nullptr);
  if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
    attached = false;
    return;
  }

  // Start the frame demux reader now that the pipe is connected. See the
  // Impl member comments for why this exists: without it, HID_IN,
  // FEATURE_REPLY, and AUDIO_IN frames race for the same blocking read.
  // Started at most once per VirtualPort: on a re-import into the same
  // port the pipe (and its reader) survive the client reconnect, and
  // assigning over a still-joinable std::thread calls std::terminate().
  if (!pipe_reader_thread.joinable()) {
    reader_exited = false;
    pipe_reader_thread = std::thread([this] { pipe_reader_loop(); });
  }

  std::vector<std::uint8_t> iso_scratch;
  while (!stop_requested.load()) {
    UsbipCmdSubmit cmd{};
    if (!recv_exact(client, &cmd, sizeof(cmd))) {
      if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: recv header failed\n"); std::fclose(dbg); }
      break;
    }
    cmd.base.command = ntoh32(cmd.base.command);
    cmd.base.seqnum = ntoh32(cmd.base.seqnum);
    cmd.base.devid = ntoh32(cmd.base.devid);
    cmd.base.direction = ntoh32(cmd.base.direction);
    cmd.base.ep = ntoh32(cmd.base.ep);
    cmd.transfer_flags = ntoh32(cmd.transfer_flags);
    cmd.transfer_buffer_length = static_cast<std::int32_t>(
        ntoh32(static_cast<std::uint32_t>(cmd.transfer_buffer_length)));
    cmd.number_of_packets = static_cast<std::int32_t>(
        ntoh32(static_cast<std::uint32_t>(cmd.number_of_packets)));

    if (usbip_debug_logging()) {
      if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) {
        std::fprintf(dbg,
                      "cmd command=%u seqnum=%u ep=%u dir=%u xfer_len=%d "
                      "npackets=%d\\n",
                      cmd.base.command, cmd.base.seqnum, cmd.base.ep,
                      cmd.base.direction, cmd.transfer_buffer_length,
                      cmd.number_of_packets);
        std::fclose(dbg);
      }
    }

    if (cmd.base.command == kCmdUnlink) {
      // Best-effort: acknowledge unlink without tracking in-flight URBs
      // individually. vDS's interrupt/iso endpoints are effectively
      // always-pending polling loops, so an unlink just means "stop
      // expecting a reply for that seqnum," which our per-request
      // synchronous handling already satisfies.
      UsbipRetUnlink unlink_reply{};
      unlink_reply.base.command = hton32(kRetUnlink);
      unlink_reply.base.seqnum = hton32(cmd.base.seqnum);
      {
        std::lock_guard<std::mutex> send_guard(socket_send_mutex);
        send_exact(client, &unlink_reply, sizeof(unlink_reply));
      }
      continue;
    }
    if (cmd.base.command != kCmdSubmit) {
      if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: unexpected command\n"); std::fclose(dbg); }
      break;
    }

    // CMD_SUBMIT wire layout (verified against BOTH usbip-win2's
    // libdrv/pdu.cpp get_isoc_descr() and usbip-virtual-device's
    // command/submit.go Decode()): header, then the transfer buffer (OUT
    // direction only), then the iso packet descriptors. The descriptors
    // come AFTER the data, not before.
    //
    // The previous order here (descriptors first, data second) never
    // desynced the stream -- 160 + 3840 bytes reads the same total as
    // 3840 + 160 -- which made it invisible: it silently parsed the first
    // 160 bytes of PCM as descriptors and forwarded byte-shifted PCM to
    // vdsd. Priming URBs carry silence (all zeros), so the garbage
    // zero descriptors produced replies that happened to pass usbip-win2's
    // validation; the first URB carrying real audio put nonzero PCM into
    // the fake descriptors, our echoed reply's actual_length became a
    // garbage sum, the client's check() failed, and its recv thread exited
    // via async_detach_and_delete -- the recurring "connection dies right
    // after isoc OUT submission #2" signature.
    std::vector<std::uint8_t> out_data;
    std::vector<UsbipIsoPacketDescriptor> iso_descriptors;
    if (cmd.base.direction == kDirOut && cmd.transfer_buffer_length > 0) {
      out_data.resize(static_cast<std::size_t>(cmd.transfer_buffer_length));
      if (!recv_exact(client, out_data.data(), out_data.size())) {
        if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: recv out_data failed\n"); std::fclose(dbg); }
        break;
      }
    }
    if (cmd.number_of_packets > 0) {
      iso_descriptors.resize(static_cast<std::size_t>(cmd.number_of_packets));
      if (!recv_exact(client, iso_descriptors.data(),
                      iso_descriptors.size() * sizeof(UsbipIsoPacketDescriptor))) {
        if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: recv iso descriptors failed\n"); std::fclose(dbg); }
        break;
      }
      // recv_exact() above copies the raw wire bytes verbatim -- these
      // fields are still network byte order at this point, same as
      // cmd.number_of_packets before its explicit ntoh32() a few lines up.
      // Convert once here so every later use (pacing math, IN clamping, and
      // the RET_SUBMIT echo) works with real host-order values instead of
      // silently double-converting them when the reply is built.
      for (auto &desc : iso_descriptors) {
        desc.offset = ntoh32(desc.offset);
        desc.length = ntoh32(desc.length);
      }
    }

    try {
      if (!handle_cmd_submit(client, cmd, out_data, iso_scratch, iso_descriptors)) {
        if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: handle_cmd_submit failed\n"); std::fclose(dbg); }
        break;
      }
    } catch (const std::exception &ex) {
      if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) {
        std::fprintf(dbg, "BREAK: handle_cmd_submit threw: %s\n", ex.what());
        std::fclose(dbg);
      }
      break;
    }
  }

  attached = false;
  // Drop any not-yet-sent paced audio replies -- their socket is dead --
  // and reset the pacing anchor so the next import starts fresh.
  {
    std::lock_guard<std::mutex> guard(pacer_mutex);
    paced_replies.clear();
    iso_out_next_due = {};
    iso_in_next_due = {};
  }
  DisconnectNamedPipe(pipe_server.get());
}

Frame VirtualPort::Impl::read_daemon_frame(HANDLE pipe) {
  return vds::win::read_handle_frame(pipe, "usbip-bridge", {});
}

void VirtualPort::Impl::write_daemon_frame(HANDLE pipe, std::uint16_t type,
                                          std::span<const std::uint8_t> payload) {
  vds::win::write_handle_frame(pipe, type, payload, "usbip-bridge", {});
}

void VirtualPort::Impl::pipe_reader_loop() {
  for (;;) {
    Frame frame;
    try {
      frame = read_daemon_frame(pipe_server.get());
    } catch (...) {
      // Pipe closed/cancelled (normal on stop(), or on a real daemon-side
      // teardown). Wake any waiters so they fail fast instead of hanging.
      reader_exited = true;
      demux_cv.notify_all();
      return;
    }
    if (stop_requested.load()) {
      reader_exited = true;
      demux_cv.notify_all();
      return;
    }
    std::lock_guard<std::mutex> lk(demux_mutex);
    switch (frame.header.type) {
      case VDS_FRAME_USB_HID_IN:
        hid_in_queue.push_back(std::move(frame));
        // Bound the queue: if the HID_IN handler stalls (e.g. blocked on
        // something else) we want to drop stale reports, not build
        // unbounded latency.
        if (hid_in_queue.size() > 8) hid_in_queue.pop_front();
        break;
      case VDS_FRAME_USB_FEATURE_REPLY:
        feature_reply_queue.push_back(std::move(frame));
        break;
      case VDS_FRAME_USB_AUDIO_IN: {
        mic_received_bytes_ += frame.payload.size();
        // Boost quiet mic samples once here, as they arrive (before any
        // chunking by the URB consumer), so gain is applied exactly once.
        apply_pcm_gain_i16(frame.payload, mic_capture_gain());
        mic_ring_buffer.insert(mic_ring_buffer.end(), frame.payload.begin(),
                               frame.payload.end());
        // Cap growth to bound added latency if production briefly outpaces
        // consumption (or the interface isn't being drained yet). Drop the
        // oldest bytes past the cap.
        if (mic_ring_buffer.size() > kMicMaxBufferBytes) {
          mic_ring_buffer.erase(
              mic_ring_buffer.begin(),
              mic_ring_buffer.begin() +
                  static_cast<std::ptrdiff_t>(mic_ring_buffer.size() - kMicMaxBufferBytes));
        }
        break;
      }
      default:
        // VDS_FRAME_STATUS and others are not expected in this direction;
        // drop rather than let them desync a queue.
        break;
    }
    demux_cv.notify_all();
  }
}

bool VirtualPort::Impl::wait_for_hid_in_frame(Frame &out, DWORD timeout_ms) {
  std::unique_lock<std::mutex> lk(demux_mutex);
  if (!demux_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] {
        return !hid_in_queue.empty() || stop_requested.load();
      })) {
    return false;
  }
  if (hid_in_queue.empty()) return false;
  out = std::move(hid_in_queue.front());
  hid_in_queue.pop_front();
  return true;
}

bool VirtualPort::Impl::wait_for_feature_reply(Frame &out, DWORD timeout_ms) {
  std::unique_lock<std::mutex> lk(demux_mutex);
  if (!demux_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] {
        return !feature_reply_queue.empty() || stop_requested.load();
      })) {
    return false;
  }
  if (feature_reply_queue.empty()) return false;
  out = std::move(feature_reply_queue.front());
  feature_reply_queue.pop_front();
  return true;
}

std::vector<std::uint8_t> VirtualPort::Impl::take_mic_bytes(std::size_t count) {
  std::lock_guard<std::mutex> lk(demux_mutex);
  std::vector<std::uint8_t> out;
  ++mic_pull_count_;
  mic_requested_bytes_ += count;
  if (usbip_debug_logging() && mic_pull_count_ % 1000 == 0) {
    if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_mic_debug.log", "a")) {
      std::fprintf(dbg,
                   "mic pulls=%llu count=%zu recv_bytes=%llu req_bytes=%llu "
                   "ratio=%.2f underruns=%llu buffered=%zu primed=%d\\n",
                   static_cast<unsigned long long>(mic_pull_count_), count,
                   static_cast<unsigned long long>(mic_received_bytes_),
                   static_cast<unsigned long long>(mic_requested_bytes_),
                   mic_requested_bytes_ ? (double)mic_received_bytes_ / (double)mic_requested_bytes_ : 0.0,
                   static_cast<unsigned long long>(mic_underrun_count_),
                   mic_ring_buffer.size(), mic_primed_ ? 1 : 0);
      std::fclose(dbg);
    }
  }

  // Not yet primed: return silence until a cushion has accumulated. This is
  // what removes the crackle -- without it, the very first pulls (and every
  // pull between 10ms BT bursts) drain the buffer to empty and zero-pad.
  if (!mic_primed_) {
    if (mic_ring_buffer.size() >= kMicPrimeBytes) {
      mic_primed_ = true;
    } else {
      out.assign(count, 0);
      return out;
    }
  }

  if (mic_ring_buffer.size() >= count) {
    out.assign(mic_ring_buffer.begin(),
               mic_ring_buffer.begin() + static_cast<std::ptrdiff_t>(count));
    mic_ring_buffer.erase(
        mic_ring_buffer.begin(),
        mic_ring_buffer.begin() + static_cast<std::ptrdiff_t>(count));
    return out;
  }

  // Underrun while primed: hand over what we have, zero-pad the remainder
  // (usbip-win2 requires exactly transfer_buffer_length bytes back), and
  // drop back to unprimed so we rebuild the cushion instead of glitching on
  // every pull until the next burst arrives.
  out.assign(mic_ring_buffer.begin(), mic_ring_buffer.end());
  mic_zero_pad_bytes_ += count - out.size();
  out.resize(count, 0);
  mic_ring_buffer.clear();
  mic_primed_ = false;
  ++mic_underrun_count_;
  return out;
}

bool VirtualPort::Impl::handle_cmd_submit(SOCKET client, const UsbipCmdSubmit &cmd,
                                         std::span<const std::uint8_t> out_data,
                                         std::vector<std::uint8_t> &iso_in_scratch,
                                         const std::vector<UsbipIsoPacketDescriptor> &iso_descriptors) {
  const ProfileDescriptors desc = descriptors_for_profile(profile);
  UsbipRetSubmit ret{};
  ret.base.command = hton32(kRetSubmit);
  ret.base.seqnum = hton32(cmd.base.seqnum);
  ret.base.devid = hton32(cmd.base.devid);
  ret.base.direction = hton32(cmd.base.direction);
  ret.base.ep = hton32(cmd.base.ep);

  std::vector<std::uint8_t> reply_data;

  if (cmd.base.ep == 0) {
    // Control transfer: setup packet lives in cmd.setup (already in wire
    // byte order per spec -- USB setup packets are little-endian on the
    // wire and USB/IP does not re-encode them).
    const std::uint8_t bmRequestType = cmd.setup[0];
    const std::uint8_t bRequest = cmd.setup[1];
    const std::uint16_t wValue =
        static_cast<std::uint16_t>(cmd.setup[2] | (cmd.setup[3] << 8));
    const std::uint16_t wIndex =
        static_cast<std::uint16_t>(cmd.setup[4] | (cmd.setup[5] << 8));
    const std::uint16_t wLength =
        static_cast<std::uint16_t>(cmd.setup[6] | (cmd.setup[7] << 8));
    const bool device_to_host = (bmRequestType & 0x80) != 0;
    const std::uint8_t request_type = bmRequestType & 0x60;

    // TEMP DIAGNOSTIC: dump every control transfer so we can see exactly
    // what Windows asks for during enumeration. Remove once the stall is
    // diagnosed.
    if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_ctrl_debug.log", "a")) {
      std::fprintf(dbg,
                    "ep0 bmRequestType=0x%02x bRequest=0x%02x wValue=0x%04x "
                    "wIndex=0x%04x wLength=%u\\n",
                    bmRequestType, bRequest, wValue, wIndex, wLength);
      std::fclose(dbg);
    }

    if (request_type == 0x00 && bRequest == 0x06 && device_to_host) {
      // GET_DESCRIPTOR
      const std::uint8_t desc_type = static_cast<std::uint8_t>(wValue >> 8);
      const std::uint8_t desc_index = static_cast<std::uint8_t>(wValue & 0xff);
      const std::uint8_t *src = nullptr;
      std::size_t len = 0;
      if (desc_type == 0x01) { // DEVICE
        src = desc.device;
        len = desc.device_len;
      } else if (desc_type == 0x02) { // CONFIGURATION
        src = desc.config;
        len = desc.config_len;
      } else if (desc_type == 0x21) { // HID
        src = desc.hid;
        len = desc.hid_len;
      } else if (desc_type == 0x22) { // HID REPORT
        src = desc.hid_report;
        len = desc.hid_report_len;
      } else if (desc_type == 0x03) { // STRING
        // Index 0 is the language-ID list; index 1 is iManufacturer,
        // index 2 is iProduct (per the device descriptor byte layout in
        // ds5_usb.h -- iManufacturer=1, iProduct=2, iSerialNumber=0/none).
        std::vector<std::uint8_t> built;
        if (desc_index == 0) {
          built = build_langid_descriptor();
        } else if (desc_index == 1) {
          built = build_string_descriptor(VDS_USB_MANUFACTURER_STRING);
        } else if (desc_index == 2) {
          built = build_string_descriptor(
              profile == VDS_PROFILE_DSE ? VDS_DSE_USB_PRODUCT_STRING
                                          : VDS_DS5_USB_PRODUCT_STRING);
        }
        if (!built.empty()) {
          const std::size_t copy_len = std::min<std::size_t>(built.size(), wLength);
          reply_data.assign(built.begin(), built.begin() + copy_len);
        }
        ret.status = 0;
        src = nullptr;
        len = 0;
      }
      if (src != nullptr && len > 0) {
        const std::size_t copy_len = std::min<std::size_t>(len, wLength);
        reply_data.assign(src, src + copy_len);
      }
      ret.status = 0;
    } else if (request_type == 0x00 && bRequest == 0x09) {
      // SET_CONFIGURATION -- accept, no frame to forward (interfaces start
      // at altsetting 0 / inactive until SET_INTERFACE).
      ret.status = 0;
    } else if (bmRequestType == 0x01 && bRequest == 0x0b) {
      // SET_INTERFACE(interface=wIndex, altsetting=wValue). Forward to
      // vdsd exactly as vds_usb.sys did via VDS_FRAME_USB_INTERFACE so
      // handle_virtual_frame()'s existing audio-route/mic-open logic keeps
      // working unmodified.
      vds_usb_interface_event event{};
      event.interface_number = static_cast<std::uint8_t>(wIndex);
      event.altsetting = static_cast<std::uint8_t>(wValue);
      if (wIndex == VDS_USB_AUDIO_OUT_INTERFACE) {
        event.interface_type = VDS_USB_INTERFACE_AUDIO_OUT;
      } else if (wIndex == VDS_USB_AUDIO_IN_INTERFACE) {
        event.interface_type = VDS_USB_INTERFACE_AUDIO_IN;
        // Flush the mic jitter buffer on every mic start/stop so a fresh
        // recording session doesn't inherit stale audio or a half-full
        // cushion from a previous one; re-prime from empty.
        {
          std::lock_guard<std::mutex> lk(demux_mutex);
          mic_ring_buffer.clear();
          mic_primed_ = false;
        }
      } else {
        event.interface_type = VDS_USB_INTERFACE_HID;
      }
      std::array<std::uint8_t, sizeof(event)> bytes{};
      std::memcpy(bytes.data(), &event, sizeof(event));
      write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_INTERFACE, bytes);
      ret.status = 0;
    } else if ((bmRequestType & 0x60) == 0x20 && bRequest == 0x01 &&
              device_to_host) {
      // HID GET_REPORT (feature). Ask vdsd, which caches/forwards over
      // Bluetooth as needed, then wait for the FEATURE_REPLY frame.
      const std::uint8_t report_id = static_cast<std::uint8_t>(wValue & 0xff);
      const std::array<std::uint8_t, 1> request{report_id};
      write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_FEATURE_GET, request);
      Frame reply;
      wait_for_feature_reply(reply, 500);
      if (reply.header.type == VDS_FRAME_USB_FEATURE_REPLY) {
        reply_data = reply.payload;
      }
      ret.status = 0;
    } else if ((bmRequestType & 0x60) == 0x20 && bRequest == 0x09) {
      // HID SET_REPORT (feature/output via control pipe).
      write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_FEATURE_SET, out_data);
      ret.status = 0;
    } else if ((bmRequestType & 0x60) == 0x20 &&
               (bmRequestType & 0x1f) == 0x01 &&
               (bRequest == 0x81 || bRequest == 0x82 || bRequest == 0x83 ||
                bRequest == 0x84 || bRequest == 0x01)) {
      // USB Audio Class 1.0 Feature Unit control request (GET_CUR/MIN/MAX/RES
      // or SET_CUR) targeting a control selector (wValue hi byte) on an
      // entity (wIndex hi byte). usbaudio.sys queries these (typically
      // MUTE_CONTROL=0x01, VOLUME_CONTROL=0x02) during IRP_MN_START_DEVICE;
      // a compliant device must answer rather than STALL, or the class
      // driver fails to start. We don't have a real mixer to back these, so
      // just report sane fixed values (unmuted, 0dB, wide min/max range).
      const std::uint8_t control_selector =
          static_cast<std::uint8_t>((wValue >> 8) & 0xff);
      const bool is_mute = control_selector == 0x01;
      const bool is_volume = control_selector == 0x02;
      if (device_to_host && (is_mute || is_volume)) {
        if (is_mute) {
          reply_data.assign({fu_muted.load() ? std::uint8_t{0x01}
                                             : std::uint8_t{0x00}});
        } else if (bRequest == 0x82) { // GET_MIN
          reply_data.assign({0x01, 0xda}); // -100.00 dB (0xDA01, 1/256 dB units)
        } else if (bRequest == 0x83) { // GET_MAX
          reply_data.assign({0x00, 0x00}); // 0.00 dB
        } else if (bRequest == 0x84) { // GET_RES
          reply_data.assign({0x01, 0x00}); // 1/256 dB step
        } else { // GET_CUR -- report the value the host last set
          const auto cur = static_cast<std::uint16_t>(
              static_cast<std::int16_t>(fu_volume_db256.load()));
          reply_data.assign({static_cast<std::uint8_t>(cur & 0xff),
                             static_cast<std::uint8_t>((cur >> 8) & 0xff)});
        }
        const std::size_t copy_len = std::min<std::size_t>(reply_data.size(), wLength);
        reply_data.resize(copy_len);
      } else if (!device_to_host && bRequest == 0x01) {
        // SET_CUR: actually apply it (previously accepted and discarded,
        // which left the Windows volume slider nonfunctional and audio
        // pinned at full scale). Payload arrives in the control OUT data
        // stage (out_data).
        if (is_mute && !out_data.empty()) {
          fu_muted = out_data[0] != 0;
          update_speaker_gain();
        } else if (is_volume && out_data.size() >= 2) {
          fu_volume_db256 = static_cast<std::int16_t>(
              static_cast<std::uint16_t>(out_data[0]) |
              (static_cast<std::uint16_t>(out_data[1]) << 8));
          update_speaker_gain();
        }
        if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_ctrl_debug.log", "a")) {
          std::fprintf(dbg, "FU SET_CUR selector=%u muted=%d vol_db256=%d gain_q15=%d\\n",
                       control_selector, fu_muted.load() ? 1 : 0,
                       fu_volume_db256.load(), speaker_gain_q15.load());
          std::fclose(dbg);
        }
      }
      ret.status = 0;
    } else {
      // Unhandled/unknown control request: STALL.
      ret.status = -32; // -EPIPE
    }
  } else if (cmd.base.ep == kEpHidIn && cmd.base.direction == kDirIn) {
    Frame frame;
    if (wait_for_hid_in_frame(frame, 50)) {
      reply_data = frame.payload;
    }
    ret.status = 0;
  } else if (cmd.base.ep == kEpHidOut && cmd.base.direction == kDirOut) {
    write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_HID_OUT, out_data);
    ret.status = 0;
  } else if (cmd.base.ep == kEpAudioOut && cmd.base.direction == kDirOut) {
    // Isochronous OUT (speaker/haptics). Treat the whole URB's packets as one
    // contiguous PCM buffer, matching what VDS_FRAME_USB_AUDIO_OUT already
    // expects from vds_usb.sys.
    //
    // Pacing: vds_usb.sys deliberately paces ISO URB completion (~1ms per
    // packet, see VdsCompleteUrbAfterDelay) instead of completing instantly,
    // specifically to stop Windows from submitting seconds of PCM as a burst
    // that overflows the Bluetooth write queue in vdsd. Our loopback TCP path
    // has no real USB bus timing to throttle it, so without an equivalent
    // delay here we get exactly that burst -- this was the actual cause of
    // the pipe teardown immediately after streaming activated.
    // Apply the host-set Feature Unit volume to the speaker channels
    // (ch0/1) only; ch2/3 are haptics and must not follow system volume.
    // 4-channel 16-bit interleaved: 8 bytes per frame.
    const int gain_q15 = speaker_gain_q15.load();
    std::vector<std::uint8_t> gained;
    std::span<const std::uint8_t> pcm_to_forward = out_data;
    if (gain_q15 != (1 << 15)) {
      gained.assign(out_data.begin(), out_data.end());
      for (std::size_t off = 0; off + 8 <= gained.size(); off += 8) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
          const std::size_t idx = off + ch * 2;
          const auto sample = static_cast<std::int16_t>(
              static_cast<std::uint16_t>(gained[idx]) |
              (static_cast<std::uint16_t>(gained[idx + 1]) << 8));
          const int scaled = (static_cast<int>(sample) * gain_q15) >> 15;
          const auto clamped = static_cast<std::int16_t>(
              scaled > 32767 ? 32767 : (scaled < -32768 ? -32768 : scaled));
          gained[idx] = static_cast<std::uint8_t>(clamped & 0xff);
          gained[idx + 1] =
              static_cast<std::uint8_t>((clamped >> 8) & 0xff);
        }
      }
      pcm_to_forward = gained;
    }
    write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_AUDIO_OUT,
                       pcm_to_forward);
    ret.status = 0;
    ret.number_of_packets = cmd.number_of_packets;
    // Completion pacing happens at the send stage below (see the PacedReply
    // member comments): the reply for this URB is handed to the pacer
    // thread and sent at its real-time due moment instead of instantly.
    // (An earlier blocking-sleep pacing attempt was removed as it stalled
    // the shared session thread; the dedicated pacer avoids that.)
  } else if (cmd.base.ep == kEpAudioIn && cmd.base.direction == kDirIn) {
    // Isochronous IN (mic). Answer with exactly transfer_buffer_length bytes
    // from the jitter buffer (zero-padded on underrun) so the reply is
    // wire-consistent. Crucially, this reply is PACED at the send stage
    // below, exactly like audio OUT: without pacing, Windows completes and
    // resubmits isoc IN URBs as fast as the loopback allows (~10x real
    // time), draining the buffer far faster than the Bluetooth mic can fill
    // it -- so ~90% of every reply was zero-pad (measured recv/req ratio
    // ~0.09) and the mic was pure stutter. Pacing throttles Windows to the
    // real 48kHz rate so each URB carries the audio that actually arrived in
    // that interval.
    reply_data = take_mic_bytes(static_cast<std::size_t>(cmd.transfer_buffer_length));
    ret.status = 0;
    ret.number_of_packets = cmd.number_of_packets;
  } else {
    ret.status = -32; // -EPIPE: unknown endpoint
  }

  if (cmd.base.ep == 0) {
    if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_ctrl_debug.log", "a")) {
      std::fprintf(dbg, "  -> status=%d actual_length=%zu\\n",
                    static_cast<int>(ret.status), reply_data.size());
      std::fclose(dbg);
    }
  }

  std::vector<UsbipIsoPacketDescriptor> reply_iso;
  std::uint32_t iso_actual_total = 0;
  if (cmd.number_of_packets > 0) {
    // Build the iso reply array up front (not just before sending it) so the
    // RET_SUBMIT header's actual_length can be the sum of per-packet
    // actual_lengths -- usbip-win2's client validates that invariant, and a
    // mismatch (previously: header always used reply_data.size(), which is 0
    // for OUT transfers since OUT has no reply payload) silently poisons the
    // connection right after the first real iso OUT submission.
    reply_iso.resize(iso_descriptors.size());
    for (std::size_t i = 0; i < reply_iso.size(); ++i) {
      const std::uint32_t off = iso_descriptors[i].offset;
      const std::uint32_t len = iso_descriptors[i].length;
      std::int32_t actual = static_cast<std::int32_t>(len);
      if (cmd.base.direction == kDirIn) {
        actual = (off >= reply_data.size())
                     ? 0
                     : static_cast<std::int32_t>(
                           std::min<std::size_t>(len, reply_data.size() - off));
      }
      reply_iso[i].offset = hton32(off);
      reply_iso[i].length = hton32(len);
      reply_iso[i].actual_length = hton32(static_cast<std::uint32_t>(actual));
      reply_iso[i].status = hton32(0);
      iso_actual_total += static_cast<std::uint32_t>(actual);
    }
  }

  ret.actual_length = hton32(cmd.number_of_packets > 0
                                  ? iso_actual_total
                                  : static_cast<std::uint32_t>(reply_data.size()));
  ret.status = hton32(static_cast<std::uint32_t>(ret.status));
  // BUG FIX: number_of_packets (and start_frame/error_count) must be sent
  // in network byte order like status/actual_length -- this field was being
  // copied straight from the host-order cmd.number_of_packets and never
  // swapped. For HID (number_of_packets == -1, i.e. 0xFFFFFFFF) this is
  // byte-order symmetric so it looked fine all session; the moment real
  // isochronous audio carried a real nonzero packet count (10), usbip-win2
  // received a garbage value in a strict, validated field and reset the
  // TCP connection right after the first reply -- exactly matching the
  // observed "dies on the second ISO OUT submission" symptom.
  ret.number_of_packets = hton32(static_cast<std::uint32_t>(ret.number_of_packets));
  ret.start_frame = hton32(0);
  ret.error_count = hton32(0);

  // Isochronous replies (audio OUT = speaker/haptics, audio IN = mic) are
  // paced instead of sent inline -- see the PacedReply member comments.
  // Serialize the complete RET_SUBMIT and hand it to the pacer with a
  // real-time due moment: 1ms of audio per iso packet, anchored to the
  // previous URB's due time in that direction so a burst of early
  // submissions from Windows drains at exactly real time. If the stream
  // pauses (>250ms since the last due), the anchor resets so a new stream
  // does not inherit stale lateness or a far-future anchor.
  // Wire layout differs by direction: OUT carries header + iso descriptors
  // (no data); IN carries header + data payload + iso descriptors.
  const bool pace_out =
      cmd.base.ep == kEpAudioOut && cmd.base.direction == kDirOut &&
      cmd.number_of_packets > 0;
  const bool pace_in =
      cmd.base.ep == kEpAudioIn && cmd.base.direction == kDirIn &&
      cmd.number_of_packets > 0;
  if (pace_out || pace_in) {
    PacedReply paced{};
    paced.client = client;
    const std::size_t data_len = pace_in ? reply_data.size() : 0;
    const std::size_t iso_len =
        reply_iso.size() * sizeof(UsbipIsoPacketDescriptor);
    paced.bytes.resize(sizeof(ret) + data_len + iso_len);
    std::memcpy(paced.bytes.data(), &ret, sizeof(ret));
    if (data_len > 0) {
      std::memcpy(paced.bytes.data() + sizeof(ret), reply_data.data(), data_len);
    }
    if (iso_len > 0) {
      std::memcpy(paced.bytes.data() + sizeof(ret) + data_len, reply_iso.data(),
                  iso_len);
    }
    {
      std::lock_guard<std::mutex> guard(pacer_mutex);
      auto &anchor = pace_out ? iso_out_next_due : iso_in_next_due;
      const auto now = std::chrono::steady_clock::now();
      if (anchor == std::chrono::steady_clock::time_point{} ||
          anchor < now - std::chrono::milliseconds(250)) {
        anchor = now; // stream (re)start: first URB completes now
      }
      paced.due = anchor;
      anchor += std::chrono::milliseconds(cmd.number_of_packets);
      paced_replies.push_back(std::move(paced));
    }
    pacer_cv.notify_all();
    return true;
  }

  std::lock_guard<std::mutex> send_guard(socket_send_mutex);
  if (!send_exact(client, &ret, sizeof(ret))) {
    if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: send ret header failed seqnum=%u ep=%u\n", cmd.base.seqnum, cmd.base.ep); std::fclose(dbg); }
    return false;
  }
  if (!reply_data.empty() && !send_exact(client, reply_data.data(), reply_data.size())) {
    if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: send reply_data failed seqnum=%u ep=%u len=%zu\n", cmd.base.seqnum, cmd.base.ep, reply_data.size()); std::fclose(dbg); }
    return false;
  }
  if (cmd.number_of_packets > 0) {
    // reply_iso was already built above (needed early so the RET_SUBMIT
    // header's actual_length could be the sum of these packets' actual
    // lengths). Just send it here.
    if (!reply_iso.empty() &&
        !send_exact(client, reply_iso.data(),
                    reply_iso.size() * sizeof(UsbipIsoPacketDescriptor))) {
      if (FILE *dbg = std::fopen("C:\\\\ProgramData\\\\vDS\\\\usbip_cmd_debug.log", "a")) { std::fprintf(dbg, "BREAK: send reply_iso failed seqnum=%u ep=%u npkts=%zu\n", cmd.base.seqnum, cmd.base.ep, reply_iso.size()); std::fclose(dbg); }
      return false;
    }
  }
  return true;
}

VirtualPort::VirtualPort(std::uint32_t profile, unsigned port_index,
                        std::uint16_t tcp_port)
    : impl_(std::make_unique<Impl>(profile, port_index, tcp_port)) {
  impl_->start();
  pipe_path_ = impl_->pipe_path;
}

VirtualPort::~VirtualPort() {
  if (impl_) {
    impl_->stop();
  }
}

bool VirtualPort::usb_attached() const { return impl_->attached.load(); }

std::unique_ptr<VirtualPort> open_usbip_virtual_port(std::uint32_t profile,
                                                     unsigned port_index) {
  return std::make_unique<VirtualPort>(
      profile, port_index,
      static_cast<std::uint16_t>(kDefaultPort + port_index));
}

} // namespace vds::win::usbip
