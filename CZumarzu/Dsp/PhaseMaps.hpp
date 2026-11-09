#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The phase distortion of the Casio CZ, as its sound chip, the NEC uPD933,
 * computes it (MAME's src/devices/sound/upd933.cpp, by Devin Acker, is the
 * reference these maps were checked against: tests/ref/upd933).
 *
 * One cycle of the oscillator is x in [0, 1). Each waveform bends x into a
 * phase phi(x), which reads a cosine, and the resonant ones multiply that by
 * a window u(x):
 *
 *     y(x) = (1 - cos(2 pi phi(x))) * u(x) - 1
 *
 * which is the chip's unipolar cosine table, windowed, then centred. Without
 * a window, u = 1 and y = -cos(2 pi phi). For every waveform phi and u are
 * linear in x on at most four segments, split at three breakpoints. That is
 * what makes the antialiasing exact and cheap: the mean of y over the stretch
 * of x one sample sweeps, which is first-order antiderivative antialiasing
 * (ADAA), has a closed form on each segment, written around the midpoint of
 * the stretch so that nothing cancels in single precision:
 *
 *   mean = u(m) (1 - cos(th) S(z)) + e sin(th) h R(z) - 1
 *
 *   m: midpoint, h: half-width, th = 2 pi phi(m), z = pi b L (L = 2h),
 *   b and e the slopes of phi and u, S(z) = sin z / z,
 *   R(z) = (sin z - z cos z) / z^2.
 *
 * A sample that crosses breakpoints is split into pieces at them, each piece
 * weighted by its length. The chip itself keeps the pivot at least four
 * samples wide (its "DCW limit"), so a sample crosses at most two of them.
 */

