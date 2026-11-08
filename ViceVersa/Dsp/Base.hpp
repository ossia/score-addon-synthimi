#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* What all of ViceVersa uses: pi, and the checks that keep what a host or a
 * preset sends in range.
 */

#include <Dsp/Numeric.hpp>

#include <numbers>

namespace viceversa
{
inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.f * kPi;

using synthimi::finite;
using synthimi::sane;
using synthimi::saneEnum;
}
