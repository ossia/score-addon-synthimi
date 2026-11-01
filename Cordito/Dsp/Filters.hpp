#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: the filters of the loops and their designs, with their frequency
// responses, and the interpolated delay line.

#include <Cordito/Dsp/Common.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Filters, with their frequency responses: the tuning needs their phase delay
// at the fundamental.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::complex<double> unitDelay(double w) noexcept
{
  return std::polar(1., -w);
}

//! -arg(H) / w: the delay, in samples, a filter adds at w
[[nodiscard]] inline double phaseDelay(std::complex<double> h, double w) noexcept
{
  return w > 0. ? -std::arg(h) / w : 0.;
}

//! One-pole lowpass y = b x + p y[-1]: the loop loss.
struct OnePole
{
  double b = 1., p = 0., y = 0.;
  double tick(double x) noexcept { return y = b * x + p * y; }
  std::complex<double> response(double w) const noexcept
  {
    return b / (1. - p * unitDelay(w));
  }
};

//! First-order allpass (a + z^-1) / (1 + a z^-1). With a < 0 its delay falls
//! with frequency: the upper partials of the loop come back sooner, which
//! stretches them like a stiff string's.
struct Allpass1
{
  double a = 0., x1 = 0., y1 = 0.;
  double tick(double x) noexcept
  {
    const double y = a * x + x1 - a * y1;
    x1 = x;
    y1 = y;
    return y;
  }
  std::complex<double> response(double w) const noexcept
  {
    const auto z = unitDelay(w);
    return (a + z) / (1. + a * z);
  }
  //! -arg(response(w)) / w in closed form: H = e^{-jw} (1 + a e^{jw}) /
  //! (1 + a e^{-jw}), whose phase is -w + 2 atan2(a sin w, 1 + a cos w)
  double phaseDelay(double w) const noexcept
  {
    return w > 0. ? 1. - 2. * std::atan2(a * std::sin(w), 1. + a * std::cos(w)) / w : 0.;
  }
};

//! The inharmonicity B of the stiff-string law f_n = n f0 sqrt((1 + B n^2) /
//! (1 + B)), shared by the waveguide and the modes so that Stiffness is the
//! same instrument in every engine. It grows with pitch, as along a piano
//! (Fletcher, JASA 1964): at middle C, 4e-4 (a piano's) at Stiffness 0.2 and
//! 0.01 (a bar's, partial 8 four semitones sharp) at 1.
[[nodiscard]] inline double inharmonicity(double stiffness, double f0) noexcept
{
  return std::min(0.03, 0.01 * stiffness * stiffness * f0 / 261.63);
}

//! The coefficient of M identical first-order allpasses that stretch a loop
//! of fundamental f0 to inharmonicity B: Rauhala and Valimaki, "Dispersion
//! modeling in waveguide piano synthesis using tunable allpass filters",
//! DAFx-06, with the erratum of Rauhala's thesis, as Faust's
//! piano_dispersion_filter has it. Fitted at 44.1 kHz: a loop at fs is the
//! loop of f0 * 44100 / fs there, so the stretch is the same at every rate.
[[nodiscard]] inline double dispersionCoef(double B, double f0, int M, double fs) noexcept
{
  const double semitone = std::log(2.) / 12.;
  const double key = std::log(f0 * 44100. / fs / 27.5) / semitone + 1.;
  const double lb = std::log(std::max(B, 1e-6)), lm = std::log(double(M));
  const double kd = std::exp(-0.00179 * lb * lb - 0.0233 * lb - 2.93);
  const double cd = std::exp((0.0126 * lm + 0.0606) * lb - 0.00825 * lm + 1.97);
  const double d = std::exp(cd - key * kd);
  return (1. - d) / (1. + d);
}

//! Direct form II transposed biquad, a0 normalised to 1
struct Biquad
{
  double b0 = 1., b1 = 0., b2 = 0., a1 = 0., a2 = 0.;
  double s1 = 0., s2 = 0.;
  double tick(double x) noexcept
  {
    const double y = b0 * x + s1;
    s1 = b1 * x - a1 * y + s2;
    s2 = b2 * x - a2 * y;
    return y;
  }
  void reset() noexcept { s1 = s2 = 0.; }
  std::complex<double> response(double w) const noexcept
  {
    const auto z = unitDelay(w);
    const auto z2 = z * z;
    return (b0 + b1 * z + b2 * z2) / (1. + a1 * z + a2 * z2);
  }
  //! -arg(response(w)) / w with real arithmetic: the phase of N conj(D), no
  //! complex division
  double phaseDelay(double w) const noexcept
  {
    if(w <= 0.)
      return 0.;
    const double c1 = std::cos(w), s1 = std::sin(w);
    const double c2 = c1 * c1 - s1 * s1, s2 = 2. * s1 * c1;
    // z = e^{-jw}: N = b0 + b1 z + b2 z^2, D = 1 + a1 z + a2 z^2
    const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const double dr = 1. + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return -std::atan2(ni * dr - nr * di, nr * dr + ni * di) / w;
  }

