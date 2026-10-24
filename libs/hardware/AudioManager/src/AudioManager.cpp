#include "AudioManager.h"

#include <BoardConfig.h>

// Capability-gated like FrontlightManager: devices without FREEINK_CAP_AUDIO
// compile the stub bodies at the bottom and link no I2S/codec code.
#if FREEINK_CAP_AUDIO

#include <Wire.h>
#include <driver/i2s_std.h>

#include <memory>

namespace freeink {

namespace {

constexpr uint32_t CODEC_I2C_HZ = 100000;  // OEM bus speed (shared with touch)
constexpr size_t READ_CHUNK = 1024;        // mono source bytes per loop pass
constexpr size_t STREAM_CHUNK_FRAMES = 64;
constexpr uint32_t STREAM_RATE = 24000;
constexpr uint32_t STREAM_DMA_DESCRIPTORS = 4;
constexpr uint32_t STREAM_DMA_FRAMES = 120;  // 4 * 120 / 24000 = 20 ms
constexpr uint32_t STREAM_WRITE_BUDGET_MS = 100;
constexpr uint32_t STREAM_DRAIN_BUDGET_MS = 40;
constexpr uint32_t STREAM_WRITE_SLICE_MS = 20;

// ES8388 playback init recovered from the Murphy OEM firmware — exact register
// order matters (staged mute -> clocks/format -> mixers -> power -> unmute).
struct RegVal {
  uint8_t reg, val;
};
constexpr RegVal ES8388_INIT[] = {
    {0x19, 0x04}, {0x01, 0x50}, {0x02, 0x00}, {0x08, 0x00}, {0x04, 0x3e}, {0x00, 0x12}, {0x17, 0x18}, {0x18, 0x02},
    {0x26, 0x1b}, {0x27, 0x90}, {0x2a, 0x90}, {0x2b, 0x80}, {0x2d, 0x00}, {0x1b, 0x00}, {0x1a, 0x00}, {0x03, 0xff},
    {0x09, 0x88}, {0x0a, 0xf0}, {0x0b, 0x80}, {0x0c, 0x0e}, {0x0d, 0x02}, {0x10, 0x20}, {0x11, 0x20}, {0x2e, 0x1e},
    {0x2f, 0x1e}, {0x30, 0x1e}, {0x31, 0x1e}, {0x04, 0x3c}, {0x19, 0x00},
};

constexpr uint8_t ES8388_VOL_MAX_REG = 0x21;  // register full-scale for OUT volumes

// ES8311 playback init, mirroring M5Unified's PaperColor speaker bring-up
// (_speaker_enabled_cb_papercolor). The codec derives its internal MCLK from
// BCLK (reg 0x01 bit7 + reg 0x02 MULT_PRE=3, i.e. 32*fs * 8 = 256*fs), so no
// MCLK line is needed and the same init covers every sample rate. The 16-bit
// I2S SDP format (0x09) is set explicitly — M5Unified relies on the default.
constexpr RegVal ES8311_INIT[] = {
    {0x00, 0x80},  // RESET: CSM power on, slave mode
    {0x01, 0xB5},  // CLK_MANAGER: internal MCLK from BCLK pin, clocks on
    {0x02, 0x18},  // CLK_MANAGER: MULT_PRE x8 -> internal MCLK = 256*fs
    {0x09, 0x0C},  // SDP-in: I2S format, 16-bit
    {0x0D, 0x01},  // SYSTEM: power up analog circuitry
    {0x12, 0x00},  // SYSTEM: power up DAC
    {0x13, 0x10},  // SYSTEM: enable output to HP drive
    {0x32, 0xCF},  // DAC volume +8 dB (M5's default for the 1W speaker)
    {0x37, 0x08},  // DAC: bypass equalizer
};

constexpr RegVal ES8311_MCLK_INIT[] = {
    {0x00, 0x80}, {0x01, 0x3F}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x20}, {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00},
    {0x08, 0xFF}, {0x09, 0x0C}, {0x0D, 0x01}, {0x12, 0x00}, {0x13, 0x10}, {0x32, 0xCF}, {0x37, 0x08},
};

constexpr uint8_t ES8311_VOL_MAX_REG = 0xCF;  // +8 dB, 0.5 dB/step (0xBF = 0 dB)

uint32_t readLE32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t readLE16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

bool isEs8311(BoardConfig::AudioOutput output) {
  return output == BoardConfig::AudioOutput::I2sEs8311 || output == BoardConfig::AudioOutput::I2sEs8311Mclk16k;
}

}  // namespace

