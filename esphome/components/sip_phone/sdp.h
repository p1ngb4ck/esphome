#pragma once

#include "sip_message.h"

#include <cstdint>

namespace esphome::sip_phone {

enum class Codec : uint8_t { NONE = 0, OPUS, G722, PCMA, PCMU };

enum class Direction : uint8_t { SENDRECV = 0, SENDONLY, RECVONLY, INACTIVE };

const char *codec_name(Codec codec);
uint32_t codec_rtp_clock(Codec codec);

struct MediaFormat {
  Codec codec{Codec::NONE};
  uint8_t payload_type{0};
  uint8_t dtmf_payload_type{0};
  bool remote_stereo{false};
  uint16_t ptime{20};
};

struct OpusParams {
  uint32_t sample_rate{48000};
  uint8_t channels{2};
};

struct SdpInfo {
  static constexpr uint8_t MAX_MEDIA = 4;
  static constexpr uint8_t MAX_FORMATS = 12;
  struct Media {
    Span type;
    uint16_t port{0};
    Span proto;
    Span first_format;
  };
  char address[48]{};
  Media media[MAX_MEDIA];
  uint8_t media_count{0};
  int8_t audio_index{-1};
  uint16_t audio_port{0};
  uint8_t formats[MAX_FORMATS]{};
  uint8_t format_count{0};
  int16_t opus_pt{-1};
  bool opus_stereo{false};
  int16_t g722_pt{-1};
  int16_t pcma_pt{-1};
  int16_t pcmu_pt{-1};
  int16_t dtmf8k_pt{-1};
  int16_t dtmf48k_pt{-1};
  uint16_t ptime{20};
  Direction direction{Direction::SENDRECV};
};

bool parse_sdp(Span body, SdpInfo &out);
MediaFormat choose_format(const SdpInfo &remote, const Codec *preferences, uint8_t count);
Direction answer_direction(Direction remote);

void write_sdp_offer(SipWriter &w, const char *local_ip, uint16_t port, uint32_t session, uint32_t version,
                     const Codec *preferences, uint8_t count, const OpusParams &opus, uint16_t ptime,
                     Direction direction);
void write_sdp_answer(SipWriter &w, const char *local_ip, uint16_t port, uint32_t session, uint32_t version,
                      const SdpInfo &remote, const MediaFormat &format, const OpusParams &opus, Direction direction);

}
