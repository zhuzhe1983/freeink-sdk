#include "Buzzer.h"

#include <BoardConfig.h>
#if FREEINK_CAP_BUZZER && defined(CONFIG_IDF_TARGET_ESP32S3)
#include <driver/gpio.h>
#include <driver/i2s_std.h>

#include "BuzzerPcm.h"
#endif

#if FREEINK_CAP_BUZZER

namespace freeink {
namespace {
// 10-bit resolution like the vendor demo; ledcWriteTone drives a 50% square wave
// and manages the timer frequency per tone, so the base freq here is nominal.
constexpr uint8_t LEDC_RESOLUTION_BITS = 10;
constexpr uint32_t LEDC_BASE_FREQ = 2000;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr size_t CHUNK_FRAMES = 64;
constexpr size_t DMA_COUNT = 3;
constexpr size_t RAMP_FRAMES = 96;
constexpr uint32_t SAMPLE_RATE = 24000;
#endif

uint8_t buzzerPin() { return static_cast<uint8_t>(BoardConfig::ACTIVE.audio.buzzer); }
}  // namespace

bool Buzzer::begin() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (pcmUse_ == PcmUse::Stream) return false;
#endif
  if (pcmChannel_) end();
  if (pcmChannel_) return false;
  if (begun_) return true;
  if (BoardConfig::ACTIVE.audio.buzzer == BoardConfig::PIN_UNASSIGNED) return false;
  if (!ledcAttach(buzzerPin(), LEDC_BASE_FREQ, LEDC_RESOLUTION_BITS)) return false;
  ledcWrite(buzzerPin(), 0);  // start silent
  begun_ = true;
  return true;
}

void Buzzer::tone(uint32_t freqHz, uint32_t durationMs) {
  if (!begun_ || freqHz == 0) return;
  ledcWriteTone(buzzerPin(), freqHz);
  if (durationMs > 0) {
    delay(durationMs);
    noTone();
  }
}

void Buzzer::noTone() {
  if (!begun_) return;
  ledcWriteTone(buzzerPin(), 0);
  ledcWrite(buzzerPin(), 0);
}

void Buzzer::end() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (pcmChannel_) {
    const auto channel = static_cast<i2s_chan_handle_t>(pcmChannel_);
    const bool disabled = !pcmRunning_ || i2s_channel_disable(channel) == ESP_OK;
    pcmReady_ = false;
    // Disconnect the peripheral even when its disable/delete operation fails.
    // Keep the handle on error so callers can retry instead of reusing the pin.
    const auto pin = static_cast<gpio_num_t>(buzzerPin());
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    if (!disabled) return;
    pcmRunning_ = false;
    if (i2s_del_channel(channel) != ESP_OK) return;
    pcmChannel_ = nullptr;
    pcmUse_ = PcmUse::Clip;
  }
#endif
  if (!begun_) return;
  noTone();
  ledcDetach(buzzerPin());
  begun_ = false;
}

bool Buzzer::supportsPcm() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  return BoardConfig::hasBuzzer();
#else
  return false;
#endif
}

#if defined(CONFIG_IDF_TARGET_ESP32S3)
bool Buzzer::preparePcm() {
  if (begun_) end();
  if (begun_) return false;
  if (pcmChannel_ && !pcmReady_) {
    end();
    if (pcmChannel_) return false;
  }
  if (!pcmChannel_) {
    // 3 * 64 * 4 = 768 bytes of DMA data, plus SDK descriptors/control state.
    // DMA must be internal memory, owned/freed by the SDK channel. Reuse it
    // during playback; caller owns samples. Stack/static cannot provide SDK DMA.
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    config.dma_desc_num = DMA_COUNT;
    config.dma_frame_num = CHUNK_FRAMES;
    config.auto_clear = true;
    i2s_chan_handle_t channel = nullptr;
    if (i2s_new_channel(&config, &channel, nullptr) != ESP_OK) return false;
    pcmChannel_ = channel;
    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    standard.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    standard.gpio_cfg.bclk = I2S_GPIO_UNUSED;
    standard.gpio_cfg.ws = I2S_GPIO_UNUSED;
    standard.gpio_cfg.dout = static_cast<gpio_num_t>(buzzerPin());
    standard.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(channel, &standard) != ESP_OK) {
      end();
      return false;
    }
    pcmReady_ = true;
  }
  return true;
}
#endif

