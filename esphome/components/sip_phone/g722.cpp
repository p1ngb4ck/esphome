#include "g722.h"

#include <cstring>

namespace esphome::sip_phone {

static const int QMF_COEFFS[12] = {3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11};
static const int WL[8] = {-60, -30, 58, 172, 334, 538, 1198, 3042};
static const int RL42[16] = {0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0};
static const int ILB[32] = {2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383, 2435, 2489, 2543,
                            2599, 2656, 2714, 2774, 2834, 2896, 2960, 3025, 3091, 3158, 3228,
                            3298, 3371, 3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008};
static const int QM4[16] = {0,     -20456, -12896, -8968, -6288, -4240, -2584, -1200,
                            20456, 12896,  8968,   6288,  4240,  2584,  1200,  0};
static const int QM2[4] = {-7408, -1616, 7408, 1616};
static const int WH[3] = {0, -214, 798};
static const int RH2[4] = {2, 1, 2, 1};

static inline int saturate(int amp) {
  if (amp > INT16_MAX)
    return INT16_MAX;
  if (amp < INT16_MIN)
    return INT16_MIN;
  return amp;
}

static void block4(G722Band *band, int d) {
  band->d[0] = d;
  band->r[0] = saturate(band->s + d);
  band->p[0] = saturate(band->sz + d);
  for (int i = 0; i < 3; i++)
    band->sg[i] = band->p[i] >> 15;
  int wd1 = saturate(band->a[1] << 2);
  int wd2 = (band->sg[0] == band->sg[1]) ? -wd1 : wd1;
  if (wd2 > 32767)
    wd2 = 32767;
  int wd3 = (wd2 >> 7) + ((band->sg[0] == band->sg[2]) ? 128 : -128);
  wd3 += (band->a[2] * 32512) >> 15;
  if (wd3 > 12288) {
    wd3 = 12288;
  } else if (wd3 < -12288) {
    wd3 = -12288;
  }
  band->ap[2] = wd3;

  band->sg[0] = band->p[0] >> 15;
  band->sg[1] = band->p[1] >> 15;
  wd1 = (band->sg[0] == band->sg[1]) ? 192 : -192;
  wd2 = (band->a[1] * 32640) >> 15;
  band->ap[1] = saturate(wd1 + wd2);
  wd3 = saturate(15360 - band->ap[2]);
  if (band->ap[1] > wd3) {
    band->ap[1] = wd3;
  } else if (band->ap[1] < -wd3) {
    band->ap[1] = -wd3;
  }

  wd1 = (d == 0) ? 0 : 128;
  band->sg[0] = d >> 15;
  for (int i = 1; i < 7; i++) {
    band->sg[i] = band->d[i] >> 15;
    wd2 = (band->sg[i] == band->sg[0]) ? wd1 : -wd1;
    wd3 = (band->b[i] * 32640) >> 15;
    band->bp[i] = saturate(wd2 + wd3);
  }

  for (int i = 6; i > 0; i--) {
    band->d[i] = band->d[i - 1];
    band->b[i] = band->bp[i];
  }
  for (int i = 2; i > 0; i--) {
    band->r[i] = band->r[i - 1];
    band->p[i] = band->p[i - 1];
    band->a[i] = band->ap[i];
  }

  wd1 = saturate(band->r[1] + band->r[1]);
  wd1 = (band->a[1] * wd1) >> 15;
  wd2 = saturate(band->r[2] + band->r[2]);
  wd2 = (band->a[2] * wd2) >> 15;
  band->sp = saturate(wd1 + wd2);

  band->sz = 0;
  for (int i = 6; i > 0; i--) {
    wd1 = saturate(band->d[i] + band->d[i]);
    band->sz += (band->b[i] * wd1) >> 15;
  }
  band->sz = saturate(band->sz);
  band->s = saturate(band->sp + band->sz);
}

static void scale_low(G722Band *band, int il4) {
  const int wd = (band->nb * 127) >> 7;
  band->nb = wd + WL[il4];
  if (band->nb < 0) {
    band->nb = 0;
  } else if (band->nb > 18432) {
    band->nb = 18432;
  }
  const int wd1 = (band->nb >> 6) & 31;
  const int wd2 = 8 - (band->nb >> 11);
  const int wd3 = (wd2 < 0) ? (ILB[wd1] << -wd2) : (ILB[wd1] >> wd2);
  band->det = wd3 << 2;
}

static void scale_high(G722Band *band, int ih2) {
  const int wd = (band->nb * 127) >> 7;
  band->nb = wd + WH[ih2];
  if (band->nb < 0) {
    band->nb = 0;
  } else if (band->nb > 22528) {
    band->nb = 22528;
  }
  const int wd1 = (band->nb >> 6) & 31;
  const int wd2 = 10 - (band->nb >> 11);
  const int wd3 = (wd2 < 0) ? (ILB[wd1] << -wd2) : (ILB[wd1] >> wd2);
  band->det = wd3 << 2;
}

void G722Encoder::reset() {
  memset(this->x_, 0, sizeof(this->x_));
  memset(this->band_, 0, sizeof(this->band_));
  this->band_[0].det = 32;
  this->band_[1].det = 8;
}

size_t G722Encoder::encode(const int16_t *pcm, size_t samples, uint8_t *out) {
  static const int Q6[32] = {0,   35,  72,  110, 150, 190, 233,  276,  323,  370,  422,  473,  530,  587,  650,  714,
                             786, 858, 940, 1023, 1121, 1219, 1339, 1458, 1612, 1765, 1980, 2195, 2557, 2919, 0,    0};
  static const int ILN[32] = {0,  63, 62, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
                              18, 17, 16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  0};
  static const int ILP[32] = {0,  61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47,
                              46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 0};
  static const int IHN[3] = {0, 1, 0};
  static const int IHP[3] = {0, 3, 2};

  size_t bytes = 0;
  for (size_t j = 0; j + 1 < samples; j += 2) {
    for (int i = 0; i < 22; i++)
      this->x_[i] = this->x_[i + 2];
    this->x_[22] = pcm[j];
    this->x_[23] = pcm[j + 1];
    int sumeven = 0;
    int sumodd = 0;
    for (int i = 0; i < 12; i++) {
      sumodd += this->x_[2 * i] * QMF_COEFFS[i];
      sumeven += this->x_[2 * i + 1] * QMF_COEFFS[11 - i];
    }
    const int xlow = (sumeven + sumodd) >> 14;
    const int xhigh = (sumeven - sumodd) >> 14;

    G722Band *low = &this->band_[0];
    const int el = saturate(xlow - low->s);
    const int wd = (el >= 0) ? el : -(el + 1);
    int i = 1;
    for (; i < 30; i++) {
      if (wd < ((Q6[i] * low->det) >> 12))
        break;
    }
    const int ilow = (el < 0) ? ILN[i] : ILP[i];
    const int ril = ilow >> 2;
    const int dlow = (low->det * QM4[ril]) >> 15;
    scale_low(low, RL42[ril]);
    block4(low, dlow);

    G722Band *high = &this->band_[1];
    const int eh = saturate(xhigh - high->s);
    const int wdh = (eh >= 0) ? eh : -(eh + 1);
    const int mih = (wdh >= ((564 * high->det) >> 12)) ? 2 : 1;
    const int ihigh = (eh < 0) ? IHN[mih] : IHP[mih];
    const int dhigh = (high->det * QM2[ihigh]) >> 15;
    scale_high(high, RH2[ihigh]);
    block4(high, dhigh);

    out[bytes++] = static_cast<uint8_t>((ihigh << 6) | ilow);
  }
  return bytes;
}

void G722Decoder::reset() {
  memset(this->x_, 0, sizeof(this->x_));
  memset(this->band_, 0, sizeof(this->band_));
  this->band_[0].det = 32;
  this->band_[1].det = 8;
}

size_t G722Decoder::decode(const uint8_t *data, size_t len, int16_t *pcm) {
  static const int QM6[64] = {
      -136,   -136,   -136,   -136,   -24808, -21904, -19008, -16704, -14984, -13512, -12280, -11192, -10232,
      -9360,  -8576,  -7856,  -7192,  -6576,  -6000,  -5456,  -4944,  -4464,  -4008,  -3576,  -3168,  -2776,
      -2400,  -2032,  -1688,  -1360,  -1040,  -728,   24808,  21904,  19008,  16704,  14984,  13512,  12280,
      11192,  10232,  9360,   8576,   7856,   7192,   6576,   6000,   5456,   4944,   4464,   4008,   3576,
      3168,   2776,   2400,   2032,   1688,   1360,   1040,   728,    432,    136,    -432,   -136};

  size_t out = 0;
  for (size_t j = 0; j < len; j++) {
    const int code = data[j];
    int wd1 = code & 0x3F;
    const int ihigh = (code >> 6) & 0x03;
    int wd2 = QM6[wd1];
    wd1 >>= 2;

    G722Band *low = &this->band_[0];
    wd2 = (low->det * wd2) >> 15;
    int rlow = low->s + wd2;
    if (rlow > 16383) {
      rlow = 16383;
    } else if (rlow < -16384) {
      rlow = -16384;
    }
    const int dlowt = (low->det * QM4[wd1]) >> 15;
    scale_low(low, RL42[wd1]);
    block4(low, dlowt);

    G722Band *high = &this->band_[1];
    const int dhigh = (high->det * QM2[ihigh]) >> 15;
    int rhigh = dhigh + high->s;
    if (rhigh > 16383) {
      rhigh = 16383;
    } else if (rhigh < -16384) {
      rhigh = -16384;
    }
    scale_high(high, RH2[ihigh]);
    block4(high, dhigh);

    for (int i = 0; i < 22; i++)
      this->x_[i] = this->x_[i + 2];
    this->x_[22] = rlow + rhigh;
    this->x_[23] = rlow - rhigh;
    int xout1 = 0;
    int xout2 = 0;
    for (int i = 0; i < 12; i++) {
      xout2 += this->x_[2 * i] * QMF_COEFFS[i];
      xout1 += this->x_[2 * i + 1] * QMF_COEFFS[11 - i];
    }
    pcm[out++] = static_cast<int16_t>(saturate(xout1 >> 11));
    pcm[out++] = static_cast<int16_t>(saturate(xout2 >> 11));
  }
  return out;
}

}
