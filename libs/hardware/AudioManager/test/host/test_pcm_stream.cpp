#include <AudioManager.h>
#include <BoardConfig.h>
#include <Wire.h>
#include <driver/i2s_std.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

void resetIo() {
  writtenSlots.clear();
  waitBudgets.clear();
  maxWrite = SIZE_MAX;
  zeroWriteAt = hardFailAt = oddWriteAt = overWriteAt = SIZE_MAX;
  writeDelay = 0;
  timeoutResult = false;
}

void assertReleased() {
  assert(txFake.deleted && !txFake.enabled && gpioLevels[39] == LOW);
  assert(allocations == deletions);
}

void assertStarted(AudioManager& audio) {
  resetIo();
  assert(audio.beginPcmStream());
  assert(audio.isPlaying() && gpioLevels[39] == HIGH && txFake.enabled);
  assert(standardConfig.clk_cfg.sample_rate_hz == 24000);
  assert(standardConfig.clk_cfg.mclk_multiple == 256);
  assert(channelConfig.dma_desc_num == 4 && channelConfig.dma_frame_num == 120);
  assert(Wire.registers[0x04] == 0x10 && Wire.registers[0x32] == 0xBF);
  assert(Wire.registers[0x31] == 0);
  assert(!audio.beginPcmStream() && !audio.requestRestart());
  resetIo();
}

void assertSamples(const std::vector<int16_t>& samples, uint8_t volume) {
  assert(writtenSlots.size() == samples.size() * 2);
  for (size_t i = 0; i < samples.size(); ++i) {
    const int16_t expected = static_cast<int16_t>(static_cast<int32_t>(samples[i]) * std::min<int>(volume, 100) / 100);
    assert(writtenSlots[2 * i] == expected && writtenSlots[2 * i + 1] == expected);
  }
  for (const auto timeout : waitBudgets) assert(timeout > 0 && timeout <= 20);
}

AudioManager::PcmSource emptySource() {
  return {[](uint8_t*, size_t) { return 0; }, {}};
}

}  // namespace