bool AudioManager::present() const {
  return BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sEs8388 ||
         isEs8311(BoardConfig::ACTIVE.audio.output) ||
         BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sDac;
}

bool AudioManager::codecWrite(uint8_t reg, uint8_t value) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  Wire.beginTransmission(cfg.codecAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool AudioManager::codecInit() {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.codecAddr == 0) return true;  // plain I2S DAC, nothing to configure

  const RegVal* seq;
  size_t seqLen;
  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311Mclk16k) {
    seq = ES8311_MCLK_INIT;
    seqLen = sizeof(ES8311_MCLK_INIT) / sizeof(ES8311_MCLK_INIT[0]);
  } else if (cfg.output == BoardConfig::AudioOutput::I2sEs8311) {
    seq = ES8311_INIT;
    seqLen = sizeof(ES8311_INIT) / sizeof(ES8311_INIT[0]);
  } else {
    seq = ES8388_INIT;
    seqLen = sizeof(ES8388_INIT) / sizeof(ES8388_INIT[0]);
  }

  Wire.begin(cfg.codecSda, cfg.codecScl, CODEC_I2C_HZ);

  // The OEM firmware retries until the codec ACKs; three attempts is plenty
  // for a codec already powered.
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool ok = true;
    for (size_t i = 0; i < seqLen; ++i) {
      if (!codecWrite(seq[i].reg, seq[i].val)) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
    delay(100);
  }
  return false;
}

bool AudioManager::begin() {
  if (begun_) return true;
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (!present()) return false;

  if (cfg.ampEnable != BoardConfig::PIN_UNASSIGNED) {
    pinMode(cfg.ampEnable, OUTPUT);
    digitalWrite(cfg.ampEnable, LOW);  // amp comes up only during playback
  }
  if (cfg.enable != BoardConfig::PIN_UNASSIGNED) {
    pinMode(cfg.enable, OUTPUT);
    digitalWrite(cfg.enable, cfg.enableActiveHigh ? HIGH : LOW);
    delay(10);  // codec rail ramp before the first I2C access
  }

  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311Mclk16k && !ensureI2s(16000)) {
    log_e("audio mclk setup failed");
    rollbackBegin();
    return false;
  }

  if (!codecInit()) {
    log_e("audio codec init failed");
    shutdownCodec();  // best-effort suspend after a partially applied sequence
    rollbackBegin();
    return false;
  }
  begun_ = true;
  return true;
}

void AudioManager::setVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.codecAddr == 0) return;
  if (isEs8311(cfg.output)) {
    // Single DAC volume register, 0.5 dB/step; full scale matches the init
    // value M5 ships for this speaker (+8 dB).
    codecWrite(0x32, (uint8_t)((uint16_t)percent * ES8311_VOL_MAX_REG / 100));
    return;
  }
  const uint8_t reg = (uint8_t)((uint16_t)percent * ES8388_VOL_MAX_REG / 100);
  // OUT1 (0x2e/0x2f) and OUT2 (0x30/0x31) pairs, like the OEM volume path.
  codecWrite(0x2e, reg);
  codecWrite(0x2f, reg);
  codecWrite(0x30, reg);
  codecWrite(0x31, reg);
}

// DAC mute between alarms so nothing residual reaches the output.
bool AudioManager::codecMute(bool mute) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.codecAddr == 0) return true;
  if (isEs8311(cfg.output)) {
    return codecWrite(0x31, mute ? 0x60 : 0x00);  // DAC_MUTE bits
  }
  return codecWrite(0x19, mute ? 0x04 : 0x00);  // ES8388 DACCONTROL3 soft mute
}

void AudioManager::setAmp(bool on) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.ampEnable == BoardConfig::PIN_UNASSIGNED) return;
  digitalWrite(cfg.ampEnable, on ? HIGH : LOW);
}

void AudioManager::rollbackBegin() {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  setAmp(false);
  teardownI2s();
  if (cfg.enable != BoardConfig::PIN_UNASSIGNED) {
    digitalWrite(cfg.enable, cfg.enableActiveHigh ? LOW : HIGH);
  }
}

