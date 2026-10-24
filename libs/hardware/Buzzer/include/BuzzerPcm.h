#pragma once

#include <cstdint>

namespace freeink {
// First-order pulse-density conversion. Each 24 kHz sample becomes 32 bits at
// 768 kHz. Two 16-bit I2S slots are emitted in chronological, MSB-first order.
// Error carries across samples/chunks; no allocation or lookup table is needed.
class BuzzerPcm {
 public:
  static int16_t scale(int16_t sample, uint8_t volume, uint8_t maxGainPercent = 100) {
    const uint32_t gain = volume > 100 ? 100 : volume;
    const uint32_t maximum = maxGainPercent > 100 ? 100 : maxGainPercent;
    return static_cast<int16_t>(static_cast<int32_t>(sample) * static_cast<int32_t>(gain * maximum) / 10000);
  }

  void encode(int16_t sample, uint16_t* slots) {
    const uint32_t level = static_cast<int32_t>(sample) + 32768;
    for (unsigned slot = 0; slot < 2; ++slot) {
      uint16_t bits = 0;
      for (unsigned bit = 0; bit < 16; ++bit) {
        accumulator_ += level;
        bits = static_cast<uint16_t>((bits << 1) | (accumulator_ >> 16));
        accumulator_ &= 0xffff;
      }
      slots[slot] = bits;
    }
  }

 private:
  uint32_t accumulator_ = 32768;
};
}  // namespace freeink
