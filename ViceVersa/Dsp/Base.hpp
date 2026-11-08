#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* What all of ViceVersa uses: pi, and the checks that keep what a host or a
 * preset sends in range.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numbers>

namespace viceversa
{
inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.f * kPi;

//! By its bits, so that it holds whatever the compiler assumes of NaN
[[nodiscard]] inline bool finite(float x) noexcept
{
  std::uint32_t u;
  std::memcpy(&u, &x, sizeof(u));
  return (u & 0x7f800000u) != 0x7f800000u;
}
[[nodiscard]] inline bool finite(double x) noexcept
{
  std::uint64_t u;
  std::memcpy(&u, &x, sizeof(u));
  return (u & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

[[nodiscard]] inline float sane(float x, float lo, float hi, float fallback) noexcept
{
  return finite(x) ? std::clamp(x, lo, hi) : fallback;
}
[[nodiscard]] inline double sane(double x, double lo, double hi, double fallback) noexcept
{
  return finite(x) ? std::clamp(x, lo, hi) : fallback;
}

//! An enum of `count` values from 0: anything else is the first
template <typename E>
[[nodiscard]] constexpr E saneEnum(E e, int count) noexcept
{
  const int i = int(e);
  return (i >= 0 && i < count) ? e : E{};
}
}