void AudioManager::powerDown() {
  if (!begun_) return;
  stop();
  // A blocking source callback can outlive stop's bounded wait. Its task still
  // owns the channel; never delete/reconfigure that channel underneath it.
  if (!begun_ || isPlaying()) return;
  shutdownCodec();
  teardownI2s();
}

void AudioManager::shutdownCodec() {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (isEs8311(cfg.output)) {
    // Like M5Unified's disable path: drop the amp and the codec rail.
    setAmp(false);
    if (cfg.enable != BoardConfig::PIN_UNASSIGNED) {
      digitalWrite(cfg.enable, cfg.enableActiveHigh ? LOW : HIGH);
    } else if (cfg.output == BoardConfig::AudioOutput::I2sEs8311Mclk16k) {
      // Wave's codec rail is shared and has no local GPIO gate. Suspend the
      // DAC/analog path and its clocks without cutting unrelated peripherals.
      codecWrite(0x12, 0x02);
      codecWrite(0x0D, 0xFA);
      codecWrite(0x01, 0x00);
    }
  } else if (cfg.codecAddr != 0) {
    codecWrite(0x02, 0xff);  // ES8388 CHIPPOWER: everything off
  }
  begun_ = false;
}

bool AudioManager::parseWavHeader(const WavSource& source, WavInfo& info) {
  uint8_t hdr[12];
  if (!source.seek(0)) return false;
  if (source.read(hdr, 12) != 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

  size_t pos = 12;
  bool haveFmt = false;
  for (int guard = 0; guard < 32; ++guard) {
    uint8_t chunk[8];
    if (!source.seek(pos) || source.read(chunk, 8) != 8) return false;
    const uint32_t size = readLE32(chunk + 4);
    pos += 8;

    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || source.read(fmt, 16) != 16) return false;
      const uint16_t audioFormat = readLE16(fmt);
      info.channels = readLE16(fmt + 2);
      info.sampleRate = readLE32(fmt + 4);
      info.bitsPerSample = readLE16(fmt + 14);
      if (audioFormat != 1) return false;  // PCM only
      haveFmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      info.dataStart = pos;
      info.dataLength = size;
      break;
    }
    pos += size + (size & 1);  // chunks are word-aligned
  }

  return haveFmt && info.dataStart != 0 && info.bitsPerSample == 16 && (info.channels == 1 || info.channels == 2) &&
         info.sampleRate >= 8000 && info.sampleRate <= 48000;
}

