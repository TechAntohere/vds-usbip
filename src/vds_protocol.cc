// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
//
// Partly influenced by DS5Dongle source:
// Copyright (c) 2026 awalol, released under the MIT license.
//
// DualSense USB/Bluetooth protocol helpers: CRC, output state merging, HID
// report framing, and USB audio to Bluetooth haptics/speaker packet conversion.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include <opus/opus.h>

#include "uapi/vds.h"
#include "vds_protocol.hh"

namespace vds {

namespace {

constexpr std::uint32_t kOutputCrcSeed = 0xeada2d49;
constexpr std::uint32_t kFeatureCrcSeed = 0x2060efc3;
constexpr std::size_t kPcmChannels = VDS_AUDIO_CHANNELS;
constexpr std::size_t kPcmSampleSize = sizeof(std::int16_t);
constexpr std::size_t kPcmFrameSize = kPcmChannels * kPcmSampleSize;
constexpr std::uint32_t kHapticsDecimation =
    VDS_AUDIO_SAMPLE_RATE / VDS_HAPTICS_SAMPLE_RATE;
constexpr std::size_t kSetStateSize = VDS_SET_STATE_SIZE;
constexpr std::size_t kBtStateOffset = 3;
constexpr std::size_t kSpeakerBlockOffset = 142;
constexpr std::size_t kSpeakerDataOffset = 144;
constexpr int kSpeakerOpusBitrate = kSpeakerOpusSize * 8 * 100;
constexpr std::uint8_t kBtHapticsAudioBufferLength = 128;
constexpr std::size_t kOutputFlag0Offset = 0;
constexpr std::size_t kOutputFlag1Offset = 1;
constexpr std::size_t kOutputHeadphoneVolumeOffset = 4;
constexpr std::size_t kOutputSpeakerVolumeOffset = 5;
constexpr std::size_t kOutputAudioControlOffset = 7;
constexpr std::size_t kOutputAudioControl2Offset = 37;
constexpr std::uint8_t kOutputFlag0HeadphoneVolumeEnable = 0x10;
constexpr std::uint8_t kOutputFlag0SpeakerVolumeEnable = 0x20;
constexpr std::uint8_t kOutputFlag0AudioControlEnable = 0x80;
constexpr std::uint8_t kOutputFlag1AudioControl2Enable = 0x80;
constexpr std::uint8_t kOutputHeadphoneVolumeMax = 0x7f;
constexpr std::uint8_t kOutputSpeakerVolumeMax = 0x64;
constexpr std::uint8_t kOutputPathHeadphones = 0x00;
constexpr std::uint8_t kOutputPathSpeaker = 0x30;
constexpr std::uint8_t kOutputSpeakerPreampGain = 0x02;
constexpr DsState kInitialDsState{
    0xfd, 0xf7, 0x00, 0x00, 0x7f, 0x64, 0xff, 0x09, 0x00, 0x0f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0a, 0x07, 0x00, 0x00, 0x02, 0x01, 0x00, 0xff, 0xd7, 0x00};

constexpr std::uint32_t crc32_table_entry(std::uint32_t index) {
  for (unsigned bit = 0; bit < 8; ++bit) {
    index = (index >> 1) ^ (0xedb88320u & (0u - (index & 1u)));
  }
  return index;
}

constexpr auto make_crc32_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    table[i] = crc32_table_entry(i);
  }
  return table;
}

constexpr auto kCrc32Table = make_crc32_table();

std::int8_t s16_to_s8_haptic(std::int16_t sample) {
  const int value = std::clamp(static_cast<int>(sample / 256), -128, 127);
  return static_cast<std::int8_t>(value);
}

std::int16_t read_le_s16(const std::uint8_t *data) {
  const auto lo = static_cast<std::uint16_t>(data[0]);
  const auto hi = static_cast<std::uint16_t>(data[1]) << 8;
  return static_cast<std::int16_t>(lo | hi);
}

std::int16_t lerp_i16(std::int16_t left, std::int16_t right,
                      std::uint32_t numerator, std::uint32_t denominator) {
  const auto left_part = static_cast<std::int32_t>(left) *
                         static_cast<std::int32_t>(denominator - numerator);
  const auto right_part =
      static_cast<std::int32_t>(right) * static_cast<std::int32_t>(numerator);
  const auto mixed = static_cast<int>((left_part + right_part) /
                                      static_cast<std::int32_t>(denominator));
  return static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
}

