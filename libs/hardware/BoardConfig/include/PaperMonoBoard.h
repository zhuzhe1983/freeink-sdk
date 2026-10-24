#pragma once

// M5Stack Paper Mono — board bring-up over the PY32 helper chips, single owner.
//
// The Paper Mono routes its power/reset plumbing through the M5PM1 PMIC
// (M5Pm1.h) and the M5IOE1 expander (M5Ioe1.h) instead of ESP GPIOs. This
// header is the one place that sequences them, so the SDK's hardware managers
// can bring the board up themselves and consumer firmware never touches the
// PMIC/expander directly:
//   - EpdBus calls setEpdPower()/setEpdReset() (the profile leaves the display
//     powerEnable/rst pins unassigned),
//   - InputManager calls enableTouch() before probing the FT6336 and polls
//     pollPowerButtonClicked() into BTN_POWER events,
//   - LedManager / FrontlightManager already talk to the PM1/IOE1 via their
//     own configs.
// Everything funnels through ensureBooted(), so whichever manager runs first
// establishes the boot state (same idempotent-init pattern as the PaperColor's
// m5pm1::applyBootPowerPolicy()).
//
// Header-only and PIN-compatible with nothing else: only compile-time
// papermono paths include it.

#include <Arduino.h>

#include "M5Ioe1.h"
#include "M5Pm1.h"