bool AudioManager::ensureI2s(uint32_t sampleRate, bool stream) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311Mclk16k && sampleRate != 16000 && sampleRate != 24000) {
    log_e("external-mclk ES8311 supports 16/24 kHz PCM only");
    return false;
  }
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;

  if (tx && currentRate_ == sampleRate) {
    // Channel exists but was disabled when the last playback drained.
    if (!chanEnabled_) {
      if (i2s_channel_enable(tx) != ESP_OK) return false;
      chanEnabled_ = true;
    }
    return configureCodecRate(sampleRate);
  }

  if (tx) {
    if (chanEnabled_) i2s_channel_disable(tx);
    chanEnabled_ = false;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // codec runs 256x MCLK/LRCK
    if (i2s_channel_reconfig_std_clock(tx, &clk) != ESP_OK) return false;
    if (i2s_channel_enable(tx) != ESP_OK) return false;
    chanEnabled_ = true;
    currentRate_ = sampleRate;
    return configureCodecRate(sampleRate);
  }

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Without auto_clear the DMA replays its last buffers on underrun — heard
  // as a looping stutter after playback stops.
  chanCfg.auto_clear = true;
  if (stream) {
    chanCfg.dma_desc_num = STREAM_DMA_DESCRIPTORS;
    chanCfg.dma_frame_num = STREAM_DMA_FRAMES;
  }
  if (i2s_new_channel(&chanCfg, &tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t std = {};
  std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std.gpio_cfg.mclk = cfg.mclk == BoardConfig::PIN_UNASSIGNED ? I2S_GPIO_UNUSED : (gpio_num_t)cfg.mclk;
  std.gpio_cfg.bclk = (gpio_num_t)cfg.bclk;
  std.gpio_cfg.ws = (gpio_num_t)cfg.lrclk;
  std.gpio_cfg.dout = (gpio_num_t)cfg.dout;
  std.gpio_cfg.din = I2S_GPIO_UNUSED;

  if (i2s_channel_init_std_mode(tx, &std) != ESP_OK) {
    i2s_del_channel(tx);
    return false;
  }
  if (i2s_channel_enable(tx) != ESP_OK) {
    i2s_del_channel(tx);
    return false;
  }
  txChan_ = tx;
  chanEnabled_ = true;
  currentRate_ = sampleRate;
  return configureCodecRate(sampleRate);
}

bool AudioManager::configureCodecRate(uint32_t sampleRate) {
  if (begun_ && BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sEs8311Mclk16k) {
    // Espressif's 256-fs coefficients use DAC OSR 32 at 16 kHz, 16 at 24 kHz.
    // Write only once clocks are running. Other clock divisors are identical.
    // See docs/waveshare-audio-stream.md for the pinned source and coefficients.
    if (!codecWrite(0x04, sampleRate == 16000 ? 0x20 : 0x10)) {
      teardownI2s();
      return false;
    }
  }
  return true;
}

void AudioManager::teardownI2s() {
  if (!txChan_) return;
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;
  if (chanEnabled_) i2s_channel_disable(tx);
  i2s_del_channel(tx);
  txChan_ = nullptr;
  chanEnabled_ = false;
  currentRate_ = 0;
}

bool AudioManager::play(const WavSource& source, bool loop) {
  const PlaybackState state = playbackState_.load(std::memory_order_acquire);
  if (state == PlaybackState::PcmStream || state == PlaybackState::PcmStreamFailed) return false;
  if (!begun_ && !begin()) return false;
  stop();
  if (isPlaying()) return false;

  WavInfo info;
  if (!parseWavHeader(source, info)) {
    log_e("unsupported WAV (need 16-bit PCM, 1-2ch, 8-48 kHz)");
    return false;
  }
  if (!ensureI2s(info.sampleRate)) {
    log_e("i2s setup failed");
    return false;
  }
  if (!source.seek(info.dataStart)) return false;

  return startPlayback(source, info, loop);
}

bool AudioManager::playPcm(const PcmSource& source, uint32_t sampleRate, uint8_t channels) {
  if (!source.read || sampleRate < 8000 || sampleRate > 48000 || (channels != 1 && channels != 2)) {
    log_e("unsupported PCM (need 16-bit LE, 1-2ch, 8-48 kHz)");
    return false;
  }
  const PlaybackState state = playbackState_.load(std::memory_order_acquire);
  if (state == PlaybackState::PcmStream || state == PlaybackState::PcmStreamFailed) return false;
  if (!begun_ && !begin()) return false;
  stop();
  if (isPlaying()) return false;
  if (source.seek && !source.seek(0)) {
    log_e("PCM source seek failed");
    return false;
  }
  if (!ensureI2s(sampleRate)) {
    log_e("i2s setup failed");
    return false;
  }

  WavInfo info;
  info.sampleRate = sampleRate;
  info.channels = channels;
  info.bitsPerSample = 16;

  WavSource wavSource;
  wavSource.read = source.read;
  wavSource.seek = source.seek;
  return startPlayback(wavSource, info, false);
}

bool AudioManager::beginPcmStream(uint32_t sampleRate) {
  if (sampleRate != STREAM_RATE || isPlaying()) return false;
  if (!begun_ && !begin()) return false;
  setAmp(false);
  if (!codecMute(true)) {
    powerDown();
    return false;
  }
  // Recreate idle DMA with a bounded 20 ms queue; async cue geometry stays as-is.
  teardownI2s();
  if (!ensureI2s(sampleRate, true)) {
    powerDown();
    return false;
  }
  if (isEs8311(BoardConfig::ACTIVE.audio.output) && !codecWrite(0x32, 0xBF)) {
    powerDown();
    return false;
  }
  playbackState_.store(PlaybackState::PcmStream, std::memory_order_release);
  streamLastSample_ = 0;
  int16_t silence[STREAM_CHUNK_FRAMES * 2] = {};
  if (!writeStreamSlots(silence, sizeof(silence), millis(), STREAM_WRITE_BUDGET_MS) || !codecMute(false)) {
    failPcmStream();
    endPcmStream();
    return false;
  }
  setAmp(true);
  if (BoardConfig::ACTIVE.audio.ampSettleMs > 0) {
    vTaskDelay(pdMS_TO_TICKS(BoardConfig::ACTIVE.audio.ampSettleMs));
  }
  return true;
}

bool AudioManager::writeStreamSlots(const int16_t* slots, size_t bytes, uint32_t started, uint32_t budgetMs) {
  size_t offset = 0;
  while (offset < bytes) {
    const uint32_t elapsed = static_cast<uint32_t>(millis()) - started;
    if (elapsed >= budgetMs) return false;
    const uint32_t remaining = budgetMs - elapsed;
    const uint32_t timeout = remaining < STREAM_WRITE_SLICE_MS ? remaining : STREAM_WRITE_SLICE_MS;
    size_t written = 0;
    const esp_err_t result =
        i2s_channel_write((i2s_chan_handle_t)txChan_, reinterpret_cast<const uint8_t*>(slots) + offset, bytes - offset,
                          &written, timeout);  // IDF takes milliseconds, not ticks.
    if (written > bytes - offset || written % (2 * sizeof(int16_t)) != 0 || written == 0) return false;
    offset += written;
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) return false;
  }
  return true;
}

