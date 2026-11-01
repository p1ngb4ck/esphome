#include "sdp.h"

#include <cstring>

namespace esphome::sip_phone {

static constexpr uint8_t OPUS_PT = 111;
static constexpr uint8_t DTMF_8K_PT = 101;
static constexpr uint8_t DTMF_48K_PT = 102;

const char *codec_name(Codec codec) {
  switch (codec) {
    case Codec::OPUS:
      return "opus";
    case Codec::G722:
      return "g722";
    case Codec::PCMA:
      return "pcma";
    case Codec::PCMU:
      return "pcmu";
    default:
      return "none";
  }
}

uint32_t codec_rtp_clock(Codec codec) { return codec == Codec::OPUS ? 48000 : 8000; }

static const char *direction_name(Direction direction) {
  switch (direction) {
    case Direction::SENDONLY:
      return "sendonly";
    case Direction::RECVONLY:
      return "recvonly";
    case Direction::INACTIVE:
      return "inactive";
    default:
      return "sendrecv";
  }
}

static Span next_token(Span &rest) {
  while (rest.n > 0 && *rest.p == ' ') {
    rest.p++;
    rest.n--;
  }
  Span token{rest.p, 0};
  while (rest.n > 0 && *rest.p != ' ') {
    rest.p++;
    rest.n--;
    token.n++;
  }
  return token;
}

static void apply_rtpmap(SdpInfo &out, Span value) {
  Span rest = value;
  const int pt = next_token(rest).to_int(-1);
  Span encoding = trim(rest);
  if (pt < 0 || pt > 127)
    return;
  if (encoding.starts_with("opus/")) {
    if (out.opus_pt < 0)
      out.opus_pt = static_cast<int16_t>(pt);
  } else if (encoding.starts_with("G722/")) {
    out.g722_pt = static_cast<int16_t>(pt);
  } else if (encoding.starts_with("PCMA/")) {
    out.pcma_pt = static_cast<int16_t>(pt);
  } else if (encoding.starts_with("PCMU/")) {
    out.pcmu_pt = static_cast<int16_t>(pt);
  } else if (encoding.starts_with("telephone-event/8000")) {
    out.dtmf8k_pt = static_cast<int16_t>(pt);
  } else if (encoding.starts_with("telephone-event/48000")) {
    out.dtmf48k_pt = static_cast<int16_t>(pt);
  }
}

static void apply_fmtp(SdpInfo &out, Span value) {
  Span rest = value;
  const int pt = next_token(rest).to_int(-1);
  if (pt < 0 || pt != out.opus_pt)
    return;
  const char *found = nullptr;
  for (uint16_t i = 0; i + 8 <= rest.n; i++) {
    if (Span{rest.p + i, static_cast<uint16_t>(rest.n - i)}.starts_with("stereo=1") &&
        (i == 0 || rest.p[i - 1] == ';' || rest.p[i - 1] == ' ')) {
      found = rest.p + i;
      break;
    }
  }
  out.opus_stereo = found != nullptr;
}

static bool has_format(const SdpInfo &info, int pt) {
  for (uint8_t i = 0; i < info.format_count; i++) {
    if (info.formats[i] == pt)
      return true;
  }
  return false;
}

bool parse_sdp(Span body, SdpInfo &out) {
  out = SdpInfo{};
  const char *cursor = body.p;
  const char *end = body.p + body.n;
  bool in_audio = false;
  bool seen_media = false;
  Direction session_direction = Direction::SENDRECV;
  bool audio_direction_set = false;
  char session_address[sizeof(out.address)] = {};
  while (cursor < end) {
    const char *nl = static_cast<const char *>(memchr(cursor, '\n', end - cursor));
    const char *stop = nl != nullptr ? nl : end;
    Span line = trim(Span{cursor, static_cast<uint16_t>(stop - cursor)});
    cursor = nl != nullptr ? nl + 1 : end;
    if (line.n < 2 || line.p[1] != '=')
      continue;
    const char kind = line.p[0];
    Span value{line.p + 2, static_cast<uint16_t>(line.n - 2)};
    if (kind == 'm') {
      seen_media = true;
      in_audio = false;
      if (out.media_count >= SdpInfo::MAX_MEDIA)
        continue;
      SdpInfo::Media &media = out.media[out.media_count];
      Span rest = value;
      media.type = next_token(rest);
      media.port = static_cast<uint16_t>(next_token(rest).to_int(0));
      media.proto = next_token(rest);
      media.first_format = next_token(rest);
      if (media.type.equals("audio") && out.audio_index < 0 && media.port != 0) {
        out.audio_index = static_cast<int8_t>(out.media_count);
        out.audio_port = media.port;
        in_audio = true;
        Span formats = Span{media.first_format.p, static_cast<uint16_t>(value.p + value.n - media.first_format.p)};
        while (formats.n > 0 && out.format_count < SdpInfo::MAX_FORMATS) {
          const int pt = next_token(formats).to_int(-1);
          if (pt < 0 || pt > 127)
            break;
          out.formats[out.format_count++] = static_cast<uint8_t>(pt);
        }
      }
      out.media_count++;
    } else if (kind == 'c') {
      Span rest = value;
      next_token(rest);
      next_token(rest);
      Span addr = next_token(rest);
      const char *slash = static_cast<const char *>(memchr(addr.p, '/', addr.n));
      if (slash != nullptr)
        addr.n = static_cast<uint16_t>(slash - addr.p);
      if (!seen_media) {
        addr.copy_to(session_address, sizeof(session_address));
      } else if (in_audio) {
        addr.copy_to(out.address, sizeof(out.address));
      }
    } else if (kind == 'a') {
      Direction direction;
      bool is_direction = true;
      if (value.equals("sendrecv")) {
        direction = Direction::SENDRECV;
      } else if (value.equals("sendonly")) {
        direction = Direction::SENDONLY;
      } else if (value.equals("recvonly")) {
        direction = Direction::RECVONLY;
      } else if (value.equals("inactive")) {
        direction = Direction::INACTIVE;
      } else {
        is_direction = false;
        direction = Direction::SENDRECV;
      }
      if (is_direction) {
        if (!seen_media) {
          session_direction = direction;
        } else if (in_audio) {
          out.direction = direction;
          audio_direction_set = true;
        }
        continue;
      }
      if (!in_audio)
        continue;
      if (value.starts_with("rtpmap:")) {
        apply_rtpmap(out, Span{value.p + 7, static_cast<uint16_t>(value.n - 7)});
      } else if (value.starts_with("fmtp:")) {
        apply_fmtp(out, Span{value.p + 5, static_cast<uint16_t>(value.n - 5)});
      } else if (value.starts_with("ptime:")) {
        const int ptime = Span{value.p + 6, static_cast<uint16_t>(value.n - 6)}.to_int(20);
        if (ptime >= 10 && ptime <= 120)
          out.ptime = static_cast<uint16_t>(ptime);
      }
    }
  }
  if (out.audio_index < 0)
    return false;
  if (out.address[0] == '\0')
    memcpy(out.address, session_address, sizeof(out.address));
  if (!audio_direction_set)
    out.direction = session_direction;
  if (out.g722_pt < 0 && has_format(out, 9))
    out.g722_pt = 9;
  if (out.pcma_pt < 0 && has_format(out, 8))
    out.pcma_pt = 8;
  if (out.pcmu_pt < 0 && has_format(out, 0))
    out.pcmu_pt = 0;
  return out.address[0] != '\0';
}

MediaFormat choose_format(const SdpInfo &remote, const Codec *preferences, uint8_t count) {
  MediaFormat format;
  format.ptime = remote.ptime;
  for (uint8_t i = 0; i < count && format.codec == Codec::NONE; i++) {
    int pt = -1;
    switch (preferences[i]) {
      case Codec::OPUS:
        pt = remote.opus_pt;
        break;
      case Codec::G722:
        pt = remote.g722_pt;
        break;
      case Codec::PCMA:
        pt = remote.pcma_pt;
        break;
      case Codec::PCMU:
        pt = remote.pcmu_pt;
        break;
      default:
        break;
    }
    if (pt >= 0 && has_format(remote, pt)) {
      format.codec = preferences[i];
      format.payload_type = static_cast<uint8_t>(pt);
    }
  }
  if (format.codec == Codec::OPUS) {
    format.remote_stereo = remote.opus_stereo;
    if (remote.dtmf48k_pt >= 0 && has_format(remote, remote.dtmf48k_pt))
      format.dtmf_payload_type = static_cast<uint8_t>(remote.dtmf48k_pt);
  } else if (format.codec != Codec::NONE) {
    if (remote.dtmf8k_pt >= 0 && has_format(remote, remote.dtmf8k_pt))
      format.dtmf_payload_type = static_cast<uint8_t>(remote.dtmf8k_pt);
  }
  return format;
}

Direction answer_direction(Direction remote) {
  switch (remote) {
    case Direction::SENDONLY:
      return Direction::RECVONLY;
    case Direction::RECVONLY:
      return Direction::SENDONLY;
    case Direction::INACTIVE:
      return Direction::INACTIVE;
    default:
      return Direction::SENDRECV;
  }
}

static void write_header(SipWriter &w, const char *local_ip, uint32_t session, uint32_t version) {
  w.add("v=0\r\no=- %u %u IN IP4 %s\r\ns=-\r\nc=IN IP4 %s\r\nt=0 0\r\n", static_cast<unsigned>(session),
        static_cast<unsigned>(version), local_ip, local_ip);
}

static void write_opus_attributes(SipWriter &w, uint8_t pt, const OpusParams &opus) {
  const int stereo = opus.channels > 1 ? 1 : 0;
  w.add("a=rtpmap:%u opus/48000/2\r\na=fmtp:%u useinbandfec=1;stereo=%d;sprop-stereo=%d;maxplaybackrate=%u;"
        "sprop-maxcapturerate=%u\r\n",
        pt, pt, stereo, stereo, static_cast<unsigned>(opus.sample_rate), static_cast<unsigned>(opus.sample_rate));
}

void write_sdp_offer(SipWriter &w, const char *local_ip, uint16_t port, uint32_t session, uint32_t version,
                     const Codec *preferences, uint8_t count, const OpusParams &opus, uint16_t ptime,
                     Direction direction) {
  write_header(w, local_ip, session, version);
  bool has_opus = false;
  bool has_narrow = false;
  w.add("m=audio %u RTP/AVP", port);
  for (uint8_t i = 0; i < count; i++) {
    switch (preferences[i]) {
      case Codec::OPUS:
        w.add(" %u", OPUS_PT);
        has_opus = true;
        break;
      case Codec::G722:
        w.add(" 9");
        has_narrow = true;
        break;
      case Codec::PCMA:
        w.add(" 8");
        has_narrow = true;
        break;
      case Codec::PCMU:
        w.add(" 0");
        has_narrow = true;
        break;
      default:
        break;
    }
  }
  if (has_opus)
    w.add(" %u", DTMF_48K_PT);
  if (has_narrow)
    w.add(" %u", DTMF_8K_PT);
  w.add("\r\n");
  for (uint8_t i = 0; i < count; i++) {
    switch (preferences[i]) {
      case Codec::OPUS:
        write_opus_attributes(w, OPUS_PT, opus);
        break;
      case Codec::G722:
        w.add("a=rtpmap:9 G722/8000\r\n");
        break;
      case Codec::PCMA:
        w.add("a=rtpmap:8 PCMA/8000\r\n");
        break;
      case Codec::PCMU:
        w.add("a=rtpmap:0 PCMU/8000\r\n");
        break;
      default:
        break;
    }
  }
  if (has_opus)
    w.add("a=rtpmap:%u telephone-event/48000\r\na=fmtp:%u 0-16\r\n", DTMF_48K_PT, DTMF_48K_PT);
  if (has_narrow)
    w.add("a=rtpmap:%u telephone-event/8000\r\na=fmtp:%u 0-16\r\n", DTMF_8K_PT, DTMF_8K_PT);
  w.add("a=ptime:%u\r\na=%s\r\n", ptime, direction_name(direction));
}

void write_sdp_answer(SipWriter &w, const char *local_ip, uint16_t port, uint32_t session, uint32_t version,
                      const SdpInfo &remote, const MediaFormat &format, const OpusParams &opus, Direction direction) {
  write_header(w, local_ip, session, version);
  for (uint8_t i = 0; i < remote.media_count; i++) {
    const SdpInfo::Media &media = remote.media[i];
    if (i != remote.audio_index || format.codec == Codec::NONE) {
      w.add("m=");
      w.add_span(media.type);
      w.add(" 0 ");
      w.add_span(media.proto);
      w.add(" ");
      w.add_span(media.first_format);
      w.add("\r\n");
      continue;
    }
    w.add("m=audio %u RTP/AVP %u", port, format.payload_type);
    if (format.dtmf_payload_type != 0)
      w.add(" %u", format.dtmf_payload_type);
    w.add("\r\n");
    switch (format.codec) {
      case Codec::OPUS:
        write_opus_attributes(w, format.payload_type, opus);
        break;
      case Codec::G722:
        w.add("a=rtpmap:%u G722/8000\r\n", format.payload_type);
        break;
      case Codec::PCMA:
        w.add("a=rtpmap:%u PCMA/8000\r\n", format.payload_type);
        break;
      case Codec::PCMU:
        w.add("a=rtpmap:%u PCMU/8000\r\n", format.payload_type);
        break;
      default:
        break;
    }
    if (format.dtmf_payload_type != 0) {
      w.add("a=rtpmap:%u telephone-event/%u\r\na=fmtp:%u 0-16\r\n", format.dtmf_payload_type,
            static_cast<unsigned>(codec_rtp_clock(format.codec)), format.dtmf_payload_type);
    }
    w.add("a=ptime:%u\r\na=%s\r\n", format.ptime, direction_name(direction));
  }
}

}