bool Buzzer::playPcm(const int16_t* samples, size_t count) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (!supportsPcm() || !samples || count == 0 || count > SAMPLE_RATE / 5 || pcmRunning_ || pcmUse_ == PcmUse::Stream)
    return false;
  if (!preparePcm()) return false;
  const auto channel = static_cast<i2s_chan_handle_t>(pcmChannel_);
  if (i2s_channel_enable(channel) != ESP_OK) {
    end();
    return false;
  }
  pcmRunning_ = true;
  const uint32_t started = millis();
  BuzzerPcm encoder;
  uint16_t chunk[CHUNK_FRAMES * 2];  // 256-byte stack scratch, reused for the clip.
  const size_t signalFrames = RAMP_FRAMES + count + RAMP_FRAMES;
  // More than one whole DMA ring of GPIO-low tail ensures the clip has left
  // the ring before disabling; write completion alone only means data queued.
  const size_t totalFrames = signalFrames + CHUNK_FRAMES * (DMA_COUNT + 1);
  bool ok = true;
  for (size_t first = 0; first < totalFrames && ok; first += CHUNK_FRAMES) {
    const size_t frames = totalFrames - first < CHUNK_FRAMES ? totalFrames - first : CHUNK_FRAMES;
    for (size_t offset = 0; offset < frames; ++offset) {
      const size_t frame = first + offset;
      int16_t sample;
      if (frame < RAMP_FRAMES) {
        sample =
            static_cast<int16_t>(-32768 + 32768 * static_cast<int32_t>(frame) / static_cast<int32_t>(RAMP_FRAMES - 1));
      } else if (frame < RAMP_FRAMES + count) {
        sample = samples[frame - RAMP_FRAMES];
      } else if (frame < signalFrames) {
        sample = static_cast<int16_t>(-32768 * static_cast<int32_t>(frame - RAMP_FRAMES - count) /
                                      static_cast<int32_t>(RAMP_FRAMES - 1));
      } else {
        sample = INT16_MIN;
      }
      encoder.encode(sample, chunk + 2 * offset);
    }
    size_t written = 0;
    const size_t bytes = frames * 2 * sizeof(uint16_t);
    ok = millis() - started < 400 && i2s_channel_write(channel, chunk, bytes, &written, 40) == ESP_OK &&
         written == bytes;
  }
  const bool stopped = i2s_channel_disable(channel) == ESP_OK;
  if (stopped) pcmRunning_ = false;
  if (!ok || !stopped) end();
  return ok && stopped;
#else
  (void)samples;
  (void)count;
  return false;
#endif
}

#if defined(CONFIG_IDF_TARGET_ESP32S3)
bool Buzzer::writeStreamWords(const uint16_t* words, size_t frames, uint32_t started, uint32_t budget) {
  const uint32_t elapsed = millis() - started;
  if (elapsed >= budget) return false;
  const uint32_t remaining = budget - elapsed;
  const uint32_t timeout = remaining < 20 ? remaining : 20;
  const size_t bytes = frames * 2 * sizeof(uint16_t);
  size_t written = 0;
  return i2s_channel_write(static_cast<i2s_chan_handle_t>(pcmChannel_), words, bytes, &written, timeout) == ESP_OK &&
         written == bytes && millis() - started <= budget;
}
#endif

