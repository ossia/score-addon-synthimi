#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* 2x decimation by a polyphase IIR half-band filter: two chains of first-order
 * allpasses, the even coefficients on one sample of each pair and the odd ones
 * on the other (Valenzuela and Constantinides, as in Laurent de Soras' HIIR,
 * WTFPL). The coefficients come from the elliptic design HIIR uses, computed
 * once in prepare().
 */

#include <array>
#include <cmath>
#include <numbers>

namespace czumarzu
{
namespace halfband
{
[[nodiscard]] inline double ipow(double x, int n) noexcept
{
  double r = 1.;
  while(n-- > 0)
    r *= x;
  return r;
}

//! HIIR's PolyphaseIir2Designer::compute_coefs_spec_order_tbw
template <std::size_t N>
[[nodiscard]] std::array<double, N> design(double transition)
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
}

/*! N coefficients, a transition `Transition` of the input rate wide, centred
 *  on a quarter of it: the passband reaches 0.25 - Transition / 2.
 */
template <std::size_t N>
class Halfband
{
public:
  static constexpr std::size_t kCoefs = N;

  void prepare(double transition) noexcept
  {
    const auto c = halfband::design<N>(transition);
    for(std::size_t i = 0; i < N; i++)
      coef[i] = float(c[i]);
    reset();
  }

  void reset() noexcept
  {
    x.fill(0.f);
    y.fill(0.f);
  }

  //! Where the FPU does not flush denormals: the allpasses' states, which
  //! decay towards them in silence, to zero
  void flushDenormals() noexcept
  {
    for(auto* v : {&x, &y})
      for(auto& s : *v)
        if(std::abs(s) < 1e-20f)
          s = 0.f;
  }

  //! One output sample from two input samples, in0 being the earlier
  [[nodiscard]] float process(float in0, float in1) noexcept
  {
    float s0 = in1, s1 = in0;
    for(std::size_t i = 0; i < N; i += 2)
    {
      const float t0 = (s0 - y[i]) * coef[i] + x[i];
      x[i] = s0;
      y[i] = t0;
      s0 = t0;

      if(i + 1 < N)
      {
        const float t1 = (s1 - y[i + 1]) * coef[i + 1] + x[i + 1];
        x[i + 1] = s1;
        y[i + 1] = t1;
        s1 = t1;
      }
    }
    return 0.5f * (s0 + s1);
  }

  std::array<float, N> coef{};

private:
  std::array<float, N> x{}, y{};
};

//! The last stage, to the output rate: ten coefficients with a transition of
//! 0.04, so the passband reaches 0.23 of the input rate (22 kHz at 48 kHz),
//! the stopband starts at 0.27, more than 100 dB down
class Decimator2x : public Halfband<10>
{
public:
  static constexpr double kTransition = 0.04;
  void prepare() noexcept { Halfband<10>::prepare(kTransition); }
};

//! The first stage of 4x, to twice the output rate: it only has to keep what
//! will fold into the output's band, below 0.1125 of its input rate, so its
//! transition can be wide (0.125 to 0.375): four coefficients, 117 dB down
class Decimator4x : public Halfband<4>
{
public:
  static constexpr double kTransition = 0.25;
  void prepare() noexcept { Halfband<4>::prepare(kTransition); }
};
}
