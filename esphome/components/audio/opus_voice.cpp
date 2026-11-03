#include "opus_voice.h"

#ifdef USE_AUDIO_OPUS_SUPPORT

#include <opus.h>

namespace esphome::audio {

bool OpusVoiceEncoder::start(uint32_t sample_rate, uint8_t channels, uint32_t frame_ms, int32_t bitrate,
                             int complexity, bool fec, bool mono) {
  this->stop();
  int err = OPUS_OK;
  OpusEncoder *encoder = opus_encoder_create(static_cast<opus_int32>(sample_rate), channels,
                                             channels > 1 ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP, &err);
  if (encoder == nullptr || err != OPUS_OK)
    return false;
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
  if (complexity >= 0)
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity));
  if (fec) {
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
  }
  if (channels > 1 && mono)
    opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1));
  this->encoder_ = encoder;
  this->channels_ = channels;
  this->frame_samples_ = sample_rate * frame_ms / 1000;
  return true;
}

void OpusVoiceEncoder::stop() {
  if (this->encoder_ != nullptr) {
    opus_encoder_destroy(static_cast<OpusEncoder *>(this->encoder_));
    this->encoder_ = nullptr;
  }
}

int32_t OpusVoiceEncoder::lookahead() const {
  opus_int32 lookahead = 0;
  if (this->encoder_ != nullptr)
    opus_encoder_ctl(static_cast<OpusEncoder *>(this->encoder_), OPUS_GET_LOOKAHEAD(&lookahead));
  return lookahead;
}

int OpusVoiceEncoder::encode(const int16_t *pcm, uint8_t *out, size_t out_max) {
  if (this->encoder_ == nullptr)
    return -1;
  return opus_encode(static_cast<OpusEncoder *>(this->encoder_), pcm, static_cast<int>(this->frame_samples_), out,
                     static_cast<opus_int32>(out_max));
}

bool OpusVoiceDecoder::start(uint32_t sample_rate, uint8_t channels) {
  this->stop();
  int err = OPUS_OK;
  OpusDecoder *decoder = opus_decoder_create(static_cast<opus_int32>(sample_rate), channels, &err);
  if (decoder == nullptr || err != OPUS_OK)
    return false;
  this->decoder_ = decoder;
  return true;
}

void OpusVoiceDecoder::stop() {
  if (this->decoder_ != nullptr) {
    opus_decoder_destroy(static_cast<OpusDecoder *>(this->decoder_));
    this->decoder_ = nullptr;
  }
}

int OpusVoiceDecoder::decode(const uint8_t *payload, size_t len, int16_t *pcm, size_t max_samples, bool fec) {
  if (this->decoder_ == nullptr)
    return -1;
  return opus_decode(static_cast<OpusDecoder *>(this->decoder_), payload, static_cast<opus_int32>(len), pcm,
                     static_cast<int>(max_samples), fec ? 1 : 0);
}

int OpusVoiceDecoder::conceal(int16_t *pcm, size_t samples) {
  if (this->decoder_ == nullptr)
    return -1;
  return opus_decode(static_cast<OpusDecoder *>(this->decoder_), nullptr, 0, pcm, static_cast<int>(samples), 0);
}

}

#endif
