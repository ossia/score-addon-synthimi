#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* What the synths of this add-on use to keep what a host or a preset sends in
 * range.
 */

#include <algorithm>
#include <cstdint>

#include <ossia/math/safe_math.hpp>

namespace synthimi
{
//! Not NaN nor infinite, also under -ffast-math, where std::isfinite is
//! folded to true
[[nodiscard]] inline bool finite(double x) noexcept
{
  return ossia::safe_isfinite(x);
}

//! A finite value clamped to [lo, hi]; `fallback` for NaN and infinities
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
  const int i = static_cast<int>(e);
  return (i >= 0 && i < count) ? e : E{};
}
}
