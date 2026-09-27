#pragma once

#include <cstdint>

using TickType_t = uint32_t;
using BaseType_t = int;
using SemaphoreHandle_t = void *;

constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr BaseType_t pdTRUE = 1;
