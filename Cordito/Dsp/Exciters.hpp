#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: what sets a string in motion.

#include <Cordito/Dsp/Common.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Exciters
// ---------------------------------------------------------------------------

//! A finite excitation signal: a velocity wave for the waveguide, a force for
//! the modes.
struct ExciteGen
{
  enum Kind : int
  {
    None,
    Pulse, // Hann pulse
    Step,  // a released force: a plectrum or a finger letting go
    Noise  // lowpassed noise
  };
  Kind kind = None;
  int n = 0;
  int len = 0, total = 0;
  double amp = 0.;
  // Step: amp (s - f (1 + fl n)), s and f decaying by ps and pf per sample
  double s = 0., f = 0., ps = 0., pf = 0., fl = 0.;
  double noiseAmp = 0.;
  int noiseLen = 0;
  double lp = 0., lpA = 1.;

  bool active() const noexcept { return kind != None && n < total; }

  static double hann(int k, int l) noexcept
  {
    return 0.5 * (1. - std::cos(kTwoPi * (k + 0.5) / l));
  }

  //! A force let go: the string, held aside, is released. In velocity waves
  //! that is a step, whose spectrum falls as 1/n, where a velocity pulse
  //! (a strike) is flat. It returns to zero with a corner at slow Hz, below
  //! the fundamental, and rises with a corner at fast Hz: the plectrum's
  //! or the finger's release, a 1/n^2 roll-off above it (1/n^3 when
  //! `soft`, the flesh of a finger), and no spectral null anywhere.
  void step(double a, double slow, double fast, bool soft, double fs) noexcept
  {
    kind = Step;
    // Height at the fundamental fixed, whatever the corners: they colour the
    // note and do not change its level. f0 is twice slow.
    const std::complex<double> jw{0., 2. * slow};
    const auto h = 1. / (jw + slow)
                   - (soft ? (jw + 2. * fast) / ((jw + fast) * (jw + fast)) : 1. / (jw + fast));
    amp = a / (std::abs(h) * 2. * slow);
    s = f = 1.;
    ps = std::exp(-kTwoPi * slow / fs);
    pf = std::exp(-kTwoPi * fast / fs);
    fl = soft ? kTwoPi * fast / fs : 0.;
    // Until it is down by 60 dB
    total = std::max(2, int(std::ceil(kLn1000 * fs / (kTwoPi * slow))));
  }

  //! The noise's lowpass at fc Hz, and its level set so that the same noise
  //! is heard at every sample rate: a lowpass of white noise keeps a share
  //! of its power that falls as the rate rises
  void noise(double level, double fc, double fs) noexcept
  {
    lpA = 1. - std::exp(-kTwoPi * std::min(fc, kTopFreq * fs) / fs);
    noiseAmp = level * std::sqrt(fs / 48000.);
  }

  double tick(Rng& rng) noexcept
  {
    if(!active())
      return 0.;
    double v = 0.;
    switch(kind)
    {
      case Pulse:
        v = amp * hann(n, len);
        break;
      case Step:
        v = amp * (s - f * (1. + fl * n));
        s *= ps;
        f *= pf;
        break;
      case Noise:
        break;
      case None:
        break;
    }
    if(n < noiseLen)
    {
      lp += lpA * (rng.bipolar() - lp);
      v += noiseAmp * lp;
    }
    n++;
    return v;
  }
};

//! Hunt-Crossley felt: F = k x^a (1 + mu dx/dt) for a compression x > 0, a
//! hammer of unit mass. The string is taken as rigid at the strike point: the
//! hammer always leaves, and its contact time, hence the brightness of the
//! note, follows from the velocity through the exponent (the harder the hit,
//! the shorter the contact), as in a piano. Integrated at 384 kHz during
//! contact: 8x at 48 kHz, fewer steps at higher rates.
struct Hammer
{
  bool active = false;
  double x = 0., v = 0.;
  double k = 0., alpha = 2.5, mu = 0.2;
  double dt = 0.;
  int oversample = 8;

  //! tc: contact time at a velocity of 1. For F = k x^a with unit mass, the
  //! maximum compression is ((a+1) v^2 / 2k)^(1/(a+1)) and the contact time
  //! about 3 x_max / v, which gives k for a requested contact time.
  void strike(double velocity, double tc, double a, double fs) noexcept
  {
    alpha = a;
    const double xm = tc / 3.;
    k = (alpha + 1.) / (2. * std::pow(xm, alpha + 1.));
    x = 0.;
    v = std::max(velocity, 1e-4);
    oversample = std::clamp(int(std::ceil(384000. / fs)), 1, 8);
    dt = 1. / (fs * oversample);
    active = true;
  }

  //! The momentum given to the string during this sample
  double tick() noexcept
  {
    if(!active)
      return 0.;
    double impulse = 0.;
    for(int i = 0; i < oversample; i++)
    {
      double f = 0.;
      if(x > 0.)
        f = std::max(0., k * std::pow(x, alpha) * (1. + mu * v));
      v -= f * dt;
      x += v * dt;
      impulse += f * dt;
    }
    if(x <= 0. && v <= 0.)
      active = false;
    return impulse;
  }
};
}