int main() {
  assert(BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sEs8311Mclk16k);
  static_assert(FREEINK_CAP_AUDIO);
  AudioManager audio;
  int16_t sample = 1234;
  assert(!audio.writePcmStream(&sample, 1));
  assert(!audio.beginPcmStream(16000) && !audio.beginPcmStream(48000));
  assert(allocations == 0);

  // Every conversion preserves order, sample count, extremes and sample clock.
  for (const size_t count : {size_t(1), size_t(2), size_t(63), size_t(64), size_t(65), size_t(511), size_t(1024)}) {
    for (const uint8_t volume : {uint8_t(0), uint8_t(1), uint8_t(50), uint8_t(100), uint8_t(255)}) {
      assertStarted(audio);
      const size_t i2cBefore = Wire.writes.size();
      std::vector<int16_t> samples(count);
      for (size_t i = 0; i < count; ++i) samples[i] = static_cast<int16_t>((i * 131 + 32768) & 0xFFFF);
      if (count > 1) samples[1] = INT16_MAX;
      assert(audio.writePcmStream(samples.data(), count, volume));
      assertSamples(samples, volume);
      assert(Wire.writes.size() == i2cBefore && taskCreations == 0);
      audio.endPcmStream();
      assertReleased();
      assert(!audio.isPlaying());
      assert(Wire.registers[0x31] == 0x60 && Wire.registers[0x12] == 0x02 && Wire.registers[0x0D] == 0xFA);
      assert(Wire.registers[0x01] == 0);
      audio.endPcmStream();
      assertReleased();
    }
  }

  // One second of a 1 kHz waveform stays 24000 source frames at a 24000 Hz
  // output clock, regardless of the producer's uneven chunk sizes.
  assertStarted(audio);
  std::vector<int16_t> oneSecond(24000);
  for (size_t i = 0; i < oneSecond.size(); ++i) oneSecond[i] = i % 24 < 12 ? 12000 : -12000;
  for (size_t offset = 0; offset < oneSecond.size();) {
    const size_t want = offset % 3 == 0 ? 37 : 1024;
    const size_t count = std::min(want, oneSecond.size() - offset);
    assert(audio.writePcmStream(oneSecond.data() + offset, count));
    offset += count;
  }
  assertSamples(oneSecond, 100);
  assert(standardConfig.clk_cfg.sample_rate_hz == 24000);
  audio.endPcmStream();
  assertReleased();

  // Short writes, including accepted prefixes returned with TIMEOUT, are sent
  // exactly once; arbitrary chunk boundaries do not alter samples or timing.
  for (const size_t limit : {size_t(4), size_t(12), size_t(60), size_t(256)}) {
    assertStarted(audio);
    maxWrite = limit;
    timeoutResult = true;
    const std::vector<int16_t> samples = {INT16_MIN, -2000, -1, 0, 1, 2000, INT16_MAX};
    assert(audio.writePcmStream(samples.data(), samples.size(), 100));
    assertSamples(samples, 100);
    audio.endPcmStream();
    assertReleased();
  }

  // Invalid input is terminal; end is still required and must not drain/replay.
  for (int invalid = 0; invalid < 3; ++invalid) {
    assertStarted(audio);
    const size_t calls = writeCalls;
    assert(!audio.writePcmStream(invalid == 0 ? nullptr : &sample, invalid == 1 ? 0 : invalid == 2 ? 1025 : 1));
    assert(!audio.writePcmStream(&sample, 1));
    assert(gpioLevels[39] == LOW && audio.isPlaying());
    audio.endPcmStream();
    assert(writeCalls == calls);
    assertReleased();
  }

  // A mid-buffer failure cannot be retried, even after a successful prefix.
  for (int failure = 0; failure < 4; ++failure) {
    assertStarted(audio);
    std::vector<int16_t> samples(200, -1234);
    if (failure == 0) zeroWriteAt = writeCalls + 2;
    if (failure == 1) hardFailAt = writeCalls + 2;
    if (failure == 2) oddWriteAt = writeCalls + 2;
    if (failure == 3) overWriteAt = writeCalls + 2;
    const size_t i2cBefore = Wire.writes.size();
    assert(!audio.writePcmStream(samples.data(), samples.size()));
    assert(writtenSlots.size() == 128 && gpioLevels[39] == LOW);
    const size_t calls = writeCalls;
    assert(!audio.writePcmStream(samples.data(), samples.size()));
    assert(writeCalls == calls && Wire.writes.size() == i2cBefore);
    audio.endPcmStream();
    assert(writeCalls == calls);
    assertReleased();
  }

  // Bound waits across all chunks, including millis() rollover; the test tick
  // is 10 ms so accidentally passing ticks to IDF's ms API is observable.
  for (const uint32_t now : {uint32_t(1000), UINT32_MAX - 35}) {
    assertStarted(audio);
    fakeNow = now;
    maxWrite = 4;
    writeDelay = 20;
    timeoutResult = true;
    std::vector<int16_t> samples(1024, 1234);
    assert(!audio.writePcmStream(samples.data(), samples.size()));
    assert(static_cast<uint32_t>(fakeNow - now) == 100);
    assert(waitBudgets.size() == 5 && *std::max_element(waitBudgets.begin(), waitBudgets.end()) == 20);
    audio.endPcmStream();
    assertReleased();
  }

  // End appends a correctly signed ramp and sufficient silence, then releases.
  assertStarted(audio);
  sample = -32000;
  assert(audio.writePcmStream(&sample, 1));
  resetIo();
  audio.endPcmStream();
  assert(writtenSlots.size() == (64 + 512) * 2);
  for (size_t i = 0; i < 64; ++i) {
    assert(writtenSlots[2 * i] == -32000 * static_cast<int>(63 - i) / 64);
    assert(writtenSlots[2 * i] == writtenSlots[2 * i + 1]);
  }
  assert(std::all_of(writtenSlots.begin() + 128, writtenSlots.end(), [](int16_t value) { return value == 0; }));
  assertReleased();
  assertStarted(audio);
  writeDelay = 20;
  maxWrite = 4;
  const uint32_t endStart = fakeNow;
  audio.endPcmStream();
  assert(static_cast<uint32_t>(fakeNow - endStart) == 40);
  assertReleased();

  // Setup rollback at each transport boundary, then retry using the same object.
  resetIo();
  for (bool* failure : {&failAllocation, &failInit, &failEnable}) {
    for (bool alreadyBegun : {false, true}) {
      if (alreadyBegun) assert(audio.begin());
      *failure = true;
      assert(!audio.beginPcmStream());
      assertReleased();
      *failure = false;
      assertStarted(audio);
      audio.endPcmStream();
      assertReleased();
    }
  }
  Wire.failAll = true;
  assert(!audio.beginPcmStream());
  assertReleased();
  Wire.failAll = false;
  for (const int reg : {0x31, 0x04, 0x32}) {
    assert(audio.begin());
    Wire.failReg = reg;
    assert(!audio.beginPcmStream());
    assertReleased();
    Wire.failReg = -1;
  }
  assert(audio.begin());
  Wire.failAt = Wire.attempts + 4;  // Final unmute after prime, clock and gain.
  assert(!audio.beginPcmStream());
  assertReleased();
  Wire.failAt = SIZE_MAX;
  zeroWriteAt = writeCalls + 1;
  assert(!audio.beginPcmStream());
  assertReleased();
  resetIo();
  assertStarted(audio);
  Wire.failAll = true;
  audio.endPcmStream();  // I2C failure must not prevent amp/I2S cleanup.
  assertReleased();
  Wire.failAll = false;
  assertStarted(audio);
  audio.powerDown();  // Owner shutdown does not release the channel twice.
  assertReleased();
  assert(!audio.isPlaying());

  // Preserve async playPcm/short cues and refuse to steal their live channel.
  assert(audio.playPcm(emptySource(), 16000, 1));
  assert(Wire.registers[0x04] == 0x20 && standardConfig.clk_cfg.sample_rate_hz == 16000);
  assert(channelConfig.dma_desc_num == 6 && channelConfig.dma_frame_num == 240);
  const size_t asyncAllocations = allocations;
  assert(!audio.beginPcmStream() && allocations == asyncAllocations);
  audio.stop();
  assert(!audio.isPlaying());
  const size_t tasksBeforeStream = taskCreations;
  assertStarted(audio);
  assert(!audio.playPcm(emptySource(), 16000, 1));
  assert(!audio.play({}, false));
  assert(taskCreations == tasksBeforeStream);
  audio.stop();
  assertReleased();
  failTask = true;
  assert(!audio.playPcm(emptySource(), 16000, 1));
  assertReleased();
  failTask = false;

  // A task which has not acknowledged stop retains its channel. Neither
  // powerDown nor a replacement playback may destroy/reconfigure it.
  assert(audio.playPcm(emptySource(), 16000, 1));
  autoRunTask = false;
  const size_t beforeStopDeletes = deletions;
  audio.stop();
  assert(audio.isPlaying() && deletions == beforeStopDeletes);
  audio.powerDown();
  assert(audio.isPlaying() && deletions == beforeStopDeletes && txFake.enabled);
  assert(!audio.beginPcmStream());
  assert(!audio.playPcm(emptySource(), 24000, 1));
  assert(standardConfig.clk_cfg.sample_rate_hz == 16000 && deletions == beforeStopDeletes);
  autoRunTask = true;
  runTask();
  audio.powerDown();
  assertReleased();
  std::puts(
      "PCM stream: format/volume, no replay, deadlines, DMA drain, codec rates, rollback and async ownership passed");
}
