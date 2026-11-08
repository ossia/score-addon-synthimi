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
 * 8x those reached -63 dB; at 16x they are under -80 dB.
 *
 * A note reads the finest level that puts no harmonic above 0.45 of the
 * rate: as it rises through that level's octave, its top harmonic rises from
 * half of that to that (10.8 to 21.6 kHz at 48 kHz). Below 84 Hz at 48 kHz
 * level 0 is safe, and keeps 256 harmonics: up to 14 kHz at 55 Hz. Over the
 * last sixth of an octave before the limit, the read fades to the next
 * level, so a glide never steps in brightness. Frames are crossfaded in the
 * time domain: the banks are built phase-aligned.
 */

#include <ViceVersa/Dsp/Simd.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace viceversa
{
inline constexpr int kTopHarmonic = 256;
inline constexpr int kLevels = 9; // 256 harmonics down to 1
inline constexpr int kMaxFrames = 256;
//! A level is safe while its top harmonic stays under this many cycles per sample
inline constexpr float kSafeCycles = 0.45f;
//! How much of an octave below that limit a level fades to the next
inline constexpr float kMipFade = 1.f / 6.f;

//! A mip level, and how far to fade to the next
struct Mip
{
  int level{0};
  float frac{0.f};
};

[[nodiscard]] inline constexpr int levelHarmonics(int level) noexcept
{
  return kTopHarmonic >> level;
}
[[nodiscard]] inline constexpr int levelSize(int level) noexcept
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
  constexpr double pi = std::numbers::pi;
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

//! Harmonics 1..kTopHarmonic of one frame: harmonic n, stored as c = c[n - 1],
//! is Re(c e^(2 i pi n t)) = re cos(2 pi n t) - im sin(2 pi n t)
using Spectrum = std::array<std::complex<double>, kTopHarmonic>;

/*! The harmonics of one cycle of `x`, whatever its length: by FFT when it is
 *  a power of two, else by direct DFT of the harmonics a frame can hold
 */
[[nodiscard]] inline Spectrum analyse(std::span<const float> x)
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
    constexpr double pi = std::numbers::pi;
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

  [[nodiscard]] int frames() const noexcept { return count; }
  [[nodiscard]] bool empty() const noexcept { return count == 0 && !isNoise; }

  //! No table: the voice makes white noise, its frame position the colour
  [[nodiscard]] static Bank noiseBank(int frames)
  {
    Bank b;
    b.count = frames;
    b.isNoise = true;
    return b;
  }
  [[nodiscard]] bool noise() const noexcept { return isNoise; }

  //! First guarded sample of a table: x[-1]
  [[nodiscard]] const float* table(int frame, int level) const noexcept
  {
    return data.data() + std::size_t(frame * stride + offset[std::size_t(level)]);
  }

  //! Cubic (Catmull-Rom) read of one table at phase x in [0, 1)
  [[nodiscard]] static float tap(const float* t, int size, float x) noexcept
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

  /*! One sample: frame position in [0, frames - 1], level and fade from
   *  levelFor(), phase in [0, 1). The reference for the lanes' read below.
   */
  [[nodiscard]] float read(float frame, Mip mip, float x) const noexcept
  {
    const int last = std::max(0, count - 1);
    const int f0 = std::clamp(int(frame), 0, last);
    const int f1 = std::min(f0 + 1, last);
    const float ff = std::clamp(frame - float(f0), 0.f, 1.f);
    const int l0 = mip.level, l1 = std::min(mip.level + 1, kLevels - 1);
    const int n0 = levelSize(l0), n1 = levelSize(l1);
    const float a0 = tap(table(f0, l0), n0, x);
    const float a = a0 + ff * (tap(table(f1, l0), n0, x) - a0);
    if(mip.frac <= 0.f)
      return a;
    const float b0 = tap(table(f0, l1), n1, x);
    const float b = b0 + ff * (tap(table(f1, l1), n1, x) - b0);
    return a + mip.frac * (b - a);
  }

  //! How many octaves an increment of dx cycles per sample is above the
  //! finest level's safe limit
  [[nodiscard]] static float octavesOver(float dx) noexcept
  {
    return std::log2(std::max(float(kTopHarmonic) * dx / kSafeCycles, 1e-12f));
  }

  /*! The level for m octaves over the finest level's limit (octavesOver()):
   *  the finest that keeps every harmonic under kSafeCycles, ceil(m), and
   *  how far to fade to the next one, which is safe too: over the last
   *  kMipFade octave below the limit, so that crossing it changes nothing
   */
  [[nodiscard]] static Mip levelFor(float m) noexcept
  {
    const int level = std::clamp(int(std::ceil(m)), 0, kLevels - 1);
    if(level >= kLevels - 1)
      return {level, 0.f};
    return {level, std::clamp((m - float(level)) / kMipFade + 1.f, 0.f, 1.f)};
  }

  /*! Four reads at once, a lane each: one bank, each lane at its own level,
   *  set up per control period by lanes(), and at its own frame position and
   *  phase, per sample
   */
  struct Lanes
  {
    std::array<const float*, kLanes> lo{}, hi{}; // the level, the next; frame 0
    vec4 nLo = vec4(64.f), nHi = vec4(64.f);    // their sizes
    vec4 frac = vec4(0.f);                       // how far each lane fades to hi
    bool fade{false};                            // whether any does
    int stride{0};
    int last{0}; // the last frame
  };

  //! Not for the noise bank, which has no table
  void lanes(Lanes& t, const std::array<Mip, kLanes>& mip) const noexcept
  {
    alignas(16) std::array<float, kLanes> nLo, nHi, frac;
    t.fade = false;
    for(std::size_t l = 0; l < kLanes; l++)
    {
      const int l0 = std::clamp(mip[l].level, 0, kLevels - 1);
      const int l1 = std::min(l0 + 1, kLevels - 1);
      t.lo[l] = table(0, l0);
      t.hi[l] = table(0, l1);
      nLo[l] = float(levelSize(l0));
      nHi[l] = float(levelSize(l1));
      frac[l] = l1 > l0 ? std::clamp(mip[l].frac, 0.f, 1.f) : 0.f;
      t.fade |= frac[l] > 0.f;
    }
    t.nLo = kfr::read<kLanes, true>(nLo.data());
    t.nHi = kfr::read<kLanes, true>(nHi.data());
    t.frac = kfr::read<kLanes, true>(frac.data());
    t.stride = stride;
    t.last = std::max(0, count - 1);
  }

  /*! As read(), a lane each. The cubic is linear in its samples: the two
   *  frames' samples are crossfaded first, then interpolated once per level.
   */
  [[nodiscard]] KFR_ALWAYS_INLINE static vec4 read(const Lanes& t, vec4 frame, vec4 x) noexcept
  {
    const vec4 f = kfr::min(kfr::max(frame, vec4(0.f)), vec4(float(t.last)));
    const vec4i f0 = kfr::cast<int>(f); // not negative: truncation is floor
    const vec4 ff = f - kfr::cast<float>(f0);
    const vec4i f1 = kfr::min(f0 + vec4i(1), vec4i(t.last));
    const vec4i o0 = f0 * vec4i(t.stride), o1 = f1 * vec4i(t.stride);
    const vec4 a = readLevel(t.lo, t.nLo, o0, o1, ff, x);
    if(!t.fade)
      return a;
    const vec4 b = readLevel(t.hi, t.nHi, o0, o1, ff, x);
    return a + t.frac * (b - a);
  }

private:
  [[nodiscard]] KFR_ALWAYS_INLINE static vec4 readLevel(
      const std::array<const float*, kLanes>& base, vec4 n, vec4i o0, vec4i o1, vec4 ff,
      vec4 x) noexcept
  {
    const vec4 pos = x * n;
    const vec4i i = kfr::min(kfr::cast<int>(pos), kfr::cast<int>(n) - vec4i(1));
    const vec4 u = pos - kfr::cast<float>(i);
    alignas(16) std::array<int, kLanes> i0, i1;
    kfr::write<true>(i0.data(), i + o0);
    kfr::write<true>(i1.data(), i + o1);
    // Each lane's four samples, x[i-1] to x[i+2], of both frames
    vec4 a = kfr::read<kLanes>(base[0] + i0[0]), b = kfr::read<kLanes>(base[1] + i0[1]),
         c = kfr::read<kLanes>(base[2] + i0[2]), d = kfr::read<kLanes>(base[3] + i0[3]);
    vec4 a1 = kfr::read<kLanes>(base[0] + i1[0]), b1 = kfr::read<kLanes>(base[1] + i1[1]),
         c1 = kfr::read<kLanes>(base[2] + i1[2]), d1 = kfr::read<kLanes>(base[3] + i1[3]);
    transpose(a, b, c, d);
    transpose(a1, b1, c1, d1);
    a += ff * (a1 - a);
    b += ff * (b1 - b);
    c += ff * (c1 - c);
    d += ff * (d1 - d);
    const vec4 k1 = vec4(0.5f) * (c - a);
    const vec4 k2 = a - vec4(2.5f) * b + vec4(2.f) * c - vec4(0.5f) * d;
    const vec4 k3 = vec4(0.5f) * (d - a) + vec4(1.5f) * (b - c);
    return ((k3 * u + k2) * u + k1) * u + b;
  }

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
