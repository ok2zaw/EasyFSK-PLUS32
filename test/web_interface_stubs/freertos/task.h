#pragma once

#include <freertos/FreeRTOS.h>

#include <stddef.h>
#include <string>

namespace TaskMock {
struct StopTask {};

extern TaskFunction_t function;
extern std::string name;
extern uint32_t stackDepth;
extern UBaseType_t priority;
extern BaseType_t core;
extern size_t createCount;
extern size_t delayCount;
extern bool stopOnDelay;
} // namespace TaskMock

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name,
                                         uint32_t stackDepth, void *,
                                         UBaseType_t priority, TaskHandle_t *,
                                         BaseType_t core) {
  TaskMock::function = function;
  TaskMock::name = name;
  TaskMock::stackDepth = stackDepth;
  TaskMock::priority = priority;
  TaskMock::core = core;
  TaskMock::createCount++;
  return 1;
}

inline void vTaskDelay(uint32_t) {
  TaskMock::delayCount++;
  if (TaskMock::stopOnDelay) throw TaskMock::StopTask{};
}