  //! Bandpass with a peak gain of exactly 1 at f (RBJ "constant 0 dB peak"):
  //! the bilinear transform of (s/Q) / (s^2 + s/Q + 1).
  static Biquad bandpass(double f, double q, double fs) noexcept
  {
    const double w = kTwoPi * f / fs;
    const double alpha = std::sin(w) / (2. * q);
    const double a0 = 1. + alpha;
    Biquad bq;
    bq.b0 = alpha / a0;
    bq.b1 = 0.;
    bq.b2 = -alpha / a0;
    bq.a1 = -2. * std::cos(w) / a0;
    bq.a2 = (1. - alpha) / a0;
    return bq;
  }

  //! 1 - k * bandpass: a partial notch, |H| <= 1 everywhere for 0 <= k <= 2.
  //! In the analog prototype |1 - k BP(jw)|^2 = ((1-k)^2 + X^2) / (1 + X^2)
  //! with X = Q (w/w0 - w0/w), and the bilinear transform maps the jw axis
  //! onto the unit circle, so the bound holds for the digital filter too. It
  //! is what makes the bridge coupling passive at any setting.
  static Biquad partialNotch(double f, double q, double k, double fs) noexcept
  {
    const double w = kTwoPi * f / fs;
    const double alpha = std::sin(w) / (2. * q);
    const double a0 = 1. + alpha;
    Biquad bq;
    bq.b0 = (1. + alpha - k * alpha) / a0;
    bq.b1 = -2. * std::cos(w) / a0;
    bq.b2 = (1. - alpha + k * alpha) / a0;
    bq.a1 = -2. * std::cos(w) / a0;
    bq.a2 = (1. - alpha) / a0;
    return bq;
  }
};

//! DC blocker g (1 - z^-1) / (1 - R z^-1), g = (1 + R) / 2. Without g its
//! gain reaches 2 / (1 + R) > 1 at Nyquist: in a loop, a string near the top
//! of the range grew instead of decaying.
struct DcBlocker
{
  double r = 0.999, g = 0.9995, x1 = 0., y1 = 0.;
  void setCutoff(double f, double fs) noexcept
  {
    r = std::exp(-kTwoPi * f / fs);
    g = 0.5 * (1. + r);
  }
  double tick(double x) noexcept
  {
    const double y = g * (x - x1) + r * y1;
    x1 = x;
    y1 = y;
    return y;
  }
  std::complex<double> response(double w) const noexcept
  {
    const auto z = unitDelay(w);
    return g * (1. - z) / (1. - r * z);
  }
};

//! The loss filter of a loop of fundamental f0: per-period gain G0 at f0, for
//! a T60 of t60 there, and G1 at fhi, for a T60 of t60 * ratio.
//!
//! |H(w)|^2 = b^2 / (1 - 2 p cos w + p^2): asking for |H(w1)| / |H(w0)| = R
//! gives p^2 - 2 B p + 1 = 0 with B = (cos w0 - R^2 cos w1) / (1 - R^2), whose
//! root inside the unit circle is p = B - sqrt(B^2 - 1). When the ratio asks
//! for more than a one-pole can do, p saturates at pmax; pmax also bounds the
//! filter's delay, p / (1 - p) at DC, which a short loop has to fit.
[[nodiscard]] inline OnePole designLoss(
    double f0, double fs, double t60, double ratio, double fhi, double pmax) noexcept
{
  OnePole lp;
  const double g0 = std::exp(-kLn1000 / (t60 * f0));
  const double w0 = kTwoPi * f0 / fs;
  const double w1 = kTwoPi * fhi / fs;
  double p = 0.;
  if(ratio < 0.9999 && w1 > w0 * 1.5)
  {
    const double g1 = std::exp(-kLn1000 / (t60 * ratio * f0));
    const double r2 = (g1 / g0) * (g1 / g0);
    const double c0 = std::cos(w0), c1 = std::cos(w1);
    const double bb = (c0 - r2 * c1) / (1. - r2);
    p = bb > 1. ? bb - std::sqrt(bb * bb - 1.) : pmax;
  }
  p = std::clamp(p, 0., pmax);
  // DC gain of the filter: its maximum, as it is a lowpass
  const double g = g0 * std::sqrt(1. - 2. * p * std::cos(w0) + p * p) / (1. - p);
  lp.p = p;
  lp.b = std::min(g, 0.99995) * (1. - p);
  return lp;
}

