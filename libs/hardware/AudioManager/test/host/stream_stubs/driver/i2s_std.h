#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

#include <algorithm>
#include <cassert>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = -2;
constexpr int I2S_NUM_0 = 0, I2S_ROLE_MASTER = 0, I2S_GPIO_UNUSED = -1;
constexpr int I2S_MCLK_MULTIPLE_256 = 256, I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_MODE_STEREO = 2;
struct FakeChannel {
  bool initialized = false, enabled = false, deleted = true;
};
using i2s_chan_handle_t = FakeChannel*;
struct i2s_chan_config_t {
  int id, role;
  bool auto_clear = false;
  uint32_t dma_desc_num = 6, dma_frame_num = 240;
};
struct i2s_std_clk_config_t {
  uint32_t sample_rate_hz;
  int mclk_multiple = 256;
};
struct i2s_std_config_t {
  i2s_std_clk_config_t clk_cfg;
  struct {
    int width, mode;
  } slot_cfg;
  struct {
    int mclk, bclk, ws, dout, din;
  } gpio_cfg;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) {id, role}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {rate}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode) {width, mode}
inline FakeChannel txFake;
inline i2s_chan_config_t channelConfig{};
inline i2s_std_config_t standardConfig{};
inline size_t allocations = 0, deletions = 0, initCalls = 0, writeCalls = 0;
inline bool failAllocation = false, failInit = false, failEnable = false, failReconfigure = false;
inline size_t maxWrite = SIZE_MAX, zeroWriteAt = SIZE_MAX, hardFailAt = SIZE_MAX, oddWriteAt = SIZE_MAX;
inline size_t overWriteAt = SIZE_MAX;
inline uint32_t writeDelay = 0;
inline bool timeoutResult = false;
inline std::vector<int16_t> writtenSlots;
inline std::vector<uint32_t> waitBudgets;
inline int i2s_new_channel(const i2s_chan_config_t* cfg, i2s_chan_handle_t* tx, i2s_chan_handle_t* rx) {
  assert(cfg->id == 0 && cfg->role == I2S_ROLE_MASTER && cfg->auto_clear && rx == nullptr);
  if (failAllocation) return ESP_FAIL;
  assert(txFake.deleted);
  channelConfig = *cfg;
  txFake = {};
  txFake.deleted = false;
  *tx = &txFake;
  ++allocations;
  return ESP_OK;
}
inline int i2s_channel_init_std_mode(i2s_chan_handle_t tx, const i2s_std_config_t* cfg) {
  ++initCalls;
  if (failInit) return ESP_FAIL;
  assert(cfg->clk_cfg.mclk_multiple == 256 && cfg->slot_cfg.width == 16 && cfg->slot_cfg.mode == 2);
  assert(cfg->gpio_cfg.mclk == 13 && cfg->gpio_cfg.bclk == 14 && cfg->gpio_cfg.ws == 47 && cfg->gpio_cfg.dout == 48 &&
         cfg->gpio_cfg.din == -1);
  standardConfig = *cfg;
  tx->initialized = true;
  return ESP_OK;
}
inline int i2s_channel_enable(i2s_chan_handle_t tx) {
  assert(tx->initialized && !tx->enabled && !tx->deleted);
  if (failEnable) return ESP_FAIL;
  tx->enabled = true;
  return ESP_OK;
}
inline int i2s_channel_disable(i2s_chan_handle_t tx) {
  assert(tx->enabled && !tx->deleted);
  tx->enabled = false;
  return ESP_OK;
}
inline int i2s_del_channel(i2s_chan_handle_t tx) {
  assert(!tx->enabled && !tx->deleted);
  tx->deleted = true;
  ++deletions;
  return ESP_OK;
}
inline int i2s_channel_reconfig_std_clock(i2s_chan_handle_t tx, const i2s_std_clk_config_t* clk) {
  assert(!tx->enabled && !tx->deleted);
  if (failReconfigure) return ESP_FAIL;
  standardConfig.clk_cfg = *clk;
  return ESP_OK;
}
inline int i2s_channel_write(i2s_chan_handle_t tx, const void* data, size_t bytes, size_t* written,
                             uint32_t timeoutMs) {
  assert(tx->enabled && !tx->deleted);
  ++writeCalls;
  waitBudgets.push_back(timeoutMs);
  fakeNow += std::min(writeDelay, timeoutMs);
  *written = 0;
  if (writeCalls == zeroWriteAt) return ESP_OK;
  if (writeCalls == hardFailAt) return ESP_FAIL;
  if (writeCalls == oddWriteAt) {
    *written = 1;
    return ESP_OK;
  }
  if (writeCalls == overWriteAt) {
    *written = bytes + 4;
    return ESP_OK;
  }
  bytes = std::min(bytes, maxWrite);
  const auto* samples = static_cast<const int16_t*>(data);
  writtenSlots.insert(writtenSlots.end(), samples, samples + bytes / sizeof(int16_t));
  *written = bytes;
  return timeoutResult ? ESP_ERR_TIMEOUT : ESP_OK;
}