#include <kfr/base.hpp>
#include <kfr/math.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace czumarzu
{
using vec4 = kfr::vec<float, 4>;
inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.f * std::numbers::pi_v<float>;

//! The CZ's eight waveforms first, in the panel's order, then the chip's
//! undocumented ones and Vector Phaseshaping. Stored by index: append only.
enum class Wave : int
{
  Saw,
  Square,
  Pulse,
  DoubleSine,
  SawPulse,
  ResoSaw,
  ResoTriangle,
  ResoTrapezoid,
  DoublePulse,   // uPD933 waveform 7
  ResoPulse,     // uPD933 window 4
  ResoDoubleSaw, // uPD933 window 5
  Vps            // Kleimola, Lazzarini, Timoney, Välimäki, DAFx-11
};
inline constexpr int kWaveCount = 12;

[[nodiscard]] constexpr bool resonant(Wave w) noexcept
{
  return w == Wave::ResoSaw || w == Wave::ResoTriangle || w == Wave::ResoTrapezoid
         || w == Wave::ResoPulse || w == Wave::ResoDoubleSaw;
}

//! phi = a + b x and u = c + e x on each of four segments, split at br[0..2]
//! (sorted, 1 where unused)
struct Shape
{
  vec4 br[3];
  vec4 a[4], b[4], c[4], e[4];
};

//! The chip's pivot never goes below 8 of its 2048 phase steps (DCW 127/128)
inline constexpr float kMinPivot = 8.f / 2048.f;
//! A resonance never above 0.2 cycles per sample (9.6 kHz at 48 kHz), so that
//! its sidebands stay below Nyquist: measured at 0.3, the resonant waves
//! aliased at -56 dB at high notes, at 0.2 -64 dB. The chip has no such limit
//! and aliases around 3 kHz.
inline constexpr float kMaxResoCycles = 0.2f;

/*! sin and cos of 2 pi t together, for any t: one reduction for both.
 *  Folded by the quadrants' symmetry to r in [0, 1/4] of a turn, where two
 *  minimax polynomials in r, odd of degree 9 and even of degree 8 (fitted by
 *  Remez exchange, cos(0) kept at 1), are within 3.4e-9 and 5.3e-8: the
 *  tests measure them, rounding included.
 */
inline void sincos2pi(vec4 t, vec4& s, vec4& c) noexcept
{
  const vec4 one(1.f), half(0.5f), quarter(0.25f);
  const vec4 u = t - kfr::floor(t + half); // [-0.5, 0.5)
  const vec4 au = kfr::abs(u);
  // cos(2 pi u) = -cos(2 pi (0.5 - |u|)), sin keeps its sign
  const auto pastQuarter = au > quarter;
  const vec4 r = kfr::select(pastQuarter, half - au, au); // [0, 1/4]
  const vec4 r2 = r * r;
  const vec4 sn
      = r
        * (vec4(6.2831851600897227f)
           + r2
                 * (vec4(-41.341655031479419f)
                    + r2
                          * (vec4(81.601004077356553f)
                             + r2 * (vec4(-76.549782387613466f) + r2 * vec4(39.536706762014836f)))));
  const vec4 cs
      = one
        + r2
              * (vec4(-19.739182072579625f)
                 + r2
                       * (vec4(64.935221469818272f)
                          + r2 * (vec4(-85.254007539277524f) + r2 * vec4(56.340648700450132f))));
  s = kfr::select(u < vec4(0.f), -sn, sn);
  c = kfr::select(pastQuarter, -cs, cs);
}

namespace detail
{
inline void fill(Shape& s, int from, vec4 a, vec4 b, vec4 c = vec4(1.f), vec4 e = vec4(0.f))
{
  for(int i = from; i < 4; i++)
  {
    s.a[i] = a;
    s.b[i] = b;
    s.c[i] = c;
    s.e[i] = e;
  }
}
}

/*! The segments of `w`.
 *  pivot: the chip's pivot / 2048, in (0, 0.5]: 0.5 at DCW 0
 *  k:     resonance multiple, 1 + 16 DCW
 *  v:     VPS's height (0.5 with a pivot of 0.5 is a sine)
 */
[[nodiscard]] inline Shape shapeOf(Wave w, vec4 pivot, vec4 k, vec4 v) noexcept
{
  using detail::fill;
  const vec4 zero(0.f), half(0.5f), one(1.f), two(2.f);
  const vec4 p = pivot;
  const vec4 hp = half / p;
  Shape s;
  s.br[0] = s.br[1] = s.br[2] = one;
  switch(w)
  {
    default:
    case Wave::Saw: {
      // Rises over [0, p), falls over [p, 1)
      s.br[0] = p;
      fill(s, 0, zero, hp);
      const vec4 b1 = half / (one - p);
      fill(s, 1, half - b1 * p, b1);
      break;
    }
    case Wave::Square: {
      // Rises over [0, p), holds; falls over [0.5, 0.5 + p), holds
      s.br[0] = p;
      s.br[1] = half;
      s.br[2] = half + p;
      fill(s, 0, zero, hp);
      fill(s, 1, half, zero);
      fill(s, 2, half - half * hp, hp);
      fill(s, 3, one, zero);
      break;
    }
    case Wave::Pulse: {
      // One whole cosine over [0, 2p), then low
      s.br[0] = two * p;
      fill(s, 0, zero, hp);
      fill(s, 1, one, zero);
      break;
    }
    case Wave::DoubleSine: {
      s.br[0] = p;
      fill(s, 0, zero, one / p);
      const vec4 b1 = one / (one - p);
      fill(s, 1, -p * b1, b1);
      break;
    }
    case Wave::SawPulse: {
      // Half a sine up, a fall over p, then low
      s.br[0] = half;
      s.br[1] = half + p;
      fill(s, 0, zero, one);
      fill(s, 1, half - half * hp, hp);
      fill(s, 2, one, zero);
      break;
    }
    case Wave::DoublePulse: {
      // Like a square that drops instead of holding: a jump, the chip's own
      s.br[0] = p;
      s.br[1] = half;
      s.br[2] = half + p;
      fill(s, 0, zero, hp);
      fill(s, 1, one, zero);
      fill(s, 2, -half * hp, hp);
      fill(s, 3, one, zero);
      break;
    }
    case Wave::ResoSaw:
      // Window falls over the cycle
      fill(s, 0, zero, k, one, -one);
      break;
    case Wave::ResoTriangle:
      s.br[0] = half;
      fill(s, 0, zero, k, zero, two);
      fill(s, 1, zero, k, two, -two);
      break;
    case Wave::ResoTrapezoid:
      s.br[0] = half;
      fill(s, 0, zero, k);
      fill(s, 1, zero, k, two, -two);
      break;
    case Wave::ResoPulse:
      s.br[0] = half;
      fill(s, 0, zero, k, one, -two);
      fill(s, 1, zero, k, zero, zero);
      break;
    case Wave::ResoDoubleSaw:
      s.br[0] = half;
      fill(s, 0, zero, k, zero, two);
      fill(s, 1, zero, k, -one, two);
      break;
    case Wave::Vps: {
      s.br[0] = p;
      fill(s, 0, zero, v / p);
      const vec4 b1 = (one - v) / (one - p);
      fill(s, 1, v - b1 * p, b1);
      break;
    }
  }
  return s;
}

/*! How many samples the pivot must span, at least, for the steepest segment
 *  of `w` to move its phase no faster than the saw's does over four: the
 *  chip's own limit, which it applies to every waveform alike, lets the
 *  double sine move twice as fast and VPS faster still with its height
 */
[[nodiscard]] inline vec4 pivotSamples(Wave w, vec4 v) noexcept
{
  switch(w)
  {
    case Wave::DoubleSine:
      return vec4(8.f);
    case Wave::Vps:
      return vec4(8.f) * kfr::max(v, vec4(0.5f));
    default:
      return vec4(4.f);
  }
}

[[nodiscard]] inline Shape selectShape(const kfr::mask<float, 4>& m, const Shape& x, const Shape& y) noexcept
{
  Shape s;
  for(int i = 0; i < 3; i++)
    s.br[i] = kfr::select(m, x.br[i], y.br[i]);
  for(int i = 0; i < 4; i++)
  {
    s.a[i] = kfr::select(m, x.a[i], y.a[i]);
    s.b[i] = kfr::select(m, x.b[i], y.b[i]);
    s.c[i] = kfr::select(m, x.c[i], y.c[i]);
    s.e[i] = kfr::select(m, x.e[i], y.e[i]);
  }
  return s;
}

//! The first breakpoint after x, or 1
[[nodiscard]] inline vec4 nextBreak(const Shape& s, vec4 x) noexcept
{
  return kfr::select(
      x < s.br[0], s.br[0],
      kfr::select(x < s.br[1], s.br[1], kfr::select(x < s.br[2], s.br[2], vec4(1.f))));
}

//! The segment's coefficient at x
[[nodiscard]] inline vec4 pick(const Shape& s, const vec4 (&c)[4], vec4 x) noexcept
{
  return kfr::select(
      x >= s.br[2], c[3], kfr::select(x >= s.br[1], c[2], kfr::select(x >= s.br[0], c[1], c[0])));
}

/*! `wave` read through the window of `window` (a resonant shape), as the chip
 *  does: its window is one register per voice, so when a line alternates a
 *  resonant waveform with another, the other cycles are windowed too.
 *  The windows break at 0.5 at most; the waveforms that do not already break
 *  there have one breakpoint, so the union still fits in three.
 */
[[nodiscard]] inline Shape windowed(Wave w, const Shape& wave, const Shape& window) noexcept
{
  const vec4 half(0.5f), one(1.f);
  Shape s;
  if(w == Wave::Square || w == Wave::SawPulse || w == Wave::DoublePulse)
  {
    s.br[0] = wave.br[0];
    s.br[1] = wave.br[1];
    s.br[2] = wave.br[2];
  }
  else
  {
    s.br[0] = kfr::min(wave.br[0], half);
    s.br[1] = kfr::max(wave.br[0], half);
    s.br[2] = one;
  }
  const vec4 lo[4] = {vec4(0.f), s.br[0], s.br[1], s.br[2]};
  const vec4 hi[4] = {s.br[0], s.br[1], s.br[2], one};
  for(int i = 0; i < 4; i++)
  {
    const vec4 m = vec4(0.5f) * (lo[i] + hi[i]);
    s.a[i] = pick(wave, wave.a, m);
    s.b[i] = pick(wave, wave.b, m);
    s.c[i] = pick(window, window.c, m);
    s.e[i] = pick(window, window.e, m);
  }
  return s;
}

//! Pointwise y(x), for tests and the first sample of a note
[[nodiscard]] inline vec4 evaluate(const Shape& s, vec4 x) noexcept
{
  const vec4 a = pick(s, s.a, x), b = pick(s, s.b, x);
  const vec4 c = pick(s, s.c, x), e = pick(s, s.e, x);
  vec4 t = a + b * x;
  t = t - kfr::floor(t);
  return (vec4(1.f) - kfr::cos(vec4(kTwoPi) * t)) * (c + e * x) - vec4(1.f);
}

//! The line's window: that of its first resonant waveform, if any
[[nodiscard]] inline int lineWindow(Wave a, Wave b) noexcept
{
  return resonant(a) ? int(a) : resonant(b) ? int(b) : -1;
}

//! The shape a line plays for waveform w, under the line's window
[[nodiscard]] inline Shape lineShape(Wave w, int window, vec4 pivot, vec4 k, vec4 v) noexcept
{
  if(window < 0)
    return shapeOf(w, pivot, k, v);
  if(resonant(w))
    return shapeOf(Wave(window), pivot, k, v);
  return windowed(w, shapeOf(w, pivot, k, v), shapeOf(Wave(window), pivot, k, v));
}

/*! The integral of y over [m - L/2, m + L/2], all of it inside one segment
 *  (the one holding m).
 */
[[nodiscard]] inline vec4 integral(const Shape& s, vec4 m, vec4 L) noexcept
{
  const vec4 one(1.f);
  const vec4 a = pick(s, s.a, m), b = pick(s, s.b, m);
  const vec4 c = pick(s, s.c, m), e = pick(s, s.e, m);

  vec4 sm, cm;
  sincos2pi(a + b * m, sm, cm);

  const vec4 z = vec4(kPi) * b * L;
  vec4 S, R;
  if(kfr::all(kfr::abs(z) <= one)) [[likely]]
  {
    // Taylor series, |error| < 3e-8 for |z| <= 1
    const vec4 z2 = z * z;
    S = one
        + z2
              * (vec4(-1.f / 6.f)
                 + z2
                       * (vec4(1.f / 120.f)
                          + z2 * (vec4(-1.f / 5040.f) + z2 * vec4(1.f / 362880.f))));
    R = z
        * (vec4(1.f / 3.f)
           + z2
                 * (vec4(-1.f / 30.f)
                    + z2 * (vec4(1.f / 840.f) + z2 * (vec4(-1.f / 45360.f)
                                                      + z2 * vec4(1.f / 3991680.f)))));
  }
  else
  {
    vec4 sz, cz;
    sincos2pi(vec4(0.5f) * b * L, sz, cz);
    const auto nearZero = kfr::abs(z) < vec4(1e-4f);
    const vec4 zs = kfr::select(nearZero, one, z);
    S = kfr::select(nearZero, one, sz / zs);
    R = kfr::select(nearZero, z * vec4(1.f / 3.f), (sz - z * cz) / (zs * zs));
  }

  const vec4 um = c + e * m;
  const vec4 mean = um * (one - cm * S) + e * sm * (vec4(0.5f) * L) * R - one;
  return L * mean;
}

/*! The mean of y over a whole cycle: its DC, which the pulses and the
 *  windows have plenty of, and which moves with the DCW
 */
[[nodiscard]] inline vec4 cycleMean(const Shape& s) noexcept
{
  const vec4 half(0.5f);
  vec4 acc(0.f), lo(0.f);
  for(int i = 0; i < 4; i++)
  {
    const vec4 hi = i < 3 ? s.br[i] : vec4(1.f);
    const vec4 L = kfr::max(hi - lo, vec4(0.f));
    acc += integral(s, lo + half * L, L);
    lo = hi;
  }
  return acc;
}

/*! The mean of the waveform over one sample: from phase x0 (in [0, 1)) over
 *  dx (in (0, 0.5)). `here` is the shape of the cycle x0 is in, `next` the
 *  shape of the cycle after the wrap: the CZ alternates its two waveforms
 *  from one cycle to the next.
 */
[[nodiscard]] inline vec4 sampleMean(const Shape& here, const Shape& next, bool sameShape, vec4 x0, vec4 dx)
    noexcept
{
  const vec4 one(1.f), zero(0.f);
  const vec4 xEnd = x0 + dx;
  vec4 start = x0;
  vec4 acc = zero;
  for(int piece = 0; piece < 3; piece++)
  {
    const auto wrapped = start >= one;
    const vec4 local = kfr::select(wrapped, start - one, start);
    const vec4 offset = kfr::select(wrapped, one, zero);
    vec4 end = xEnd;
    if(sameShape)
    {
      if(piece < 2)
        end = kfr::min(xEnd, nextBreak(here, local) + offset);
      const vec4 L = kfr::max(end - start, zero);
      acc += integral(here, local + vec4(0.5f) * L, L);
    }
    else
    {
      const Shape s = selectShape(wrapped, next, here);
      if(piece < 2)
        end = kfr::min(xEnd, nextBreak(s, local) + offset);
      const vec4 L = kfr::max(end - start, zero);
      acc += integral(s, local + vec4(0.5f) * L, L);
    }
    start = end;
    if(!kfr::any(start < xEnd))
      break;
  }
  return acc / dx;
}

}