void AudioManager::failPcmStream() {
  setAmp(false);
  playbackState_.store(PlaybackState::PcmStreamFailed, std::memory_order_release);
}

bool AudioManager::writePcmStream(const int16_t* samples, size_t count, uint8_t volume) {
  if (playbackState_.load(std::memory_order_acquire) != PlaybackState::PcmStream) return false;
  if (!samples || count == 0 || count > 1024) {
    failPcmStream();
    return false;
  }
  if (volume > 100) volume = 100;
  const uint32_t started = millis();
  int16_t slots[STREAM_CHUNK_FRAMES * 2];
  size_t offset = 0;
  while (offset < count) {
    const size_t frames = count - offset < STREAM_CHUNK_FRAMES ? count - offset : STREAM_CHUNK_FRAMES;
    for (size_t i = 0; i < frames; ++i) {
      const int16_t sample = static_cast<int16_t>(static_cast<int32_t>(samples[offset + i]) * volume / 100);
      slots[2 * i] = sample;
      slots[2 * i + 1] = sample;
    }
    if (!writeStreamSlots(slots, frames * 2 * sizeof(int16_t), started, STREAM_WRITE_BUDGET_MS)) {
      failPcmStream();
      return false;
    }
    streamLastSample_ = slots[(frames - 1) * 2];
    offset += frames;
  }
  return true;
}

void AudioManager::endPcmStream() {
  const PlaybackState state = playbackState_.load(std::memory_order_acquire);
  if (state != PlaybackState::PcmStream && state != PlaybackState::PcmStreamFailed) return;
  if (state == PlaybackState::PcmStream) {
    const uint32_t started = millis();
    int16_t slots[STREAM_CHUNK_FRAMES * 2];
    // Append a short release ramp, preserving every submitted source sample.
    for (size_t i = 0; i < STREAM_CHUNK_FRAMES; ++i) {
      const int16_t sample = static_cast<int16_t>(static_cast<int32_t>(streamLastSample_) *
                                                  static_cast<int32_t>(STREAM_CHUNK_FRAMES - 1 - i) /
                                                  static_cast<int32_t>(STREAM_CHUNK_FRAMES));
      slots[2 * i] = slots[2 * i + 1] = sample;
    }
    if (writeStreamSlots(slots, sizeof(slots), started, STREAM_DRAIN_BUDGET_MS)) {
      memset(slots, 0, sizeof(slots));
      // More silence than the complete DMA ring guarantees queued content and
      // the ramp reached the line before disabling it (normally under 24 ms).
      for (size_t frames = 0; frames < STREAM_DMA_DESCRIPTORS * STREAM_DMA_FRAMES; frames += STREAM_CHUNK_FRAMES) {
        if (!writeStreamSlots(slots, sizeof(slots), started, STREAM_DRAIN_BUDGET_MS)) break;
      }
    }
  }
  setAmp(false);
  codecMute(true);
  shutdownCodec();
  teardownI2s();
  streamLastSample_ = 0;
  playbackState_.store(PlaybackState::Idle, std::memory_order_release);
}