//! The loop loss: g ((1 - k) + k L(z)^2) (1 - c Ll(z)), L and Ll one-pole
//! lowpasses of unit gain at DC.
//! - g ((1 - k) + k L^2): a flat gain mixed with a 12 dB/octave lowpass whose
//!   corner sits
//!   between the fundamental and the reference frequency. g sets the decay of
//!   the fundamental exactly, k how much sooner the upper partials die. The
//!   one-pole it replaces had to raise its gain at DC to stay dark, up to a
//!   clamp that then cut the decay (C6 at Decay 20 s and Brightness 0 lasted
//!   0.8 s).
//! - (1 - c Ll): a shallow low shelf at f0 / 4 that brings the gain at DC
//!   down to the gain at f0, so that the loop's mode at 0 Hz (a waveguide
//!   resonates at every multiple of f0, 0 included) decays with the note
//!   instead of outliving it. It replaces a DC blocker, whose phase lead
//!   detuned the harmonics.
//! Both factors have |H| <= 1 (for a unit-DC one-pole L, Re L >= |L|^2), so
//! with g <= 1 the loss is passive at any setting.
struct LossShelf
{
  double g = 1., k = 0., q = 0., c = 0., ql = 0.;
  double lp = 0., lp2 = 0., lpl = 0.;
  double tick(double x) noexcept
  {
    lpl = (1. - ql) * x + ql * lpl;
    const double xs = x - c * lpl;
    lp = (1. - q) * xs + q * lp;
    lp2 = (1. - q) * lp + q * lp2;
    return g * ((1. - k) * xs + k * lp2);
  }
  static std::complex<double> lowpass(double pole, double w) noexcept
  {
    return (1. - pole) / (1. - pole * unitDelay(w));
  }
  std::complex<double> response(double w) const noexcept
  {
    const auto l = lowpass(q, w);
    return g * ((1. - k) + k * l * l) * (1. - c * lowpass(ql, w));
  }
  //! -arg(response(w)) / w with real arithmetic. A unit-DC one-pole at
  //! z = e^{-jw} is (1 - p) (u + j v) / (u^2 + v^2) with u = 1 - p cos w,
  //! v = -p sin w: only the phase is wanted, so the positive scales go.
  double phaseDelay(double w) const noexcept
  {
    if(w <= 0.)
      return 0.;
    const double cw = std::cos(w), sw = std::sin(w);
    // l^2 for the main lowpass
    const double u = 1. - q * cw, v = -q * sw, m = (1. - q) / (u * u + v * v);
    const double lr = m * u, li = m * v;
    const double ar = (1. - k) + k * (lr * lr - li * li), ai = k * 2. * lr * li;
    // 1 - c Ll
    const double ul = 1. - ql * cw, vl = -ql * sw, ml = c * (1. - ql) / (ul * ul + vl * vl);
    const double br = 1. - ml * ul, bi = -ml * vl;
    const double pr = ar * br - ai * bi, pi = ar * bi + ai * br;
    // g > 0 does not turn the phase
    return -std::atan2(pi, pr) / w;
  }
};