namespace czumarzu
{
//! Over one piece of length L inside one segment, around its midpoint m:
//! i0 = integral of y, i1 = integral of (x - m) y
struct Moments
{
  vec4 i0, i1;
};

[[nodiscard]] inline Moments moments(const Shape& s, vec4 m, vec4 L) noexcept
{
  const vec4 one(1.f);
  const vec4 a = pick(s, s.a, m), b = pick(s, s.b, m);
  const vec4 c = pick(s, s.c, m), e = pick(s, s.e, m);

  vec4 sm, cm;
  sincos2pi(a + b * m, sm, cm);

  // Over t in [-h, h], z = B h:
  //   mean(cos(Bt)) = S, mean(t sin(Bt)) = h R, mean(t^2 cos(Bt)) = h^2 P
  const vec4 z = vec4(kPi) * b * L;
  vec4 S, R, P;
  if(kfr::all(kfr::abs(z) <= one)) [[likely]]
  {
    const vec4 z2 = z * z;
    S = one
        + z2
              * (vec4(-1.f / 6.f)
                 + z2
                       * (vec4(1.f / 120.f)
                          + z2 * (vec4(-1.f / 5040.f) + z2 * vec4(1.f / 362880.f))));
    R = z
        * (vec4(1.f / 3.f)
           + z2
                 * (vec4(-1.f / 30.f)
                    + z2 * (vec4(1.f / 840.f) + z2 * (vec4(-1.f / 45360.f)
                                                      + z2 * vec4(1.f / 3991680.f)))));
    // sum (-1)^k z^2k / ((2k)! (2k + 3))
    P = vec4(1.f / 3.f)
        + z2
              * (vec4(-1.f / 10.f)
                 + z2
                       * (vec4(1.f / 168.f)
                          + z2 * (vec4(-1.f / 6480.f) + z2 * vec4(1.f / 443520.f))));
  }
  else
  {
    vec4 sz, cz;
    sincos2pi(vec4(0.5f) * b * L, sz, cz);
    const auto nearZero = kfr::abs(z) < vec4(1e-3f);
    const vec4 zs = kfr::select(nearZero, one, z);
    const vec4 z2 = zs * zs;
    S = kfr::select(nearZero, one, sz / zs);
    R = kfr::select(nearZero, z * vec4(1.f / 3.f), (sz - z * cz) / z2);
    P = kfr::select(
        nearZero, vec4(1.f / 3.f), (z2 * sz + vec4(2.f) * zs * cz - vec4(2.f) * sz) / (z2 * zs));
  }

  const vec4 h = vec4(0.5f) * L;
  const vec4 um = c + e * m;
  const vec4 meanY = um * (one - cm * S) + e * sm * h * R - one;
  // mean(t y) = e h^2/3 - um mean(t cos) - e mean(t^2 cos)
  const vec4 meanTY = e * h * h * vec4(1.f / 3.f) + um * sm * h * R - e * cm * h * h * P;
  return {L * meanY, L * meanTY};
}

/*! Second-order antialiasing: the waveform under a triangle two samples wide,
 *  centred on the phase x0 the sample starts at, its sides dx long. That is
 *  the first-order box convolved with itself, a linear B-spline: images fall
 *  as sinc^2 instead of sinc, and a jump is smoothed over two samples. The
 *  output is one sample late, as the box's was half a sample late.
 *
 *  `here`: the shape of the cycle x0 is in; `other`: the shape of the cycles
 *  either side of it, which the window can reach.
 */
[[nodiscard]] inline vec4
sampleTriangle(const Shape& here, const Shape& other, bool sameShape, vec4 x0, vec4 dx)
    noexcept
{
  const vec4 one(1.f), zero(0.f), half(0.5f);
  const vec4 inv = one / dx;
  const vec4 inv2 = inv * inv;
  vec4 acc = zero;
  for(int side = 0; side < 2; side++)
  {
    const vec4 from = side ? x0 : x0 - dx;
    const vec4 to = side ? x0 + dx : x0;
    // The weight falls away from x0: its slope, on this side
    const vec4 slope = side ? -inv2 : inv2;
    vec4 start = from;
    for(int piece = 0; piece < 3; piece++)
    {
      const vec4 offset = kfr::select(
          start < zero, vec4(-1.f), kfr::select(start >= one, one, zero));
      const vec4 local = start - offset;
      vec4 end = to;
      Moments mo;
      vec4 L;
      if(sameShape)
      {
        if(piece < 2)
          end = kfr::min(to, nextBreak(here, local) + offset);
        L = kfr::max(end - start, zero);
        mo = moments(here, local + half * L, L);
      }
      else
      {
        const Shape s = selectShape(kfr::abs(offset) > half, other, here);
        if(piece < 2)
          end = kfr::min(to, nextBreak(s, local) + offset);
        L = kfr::max(end - start, zero);
        mo = moments(s, local + half * L, L);
      }
      const vec4 mid = start + half * L;
      const vec4 w = (one - kfr::abs(mid - x0) * inv) * inv;
      acc += w * mo.i0 + slope * mo.i1;
      start = end;
      if(!kfr::any(start < to))
        break;
    }
  }
  return acc;
}
}

