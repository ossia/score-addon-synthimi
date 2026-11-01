#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: the partials of a struck string.

#include <Cordito/Dsp/Common.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Modal string
// ---------------------------------------------------------------------------

struct ModalString
{
  // Two courses (unison strings) of up to kMaxModes partials each
  static constexpr int kSlots = 2 * kMaxModes;
  alignas(64) std::array<double, kSlots> cr{}, ci{}, zr{}, zi{}, ex{}, wl{}, wr{};
  //! Per-sample decay of each partial, kept while the pitch moves
  std::array<double, kSlots> rad{};
  int count = 0;

  void clear() noexcept
  {
    zr.fill(0.);
    zi.fill(0.);
  }

  //! A complex one-pole per partial, z = r e^{jw} z[-1] + x: well
  //! conditioned at the high Qs of a string, unlike a direct-form biquad.
  void tick(double in, double& l, double& r) noexcept
  {
    double ol = 0., or_ = 0.;
    // Without fast-math the compiler keeps the sums in order and does not
    // vectorise them: Struck took 1.7x longer in developer builds
#pragma omp simd reduction(+ : ol, or_)
    for(int i = 0; i < count; i++)
    {
      const std::size_t k = std::size_t(i);
      const double re = cr[k] * zr[k] - ci[k] * zi[k] + in * ex[k];
      const double im = cr[k] * zi[k] + ci[k] * zr[k];
      zr[k] = re;
      zi[k] = im;
      ol += im * wl[k];
      or_ += im * wr[k];
    }
    l = ol;
    r = or_;
  }

  void flushDenormals() noexcept
  {
    for(int i = 0; i < count; i++)
    {
      const std::size_t k = std::size_t(i);
      if(std::abs(zr[k]) + std::abs(zi[k]) < 1e-20)
        zr[k] = zi[k] = 0.;
    }
  }
};
}