std::array<opus_int16, kSpeakerOpusFrames * kSpeakerChannels>
resample_speaker_input(
    std::span<const std::int16_t, kSpeakerInputFrames * kSpeakerChannels>
        input) {
  std::array<opus_int16, kSpeakerOpusFrames * kSpeakerChannels> output{};

  // USB audio is accumulated in 512-frame speaker windows, then resampled into
  // one 10 ms, 480-frame Opus speaker block for the 0x36 packet.
  for (std::size_t frame = 0; frame < kSpeakerOpusFrames; ++frame) {
    const std::size_t source_numerator = frame * kSpeakerInputFrames;
    const std::size_t source_frame = source_numerator / kSpeakerOpusFrames;
    const auto fraction =
        static_cast<std::uint32_t>(source_numerator % kSpeakerOpusFrames);
    const std::size_t next_frame =
        std::min(source_frame + 1, kSpeakerInputFrames - 1);

    for (std::size_t channel = 0; channel < kSpeakerChannels; ++channel) {
      output[frame * kSpeakerChannels + channel] =
          lerp_i16(input[source_frame * kSpeakerChannels + channel],
                   input[next_frame * kSpeakerChannels + channel], fraction,
                   kSpeakerOpusFrames);
    }
  }

  return output;
}

void opus_ctl_checked(int result, const char *name) {
  if (result != OPUS_OK) {
    throw std::runtime_error(std::string(name) +
                             " failed: " + opus_strerror(result));
  }
}

std::uint32_t crc32_seeded(std::span<const std::uint8_t> data,
                           std::uint32_t seed) {
  std::uint32_t crc = ~seed;

  for (const std::uint8_t byte : data) {
    crc = (crc >> 8) ^ kCrc32Table[(crc ^ byte) & 0xffu];
  }

  return ~crc;
}

std::uint32_t output_crc32(std::span<const std::uint8_t> data) {
  return crc32_seeded(data, kOutputCrcSeed);
}

std::uint32_t feature_crc32(std::span<const std::uint8_t> data) {
  return crc32_seeded(data, kFeatureCrcSeed);
}

void copy_state_bytes(DsState &state, std::span<const std::uint8_t> update,
                      std::size_t offset, std::size_t length) {
  std::copy(update.begin() + static_cast<std::ptrdiff_t>(offset),
            update.begin() + static_cast<std::ptrdiff_t>(offset + length),
            state.begin() + static_cast<std::ptrdiff_t>(offset));
}

void set_state_bit(std::uint8_t &byte, int bit, bool value) {
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << bit);
  byte = static_cast<std::uint8_t>((byte & ~mask) |
                                   (value ? mask : std::uint8_t{0}));
}

} // namespace

struct PcmAudioExtractor::SpeakerEncoder {
  SpeakerEncoder() {
    int error = OPUS_OK;
    encoder = opus_encoder_create(VDS_AUDIO_SAMPLE_RATE, kSpeakerChannels,
                                  OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) {
      throw std::runtime_error("opus_encoder_create failed: " +
                               std::string(opus_strerror(error)));
    }

    opus_ctl_checked(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)),
                     "OPUS_SET_VBR");
    opus_ctl_checked(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)),
                     "OPUS_SET_COMPLEXITY");
    opus_ctl_checked(opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(
                                                   OPUS_FRAMESIZE_10_MS)),
                     "OPUS_SET_EXPERT_FRAME_DURATION");
    opus_ctl_checked(
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kSpeakerOpusBitrate)),
        "OPUS_SET_BITRATE");
  }

  ~SpeakerEncoder() {
    if (encoder) {
      opus_encoder_destroy(encoder);
    }
  }

  SpeakerEncoder(const SpeakerEncoder &) = delete;
  SpeakerEncoder &operator=(const SpeakerEncoder &) = delete;

  SpeakerChunk
  encode(std::span<const std::int16_t, kSpeakerInputFrames * kSpeakerChannels>
             input) {
    SpeakerChunk output{};
    const auto opus_input = resample_speaker_input(input);
    const int bytes =
        opus_encode(encoder, opus_input.data(), kSpeakerOpusFrames,
                    output.data(), static_cast<opus_int32>(output.size()));
    if (bytes < 0) {
      throw std::runtime_error("opus_encode failed: " +
                               std::string(opus_strerror(bytes)));
    }
    if (static_cast<std::size_t>(bytes) < output.size()) {
      std::fill(output.begin() + static_cast<std::ptrdiff_t>(bytes),
                output.end(), 0);
    }
    return output;
  }

  OpusEncoder *encoder = nullptr;
};