namespace czumarzu
{
/*! The triangle of sampleTriangle, computed one interval at a time.
 *
 *  Over the interval a sample sweeps, [x0, x0 + dx], the falling side of this
 *  sample's triangle and the rising side of the next one's add up to a box:
 *  (1 - (x - x0)/dx)/dx + (x - x0)/dx^2 = 1/dx. So each interval is
 *  integrated once, for its box B and its falling side R; this sample is
 *  then the carry from the previous interval plus R, and the next one's
 *  carry is B/dx - R. Half the work of integrating both sides.
 */
struct Halves
{
  vec4 box, falling;
};

/*! Where the waveform jumps inside an interval, if it does: at most two
 *  boundaries per sample (the chip keeps the pivot four samples wide)
 */
struct Jumps
{
  vec4 at[2];     // in samples from the interval's start, [0, 1)
  vec4 height[2]; // value after minus value before; 0 where no jump
};

namespace detail
{
//! y at x with the coefficients of the segment holding m
[[nodiscard]] inline vec4 valueAt(const Shape& s, vec4 m, vec4 x) noexcept
{
  const vec4 a = pick(s, s.a, m), b = pick(s, s.b, m);
  const vec4 c = pick(s, s.c, m), e = pick(s, s.e, m);
  vec4 sn, cs;
  sincos2pi(a + b * x, sn, cs);
  return (vec4(1.f) - cs) * (c + e * x) - vec4(1.f);
}
}

//! `inv` is 1 / dx, which the caller has once per control tick
template <bool FindJumps = false>
[[nodiscard]] inline Halves sampleInterval(
    const Shape& here, const Shape& next, bool sameShape, vec4 x0, vec4 dx, vec4 inv,
    Jumps* jumps = nullptr) noexcept
{
  const vec4 one(1.f), zero(0.f), half(0.5f);
  const vec4 inv2 = inv * inv;
  const vec4 xEnd = x0 + dx;
  vec4 start = x0;
  vec4 box = zero, falling = zero;
  vec4 lastEnd = zero; // the value at the end of the previous piece
  if constexpr(FindJumps)
    for(int j = 0; j < 2; j++)
      jumps->at[j] = jumps->height[j] = zero;
  for(int piece = 0; piece < 3; piece++)
  {
    const auto wrapped = start >= one;
    const vec4 offset = kfr::select(wrapped, one, zero);
    const vec4 local = start - offset;
    vec4 end = xEnd;
    // Lanes past the wrap read the next cycle's shape: a copy only when the
    // lanes disagree
    Shape sel;
    const Shape* s = &here;
    if(!sameShape && kfr::any(wrapped))
    {
      if(kfr::all(wrapped))
        s = &next;
      else
      {
        sel = selectShape(wrapped, next, here);
        s = &sel;
      }
    }
    if(piece < 2)
      end = kfr::min(xEnd, nextBreak(*s, local) + offset);
    const vec4 L = kfr::max(end - start, zero);
    const vec4 m = local + half * L;
    const Moments mo = moments(*s, m, L);
    const vec4 mid = start + half * L;
    box += mo.i0;
    falling += (one - (mid - x0) * inv) * inv * mo.i0 - inv2 * mo.i1;

    if constexpr(FindJumps)
    {
      // A boundary crossed before this piece: what the value did there
      if(piece > 0)
      {
        const auto crossed = start < xEnd;
        const vec4 h = detail::valueAt(*s, m, local) - lastEnd;
        jumps->height[piece - 1] = kfr::select(crossed, h, zero);
        jumps->at[piece - 1] = (start - x0) * inv;
      }
      lastEnd = detail::valueAt(*s, m, local + L);
    }

    start = end;
    if(!kfr::any(start < xEnd))
      break;
  }
  return {box, falling};
}

/*! The step a jump should make, minus the one the triangle already makes.
 *
 *  The target is the triangle's own step further low-passed by a half-band
 *  windowed sinc (Kaiser, beta 8, +-8 samples, cut at half the rate): in the
 *  band, a jump then sounds exactly as the rest of the waveform, which the
 *  compensation filter flattens; above it, what would fold back is gone. As
 *  blargg's band-limited step synthesis in the chip emulations, but against
 *  the step already there. Tabulated over u = (sample - jump), in samples.
 *
 *  build() takes a few milliseconds and does not depend on the rate: the
 *  synth calls it once, in its first prepare().
 */
class StepResidual
{
public:
  static constexpr int kSinc = 8;          // the low-pass's half width
  static constexpr int kHalf = kSinc + 1;  // and the triangle's
  static constexpr int kPerSample = 128;
  static constexpr int kSize = 2 * kHalf * kPerSample + 1;

