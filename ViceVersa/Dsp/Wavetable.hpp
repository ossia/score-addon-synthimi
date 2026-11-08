#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Band-limited wave banks: a bank is a set of single-cycle frames, each kept
 * as a mip-map of tables, one per octave.
 *
 * Built off the audio thread from either a spectrum (harmonic amplitudes and
 * phases) or a cycle of samples of any length, which is analysed first. Level
 * L keeps the harmonics up to kTopHarmonic >> L and is sixteen times
 * oversampled, at least 64 samples. A table is read with a stride of up to
 * 16 * 0.45 samples: what aliases is the cubic interpolation's images, and at
 * 8x those reached -63 dB; at 16x they are under -80 dB. 256 harmonics keep
 * a 55 Hz note's spectrum to 14 kHz; lower notes lose what is above 256 f0. A note picks the coarsest level that
 * puts no harmonic above 0.45 of the rate, and crossfades with the next one
 * as its pitch moves, so a glide never steps in brightness. Frames are
 * crossfaded in the time domain: the banks are built phase-aligned.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace viceversa
{
inline constexpr int kTopHarmonic = 256;
inline constexpr int kLevels = 9; // 256 harmonics down to 1
inline constexpr int kMaxFrames = 256;
//! A level is safe while its top harmonic stays under this many cycles per sample
inline constexpr float kSafeCycles = 0.45f;

inline constexpr int levelHarmonics(int level) noexcept
{
  return kTopHarmonic >> level;
}
inline constexpr int levelSize(int level) noexcept
{
  return std::max(64, 16 * levelHarmonics(level));
}

//! In-place radix-2 FFT, `inverse` without the 1/n
inline void fft(std::vector<std::complex<double>>& a, bool inverse)
{
  const std::size_t n = a.size();
  for(std::size_t i = 1, j = 0; i < n; i++)
  {
    std::size_t bit = n >> 1;
    for(; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if(i < j)
      std::swap(a[i], a[j]);
  }
  constexpr double pi = 3.14159265358979323846;
  for(std::size_t len = 2; len <= n; len <<= 1)
  {
    const double ang = (inverse ? 2. : -2.) * pi / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for(std::size_t i = 0; i < n; i += len)
    {
      std::complex<double> w(1.);
      for(std::size_t j = 0; j < len / 2; j++)
      {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

//! Harmonics 1..kTopHarmonic of one frame: harmonic n is
//! re * cos(2 pi n t) + im * sin(2 pi n t)... stored as c[n - 1]
using Spectrum = std::array<std::complex<double>, kTopHarmonic>;

/*! The harmonics of one cycle of `x`, whatever its length: by FFT when it is
 *  a power of two, else by direct DFT of the harmonics a frame can hold
 */
inline Spectrum analyse(std::span<const float> x)
{
  Spectrum s{};
  const std::size_t n = x.size();
  if(n < 2)
    return s;
  const std::size_t top = std::min<std::size_t>(kTopHarmonic, n / 2 - (n % 2 == 0 ? 1 : 0));
  if((n & (n - 1)) == 0)
  {
    std::vector<std::complex<double>> a(n);
    for(std::size_t i = 0; i < n; i++)
      a[i] = double(x[i]);
    fft(a, false);
    for(std::size_t k = 1; k <= top; k++)
      s[k - 1] = a[k] * (2. / double(n));
  }
  else
  {
    constexpr double pi = 3.14159265358979323846;
    for(std::size_t k = 1; k <= top; k++)
    {
      std::complex<double> acc{};
      const std::complex<double> step = std::polar(1., -2. * pi * double(k) / double(n));
      std::complex<double> w(1.);
      for(std::size_t i = 0; i < n; i++)
      {
        acc += double(x[i]) * w;
        w *= step;
        if((i & 255) == 255) // keep the rotation on the unit circle
          w = std::polar(1., -2. * pi * double(k) * double(i + 1) / double(n));
      }
      s[k - 1] = acc * (2. / double(n));
    }
  }
  return s;
}

//! The frames of a bank, each a mip-map of guarded tables
class Bank
{
public:
  //! Per table: [x[N-1], x[0..N-1], x[0], x[1]], for a cubic read
  static constexpr int kGuard = 3;

  Bank() = default;

  explicit Bank(std::span<const Spectrum> frames, double rmsTarget = 0.)
  {
    build(frames, rmsTarget);
  }

  void build(std::span<const Spectrum> frames, double rmsTarget = 0.)
  {
    int off = 0;
    for(int l = 0; l < kLevels; l++)
    {
      offset[std::size_t(l)] = off;
      off += levelSize(l) + kGuard;
    }
    stride = off;
    count = int(std::min<std::size_t>(frames.size(), kMaxFrames));
    data.assign(std::size_t(std::max(1, count) * stride), 0.f);

    for(int f = 0; f < count; f++)
    {
      Spectrum s = frames[std::size_t(f)];
      if(rmsTarget > 0.)
      {
        // Parseval: the frame's RMS from its harmonics
        double e = 0.;
        for(auto& c : s)
          e += std::norm(c);
        const double rms = std::sqrt(0.5 * e);
        if(rms > 1e-9)
          for(auto& c : s)
            c *= rmsTarget / rms;
      }
      for(int l = 0; l < kLevels; l++)
        synthesise(s, l, table(f, l));
    }
  }

  int frames() const noexcept { return count; }
  bool empty() const noexcept { return count == 0 && !isNoise; }

  //! No table: the voice makes white noise, its frame position the colour
  static Bank noiseBank(int frames)
  {
    Bank b;
    b.count = frames;
    b.isNoise = true;
    return b;
  }
  bool noise() const noexcept { return isNoise; }

  //! First guarded sample of a table: x[-1]
  const float* table(int frame, int level) const noexcept
  {
    return data.data() + std::size_t(frame * stride + offset[std::size_t(level)]);
  }

  //! Cubic (Catmull-Rom) read of one table at phase x in [0, 1)
  static float tap(const float* t, int size, float x) noexcept
  {
    const float pos = x * float(size);
    int i = int(pos);
    i = std::clamp(i, 0, size - 1);
    const float u = pos - float(i);
    const float* p = t + i; // p[0] = x[i-1]
    const float a = p[0], b = p[1], c = p[2], d = p[3];
    const float c1 = 0.5f * (c - a);
    const float c2 = a - 2.5f * b + 2.f * c - 0.5f * d;
    const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * u + c2) * u + c1) * u + b;
  }

  /*! One sample: frame position in [0, frames - 1], level from levelFor(),
   *  phase in [0, 1)
   */
  float read(float frame, int level, float levelFrac, float x) const noexcept
  {
    const int f0 = std::clamp(int(frame), 0, count - 1);
    const int f1 = std::min(f0 + 1, count - 1);
    const float ff = std::clamp(frame - float(f0), 0.f, 1.f);
    const int l0 = level, l1 = std::min(level + 1, kLevels - 1);
    const int n0 = levelSize(l0), n1 = levelSize(l1);
    const float a = tap(table(f0, l0), n0, x) + ff * (tap(table(f1, l0), n0, x) - tap(table(f0, l0), n0, x));
    if(levelFrac <= 0.f)
      return a;
    const float b = tap(table(f0, l1), n1, x) + ff * (tap(table(f1, l1), n1, x) - tap(table(f0, l1), n1, x));
    return a + levelFrac * (b - a);
  }

  /*! The level for an increment of dx cycles per sample: the coarsest that
   *  keeps every harmonic under kSafeCycles, and how far to fade to the next
   */
  static void levelFor(float dx, int& level, float& frac) noexcept
  {
    // m: how many octaves above the finest level's safe limit. The level
    // needed is ceil(m); over the octave below that limit it already fades
    // to the next, so that crossing it changes nothing
    const float m = std::log2(std::max(float(kTopHarmonic) * dx / kSafeCycles, 1e-12f));
    const int need = std::max(0, int(std::ceil(m)));
    level = std::min(need, kLevels - 1);
    frac = level >= kLevels - 1 ? 0.f : std::clamp(m - float(level - 1), 0.f, 1.f);
  }

private:
  float* table(int frame, int level) noexcept
  {
    return data.data() + std::size_t(frame * stride + offset[std::size_t(level)]);
  }

  static void synthesise(const Spectrum& s, int level, float* out)
  {
    const int n = levelSize(level);
    const int h = std::min(levelHarmonics(level), n / 2 - 1);
    std::vector<std::complex<double>> a(std::size_t(n), 0.);
    for(int k = 1; k <= h; k++)
    {
      a[std::size_t(k)] = 0.5 * s[std::size_t(k - 1)];
      a[std::size_t(n - k)] = std::conj(a[std::size_t(k)]);
    }
    fft(a, true);
    float* t = out + 1;
    for(int i = 0; i < n; i++)
      t[i] = float(a[std::size_t(i)].real());
    out[0] = t[n - 1];
    t[n] = t[0];
    t[n + 1] = t[1];
  }

  std::vector<float> data;
  std::array<int, kLevels> offset{};
  int stride{0};
  int count{0};
  bool isNoise{false};
};
}
