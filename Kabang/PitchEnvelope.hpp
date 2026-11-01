#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <algorithm>
#include <cmath>

namespace kbng
{
//! The range RubberBand accepts as a pitch scale.
inline constexpr double min_pitch_scale = 1. / 5.;
inline constexpr double max_pitch_scale = 5.;

/**
 * @brief The pitch ratio to hand the stretcher for one frame.
 *
 * @param pitch     the Pitch knob, a ratio around 1
 * @param envelope  the pitch ADSR at this frame, in [0; 1]
 * @param amount    signed pitch-envelope depth in semitones
 * @param enabled   the "P. Env" toggle
 * @param vel_track "Vel->Pitch" already scaled by the note velocity, in [-1; 1]
 */
inline double
pitch_scale(
    double pitch, double envelope, double amount, bool enabled,
    double vel_track) noexcept
{
  const double semitones = enabled ? amount * envelope * (1. + vel_track) : 0.;
  return std::clamp(
      pitch * std::exp2(semitones / 12.), min_pitch_scale, max_pitch_scale);
}
}
