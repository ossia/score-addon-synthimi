#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: one polarisation of a waveguide string, and the bow's friction.

#include <Cordito/Dsp/Filters.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Waveguide string
// ---------------------------------------------------------------------------

//! The bow's friction on the string: hyperbolic, mu(u) = mud + (mus - mud)
//! v0 / (v0 + u) for a sliding speed u (Smith and Woodhouse, JMPS 2000; as
//! SuperCollider's DWGBowed solves it), against STK's table, a heuristic
//! curve whose playable region moved with the note and the bow position.
//!
//! At the bow, the string's velocity is the sum of the waves arriving there,
//! vh, plus what the friction adds to both outgoing waves; the force is
//! given as that velocity, fn = F / (2 Z). The bow carries the string while
//! the static friction can hold it, |vb - vh| <= fn mus. Sliding, the
//! added velocity sign(a) fn mu(u) with u the sliding speed solves
//! u^2 + (v0 - A) u + (C - A v0) = 0, A = |a| - fn mud, C = fn (mus - mud) v0,
//! a = vb - vh: the larger root, kept while it exists (the hysteresis that
//! picks between the friction curve's three solutions). Never more than
//! fn mus is added, whatever comes in.
struct BowFriction
{
  static constexpr double kMuS = 0.8, kMuD = 0.3;
  bool slipping = false;

  //! The velocity added to both waves leaving the bow
  double tick(double vb, double vh, double fn, double v0) noexcept
  {
    const double a = vb - vh;
    const double mag = std::abs(a);
    if(!slipping && mag <= fn * kMuS)
      return a;
    const double A = mag - fn * kMuD, C = fn * (kMuS - kMuD) * v0;
    const double b = v0 - A, disc = b * b - 4. * (C - A * v0);
    if(disc < 0.)
    {
      // No sliding solution: the bow catches the string again
      slipping = false;
      return std::clamp(a, -fn * kMuS, fn * kMuS);
    }
    slipping = true;
    const double u = std::max(0., 0.5 * (-b + std::sqrt(disc)));
    const double added = fn * (kMuD + (kMuS - kMuD) * v0 / (v0 + u));
    return a >= 0. ? added : -added;
  }
};

//! STK's bow table, the bow before the friction law: the reflection of the
//! velocity difference at the bow, at most 0.98
[[nodiscard]] inline double bowTable(double dv, double slope, double offset) noexcept
{
  const double x = std::abs(dv * slope) + offset;
  const double x2 = x * x;
  return std::clamp(1. / (x2 * x2), 0.01, 0.98);
}

//! One polarisation: delay lines from the junction to the bridge and to the
//! nut, and the filters at the bridge.
struct WgPol
{
  DelayLine neck, bridge;
  double dN = 2., dB = 2.;         // current delays, samples
  double dNStep = 0., dBStep = 0.; // per sample, towards the control target
  LossShelf loss;
  std::array<Allpass1, kDispStages> disp{};
  int dispCount = 0;
  std::array<Biquad, kNotches> notch{};
  int notchCount = 0;
  //! The bridge side is a whole number of samples: one fractional read in
  //! the loop instead of two, so half the interpolation loss
  bool wholeBridge = false;
  Lagrange8 neckRead, bridgeRead;
  //! What the loss, the dispersion, the notches and the delays were last
  //! computed from: the loss design searches its corner and the tuning
  //! evaluates every filter at six harmonics, worth skipping when nothing
  //! moved
  std::array<double, 10> ctrlKey{};
  //! What the filters and their delay (tau) were designed for: the pitch, and
  //! the rest of ctrlKey but the bow position; the pitch and bow position at
  //! the previous control tick
  double designF = 0., tau = 0.;
  std::array<double, 8> designKey{};
  double lastF = 0., lastBeta = -1.;
  //! Buzz: the state of the bridge's allpass
  double jaw = 0.;
  //! The E-Bow: its pickup's resonance at the string's fundamental, and the
  //! mean square its gain control follows, over ebowCoef
  Biquad ebowRes;
  double ebowEnv = 0., ebowCoef = 0.999;
  BowFriction friction;
  //! The friction's push with its corners rounded, by a one-pole of
  //! coefficient roundA. The hysteresis makes the change from sticking to
  //! sliding a step, which a real bow rounds over the width of its hair
  //! and the string's torsion; a step, the slip corner was a click once per
  //! period, 40 dB brighter than the table it replaced.
  double bowRound = 0., roundA = 1.;

  void clear() noexcept
  {
    neck.clear();
    bridge.clear();
    loss.lp = loss.lp2 = loss.lpl = 0.;
    for(auto& a : disp)
      a.x1 = a.y1 = 0.;
    for(auto& q : notch)
      q.reset();
    jaw = 0.;
    ebowRes.reset();
    ebowEnv = 0.;
    friction = {};
  }

  double filters(double x) noexcept
  {
    double y = loss.tick(x);
    for(int i = 0; i < dispCount; i++)
      y = disp[std::size_t(i)].tick(y);
    for(int i = 0; i < notchCount; i++)
      y = notch[std::size_t(i)].tick(y);
    return y;
  }

  double lossDelay(double w) const noexcept { return loss.phaseDelay(w); }
  double dispersionDelay(double w) const noexcept
  {
    // The stages are identical
    return dispCount > 0 ? dispCount * disp[0].phaseDelay(w) : 0.;
  }
  double notchDelay(double w) const noexcept
  {
    double tau = 0.;
    for(int i = 0; i < notchCount; i++)
      tau += notch[std::size_t(i)].phaseDelay(w);
    return tau;
  }
};
}
