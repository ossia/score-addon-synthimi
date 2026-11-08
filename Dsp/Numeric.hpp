#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* What the synths of this add-on use to keep what a host or a preset sends in
 * range.
 */

#include <algorithm>
#include <bit>
#include <cstdint>

namespace synthimi
{
namespace detail
{
//! The value, unknown to the optimiser: under -ffinite-math-only clang
//! knows that a float's exponent is never all ones, and folds a test of its
//! bits as it does std::isfinite. WebAssembly is left to the bit test alone.
template <typename T>
[[nodiscard]] inline T opaque(T x) noexcept
{
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__EMSCRIPTEN__)
  __asm__("" : "+r"(x));
#endif
  return x;
}
}

//! std::isfinite on the bits: -ffinite-math-only (in -Ofast) lets the
//! compiler fold std::isfinite, and x == x, to true
[[nodiscard]] inline bool finite(float x) noexcept
{
  return (detail::opaque(std::bit_cast<std::uint32_t>(x)) & 0x7f800000u) != 0x7f800000u;
}
[[nodiscard]] inline bool finite(double x) noexcept
{
  constexpr std::uint64_t exponent = 0x7ff0000000000000ull;
  return (detail::opaque(std::bit_cast<std::uint64_t>(x)) & exponent) != exponent;
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
