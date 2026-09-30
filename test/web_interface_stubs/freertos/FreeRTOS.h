#pragma once

#include <stdint.h>

using BaseType_t = int;
using UBaseType_t = unsigned int;
using TaskHandle_t = void *;
using TaskFunction_t = void (*)(void *);

#define pdMS_TO_TICKS(value) (value)
