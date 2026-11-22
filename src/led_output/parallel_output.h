#pragma once
#include <stdint.h>
bool parallelBegin();
bool parallelShow(const uint8_t* const strips[8],const int counts[8],uint8_t brightness);
bool parallelHealthy();