//! The loss of a loop of fundamental f0, for a T60 of t60 there and of
//! t60 * ratio at fhi. droop0 and droop1: what the fractional delay reads of
//! the loop already lose at f0 and at fhi, taken off the targets so that the
//! decay does not depend on each note's delay fraction.
[[nodiscard]] inline LossShelf designLossShelf(
    double f0, double fs, double t60, double ratio, double fhi, double droop0,
    double droop1) noexcept
{
  const double t0 = std::exp(-kLn1000 / (t60 * f0)) / droop0;
  const double t1 = std::exp(-kLn1000 / (t60 * ratio * f0)) / droop1;
  const double w0 = kTwoPi * f0 / fs, w1 = kTwoPi * fhi / fs;
  const double rho = std::min(1., t1 / t0);
  constexpr double kMaxGain = 0.99995;
  const double floor0 = std::min(1., t0 / kMaxGain);
  // The smallest root of a k^2 + b k + c in [0, hi], else what the fallback
  // finds
  const auto rootIn = [](double a, double b, double c, double hi, auto&& fallback) {
    double best = -1.;
    if(std::abs(a) > 1e-300)
    {
      const double disc = b * b - 4. * a * c;
      if(disc >= 0.)
      {
        const double sq = std::sqrt(disc);
        // The stable pair of formulas
        const double qq = -0.5 * (b + (b >= 0. ? sq : -sq));
        for(double x : {qq / a, qq != 0. ? c / qq : -1.})
          if(x >= 0. && x <= hi && (best < 0. || x < best))
            best = x;
      }
    }
    else if(b != 0.)
    {
      const double x = -c / b;
      if(x >= 0. && x <= hi)
        best = x;
    }
    return best >= 0. ? best : double(fallback());
  };
  const auto bisect = [](auto&& tooFar) {
    double lo = 0., hi = 1.;
    for(int i = 0; i < 32; i++)
    {
      const double mid = 0.5 * (lo + hi);
      (tooFar(mid) ? hi : lo) = mid;
    }
    return lo;
  };

  // For a corner fc: the k that gives the ratio asked, or, as the decay of
  // the fundamental comes first (k may not dim f0 below what g <= kMaxGain
  // can make up for), the darkest allowed; and the ratio it reaches
  struct Fit
  {
    double q, k, reached;
    std::complex<double> u0;
  };
  const auto fit = [&](double fc) {
    Fit r{};
    r.q = std::exp(-kTwoPi * fc / fs);
    const auto l2 = [&](double w) {
      const auto l = LossShelf::lowpass(r.q, w);
      return l * l;
    };
    r.u0 = l2(w0) - 1.;
    const auto u1 = l2(w1) - 1.;
    const auto shape = [&](double k, std::complex<double> u) { return std::abs(1. + k * u); };
    // |1 + k u|^2 = (1 + k Re u)^2 + (k Im u)^2 is a quadratic in k: both
    // searches are its roots, where they took 32 bisection steps each, and
    // made a note-on cost as much as 20 samples of the whole chord
    const double n0 = std::norm(r.u0), n1 = std::norm(u1);
    const double kMax
        = shape(1., r.u0) >= floor0
              ? 1.
              : rootIn(n0, 2. * r.u0.real(), 1. - floor0 * floor0, 1., [&] {
                  return bisect([&](double k) { return shape(k, r.u0) < floor0; });
                });
    const auto ratioAt = [&](double k) { return shape(k, u1) / shape(k, r.u0); };
    const double rho2 = rho * rho;
    r.k = ratioAt(kMax) >= rho
              ? kMax
              : rootIn(n1 - rho2 * n0, 2. * (u1.real() - rho2 * r.u0.real()), 1. - rho2, kMax, [&] {
                  return bisect([&](double k) { return ratioAt(k) < rho; });
                });
    r.reached = ratioAt(r.k);
    return r;
  };

  // The corner, between 1.5 f0 and the reference: the one that gets closest
  // to the ratio asked. Nearer f0 it darkens more but dims f0 sooner.
  Fit best = fit(std::min(1.5 * f0, 0.4 * fs));
  if(w1 > 1.2 * w0 && rho < 0.9999)
  {
    const double lo = 1.5 * f0, hi = std::min(fhi, 0.4 * fs);
    for(int i = 1; i <= 8 && hi > lo; i++)
    {
      const auto r = fit(lo * std::pow(hi / lo, i / 8.));
      if(r.reached < best.reached - 1e-9)
        best = r;
    }
  }
  else
    best.k = 0.;

  LossShelf h;
  h.q = best.q;
  h.k = best.k;
  h.ql = std::exp(-kTwoPi * 0.25 * f0 / fs);

  // Then the low shelf, so that the gain at DC, g (1 - c), is the gain at
  // f0, t0. It dims f0 a little too, so g and c settle in a few rounds.
  const double s0 = std::abs(1. + h.k * best.u0);
  const auto l0 = LossShelf::lowpass(h.ql, w0);
  double g = std::min(t0 / s0, kMaxGain), c = 0.;
  for(int i = 0; i < 4; i++)
  {
    c = std::clamp(1. - t0 / g, 0., 1.);
    g = std::min(t0 / (s0 * std::abs(1. - c * l0)), kMaxGain);
  }
  h.g = g;
  h.c = c;
  return h;
}