PcmAudioExtractor::PcmAudioExtractor()
    : speaker_encoder_(std::make_unique<SpeakerEncoder>()) {}

PcmAudioExtractor::~PcmAudioExtractor() = default;

PcmAudioExtractor::PcmAudioExtractor(PcmAudioExtractor &&) noexcept = default;

PcmAudioExtractor &
PcmAudioExtractor::operator=(PcmAudioExtractor &&) noexcept = default;

void fill_output_report_checksum(std::span<std::uint8_t> report) {
  if (report.size() < sizeof(std::uint32_t)) {
    throw std::invalid_argument("output report is too small for CRC");
  }
  const std::uint32_t crc =
      output_crc32(report.first(report.size() - sizeof(std::uint32_t)));
  const std::size_t offset = report.size() - sizeof(std::uint32_t);
  report[offset + 0] = static_cast<std::uint8_t>((crc >> 0) & 0xff);
  report[offset + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
  report[offset + 2] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
  report[offset + 3] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
}

void fill_feature_report_checksum(std::span<std::uint8_t> report) {
  if (report.size() < sizeof(std::uint32_t)) {
    throw std::invalid_argument("feature report is too small for CRC");
  }
  const std::uint32_t crc =
      feature_crc32(report.first(report.size() - sizeof(std::uint32_t)));
  const std::size_t offset = report.size() - sizeof(std::uint32_t);
  report[offset + 0] = static_cast<std::uint8_t>((crc >> 0) & 0xff);
  report[offset + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
  report[offset + 2] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
  report[offset + 3] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
}

DsOutputState::DsOutputState() : state_(kInitialDsState) {}

BtInitReport DsOutputState::build_bt_init_report() {
  BtInitReport report{};
  report[0] = 0x32;
  report[1] = 0x10;
  report[2] = 0x10 | (1 << 7);
  report[3] = kDsStateSize;
  std::copy(state_.begin(), state_.end(), report.begin() + 4);
  fill_output_report_checksum(report);
  return report;
}

bool DsOutputState::apply_usb_output_report(
    std::span<const std::uint8_t> report) {
  if (report.size() < 1 + kSetStateSize ||
      report[0] != VDS_USB_OUTPUT_REPORT_ID) {
    return false;
  }

  const std::span update = report.subspan(1, kSetStateSize);
  vds_set_state_data decoded{};
  std::memcpy(&decoded, update.data(), sizeof(decoded));

  set_state_bit(state_[0], 0, decoded.enable_rumble_emulation);
  set_state_bit(state_[0], 1, decoded.use_rumble_not_haptics);
  set_state_bit(state_[38], 2, decoded.enable_improved_rumble_emulation);

  if (decoded.use_rumble_not_haptics || decoded.enable_rumble_emulation) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, rumble_emulation_right), 2);
  }
  if (decoded.allow_headphone_volume) {
    state_[kOutputFlag0Offset] |= kOutputFlag0HeadphoneVolumeEnable;
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, volume_headphones),
                     sizeof(decoded.volume_headphones));
  }
  if (decoded.allow_speaker_volume) {
    state_[kOutputFlag0Offset] |= kOutputFlag0SpeakerVolumeEnable;
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, volume_speaker),
                     sizeof(decoded.volume_speaker));
  }
  if (decoded.allow_audio_control) {
    state_[kOutputFlag0Offset] |= kOutputFlag0AudioControlEnable;
    copy_state_bytes(state_, update, kOutputAudioControlOffset, 1);
  }
  if (decoded.allow_audio_control2) {
    state_[kOutputFlag1Offset] |= kOutputFlag1AudioControl2Enable;
    copy_state_bytes(state_, update, kOutputAudioControl2Offset, 1);
  }
  if (decoded.allow_speaker_volume && decoded.volume_speaker != 0) {
    state_[kOutputFlag0Offset] |=
        kOutputFlag0AudioControlEnable | kOutputFlag0SpeakerVolumeEnable;
    state_[kOutputFlag1Offset] |= kOutputFlag1AudioControl2Enable;
    state_[kOutputAudioControlOffset] = kOutputPathSpeaker;
    state_[kOutputAudioControl2Offset] = kOutputSpeakerPreampGain;
  }
  if (decoded.allow_mute_light) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, mute_light_mode),
                     sizeof(decoded.mute_light_mode));
  }
  if (decoded.allow_right_trigger_ffb) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, right_trigger_ffb),
                     sizeof(decoded.right_trigger_ffb));
  }
  if (decoded.allow_left_trigger_ffb) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, left_trigger_ffb),
                     sizeof(decoded.left_trigger_ffb));
  }
  if (decoded.allow_color_light_fade_animation) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, light_fade_animation),
                     sizeof(decoded.light_fade_animation));
  }
  if (decoded.allow_light_brightness_change) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, light_brightness),
                     sizeof(decoded.light_brightness));
  }
  if (decoded.allow_player_indicators) {
    // player_light* is a bitfield byte immediately before led_red.
    copy_state_bytes(state_, update, offsetof(vds_set_state_data, led_red) - 1,
                     1);
  }
  if (decoded.allow_led_color) {
    copy_state_bytes(state_, update, offsetof(vds_set_state_data, led_red), 3);
  }
  return true;
}