namespace freeink {
namespace papermono {

struct WakeState {
  uint8_t source = 0;
  uint8_t enabledPins = 0;
  bool valid = false;
};

inline WakeState& bootWakeState() {
  static WakeState state;
  return state;
}

// One-time boot bring-up (idempotent, cheap after the first call): PMIC bus up
// with watchdog off, wake source cleared, power button taken over (the PMIC's
// single-click reset is disabled so clicks reach the firmware; its hardware
// long-hold download escape stays), and all expander-switched rails/resets
// driven to a known LOW. Rails come up individually as each manager needs them.
inline bool ensureBooted() {
  static bool booted = false;
  static bool ok = false;
  if (booted) return ok;
  booted = true;
  m5pm1::beginBus();
  auto& wake = bootWakeState();
  wake.valid = m5pm1::readWakeSource(&wake.source) && m5pm1::readReg(m5pm1::REG_GPIO_WAKE_EN, &wake.enabledPins);
  m5pm1::clearWakeSource();
  m5pm1::configureAppPowerButton();
  // The red RGB leg lives in the PM1's PWR_CFG, which the PMIC retains across
  // reflashes — stock firmware leaves it lit. Drive it to the same known-off
  // state configureOutputs() gives the IOE1 green/blue legs.
  m5pm1::updateReg(m5pm1::REG_PWR_CFG, m5pm1::LED_R_EN, 0);
  ok = m5ioe1::configureOutputs();
  return ok;
}

// RTC /IRQ is PM1 GPIO0, separate from the ESP's GPIO wake sources.
inline bool setRtcWake(bool enabled) {
  if (!ensureBooted()) return false;
  using namespace m5pm1;
  if (!enabled) {
    uint8_t owners = 0;
    const bool read = readReg(REG_GPIO_WAKE_EN, &owners);
    const bool disabled = updateRegChecked(REG_GPIO_WAKE_EN, GPIO0, 0);
    const bool released = !read || (owners & GPIO_WAKE_MASK) != GPIO0 || updateRegChecked(REG_HOLD_CFG, LDO_HOLD, 0);
    return read && disabled && released;
  }
  uint8_t owners = 0;
  if (!readReg(REG_GPIO_WAKE_EN, &owners) || (owners & (GPIO_WAKE_MASK & ~GPIO0))) return false;
  const bool configured = updateRegChecked(REG_GPIO_FUNC0, 0x03, 0) && updateRegChecked(REG_GPIO_MODE, GPIO0, 0) &&
                          updateRegChecked(REG_GPIO_PULL0, 0x03, 0x01) && updateRegChecked(REG_GPIO_DRV, 0, GPIO0) &&
                          updateRegChecked(REG_GPIO_WAKE_CFG, GPIO0, 0) && updateRegChecked(REG_PWR_CFG, 0, LDO_EN) &&
                          updateRegChecked(REG_HOLD_CFG, 0, LDO_HOLD) && updateRegChecked(REG_GPIO_WAKE_EN, 0, GPIO0) &&
                          clearWakeSource();
  if (!configured) {
    updateRegChecked(REG_GPIO_WAKE_EN, GPIO0, 0);
    updateRegChecked(REG_HOLD_CFG, LDO_HOLD, 0);
  }
  return configured;
}

// EPD rail (IOE1 IO3 -> EPD_3V3_L3B LDO). Power-on settle is EpdBus's own
// 100 ms delay; power-off gets the reference bring-up's 10 ms.
inline void setEpdPower(bool enabled) {
  ensureBooted();
  m5ioe1::write(m5ioe1::PIN_EPD_POWER, enabled);
  if (!enabled) delay(10);
}

inline void setEpdReset(bool high) { m5ioe1::write(m5ioe1::PIN_EPD_RESET, high); }

// SX1262 on the dedicated SPI3 bus. Keep PM1/IOE1 ownership in this board
// helper. IOE1 schematic IO numbers are one-based; PM1 GPIO numbers are zero-based.
inline bool setRadioPower(bool enabled) {
  if (!ensureBooted()) return false;
  constexpr uint8_t pmPin = 1u << 2;
  constexpr uint16_t reset = 1u << 9;    // IO10
  constexpr uint16_t antenna = 1u << 1;  // IO2
  if (!enabled) {
    const bool resetOk = m5ioe1::write(reset | antenna, false);
    return m5pm1::updateRegChecked(m5pm1::REG_GPIO_OUT, pmPin, 0) && resetOk;
  }
  const bool powerOk = m5pm1::updateRegChecked(m5pm1::REG_GPIO_FUNC0, 0x30, 0) &&
                       m5pm1::updateRegChecked(m5pm1::REG_GPIO_PULL0, 0x30, 0) &&
                       m5pm1::updateRegChecked(m5pm1::REG_GPIO_DRV, pmPin, 0) &&
                       m5pm1::updateRegChecked(m5pm1::REG_GPIO_OUT, 0, pmPin) &&
                       m5pm1::updateRegChecked(m5pm1::REG_GPIO_MODE, 0, pmPin);
  const bool pinsOk = m5ioe1::updateReg16(m5ioe1::REG_GPIO_OUT_L, reset | antenna, 0) &&
                      m5ioe1::updateReg16(m5ioe1::REG_GPIO_DRV_L, reset | antenna, 0) &&
                      m5ioe1::updateReg16(m5ioe1::REG_GPIO_MODE_L, 0, reset | antenna);
  if (!powerOk || !pinsOk) return false;
  delay(200);
  if (!m5ioe1::write(reset, true)) return false;
  delay(200);
  return true;
}

inline bool setRadioTransmit(bool transmit) { return m5ioe1::write(1u << 1, transmit); }

// TF-card rail (IOE1 IO14 PYB_TF_EN -> TF_3V3_L3B LDO). configureOutputs()
// boots it LOW, so the card is unpowered until SDCardManager raises it before
// the SDMMC mount. 10 ms settle on power-up so the rail is stable before the
// first CMD0 (same as the touch rail's bring-up).
inline void setSdPower(bool enabled) {
  ensureBooted();
  m5ioe1::write(m5ioe1::PIN_SD_POWER, enabled);
  if (enabled) delay(10);
}

// Touch rail up + reset released so the FT6336 answers its I2C probe. Same
// 8 ms low / 10 ms settle as the reference bring-up.
inline void enableTouch() {
  ensureBooted();
  m5ioe1::write(m5ioe1::PIN_TOUCH_POWER, true);
  m5ioe1::write(m5ioe1::PIN_TOUCH_RESET, false);
  delay(8);
  m5ioe1::write(m5ioe1::PIN_TOUCH_RESET, true);
  delay(10);
}

// Poll the PM1's power button (the event bit is read-clear in the PMIC).
// Returns true once per click. Rate-limited internally: each poll costs two
// paced I2C transactions to a firmware-emulated slave (~2 ms), so per-tick
// polling from update() would be a tax on every loop.
inline bool pollPowerButtonClicked(unsigned long nowMs) {
  static unsigned long lastPollMs = 0;
  constexpr unsigned long POLL_INTERVAL_MS = 250;
  if (nowMs - lastPollMs < POLL_INTERVAL_MS) return false;
  lastPollMs = nowMs;
  if (!ensureBooted()) return false;
  uint8_t status = 0;
  if (!m5pm1::readButtonStatus(&status)) return false;
  return (status & m5pm1::BTN_EVENT) != 0;
}

}  // namespace papermono
}  // namespace freeink