  [[nodiscard]] float operator()(float u) const noexcept
  {
    const float x = (u + float(kHalf)) * float(kPerSample);
    if(!(x > 0.f) || x >= float(kSize - 1))
      return 0.f;
    const int i = int(x);
    const float f = x - float(i);
    return table[std::size_t(i)] + f * (table[std::size_t(i + 1)] - table[std::size_t(i)]);
  }

  void build() noexcept
  {
    constexpr double pi = std::numbers::pi;
    constexpr double beta = 8.;
    // The low-pass, on the table's grid, normalised to a unit sum. On the
    // stack, in single precision: 8 KB, and no allocation
    constexpr int taps = 2 * kSinc * kPerSample + 1;
    std::array<float, taps> lp;
    double sum = 0.;
    for(int i = 0; i < taps; i++)
    {
      const double t = double(i - kSinc * kPerSample) / kPerSample;
      const double x = t; // cut at 0.5 cycles per sample: sinc(2 * 0.5 * t)
      const double sinc = std::abs(x) < 1e-12 ? 1. : std::sin(pi * x) / (pi * x);
      const double r = t / kSinc;
      const double w = besselI0(beta * std::sqrt(std::max(0., 1. - r * r))) / besselI0(beta);
      lp[std::size_t(i)] = float(sinc * w);
      sum += lp[std::size_t(i)];
    }

    // The target step: the triangle's, through the low-pass
    for(int i = 0; i < kSize; i++)
    {
      const double u = -kHalf + double(i) / kPerSample;
      double acc = 0.;
      for(int j = 0; j < taps; j++)
      {
        const double tau = double(j - kSinc * kPerSample) / kPerSample;
        acc += double(lp[std::size_t(j)]) * triangleStep(u - tau);
      }
      table[std::size_t(i)] = float(acc / sum - triangleStep(u));
    }
  }

private:
  [[nodiscard]] static double triangleStep(double u) noexcept
  {
    return u <= -1. ? 0.
           : u < 0. ? 0.5 * (u + 1.) * (u + 1.)
           : u < 1. ? 1. - 0.5 * (1. - u) * (1. - u)
                    : 1.;
  }

  [[nodiscard]] static double besselI0(double x) noexcept
  {
    double sum = 1., term = 1.;
    for(int k = 1; k < 40; k++)
    {
      term *= (x / (2. * k)) * (x / (2. * k));
      sum += term;
    }
    return sum;
  }

  std::array<float, kSize> table{};
};
}
