#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#define IRAM_ATTR
#define log_e(...) ((void)0)
constexpr int HIGH = 1, LOW = 0, INPUT = 1, OUTPUT = 3;
inline uint32_t fakeNow = 1000;
inline int gpioLevels[49] = {};
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { gpioLevels[pin] = value; }
inline int digitalRead(int pin) { return gpioLevels[pin]; }
inline void delay(uint32_t ms) { fakeNow += ms; }
inline void delayMicroseconds(uint32_t) {}
inline uint32_t millis() { return fakeNow; }
struct SerialStub {};
inline SerialStub Serial;
using TaskHandle_t = void*;
constexpr int pdPASS = 1;
#define pdMS_TO_TICKS(ms) ((ms) / 10)
inline void (*pendingTask)(void*) = nullptr;
inline void* pendingArg = nullptr;
inline bool failTask = false, autoRunTask = true;
inline size_t taskCreations = 0;
inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, int, void* arg, int, TaskHandle_t* task, int) {
  if (failTask) return 0;
  ++taskCreations;
  pendingTask = fn;
  pendingArg = arg;
  *task = arg;
  return pdPASS;
}
inline void runTask() {
  auto fn = pendingTask;
  pendingTask = nullptr;
  fn(pendingArg);
}
inline void vTaskDelay(uint32_t ticks) {
  fakeNow += ticks * 10;
  if (autoRunTask && pendingTask) runTask();
}
inline void vTaskDelete(void*) {}
