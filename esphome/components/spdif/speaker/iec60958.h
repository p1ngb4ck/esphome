#pragma once

#include <cstdint>
#include <cstring>

namespace esphome {
namespace spdif {

// IEC 60958 frame assembly and biphase-mark coding.
//
// Kept apart from the speaker so the same code serves both payloads: linear PCM writes
// samples into the subframes, and IEC 61937 writes a compressed burst into the very same
// subframes with one channel status bit flipped. Nothing below the burst framing differs
// between the two.
//
// Sizes, all fixed by the standard:
//
//   time slot   1 bit                    2 biphase cells
//   subframe    32 time slots            64 cells
//   frame       2 subframes (A and B)    128 cells
//   block       192 frames               one channel status word per subframe bit 30
//
// A cell is one output bit, so the pin runs at 128 * Fs -- 6.144 MHz at 48 kHz.
static const uint8_t CELLS_PER_SUBFRAME = 64;
static const uint8_t CELLS_PER_FRAME = 128;
static const uint16_t FRAMES_PER_BLOCK = 192;
static const uint8_t CHANNEL_STATUS_BYTES = 24;  // 192 bits

// Preambles occupy the first four time slots and are deliberately not valid biphase-mark:
// they carry no transition where one is due, which is what makes them findable in a stream
// that otherwise never goes three cells without a change. Written here for a preceding cell
// of 0; if the last cell was 1 the whole pattern inverts.
//
//   Z (B)  block start, subframe A of frame 0
//   X (M)  subframe A of every other frame
//   Y (W)  subframe B
static const uint8_t PREAMBLE_Z = 0b11101000;
static const uint8_t PREAMBLE_X = 0b11100010;
static const uint8_t PREAMBLE_Y = 0b11100100;

// Consumer channel status, byte 0.
static const uint8_t CS0_CONSUMER = 0x00;      // bit 0 clear
static const uint8_t CS0_NON_PCM = 0x02;       // bit 1: payload is a 61937 burst, not audio
static const uint8_t CS0_NO_COPYRIGHT = 0x04;  // bit 2

// Consumer channel status, byte 3, bits 24..27 in transmission order. The nibble reads
// LSB-first on the wire, which is why 48 kHz is 0x02 here and not the 0x04 the bit pattern
// 0100 suggests when read the other way round.
static const uint8_t CS3_FS_44100 = 0x00;
static const uint8_t CS3_FS_48000 = 0x02;
static const uint8_t CS3_FS_32000 = 0x03;

// Consumer channel status, byte 4: 16 bit sample in a 20 bit maximum word length.
static const uint8_t CS4_16BIT = 0x02;

/// @brief Builds the 24 byte consumer channel status word for a stream.
/// @param sample_rate Frame rate in Hz; anything other than 32000, 44100 or 48000 is
///                    reported as 44100, which is the value a receiver treats as unknown.
/// @param non_pcm Set for an IEC 61937 burst, clear for linear PCM. A receiver that sees
///                this clear will hand a compressed burst to its DAC as if it were audio.
/// @param out Destination, CHANNEL_STATUS_BYTES long.
inline void build_channel_status(uint32_t sample_rate, bool non_pcm, uint8_t *out) {
  memset(out, 0, CHANNEL_STATUS_BYTES);
  out[0] = CS0_CONSUMER | CS0_NO_COPYRIGHT | (non_pcm ? CS0_NON_PCM : 0x00);
  switch (sample_rate) {
    case 48000:
      out[3] = CS3_FS_48000;
      break;
    case 32000:
      out[3] = CS3_FS_32000;
      break;
    default:
      out[3] = CS3_FS_44100;
      break;
  }
  out[4] = CS4_16BIT;
}

/// @brief Encodes one IEC 60958 frame -- both subframes -- as biphase-mark cells.
///
/// Cells are written one per bit into @p out, packed eight to a byte, least significant bit
/// first, which is the order the parallel output shifts them.
class Iec60958Encoder {
 public:
  void set_channel_status(const uint8_t *channel_status) {
    memcpy(this->channel_status_, channel_status, CHANNEL_STATUS_BYTES);
  }

  /// @brief Restarts at the top of a block. The next frame emitted carries preamble Z.
  void reset() {
    this->frame_in_block_ = 0;
    this->last_cell_ = 0;
  }