//! 8-point (7th-order) Lagrange interpolation weights for the samples at
//! delays m - 3 .. m + 4 of a read at m + f, cached: they only change when
//! the delay's fractional part does, during a bend or a slide. A loop reads
//! once per period, so the interpolator's loss at high frequencies compounds:
//! the 4-point one lost 2.6 % per period at 8 kHz (48 kHz) in the worst case,
//! which alone made the partials there of C5 die in 0.5 s, and by an amount
//! that changed from one key to the next. This one loses 0.12 %.
struct Lagrange8
{
  static constexpr int kTaps = 8;
  static constexpr int kBefore = 3; // taps before the integer part
  double f = -1.;
  std::array<double, kTaps> h{};

  //! 1 / prod_{j != k} (k - j): the denominators of the weights
  static constexpr std::array<double, kTaps> kInvDen = [] {
    std::array<double, kTaps> d{};
    for(int k = 0; k < kTaps; k++)
    {
      double c = 1.;
      for(int j = 0; j < kTaps; j++)
        if(j != k)
          c *= double(k - j);
      d[std::size_t(k)] = 1. / c;
    }
    return d;
  }();

  //! prod_{j != k} (frac - x_j) as the product of the factors before k and
  //! after it: 3 multiplies per tap instead of 7 multiplies and 7 divisions.
  //! A sliding delay (a bend, the tension kick) needs them at every sample.
  static std::array<double, kTaps> weights(double frac) noexcept
  {
    std::array<double, kTaps> d{}, before{}, after{};
    for(int j = 0; j < kTaps; j++)
      d[std::size_t(j)] = frac - (j - kBefore);
    before[0] = 1.;
    for(int k = 1; k < kTaps; k++)
      before[std::size_t(k)] = before[std::size_t(k - 1)] * d[std::size_t(k - 1)];
    after[kTaps - 1] = 1.;
    for(int k = kTaps - 2; k >= 0; k--)
      after[std::size_t(k)] = after[std::size_t(k + 1)] * d[std::size_t(k + 1)];
    std::array<double, kTaps> w{};
    for(int k = 0; k < kTaps; k++)
      w[std::size_t(k)] = before[std::size_t(k)] * after[std::size_t(k)] * kInvDen[std::size_t(k)];
    return w;
  }
  void set(double frac) noexcept
  {
    if(frac != f)
    {
      f = frac;
      h = weights(frac);
    }
  }
};

//! |gain| at w of a Lagrange8 read of fractional part f: what it loses there
[[nodiscard]] inline double lagrangeGain(double f, double w) noexcept
{
  if(f == 0.)
    return 1.;
  const auto h = Lagrange8::weights(f);
  std::complex<double> sum{};
  for(int k = 0; k < Lagrange8::kTaps; k++)
    sum += h[std::size_t(k)] * std::polar(1., -w * (k - Lagrange8::kBefore));
  return std::abs(sum);
}

//! A delay line read with Lagrange interpolation: stateless, so a pitch bend
//! or a slide never leaves a transient behind, as a Thiran allpass does.
//! Reads happen before the write of the same sample, so a delay `d` returns
//! x[n - d].
struct DelayLine
{
  float* buf = nullptr;
  int mask = 0;
  int w = 0;

  void attach(float* b, int sizePow2) noexcept
  {
    buf = b;
    mask = sizePow2 - 1;
    w = 0;
  }
  void clear() noexcept
  {
    if(buf)
      std::memset(buf, 0, sizeof(float) * std::size_t(mask + 1));
    w = 0;
  }
  float at(int k) const noexcept { return buf[(w - k) & mask]; }
  //! An 8-point Lagrange read, exact for a whole number of samples; d >= 4
  double read(double d, Lagrange8& c) const noexcept
  {
    const int m = int(d);
    const double f = d - m;
    if(f == 0.)
      return at(m);
    c.set(f);
    double y = 0.;
    for(int k = 0; k < Lagrange8::kTaps; k++)
      y += c.h[std::size_t(k)] * at(m + k - Lagrange8::kBefore);
    return y;
  }
  void write(double x) noexcept
  {
    buf[w] = float(x);
    w = (w + 1) & mask;
  }
};
}
