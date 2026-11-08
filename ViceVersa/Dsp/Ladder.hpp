#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* A four-pole ladder low-pass, in four SIMD lanes (one voice each), solved
 * without a delay in its feedback (Zavalishin, The Art of VA Filter Design):
 * the linear loop is solved exactly, then its input goes through a saturator
 * evaluated at that solution, so it drives and self-oscillates without
 * blowing up. Each pole is a trapezoidal one-pole:
 *
 *   v = G (x - s),  y = v + s,  s' = y + v,   G = g / (1 + g), g = tan(pi fc / fs)
 *
 * so y4 = G^4 x + S with S = (1-G)(G^3 s1 + G^2 s2 + G s3 + s4), and with
 * x = u - k y4: y4 = (G^4 u + S) / (1 + k G^4).
 */

#include <kfr/base.hpp>
#include <kfr/math.hpp>

namespace viceversa
{
using vec4 = kfr::vec<float, 4>;

//! tanh's [3/3] Pade approximant, clamped where it reaches 1
inline vec4 saturate(vec4 x) noexcept
{
  const vec4 c = kfr::min(vec4(3.f), kfr::max(vec4(-3.f), x));
  const vec4 c2 = c * c;
  return c * (vec4(27.f) + c2) / (vec4(27.f) + vec4(9.f) * c2);
}

struct Ladder
{
  // kfr::vec has no zeroing default constructor: all set explicitly
  vec4 s1 = vec4(0.f), s2 = vec4(0.f), s3 = vec4(0.f), s4 = vec4(0.f);

  void reset() noexcept { s1 = s2 = s3 = s4 = vec4(0.f); }

  //! G and k per lane; drive scales the input into the saturator
  vec4 process(vec4 in, vec4 G, vec4 k, vec4 drive) noexcept
  {
    const vec4 one(1.f);
    const vec4 G2 = G * G, G3 = G2 * G, G4 = G2 * G2;
    const vec4 S = (one - G) * (G3 * s1 + G2 * s2 + G * s3 + s4);
    const vec4 u = in * drive;
    const vec4 y4 = (G4 * u + S) / (one + k * G4);
    const vec4 x = saturate(u - k * y4);

    vec4 v = G * (x - s1);
    const vec4 y1 = v + s1;
    s1 = y1 + v;
    v = G * (y1 - s2);
    const vec4 y2 = v + s2;
    s2 = y2 + v;
    v = G * (y2 - s3);
    const vec4 y3 = v + s3;
    s3 = y3 + v;
    v = G * (y3 - s4);
    const vec4 y = v + s4;
    s4 = y + v;

    // Back to the input's level; resonance thins the passband, give some back
    return y * (one + vec4(0.5f) * k) / drive;
  }
};
}
