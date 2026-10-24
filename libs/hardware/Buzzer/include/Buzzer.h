#pragma once

// FreeInk buzzer (LEDC PWM tone beeper).
//
// Drives the passive buzzer on BoardConfig::ACTIVE.audio.buzzer (e.g. the
// Sticky's GPIO48, Murphy's GPIO46) as a square-wave tone generator via the
// ESP32 LEDC peripheral — the same approach as the vendor demo. This is a tone
// device, independent of AudioManager (which streams WAV through an I2S codec).
// On ESP32-S3, short PCM clips can also use single-pin I2S delta-sigma output.
// Boards without a buzzer (FREEINK_CAP_BUZZER off, or the
// buzzer pin unassigned) link stub bodies and present() returns false.

#include <Arduino.h>

#include <cstdint>

#include "BuzzerPcm.h"

namespace freeink {

class Buzzer {
 public:
  static constexpr uint32_t kDefaultBeepFreq = 2000;  // Hz
  static constexpr uint32_t kDefaultBeepMs = 80;

  // Attaches LEDC to the buzzer pin. Returns false when the active board has no
  // buzzer.
  bool begin();
  bool present() const { return begun_ || pcmChannel_; }

  // Fixed 24 kHz, signed 16-bit mono, at most 200 ms. Samples may live in Flash.
  // S3 only; allocates a small DMA channel lazily, reused until end(). Blocking,
  // with a 400 ms total deadline. Do not call concurrently with tone()/end().
  static bool supportsPcm();
  bool playPcm(const int16_t* samples, size_t count);

  // Continuous 24 kHz signed 16-bit mono. One producer owns begin/write/end;
  // these methods are not thread-safe. No task or source buffer is allocated.
  // A write accepts 1..1024 samples, scales volume (0..100), and has a 100 ms
  // DMA-write budget (each wait <=20 ms). I/O failure releases the output.
  // Short clips and LEDC begin() refuse to interrupt an active stream.
  static constexpr size_t kStreamMaxSamples = 1024;
  bool beginStream();
  bool writeStream(const int16_t* samples, size_t count, uint8_t volume = 100, uint8_t maxGainPercent = 100);
  // Graceful fade/drain has a 40 ms DMA-write budget, then releases the pin.
  // Returns false on write/driver cleanup failure; end() can retry cleanup.
  bool endStream();

  // Plays a square-wave tone at freqHz. durationMs > 0 blocks for that long and
  // then stops; durationMs == 0 starts a continuous tone (end it with noTone()).
  void tone(uint32_t freqHz, uint32_t durationMs = 0);

  // A short default beep (kDefaultBeepFreq for kDefaultBeepMs).
  void beep() { tone(kDefaultBeepFreq, kDefaultBeepMs); }

  // Silences a continuous tone.
  void noTone();

  // Releases the LEDC channel / pin. begin() re-attaches.
  void end();

 private:
  bool begun_ = false;
  void* pcmChannel_ = nullptr;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  bool pcmRunning_ = false;
  bool pcmReady_ = false;
  enum class PcmUse : uint8_t { Clip, Stream };
  PcmUse pcmUse_ = PcmUse::Clip;
  BuzzerPcm streamEncoder_;
  int16_t streamLast_ = 0;
  bool preparePcm();
  bool writeStreamWords(const uint16_t* words, size_t frames, uint32_t started, uint32_t budget);
#endif
};

}  // namespace freeink

using Buzzer = freeink::Buzzer;
