#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::sip_phone {

struct G722Band {
  int s;
  int sp;
  int sz;
  int r[3];
  int a[3];
  int ap[3];
  int p[3];
  int d[7];
  int b[7];
  int bp[7];
  int sg[7];
  int nb;
  int det;
};

class G722Encoder {
 public:
  void reset();
  size_t encode(const int16_t *pcm, size_t samples, uint8_t *out);

 protected:
  int x_[24];
  G722Band band_[2];
};

class G722Decoder {
 public:
  void reset();
  size_t decode(const uint8_t *data, size_t len, int16_t *pcm);

 protected:
  int x_[24];
  G722Band band_[2];
};

}