  uint16_t frame_in_block() const { return this->frame_in_block_; }

  /// @brief Encodes one frame of two samples.
  /// @param left Sample for subframe A, left aligned in the low 24 bits.
  /// @param right Sample for subframe B.
  /// @param out Destination for CELLS_PER_FRAME cells, i.e. 16 bytes.
  void encode_frame(int32_t left, int32_t right, uint8_t *out) {
    // Channel status is per subframe bit 30, and both subframes of a frame carry the same
    // bit of the same word: the consumer format describes the stream, not one channel.
    uint16_t bit_index = this->frame_in_block_;
    bool cs_bit = (this->channel_status_[bit_index >> 3] >> (bit_index & 0x07)) & 0x01;

    uint8_t preamble_a = (this->frame_in_block_ == 0) ? PREAMBLE_Z : PREAMBLE_X;
    this->encode_subframe_(left, cs_bit, preamble_a, out);
    this->encode_subframe_(right, cs_bit, PREAMBLE_Y, out + (CELLS_PER_SUBFRAME / 8));

    if (++this->frame_in_block_ >= FRAMES_PER_BLOCK) {
      this->frame_in_block_ = 0;
    }
  }

 protected:
  /// @brief Encodes one subframe into eight bytes of cells.
  /// @param sample Audio or burst payload, in the low 24 bits.
  /// @param cs_bit This frame's channel status bit.
  /// @param preamble One of PREAMBLE_Z, PREAMBLE_X, PREAMBLE_Y, written for a preceding 0.
  void encode_subframe_(int32_t sample, bool cs_bit, uint8_t preamble, uint8_t *out) {
    uint8_t cursor = 0;

    // Time slots 0..3. The pattern is absolute rather than biphase coded, so it is placed
    // as it stands, inverted when the previous cell left the line high.
    uint8_t pattern = this->last_cell_ ? (uint8_t) ~preamble : preamble;
    for (int8_t i = 7; i >= 0; i--) {
      this->put_cell_((pattern >> i) & 0x01, out, cursor);
    }
    this->last_cell_ = pattern & 0x01;

    // Time slots 4..27, the sample, least significant bit first. A 16 bit sample sits in
    // slots 12..27, so the low eight slots of the 24 bit field carry zero.
    uint32_t payload = ((uint32_t) sample) & 0x00FFFFFFu;
    uint8_t parity = 0;
    for (uint8_t i = 0; i < 24; i++) {
      uint8_t bit = (payload >> i) & 0x01;
      parity ^= bit;
      this->put_bit_(bit, out, cursor);
    }

    // Slot 28 validity: clear means the receiver may convert the sample. Slot 29 user data,
    // unused. Slot 30 channel status. Slot 31 parity, chosen to make slots 4..31 even.
    this->put_bit_(0, out, cursor);
    this->put_bit_(0, out, cursor);
    this->put_bit_(cs_bit, out, cursor);
    parity ^= (cs_bit ? 1 : 0);
    this->put_bit_(parity, out, cursor);
  }

  /// @brief Writes one time slot as two biphase-mark cells.
  ///
  /// Every slot begins with a transition, so the line never holds a level longer than two
  /// cells and the receiver can recover the clock from the data alone. A one adds a second
  /// transition in the middle of the slot; a zero does not.
  void put_bit_(uint8_t bit, uint8_t *out, uint8_t &cursor) {
    uint8_t first = this->last_cell_ ^ 0x01;
    uint8_t second = bit ? (uint8_t) (first ^ 0x01) : first;
    this->put_cell_(first, out, cursor);
    this->put_cell_(second, out, cursor);
    this->last_cell_ = second;
  }

  static void put_cell_(uint8_t cell, uint8_t *out, uint8_t &cursor) {
    uint8_t byte_index = cursor >> 3;
    uint8_t bit_index = cursor & 0x07;
    if (bit_index == 0) {
      out[byte_index] = 0;
    }
    out[byte_index] |= (uint8_t) (cell << bit_index);
    cursor++;
  }

  uint8_t channel_status_[CHANNEL_STATUS_BYTES]{};
  uint16_t frame_in_block_{0};
  uint8_t last_cell_{0};
};

}  // namespace spdif
}  // namespace esphome
