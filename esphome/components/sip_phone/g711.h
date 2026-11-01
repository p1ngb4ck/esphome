#pragma once

#include <cstdint>

namespace esphome::sip_phone::g711 {

inline int segment_of(int value, const int16_t *ends) {
  for (int i = 0; i < 8; i++) {
    if (value <= ends[i])
      return i;
  }
  return 8;
}

inline uint8_t linear_to_alaw(int16_t pcm) {
  static const int16_t SEG_END[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
  int value = pcm >> 3;
  uint8_t mask;
  if (value >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    value = -value - 1;
  }
  const int seg = segment_of(value, SEG_END);
  if (seg >= 8)
    return static_cast<uint8_t>(0x7F ^ mask);
  uint8_t aval = static_cast<uint8_t>(seg << 4);
  aval |= seg < 2 ? (value >> 1) & 0x0F : (value >> seg) & 0x0F;
  return static_cast<uint8_t>(aval ^ mask);
}

inline int16_t alaw_to_linear(uint8_t alaw) {
  alaw ^= 0x55;
  int t = (alaw & 0x0F) << 4;
  const int seg = (alaw & 0x70) >> 4;
  if (seg == 0) {
    t += 8;
  } else {
    t += 0x108;
    if (seg > 1)
      t <<= seg - 1;
  }
  return static_cast<int16_t>((alaw & 0x80) ? t : -t);
}

inline uint8_t linear_to_ulaw(int16_t pcm) {
  static const int16_t SEG_END[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
  int value = pcm >> 2;
  uint8_t mask;
  if (value < 0) {
    value = -value;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }
  if (value > 8159)
    value = 8159;
  value += 0x84 >> 2;
  const int seg = segment_of(value, SEG_END);
  if (seg >= 8)
    return static_cast<uint8_t>(0x7F ^ mask);
  const uint8_t uval = static_cast<uint8_t>((seg << 4) | ((value >> (seg + 1)) & 0x0F));
  return static_cast<uint8_t>(uval ^ mask);
}

inline int16_t ulaw_to_linear(uint8_t ulaw) {
  ulaw = static_cast<uint8_t>(~ulaw);
  int t = ((ulaw & 0x0F) << 3) + 0x84;
  t <<= (ulaw & 0x70) >> 4;
  return static_cast<int16_t>((ulaw & 0x80) ? (0x84 - t) : (t - 0x84));
}

}
