#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* 2x up- and downsampling by polyphase IIR half-band filters: two chains of
 * first-order allpasses, the even coefficients on one sample of each pair and
 * the odd ones on the other (Valenzuela and Constantinides, as in Laurent de
 * Soras' HIIR, WTFPL), with the coefficients of the elliptic design HIIR uses.
 * The coefficients do not depend on the rate: each filter computes its own
 * once, in prepare(). T is a sample, or a vector of lanes filtered alike.
 */

#include <array>
#include <cmath>
#include <numbers>

namespace viceversa
{
namespace halfband
{
inline double ipow(double x, int n) noexcept
{
  double r = 1.;
  while(n-- > 0)
    r *= x;
  return r;
}

/*! HIIR's PolyphaseIir2Designer::compute_coefs_spec_order_tbw: N
 *  coefficients, a transition `transition` of the higher rate wide, centred
 *  on a quarter of it, so the passband reaches 0.25 - transition / 2
 */
template <std::size_t N>
[[nodiscard]] std::array<double, N> design(double transition) noexcept
{
  constexpr double pi = std::numbers::pi;
  double k = std::tan((1. - transition * 2.) * pi / 4.);
  k *= k;
  const double kksqrt = std::pow(1. - k * k, 0.25);
  const double e = 0.5 * (1. - kksqrt) / (1. + kksqrt);
  const double e2 = e * e;
  const double e4 = e2 * e2;
  const double q = e * (1. + e4 * (2. + e4 * (15. + 150. * e4)));

  const int order = int(N) * 2 + 1;
  std::array<double, N> coefs{};
  for(int index = 0; index < int(N); index++)
  {
    const int c = index + 1;

    double num = 0.;
    {
      int i = 0, j = 1;
      double t;
      do
      {
        t = ipow(q, i * (i + 1)) * std::sin((i * 2 + 1) * c * pi / order) * j;
        num += t;
        j = -j;
        ++i;
      } while(std::abs(t) > 1e-100);
    }
    num *= std::pow(q, 0.25);

    double den = 0.;
    {
      int i = 1, j = -1;
      double t;
      do
      {
        t = ipow(q, i * i) * std::cos(i * 2 * c * pi / order) * j;
        den += t;
        j = -j;
        ++i;
      } while(std::abs(t) > 1e-100);
    }
    den += 0.5;

    const double ww = num / den;
    const double wwsq = ww * ww;
    const double x = std::sqrt((1. - wwsq * k) * (1. - wwsq / k)) / (1. + wwsq);
    coefs[std::size_t(index)] = (1. - x) / (1. + x);
  }
  return coefs;
}

//! The two allpass chains, both advanced by one sample each
template <typename T, std::size_t N>
struct Chains
{
  std::array<T, N> coef{}, x{}, y{};

  void prepare(double transition) noexcept
  {
    const auto c = design<N>(transition);
    for(std::size_t i = 0; i < N; i++)
      coef[i] = T(float(c[i]));
    reset();
  }
  void reset() noexcept
  {
    x.fill(T(0.f));
    y.fill(T(0.f));
  }

  void process(T& s0, T& s1) noexcept
  {
    for(std::size_t i = 0; i < N; i += 2)
    {
      const T t0 = (s0 - y[i]) * coef[i] + x[i];
      x[i] = s0;
      y[i] = t0;
      s0 = t0;
      if(i + 1 < N)
      {
        const T t1 = (s1 - y[i + 1]) * coef[i + 1] + x[i + 1];
        x[i + 1] = s1;
        y[i + 1] = t1;
        s1 = t1;
      }
    }
  }
};
}

/*! Two output samples from each input sample. Its input is already
 *  band-limited to 0.45 of the input rate: four coefficients with a
 *  transition of 0.1 keep it flat to 0.4 (0.013 dB down at 0.45) and its
 *  image 70 dB down from 0.6.
 */
template <typename T>
struct Upsampler2x
{
  static constexpr double kTransition = 0.1;
  halfband::Chains<T, 4> chains;

  void prepare() noexcept { chains.prepare(kTransition); }
  void reset() noexcept { chains.reset(); }
  //! out0 is the earlier
  void process(T in, T& out0, T& out1) noexcept
  {
    out0 = in;
    out1 = in;
    chains.process(out0, out1);
  }
};

/*! One output sample from two input samples: ten coefficients with a
 *  transition of 0.04, so the passband reaches 0.23 of the input rate (0.46
 *  of the output's), the stopband starts at 0.27, more than 120 dB down.
 */
template <typename T>
struct Decimator2x
{
  static constexpr double kTransition = 0.04;
  halfband::Chains<T, 10> chains;

  void prepare() noexcept { chains.prepare(kTransition); }
  void reset() noexcept { chains.reset(); }
  //! in0 is the earlier
  T process(T in0, T in1) noexcept
  {
    T s0 = in1, s1 = in0;
    chains.process(s0, s1);
    return T(0.5f) * (s0 + s1);
  }
};
}
