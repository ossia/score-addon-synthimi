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
 * x = u - k y4: y4 = (G^4 u + S) / (1 + k G^4). The trapezoidal poles keep the
 * analog loop's phase at the cutoff, so it oscillates from k = 4 exactly, at
 * any cutoff, and the saturator sets the level it settles at.
 *
 * The engine runs it at twice the rate (Dsp/Halfband.hpp): the saturator's
 * harmonics fold back from twice as far.
 */

#include <ViceVersa/Dsp/Simd.hpp>

namespace viceversa
{
/*! tanh's [3/2] Pade approximant, x (27 + x^2) / (27 + 9 x^2), clamped at 3,
 *  where it reaches 1 with a zero slope. Within 0.024 of tanh (at 1.57),
 *  and like it, odd, with a unit slope at 0.
 */
[[nodiscard]] inline vec4 saturate(vec4 x) noexcept
{
  const vec4 c = kfr::min(vec4(3.f), kfr::max(vec4(-3.f), x));
  const vec4 c2 = c * c;
  return c * (vec4(27.f) + c2) / (vec4(27.f) + vec4(9.f) * c2);
}

/*! tan for x in [0, 0.71] (a cutoff up to 0.45 of the rate, at twice the
 *  rate): its [5/4] Pade approximant, within 3e-7 of it relatively in
 *  float, a thousandth of a cent of cutoff
 */
[[nodiscard]] inline float tanCutoff(float x) noexcept
{
  const float x2 = x * x;
  return x * (945.f - 105.f * x2 + x2 * x2) / (945.f - 420.f * x2 + 15.f * x2 * x2);
}

struct Ladder
{
  //! What a sample needs, from G and k per lane: computed once per sample and
  //! shared by both sides and both halves of the oversampled sample
  struct Coefs
  {
    vec4 G, G2, G3, G4, k, inv; // inv = 1 / (1 + k G^4)

    [[nodiscard]] static Coefs make(vec4 G, vec4 k, vec4 inv) noexcept
    {
      const vec4 G2 = G * G;
      return {G, G2, G2 * G, G2 * G2, k, inv};
    }
    [[nodiscard]] static Coefs make(vec4 G, vec4 k) noexcept
    {
      const vec4 G4 = G * G * G * G;
      return make(G, k, vec4(1.f) / (vec4(1.f) + k * G4));
    }
  };

  // kfr::vec has no zeroing default constructor: all set explicitly
  vec4 s1 = vec4(0.f), s2 = vec4(0.f), s3 = vec4(0.f), s4 = vec4(0.f);

  void reset() noexcept { s1 = s2 = s3 = s4 = vec4(0.f); }
  void clearLane(std::size_t l) noexcept
  {
    for(vec4* s : {&s1, &s2, &s3, &s4})
      viceversa::clearLane(*s, l);
  }
  void flushTiny() noexcept
  {
    for(vec4* s : {&s1, &s2, &s3, &s4})
      viceversa::flushTiny(*s);
  }

  /*! u: the input, already scaled by the drive into the saturator. The
   *  output at the saturator's level: the caller scales it back
   */
  [[nodiscard]] vec4 process(vec4 u, const Coefs& c) noexcept
  {
    const vec4 S = (vec4(1.f) - c.G) * (c.G3 * s1 + c.G2 * s2 + c.G * s3 + s4);
    const vec4 y4 = (c.G4 * u + S) * c.inv;
    const vec4 x = saturate(u - c.k * y4);

    vec4 v = c.G * (x - s1);
    const vec4 y1 = v + s1;
    s1 = y1 + v;
    v = c.G * (y1 - s2);
    const vec4 y2 = v + s2;
    s2 = y2 + v;
    v = c.G * (y2 - s3);
    const vec4 y3 = v + s3;
    s3 = y3 + v;
    v = c.G * (y3 - s4);
    const vec4 y = v + s4;
    s4 = y + v;
    return y;
  }
};
}
