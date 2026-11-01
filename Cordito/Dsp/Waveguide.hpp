#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: one polarisation of a waveguide string, and the bow's friction.

#include <Cordito/Dsp/Filters.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Waveguide string
// ---------------------------------------------------------------------------

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
  std::array<double, 9> ctrlKey{};
  //! What the filters and their delay (tau) were designed for: the pitch, and
  //! the rest of ctrlKey but the bow position; the pitch and bow position at
  //! the previous control tick
  double designF = 0., tau = 0.;
  std::array<double, 7> designKey{};
  double lastF = 0., lastBeta = -1.;
  //! Buzz: the state of the bridge's allpass
  double jaw = 0.;
  //! The E-Bow: its pickup's resonance at the string's fundamental, and the
  //! mean square its gain control follows, over ebowCoef
  Biquad ebowRes;
  double ebowEnv = 0., ebowCoef = 0.999;

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

//! STK's bow table: the reflection of the velocity difference at the bow.
//! At most 0.98, so the junction never adds more than the bow gives.
[[nodiscard]] inline double bowTable(double dv, double slope, double offset) noexcept
{
  // x^-4 as 1 / (x^2)^2: pow per sample was a quarter of a bowed voice
  const double x = std::abs(dv * slope) + offset;
  const double x2 = x * x;
  const double t = 1. / (x2 * x2);
  return std::clamp(t, 0.01, 0.98);
}
}
