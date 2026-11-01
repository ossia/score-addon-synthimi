#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: the low-frequency oscillators that modulate a voice.

#include <Cordito/Dsp/Common.hpp>

namespace crd
{
//! One LFO of one voice: restarted with the note, advanced at the control
//! rate. The sine and the triangle start from 0, rising, so that a vibrato
//! does not jump at the note's start.
struct Lfo
{
  double phase = 0.;
  double held = 0.;
  std::uint32_t rng = 0x9E3779B9u;

  void start(std::uint32_t seed) noexcept
  {
    phase = 0.;
    rng = seed | 1u;
    held = random();
  }

  //! The value at the current phase, in [-1, 1], then the phase moves on by
  //! `advance` periods
  double tick(LfoShape shape, double advance) noexcept
  {
    const double p = phase;
    double v = 0.;
    switch(shape)
    {
      case LfoShape::Sine:
        v = std::sin(kTwoPi * p);
        break;
      case LfoShape::Triangle:
        v = p < 0.25 ? 4. * p : p < 0.75 ? 2. - 4. * p : 4. * p - 4.;
        break;
      case LfoShape::SawUp:
        v = 2. * p - 1.;
        break;
      case LfoShape::SawDown:
        v = 1. - 2. * p;
        break;
      case LfoShape::Square:
        v = p < 0.5 ? 1. : -1.;
        break;
      case LfoShape::Random:
        v = held;
        break;
    }
    phase += advance;
    if(phase >= 1.)
    {
      phase -= std::floor(phase);
      held = random();
    }
    return v;
  }

private:
  double random() noexcept
  {
    rng = rng * 1664525u + 1013904223u;
    return double(rng >> 8) * (2. / 16777216.) - 1.;
  }
};
}