void DsOutputState::set_audio_out_stream_active(bool active) {
  state_[kOutputFlag0Offset] |= kOutputFlag0AudioControlEnable;
  state_[kOutputHeadphoneVolumeOffset] = kOutputHeadphoneVolumeMax;

  if (active) {
    state_[kOutputFlag0Offset] |= kOutputFlag0SpeakerVolumeEnable;
    state_[kOutputFlag1Offset] |= kOutputFlag1AudioControl2Enable;
    state_[kOutputSpeakerVolumeOffset] = kOutputSpeakerVolumeMax;
    state_[kOutputAudioControlOffset] = kOutputPathSpeaker;
    state_[kOutputAudioControl2Offset] = kOutputSpeakerPreampGain;
    return;
  }

  state_[kOutputFlag0Offset] &= ~kOutputFlag0SpeakerVolumeEnable;
  state_[kOutputFlag1Offset] &= ~kOutputFlag1AudioControl2Enable;
  state_[kOutputSpeakerVolumeOffset] = 0;
  state_[kOutputAudioControlOffset] = kOutputPathHeadphones;
  state_[kOutputAudioControl2Offset] = 0;
}

BtStateReport DsOutputState::build_bt_state_report() {
  BtStateReport report{};
  report[0] = VDS_BT_STATE_REPORT_ID;
  report[1] = report_sequence_ << 4;
  report_sequence_ = (report_sequence_ + 1) & 0x0f;
  report[2] = 0x10;
  std::copy(state_.begin(), state_.begin() + kSetStateSize,
            report.begin() + kBtStateOffset);
  fill_output_report_checksum(report);
  return report;
}

BtReport HapticsPacketBuilder::build_packet(
    std::span<const std::int8_t, kHapticsSampleSize> haptics,
    std::span<const std::uint8_t, kSpeakerOpusSize> speaker,
    std::span<const std::uint8_t, kDsStateSize> state) {
  BtReport packet{};
  packet[0] = VDS_BT_HAPTICS_REPORT_ID;
  packet[1] = report_sequence_ << 4;
  report_sequence_ = (report_sequence_ + 1) & 0x0f;
  packet[2] = 0x11 | (1 << 7);
  packet[3] = 7;
  packet[4] = 0xfe;
  /*
   * The controller accepts a larger haptics buffer length than the nominal
   * 64-byte sample block. Use the largest observed working value to absorb
   * Linux userspace scheduling jitter and reduce Bluetooth audio underruns.
   */
  packet[5] = kBtHapticsAudioBufferLength;
  packet[6] = kBtHapticsAudioBufferLength;
  packet[7] = kBtHapticsAudioBufferLength;
  packet[8] = kBtHapticsAudioBufferLength;
  packet[9] = kBtHapticsAudioBufferLength;
  packet[10] = packet_sequence_++;
  packet[11] = 0x10 | (1 << 7);
  packet[12] = kDsStateSize;
  std::copy(state.begin(), state.end(), packet.begin() + 13);
  packet[76] = 0x12 | (1 << 7);
  packet[77] = kHapticsSampleSize;
  std::memcpy(packet.data() + 78, haptics.data(), haptics.size());
  packet[kSpeakerBlockOffset] = 0x13 | (1 << 7);
  packet[kSpeakerBlockOffset + 1] = static_cast<std::uint8_t>(kSpeakerOpusSize);
  std::memcpy(packet.data() + kSpeakerDataOffset, speaker.data(),
              speaker.size());
  fill_output_report_checksum(packet);
  return packet;
}