bool Buzzer::beginStream() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (!supportsPcm() || pcmRunning_ || pcmUse_ == PcmUse::Stream || !preparePcm()) return false;
  if (i2s_channel_enable(static_cast<i2s_chan_handle_t>(pcmChannel_)) != ESP_OK) {
    end();
    return false;
  }
  pcmRunning_ = true;
  pcmUse_ = PcmUse::Stream;
  streamEncoder_ = BuzzerPcm{};
  streamLast_ = 0;
  // Bring the one-pin output from GPIO-low to the zero-PCM duty cycle.
  uint16_t chunk[CHUNK_FRAMES * 2];
  const uint32_t started = millis();
  for (size_t first = 0; first < RAMP_FRAMES; first += CHUNK_FRAMES) {
    const size_t frames = RAMP_FRAMES - first < CHUNK_FRAMES ? RAMP_FRAMES - first : CHUNK_FRAMES;
    for (size_t i = 0; i < frames; ++i) {
      const auto sample = static_cast<int16_t>(-32768 + 32768 * static_cast<int32_t>(first + i) /
                                                            static_cast<int32_t>(RAMP_FRAMES - 1));
      streamEncoder_.encode(sample, chunk + i * 2);
    }
    if (!writeStreamWords(chunk, frames, started, 40)) {
      end();
      return false;
    }
  }
  return true;
#else
  return false;
#endif
}

bool Buzzer::writeStream(const int16_t* samples, size_t count, uint8_t volume, uint8_t maxGainPercent) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (pcmUse_ != PcmUse::Stream || !pcmRunning_ || !samples || !count || count > kStreamMaxSamples) return false;
  uint16_t chunk[CHUNK_FRAMES * 2];  // 256-byte producer-stack scratch; no allocation per write.
  const uint32_t started = millis();
  for (size_t first = 0; first < count; first += CHUNK_FRAMES) {
    const size_t frames = count - first < CHUNK_FRAMES ? count - first : CHUNK_FRAMES;
    for (size_t i = 0; i < frames; ++i) {
      streamLast_ = BuzzerPcm::scale(samples[first + i], volume, maxGainPercent);
      streamEncoder_.encode(streamLast_, chunk + i * 2);
    }
    if (!writeStreamWords(chunk, frames, started, 100)) {
      end();
      return false;
    }
  }
  return true;
#else
  (void)samples;
  (void)count;
  (void)volume;
  (void)maxGainPercent;
  return false;
#endif
}

bool Buzzer::endStream() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  bool ok = true;
  if (pcmUse_ == PcmUse::Stream && pcmRunning_) {
    uint16_t chunk[CHUNK_FRAMES * 2];
    const uint32_t started = millis();
    // Fill more than one complete ring with GPIO-low tail, so the fade has
    // reached the pin before disabling DMA. Never wait indefinitely to drain.
    constexpr size_t TOTAL_FRAMES = RAMP_FRAMES + CHUNK_FRAMES * (DMA_COUNT + 1);
    for (size_t first = 0; first < TOTAL_FRAMES && ok; first += CHUNK_FRAMES) {
      const size_t frames = TOTAL_FRAMES - first < CHUNK_FRAMES ? TOTAL_FRAMES - first : CHUNK_FRAMES;
      for (size_t i = 0; i < frames; ++i) {
        const size_t frame = first + i;
        const auto sample = frame < RAMP_FRAMES
                                ? static_cast<int16_t>(streamLast_ + (-32768 - static_cast<int32_t>(streamLast_)) *
                                                                         static_cast<int32_t>(frame) /
                                                                         static_cast<int32_t>(RAMP_FRAMES - 1))
                                : INT16_MIN;
        streamEncoder_.encode(sample, chunk + i * 2);
      }
      ok = writeStreamWords(chunk, frames, started, 40);
    }
  }
  end();
  return ok && !present();
#else
  return false;
#endif
}

}  // namespace freeink

#else  // FREEINK_CAP_BUZZER — no buzzer on this board.

namespace freeink {
bool Buzzer::begin() { return false; }
bool Buzzer::supportsPcm() { return false; }
bool Buzzer::playPcm(const int16_t*, size_t) { return false; }
bool Buzzer::beginStream() { return false; }
bool Buzzer::writeStream(const int16_t*, size_t, uint8_t, uint8_t) { return false; }
bool Buzzer::endStream() { return false; }
void Buzzer::tone(uint32_t, uint32_t) {}
void Buzzer::noTone() {}
void Buzzer::end() {}
}  // namespace freeink

#endif  // FREEINK_CAP_BUZZER