bool AudioManager::startPlayback(const WavSource& source, const WavInfo& info, bool loop) {
  // Unmute the DAC (stop() mutes it). Codec writes stay on the caller's core
  // so the shared I2C bus is never touched from the audio task. The speaker
  // amp comes up in the playback task once silence is flowing — enabling it
  // against an idle I2S line is an audible pop.
  codecMute(false);

  source_ = source;
  wav_ = info;
  loop_ = loop;
  playbackState_.store(PlaybackState::Playing, std::memory_order_release);

  // Same shape as the OEM "musicTask" (high priority, core 0 — the Arduino
  // loop owns core 1); 8K stack covers the on-stack sample buffers.
  if (xTaskCreatePinnedToCore(taskEntry, "audio_play", 8192, this, 10, &task_, 0) != pdPASS) {
    task_ = nullptr;
    setAmp(false);
    codecMute(true);
    teardownI2s();
    playbackState_.store(PlaybackState::Idle, std::memory_order_release);
    return false;
  }
  return true;
}

bool AudioManager::playBuffer(const uint8_t* data, size_t len, bool loop) {
  // Shared offset state lives in the lambdas; play() copies them.
  auto offset = std::make_shared<size_t>(0);
  WavSource src;
  src.read = [data, len, offset](uint8_t* dst, size_t want) -> int {
    const size_t left = len - *offset;
    const size_t n = want < left ? want : left;
    memcpy(dst, data + *offset, n);
    *offset += n;
    return (int)n;
  };
  src.seek = [len, offset](size_t pos) {
    if (pos > len) return false;
    *offset = pos;
    return true;
  };
  return play(src, loop);
}