std::vector<AudioChunk>
PcmAudioExtractor::push_usb_audio(std::span<const std::uint8_t> pcm_bytes) {
  std::vector<AudioChunk> chunks;
  chunks.reserve(pcm_bytes.size() / (kPcmFrameSize * kSpeakerInputFrames) + 1);

  const auto consume_frame = [this, &chunks](const std::uint8_t *base) {
    std::array<std::int16_t, kPcmChannels> samples{};
    for (std::size_t channel = 0; channel < kPcmChannels; ++channel) {
      samples[channel] = read_le_s16(base + channel * kPcmSampleSize);
      chunk_has_signal_ |= samples[channel] != 0;
    }

    speaker_input_[speaker_frame_pos_ * kSpeakerChannels + 0] = samples[0];
    speaker_input_[speaker_frame_pos_ * kSpeakerChannels + 1] = samples[1];
    ++speaker_frame_pos_;

    if (++decimation_phase_ == kHapticsDecimation) {
      decimation_phase_ = 0;
      haptics_chunk_[haptics_chunk_pos_++] = s16_to_s8_haptic(samples[2]);
      haptics_chunk_[haptics_chunk_pos_++] = s16_to_s8_haptic(samples[3]);
    }

    if (speaker_frame_pos_ != kSpeakerInputFrames) {
      return;
    }

    speaker_frame_pos_ = 0;
    AudioChunk chunk{};
    chunk.haptics = haptics_chunk_;
    chunk.speaker = speaker_encoder_->encode(speaker_input_);
    chunk.has_signal = chunk_has_signal_;
    chunks.push_back(chunk);
    haptics_chunk_.fill(0);
    haptics_chunk_pos_ = 0;
    chunk_has_signal_ = false;
  };

  while (!pcm_bytes.empty()) {
    if (pending_frame_pos_ == 0 && pcm_bytes.size() >= kPcmFrameSize) {
      const std::size_t frames = pcm_bytes.size() / kPcmFrameSize;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        consume_frame(pcm_bytes.data() + frame * kPcmFrameSize);
      }
      pcm_bytes = pcm_bytes.subspan(frames * kPcmFrameSize);
      continue;
    }

    const std::size_t copy_size =
        std::min(pcm_bytes.size(), kPcmFrameSize - pending_frame_pos_);
    std::copy_n(pcm_bytes.begin(), copy_size,
                pending_frame_.begin() +
                    static_cast<std::ptrdiff_t>(pending_frame_pos_));
    pending_frame_pos_ += copy_size;
    pcm_bytes = pcm_bytes.subspan(copy_size);
    if (pending_frame_pos_ == kPcmFrameSize) {
      consume_frame(pending_frame_.data());
      pending_frame_pos_ = 0;
    }
  }

  return chunks;
}

std::vector<std::uint8_t> frame_bytes(std::uint16_t type,
                                      std::span<const std::uint8_t> payload) {
  if (payload.size() > VDS_FRAME_MAX_PAYLOAD) {
    throw std::invalid_argument("frame payload exceeds VDS_FRAME_MAX_PAYLOAD");
  }

  vds_frame_header header{};
  header.type = type;
  header.length = static_cast<std::uint32_t>(payload.size());

  std::vector<std::uint8_t> bytes(sizeof(header) + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
  return bytes;
}

std::string frame_type_name(std::uint16_t type) {
  switch (type) {
  case VDS_FRAME_STATUS:
    return "STATUS";
  case VDS_FRAME_USB_HID_OUT:
    return "USB_HID_OUT";
  case VDS_FRAME_USB_FEATURE_GET:
    return "USB_FEATURE_GET";
  case VDS_FRAME_USB_FEATURE_SET:
    return "USB_FEATURE_SET";
  case VDS_FRAME_USB_AUDIO_OUT:
    return "USB_AUDIO_OUT";
  case VDS_FRAME_USB_HID_IN:
    return "USB_HID_IN";
  case VDS_FRAME_USB_FEATURE_REPLY:
    return "USB_FEATURE_REPLY";
  case VDS_FRAME_USB_INTERFACE:
    return "USB_INTERFACE";
  default:
    return "UNKNOWN";
  }
}

} // namespace vds
