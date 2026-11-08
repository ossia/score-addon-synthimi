#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The built-in wave banks, sixteen frames each. A frame position sweeps from
 * the first frame to the last; every frame is at the same RMS level, so a
 * sweep changes the timbre and not the loudness.
 *
 * Most are computed here. The instrument banks (Brass to Mallets) are the
 * spectra of single cycles taken through recorded notes, as the Prophet VS's
 * instrument waves were: see Dsp/SampledBanks.hpp and tools/extract_waves.py.
 * Noise is no table: white noise, its frame position darkening it.
 */

#include <ViceVersa/Dsp/SampledBanks.hpp>
#include <ViceVersa/Dsp/Wavetable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace viceversa
{
enum class BankId : int
{
  // Basic shapes
  SineSaw,
  Shapes,
  SquareSaw,
  PulseWidth,
  Sync,
  // Spectra
  Partials,
  HarmonicSweep,
  Organ,
  Glass,
  Comb,
  // Distortions
  Fold,
  Digital,
  PhaseDist,
  Fm,
  // Voice
  Vowels,
  // Resynthesised from recordings
  Brass,
  Reeds,
  Strings,
  Voices,
  Keys,
  Plucked,
  Winds,
  Mallets,
  // No table
  Noise
};
inline constexpr int kBankCount = 24;
static_assert(int(BankId::Noise) + 1 == kBankCount);
inline constexpr int kBankFrames = 16;

namespace banks
{
constexpr double pi = std::numbers::pi;
constexpr std::size_t kRender = 8192; // time-domain frames, analysed by FFT

[[nodiscard]] inline double frac(double x) noexcept
{
  return x - std::floor(x);
}

//! A frame drawn in the time domain over t in [0, 1)
template <typename F>
[[nodiscard]] Spectrum drawn(F f)
{
  std::vector<float> x(kRender);
  double mean = 0.;
  for(std::size_t i = 0; i < kRender; i++)
  {
    x[i] = float(f(double(i) / double(kRender)));
    mean += x[i];
  }
  mean /= double(kRender);
  for(auto& v : x)
    v -= float(mean);
  return analyse(x);
}

//! A frame given by its harmonic amplitudes, all in sine phase
template <typename F>
[[nodiscard]] Spectrum additive(F amp)
{
  Spectrum s{};
  for(int n = 1; n <= kTopHarmonic; n++)
    s[std::size_t(n - 1)] = std::complex<double>(0., -amp(n)); // sin(2 pi n t)
  return s;
}

[[nodiscard]] inline double gauss(double x, double mu, double width) noexcept
{
  const double d = (x - mu) / width;
  return std::exp(-0.5 * d * d);
}

//! Formants of five vowels, Hz and relative level, for a voice near 110 Hz
[[nodiscard]] inline double vowel(double hz, double m) noexcept
{
  static constexpr double f[5][3] = {
      {800., 1150., 2900.}, // A
      {400., 1600., 2700.}, // E
      {270., 2100., 3000.}, // I
      {450., 800., 2830.},  // O
      {325., 700., 2530.}}; // U
  static constexpr double g[3] = {1., 0.5, 0.25};
  const double pos = m * 4.;
  const int a = std::min(3, int(pos));
  const double w = pos - a;
  double v = 0.;
  for(int k = 0; k < 3; k++)
  {
    const double fk = f[a][k] + w * (f[a + 1][k] - f[a][k]);
    v += g[k] * gauss(hz, fk, 60. + 0.08 * fk);
  }
  return v;
}

//! The recorded banks, from Brass on, in BankId's order
inline constexpr std::array<const sampled::Bank*, 8> kSampled{
    &sampled::kBrass, &sampled::kReeds, &sampled::kStrings, &sampled::kVoices,
    &sampled::kKeys,  &sampled::kPlucked, &sampled::kWinds, &sampled::kMallets};
static_assert(int(BankId::Mallets) - int(BankId::Brass) + 1 == int(kSampled.size()));

[[nodiscard]] inline Spectrum frame(BankId id, double m)
{
  switch(id)
  {
    default:
    case BankId::SineSaw:
      // The fundamental first, then the upper harmonics fade in
      return additive([m](int n) {
        return n == 1 ? 1. : std::pow(m, 0.5 + 0.12 * n) / n;
      });
    case BankId::PulseWidth: {
      const double w = 0.5 - 0.46 * m;
      return additive([w](int n) { return 4. / (pi * n) * std::sin(pi * n * w); });
    }
    case BankId::Sync: {
      // A saw hard-synced to the cycle, at 1 to 8 times its rate
      const double r = 1. + 7. * m;
      return drawn([r](double t) { return 2. * frac(r * t) - 1.; });
    }
    case BankId::Vowels:
      return additive([m](int n) { return n * 110. > 5000. ? 0. : vowel(n * 110., m) / std::sqrt(n); });
    case BankId::Organ: {
      // Drawbar registrations, from a flute to a full organ
      static constexpr double regs[4][9] = {
          {0, 1, 0, 0.6, 0, 0, 0, 0, 0},
          {0.5, 1, 0.7, 0.6, 0, 0.4, 0, 0, 0},
          {0.6, 1, 0.8, 0.8, 0.5, 0.6, 0.4, 0.3, 0.2},
          {0.8, 1, 1, 1, 0.8, 0.8, 0.7, 0.7, 0.7}};
      // Footages 16' 8' 5 1/3' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1': harmonics of 16'
      static constexpr int harm[9] = {1, 2, 3, 4, 6, 8, 10, 12, 16};
      const double pos = m * 3.;
      const int a = std::min(2, int(pos));
      const double w = pos - a;
      return additive([=](int n) {
        for(int k = 0; k < 9; k++)
          if(harm[k] == n)
            return regs[a][k] + w * (regs[a + 1][k] - regs[a][k]);
        return 0.;
      });
    }
    case BankId::Glass:
      // Odd partials, the spectral peak climbing
      return additive([m](int n) {
        if(n % 2 == 0)
          return 0.;
        return gauss(std::log2(double(n)), 0.5 + 4.5 * m, 0.9) / std::sqrt(double(n));
      });
    case BankId::Digital: {
      // A sine with fewer and fewer levels
      const double levels = std::exp2(6. - 5. * m);
      return drawn([levels](double t) {
        return std::round(std::sin(2. * pi * t) * levels) / levels;
      });
    }
    case BankId::Fold: {
      const double g = 0.6 + 5. * m;
      return drawn([g](double t) { return std::sin(pi * g * std::sin(2. * pi * t)); });
    }
    case BankId::PhaseDist: {
      // The CZ saw: a cosine read through a bent phase
      const double p = 0.5 - 0.49 * m;
      return drawn([p](double t) {
        const double ph = t < p ? 0.5 * t / p : 0.5 + 0.5 * (t - p) / (1. - p);
        return -std::cos(2. * pi * ph);
      });
    }
    case BankId::Fm: {
      // Two operators at 1:1, the index rising to 6
      const double index = 6. * m;
      return drawn([index](double t) {
        return std::sin(2. * pi * t + index * std::sin(2. * pi * t));
      });
    }
    case BankId::HarmonicSweep:
      // One harmonic at a time, from the 1st to the 24th
      return additive([m](int n) { return gauss(double(n), 1. + 23. * m, 0.6); });
    case BankId::Comb:
      // A saw through a comb whose notches spread
      return additive([m](int n) {
        return std::abs(std::cos(pi * n * (0.02 + 0.23 * m))) / n;
      });
    case BankId::Shapes: {
      // Triangle, clipped harder and harder into a square; then the
      // triangle's peak moving late, into a rising saw
      if(m <= 0.5)
      {
        const double gain = 1. + 30. * std::pow(m * 2., 2.);
        return drawn([gain](double t) {
          const double tri = 1. - 4. * std::abs(t - 0.5);
          return std::clamp(gain * tri, -1., 1.);
        });
      }
      const double peak = 0.5 + 0.495 * (m - 0.5) * 2.;
      return drawn([peak](double t) {
        return t < peak ? -1. + 2. * t / peak : 1. - 2. * (t - peak) / (1. - peak);
      });
    }
    case BankId::Partials: {
      // Fixed sets of harmonics, one per frame, equal in level
      static constexpr std::array<std::array<int, 8>, kBankFrames> sets{{
          {1},
          {1, 2},
          {1, 3},
          {1, 2, 3},
          {1, 3, 5},
          {1, 3, 5, 7, 9},
          {1, 2, 4, 8},
          {1, 5},
          {2, 3},
          {1, 8},
          {3, 5, 7},
          {2, 4, 6, 8},
          {1, 4, 7, 10},
          {1, 9, 10},
          {1, 2, 3, 4, 5, 6, 7, 8},
          {1, 6, 11, 16},
      }};
      const auto& set = sets[std::size_t(std::lround(m * (kBankFrames - 1)))];
      return additive([&set](int n) {
        for(int k : set)
          if(k == n)
            return 1.;
        return 0.;
      });
    }
    case BankId::Brass:
    case BankId::Reeds:
    case BankId::Strings:
    case BankId::Voices:
    case BankId::Keys:
    case BankId::Plucked:
    case BankId::Winds:
    case BankId::Mallets: {
      const sampled::Bank& b = *kSampled[std::size_t(int(id) - int(BankId::Brass))];
      const auto f = std::size_t(std::lround(m * (sampled::kFrames - 1)));
      Spectrum s{};
      for(int n = 0; n < sampled::kHarmonics; n++)
        s[std::size_t(n)] = std::polar(
            double(b.magnitude[f][std::size_t(n)]), double(b.phase[std::size_t(n)]));
      return s;
    }
    case BankId::Noise:
      return Spectrum{};
    case BankId::SquareSaw:
      return additive([m](int n) {
        const double square = n % 2 ? 1. / n : 0.;
        return square + m * ((n % 2 ? 0. : 1. / n));
      });
  }
}

[[nodiscard]] inline Bank build(BankId id)
{
  if(id == BankId::Noise)
    return Bank::noiseBank(kBankFrames);
  std::vector<Spectrum> frames;
  frames.reserve(kBankFrames);
  for(int f = 0; f < kBankFrames; f++)
    frames.push_back(frame(id, double(f) / double(kBankFrames - 1)));
  // All at the RMS of a sine of amplitude 0.7
  return Bank(frames, 0.5);
}
}

//! Built once per process, on first use (in prepare(), not on the audio thread)
inline const std::array<Bank, kBankCount>& builtinBanks()
{
  static const std::array<Bank, kBankCount> banks = [] {
    std::array<Bank, kBankCount> b;
    for(int i = 0; i < kBankCount; i++)
      b[std::size_t(i)] = banks::build(BankId(i));
    return b;
  }();
  return banks;
}
}