bool AudioManager::requestRestart() {
  if (!source_.seek) return false;
  PlaybackState state = playbackState_.load(std::memory_order_acquire);
  for (;;) {
    if (state == PlaybackState::RestartPending) return true;
    if (state != PlaybackState::Playing) return false;
    if (playbackState_.compare_exchange_weak(state, PlaybackState::RestartPending, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      return true;
    }
  }
}

void AudioManager::stop() {
  if (!begun_) return;
  PlaybackState state = playbackState_.load(std::memory_order_acquire);
  if (state == PlaybackState::PcmStream || state == PlaybackState::PcmStreamFailed) {
    endPcmStream();
    return;
  }
  while (state == PlaybackState::Playing || state == PlaybackState::RestartPending) {
    if (playbackState_.compare_exchange_weak(state, PlaybackState::Stopping, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      break;
    }
  }
  for (int i = 0; i < 200 && playbackState_.load(std::memory_order_acquire) != PlaybackState::Idle; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // Drop the amp and mute the DAC so nothing residual reaches the output
  // between alarms.
  setAmp(false);
  codecMute(true);
}

void AudioManager::taskEntry(void* self) { static_cast<AudioManager*>(self)->taskLoop(); }

void AudioManager::taskLoop() {
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;
  alignas(int16_t) uint8_t inBuf[READ_CHUNK];
  // Mono is duplicated into both slots, so the out buffer is 2x.
  int16_t outBuf[READ_CHUNK];

  // Prime the line with silence, then raise the amp: the AW8737A pops loudly
  // when enabled against an idle or just-started I2S line.
  memset(outBuf, 0, sizeof(outBuf));
  for (int i = 0; i < 2; ++i) {
    size_t written = 0;
    if (i2s_channel_write(tx, outBuf, sizeof(outBuf), &written, pdMS_TO_TICKS(200)) != ESP_OK) break;
  }
  setAmp(true);
  if (BoardConfig::ACTIVE.audio.ampSettleMs > 0) {
    vTaskDelay(pdMS_TO_TICKS(BoardConfig::ACTIVE.audio.ampSettleMs));
  }

  size_t consumed = 0;
  bool draining = false;
  uint8_t drainBuffers = 0;
  for (;;) {
    PlaybackState state = playbackState_.load(std::memory_order_acquire);
    if (state == PlaybackState::RestartPending) {
      if (!source_.seek || !source_.seek(wav_.dataStart)) {
        log_e("PCM source restart failed");
        playbackState_.store(PlaybackState::Stopping, std::memory_order_release);
      } else {
        consumed = 0;
        draining = false;
        drainBuffers = 0;
        PlaybackState pending = PlaybackState::RestartPending;
        playbackState_.compare_exchange_strong(pending, PlaybackState::Playing, std::memory_order_acq_rel,
                                               std::memory_order_acquire);
      }
      continue;
    }
    if (state == PlaybackState::Stopping && !draining) {
      draining = true;
      drainBuffers = 0;
    }

    if (draining) {
      if (drainBuffers >= 6) {
        if (state == PlaybackState::Stopping) break;
        PlaybackState playing = PlaybackState::Playing;
        if (playbackState_.compare_exchange_strong(playing, PlaybackState::Stopping, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
          break;
        }
        continue;
      }
      memset(outBuf, 0, sizeof(outBuf));
      size_t written = 0;
      if (i2s_channel_write(tx, outBuf, sizeof(outBuf), &written, pdMS_TO_TICKS(200)) != ESP_OK) {
        playbackState_.store(PlaybackState::Stopping, std::memory_order_release);
        break;
      }
      ++drainBuffers;
      continue;
    }

    size_t want = READ_CHUNK;
    if (wav_.dataLength > 0) {
      const size_t left = wav_.dataLength - consumed;
      if (left == 0) {
        if (loop_ && source_.seek(wav_.dataStart)) {
          consumed = 0;
          continue;
        }
        draining = true;
        continue;
      }
      if (want > left) want = left;
    }
    const size_t frameBytes = static_cast<size_t>(wav_.channels) * sizeof(int16_t);
    want -= want % frameBytes;
    if (want == 0) {
      draining = true;
      continue;
    }

    const int n = source_.read(inBuf, want);
    if (n <= 0) {
      if (loop_ && source_.seek(wav_.dataStart)) {
        consumed = 0;
        continue;
      }
      draining = true;
      continue;
    }
    if (n > static_cast<int>(want) || static_cast<size_t>(n) % frameBytes != 0) {
      log_e("PCM source returned an invalid byte count");
      playbackState_.store(PlaybackState::Stopping, std::memory_order_release);
      break;
    }
    consumed += n;

    const int16_t* samples = (const int16_t*)inBuf;
    size_t outBytes;
    if (wav_.channels == 1) {
      const int frames = n / 2;
      for (int i = 0; i < frames; ++i) {
        outBuf[i * 2] = samples[i];
        outBuf[i * 2 + 1] = samples[i];
      }
      outBytes = (size_t)frames * 4;
    } else {
      memcpy(outBuf, inBuf, n);
      outBytes = (size_t)n;
    }

    size_t written = 0;
    if (i2s_channel_write(tx, outBuf, outBytes, &written, pdMS_TO_TICKS(1000)) != ESP_OK) {
      playbackState_.store(PlaybackState::Stopping, std::memory_order_release);
      break;
    }
  }

  setAmp(false);
  i2s_channel_disable(tx);
  chanEnabled_ = false;

  task_ = nullptr;
  playbackState_.store(PlaybackState::Idle, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace freeink

#else  // !FREEINK_CAP_AUDIO — stubs so callers need no #ifdefs

namespace freeink {
bool AudioManager::present() const { return false; }
bool AudioManager::begin() { return false; }
void AudioManager::setVolume(uint8_t) {}
bool AudioManager::play(const WavSource&, bool) { return false; }
bool AudioManager::playPcm(const PcmSource&, uint32_t, uint8_t) { return false; }
bool AudioManager::beginPcmStream(uint32_t) { return false; }
bool AudioManager::writePcmStream(const int16_t*, size_t, uint8_t) { return false; }
void AudioManager::endPcmStream() {}
bool AudioManager::playBuffer(const uint8_t*, size_t, bool) { return false; }
bool AudioManager::requestRestart() { return false; }
void AudioManager::stop() {}
void AudioManager::powerDown() {}
bool AudioManager::parseWavHeader(const WavSource&, WavInfo&) { return false; }
bool AudioManager::startPlayback(const WavSource&, const WavInfo&, bool) { return false; }
bool AudioManager::ensureI2s(uint32_t, bool) { return false; }
bool AudioManager::configureCodecRate(uint32_t) { return false; }
bool AudioManager::writeStreamSlots(const int16_t*, size_t, uint32_t, uint32_t) { return false; }
void AudioManager::failPcmStream() {}
void AudioManager::shutdownCodec() {}
void AudioManager::teardownI2s() {}
bool AudioManager::codecInit() { return false; }
bool AudioManager::codecWrite(uint8_t, uint8_t) { return false; }
bool AudioManager::codecMute(bool) { return false; }
void AudioManager::setAmp(bool) {}
void AudioManager::rollbackBegin() {}
void AudioManager::taskEntry(void*) {}
void AudioManager::taskLoop() {}
}  // namespace freeink

#endif  // FREEINK_CAP_AUDIO
