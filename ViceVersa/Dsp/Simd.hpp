#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Four SIMD lanes, one voice each, and what the engine does with them. */

#include <kfr/base.hpp>
#include <kfr/math.hpp>

#include <array>
#include <cstddef>

namespace viceversa
{
inline constexpr int kLanes = 4;
using vec4 = kfr::vec<float, kLanes>;
using vec4i = kfr::vec<int, kLanes>;

//! Lane l of v set to 0, the others left
inline void clearLane(vec4& v, std::size_t l) noexcept
{
  alignas(16) std::array<float, kLanes> t;
  kfr::write<true>(t.data(), v);
  t[l] = 0.f;
  v = kfr::read<kLanes, true>(t.data());
}

/*! A decaying state too small to matter, set to 0. Where the FPU does not
 *  flush denormals (WebAssembly, some ARM targets), an IIR's state left to
 *  decay becomes one, and each operation on it costs a hundred.
 */
inline void flushTiny(vec4& v) noexcept
{
  v = kfr::select(kfr::abs(v) < vec4(1e-20f), vec4(0.f), v);
}
inline void flushTiny(float& x) noexcept
{
  x = (x < 1e-20f && x > -1e-20f) ? 0.f : x;
}

//! Rows to columns: on return, a holds the first value of each, b the second...
inline void transpose(vec4& a, vec4& b, vec4& c, vec4& d) noexcept
{
  using kfr::csizes;
  const vec4 ab0 = a.shuffle(b, csizes<0, 4, 1, 5>), ab1 = a.shuffle(b, csizes<2, 6, 3, 7>);
  const vec4 cd0 = c.shuffle(d, csizes<0, 4, 1, 5>), cd1 = c.shuffle(d, csizes<2, 6, 3, 7>);
  a = ab0.shuffle(cd0, csizes<0, 1, 4, 5>);
  b = ab0.shuffle(cd0, csizes<2, 3, 6, 7>);
  c = ab1.shuffle(cd1, csizes<0, 1, 4, 5>);
  d = ab1.shuffle(cd1, csizes<2, 3, 6, 7>);
}
}
