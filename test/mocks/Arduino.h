#pragma once

#include <cstdint>
#include <cmath>
#include "Stream.h"

inline uint32_t g_mock_millis = 0;

using std::isnan;

inline uint32_t millis() {
  return g_mock_millis;
}

inline void delay(uint32_t ms) {
  g_mock_millis += ms;
}

// Needed by rweather/Crypto's RNG.cpp, which the channel-filter test env compiles for real.
// Deterministic like millis(): nothing in these tests may depend on a real clock.
inline uint32_t micros() {
  return g_mock_millis * 1000;
}
