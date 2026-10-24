#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct WireStub {
  std::array<uint8_t, 256> registers{};
  std::vector<std::array<uint8_t, 2>> writes;
  uint8_t tx[2] = {};
  size_t count = 0;
  size_t attempts = 0, failAt = SIZE_MAX;
  int failReg = -1;
  bool failAll = false;
  bool begin(int sda, int scl, uint32_t hz) { return sda == 41 && scl == 42 && hz == 100000; }
  void beginTransmission(uint8_t address) {
    if (address != 0x18) __builtin_abort();
    count = 0;
  }
  size_t write(uint8_t byte) {
    if (count < 2) tx[count++] = byte;
    return 1;
  }
  int endTransmission() {
    if (++attempts == failAt || failAll || tx[0] == failReg) return 1;
    if (count != 2) __builtin_abort();
    registers[tx[0]] = tx[1];
    writes.push_back({tx[0], tx[1]});
    return 0;
  }
};
inline WireStub Wire;
