#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The last stage of a stereo synth: a DC blocker, a peak limiter, and a soft
 * clip of what the limiter lets through.
 */

#include <algorithm>
#include <cmath>
#include <numbers>

namespace synthimi
{
//! Continuous with a slope of 1 at 0.9, bounded by 1
[[nodiscard]] inline float softClip(float x) noexcept
{
  const float a = std::abs(x);
  if(a <= 0.9f)
    return x;
  return std::copysign(std::min(1.f, 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.f)), x);
}

struct OutputStage
{
  //! The peak the limiter holds the output to
  static constexpr float kLimit = 0.97f;

  //! A 10 Hz DC blocker; the limiter's 1 ms attack, 100 ms release
  void prepare(double fs) noexcept
  {
    dcCoef = float(1. - 2. * std::numbers::pi * 10. / fs);
    limAttack = float(1. - std::exp(-1. / (0.001 * fs)));
    limRelease = float(1. - std::exp(-1. / (0.1 * fs)));
    reset();
  }

  void reset() noexcept
  {
    dcL = dcR = dcLy = dcRy = 0.f;
    limGain = 1.f;
  }

  //! Without the FPU's help: the DC blocker's states, which decay towards
  //! denormals in silence, to zero
  void flushDenormals() noexcept
  {
    for(float* x : {&dcL, &dcR, &dcLy, &dcRy})
      if(std::abs(*x) < 1e-20f)
        *x = 0.f;
  }

  //! One-pole high-pass, in place
  void blockDc(float& l, float& r) noexcept
  {
    const float yl = l - dcL + dcCoef * dcLy;
    const float yr = r - dcR + dcCoef * dcRy;
    dcL = l;
    dcR = r;
    dcLy = yl;
    dcRy = yr;
    l = yl;
    r = yr;
  }

  /*! Limited, then soft clipped, in place; returns the peak it was given.
   *  The reduction comes in over 1 ms and goes over 100 ms; what passes
   *  meanwhile is eased under 1.
   */
  float limit(float& l, float& r) noexcept
  {
    const float peak = std::max(std::abs(l), std::abs(r));
    const float want = peak > kLimit ? kLimit / peak : 1.f;
    limGain += (want - limGain) * (want < limGain ? limAttack : limRelease);
    l = softClip(l * limGain);
    r = softClip(r * limGain);
    return peak;
  }

  float dcL{0.f}, dcR{0.f}, dcLy{0.f}, dcRy{0.f};
  float dcCoef{0.9987f};
  float limGain{1.f};
  float limAttack{0.02f}, limRelease{0.0002f};
};
}
