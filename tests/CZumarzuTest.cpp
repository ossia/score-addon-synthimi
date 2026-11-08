// CZumarzu, the phase distortion synthesizer, at the DSP level: its waveforms
// against the uPD933 (MAME's model of the CZ's chip), the exactness of its
// antialiasing, how much aliasing is left, the half-band filter, the
// envelopes, and that nothing a host or a preset sends makes it explode,
// allocate on the audio thread or keep a voice alive forever.
#include <CZumarzu/CZumarzu.hpp>
#include <ref/upd933/upd933_ref.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <random>
#include <vector>

#include <ossia/math/safe_math.hpp>

#include "AllocationCounter.hpp"

using namespace czumarzu;

namespace
{
// czumarzu::kPi is a float
constexpr double kPiD = std::numbers::pi;

float lane0(vec4 v)
{
  return v[0];
}

// ---- spectra ------------------------------------------------------------
void fft(std::vector<std::complex<double>>& a)
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
  for(std::size_t len = 2; len <= n; len <<= 1)
  {
    const double ang = -2. * kPiD / double(len);
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

//! Power spectrum of the first 2^k samples, Blackman-Harris windowed
std::vector<double> power(const std::vector<float>& x, std::size_t n)
{
  std::vector<std::complex<double>> a(n);
  for(std::size_t i = 0; i < n; i++)
  {
    const double t = 2. * kPiD * double(i) / double(n);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2. * t)
                     - 0.01168 * std::cos(3. * t);
    a[i] = w * double(x[i]);
  }
  fft(a);
  std::vector<double> p(n / 2);
  for(std::size_t i = 0; i < n / 2; i++)
    p[i] = std::norm(a[i]);
  return p;
}

/*! Energy away from the harmonics of f0 over energy on them, in dB, below
 *  `limit` Hz. Blackman-Harris leaks over +-4 bins.
 */
double aliasToSignal(const std::vector<float>& x, double f0, double fs, double limit)
{
  const std::size_t n = 1 << 15;
  const auto p = power(x, n);
  const double bin = fs / double(n);
  double harm = 0., other = 0.;
  for(std::size_t i = 1; i < p.size(); i++)
  {
    const double f = double(i) * bin;
    if(f > limit)
      break;
    const double k = std::round(f / f0);
    const bool onHarmonic = k >= 1. && std::abs(f - k * f0) <= 5. * bin;
    (onHarmonic ? harm : other) += p[i];
  }
  return 10. * std::log10(std::max(other, 1e-30) / std::max(harm, 1e-30));
}

//! Magnitude of x at frequency f, Hann windowed, in dB
double magnitudeAt(const std::vector<float>& x, std::size_t from, std::size_t n, double f, double fs)
{
  std::complex<double> acc{};
  double wsum = 0.;
  for(std::size_t i = 0; i < n; i++)
  {
    const double w = 0.5 - 0.5 * std::cos(2. * kPiD * double(i) / double(n));
    acc += w * double(x[from + i]) * std::polar(1., -2. * kPiD * f * double(i) / fs);
    wsum += w;
  }
  return 20. * std::log10(std::max(2. * std::abs(acc) / wsum, 1e-12));
}

//! Fundamental by autocorrelation, refined by a parabola
double estimateF0(const std::vector<float>& x, double fs, double lo, double hi)
{
  const std::size_t n = std::min<std::size_t>(x.size(), 32768);
  const int minLag = int(fs / hi), maxLag = int(fs / lo);
  std::vector<double> r(std::size_t(maxLag + 2), 0.);
  for(int lag = minLag - 1; lag <= maxLag + 1; lag++)
  {
    double s = 0.;
    for(std::size_t i = 0; i + std::size_t(lag) < n; i++)
      s += double(x[i]) * double(x[i + std::size_t(lag)]);
    // Unbiased: longer lags have fewer terms
    r[std::size_t(lag)] = s / double(n - std::size_t(lag));
  }
  int best = minLag;
  for(int lag = minLag; lag <= maxLag; lag++)
    if(r[std::size_t(lag)] > r[std::size_t(best)])
      best = lag;
  const double a = r[std::size_t(best - 1)], b = r[std::size_t(best)], c = r[std::size_t(best + 1)];
  const double d = (a - c) / (2. * (a - 2. * b + c));
  return fs / (double(best) + d);
}

/*! Frequency near `f` from the phase advance of its component between two
 *  windows `gap` samples apart: precise to a hundredth of a cent
 */
double measureFrequency(const std::vector<float>& x, double f, double fs)
{
  const std::size_t n = std::size_t(std::round(8. * fs / f));
  const std::size_t gap = x.size() - n - 1;
  auto phaseAt = [&](std::size_t from) {
    std::complex<double> acc{};
    for(std::size_t i = 0; i < n; i++)
    {
      const double w = 0.5 - 0.5 * std::cos(2. * kPiD * double(i) / double(n));
      acc += w * double(x[from + i]) * std::polar(1., -2. * kPiD * f * double(from + i) / fs);
    }
    return std::arg(acc);
  };
  // Refined twice: the first pass's error is far below one turn over the gap
  double d = phaseAt(gap) - phaseAt(0);
  d -= 2. * kPiD * std::round(d / (2. * kPiD));
  return f + d * fs / (2. * kPiD * double(gap));
}

// ---- the synth ----------------------------------------------------------
//! One line, one waveform, a fixed DCW and full level: a steady tone
Params steady(Wave w, float dcw, SecondWave w2 = SecondWave::Off)
{
  Params p;
  p.mode = LineMode::Line1;
  p.modulation = Modulation::Off;
  auto& l = p.line[0];
  l.wave1 = w;
  l.wave2 = w2;
  l.dca = EnvSpec::make({{1.f, 1.f}}, 0, 1);
  l.dcw = EnvSpec::make({{1.f, dcw}}, 0, 1);
  l.dco = EnvSpec::make({{1.f, 0.f}}, -1, 0);
  p.velLevel = 0.f;
  p.velDcw = 0.f;
  p.pressureDcw = 0.f;
  p.spread = 0.f;
  p.level = 1.f;
  p.sanitize();
  return p;
}

struct Rig
{
  std::unique_ptr<Synth> s = std::make_unique<Synth>();
  double fs;
  explicit Rig(double rate = 48000.)
      : fs(rate)
  {
    s->prepare(rate);
  }
  void send(unsigned char a, unsigned char b, unsigned char c, const Params& p)
  {
    const unsigned char m[3]{a, b, c};
    s->midi(m, 3, p);
  }
  void on(int note, int vel, const Params& p, int ch = 0)
  {
    send((unsigned char)(0x90 | ch), (unsigned char)note, (unsigned char)vel, p);
  }
  void off(int note, const Params& p, int ch = 0)
  {
    send((unsigned char)(0x80 | ch), (unsigned char)note, 0, p);
  }
  std::vector<float> run(double seconds, const Params& p, std::vector<float>* right = nullptr)
  {
    const int n = std::max(1, int(seconds * fs));
    std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
    // In host-sized blocks
    for(int i = 0; i < n; i += 256)
      s->render(p, l.data() + i, r.data() + i, std::min(256, n - i));
    if(right)
      *right = r;
    return l;
  }
};

std::vector<float> tail(const std::vector<float>& x, std::size_t from)
{
  return {x.begin() + std::ptrdiff_t(from), x.end()};
}

bool allFinite(const std::vector<float>& x)
{
  return std::all_of(x.begin(), x.end(), [](float v) { return ossia::safe_isfinite(v); });
}

// ---- the chip -----------------------------------------------------------
struct ChipWave
{
  Wave wave;
  int code;
  int window;
};
constexpr ChipWave kChipWaves[] = {
    {Wave::Saw, 0, 0},           {Wave::Square, 1, 0},        {Wave::Pulse, 2, 0},
    {Wave::DoubleSine, 4, 0},    {Wave::SawPulse, 5, 0},      {Wave::ResoSaw, 6, 1},
    {Wave::ResoTriangle, 6, 2},  {Wave::ResoTrapezoid, 6, 3}, {Wave::DoublePulse, 7, 0},
    {Wave::ResoPulse, 6, 4},     {Wave::ResoDoubleSaw, 6, 5},
};

//! Voice 1 of the chip playing waveform `w` (and `w2` on odd cycles) at full
//! level, its DCW held at `dcw` (0..1016)
void chipVoice(upd933_ref::upd933& chip, const ChipWave& w, const ChipWave* w2, int dcw,
               std::uint16_t pitch)
{
  const int v = 1;
  std::uint16_t reg = std::uint16_t(w.code << 13 | w.window << 6);
  if(w2)
    reg |= std::uint16_t(w2->code << 10 | 1 << 9);
  chip.write(std::uint8_t(0x68 | v), reg);
  chip.write(std::uint8_t(0x60 | v), pitch);
  chip.write(std::uint8_t(0x98 | v), 0);
  chip.m_dca[v].m_current = chip.m_dca[v].m_target = 127u << 18;
  chip.m_dcw[v].m_current = chip.m_dcw[v].m_target = std::uint32_t(dcw) << 16;
}
}

// =========================================================================
TEST_CASE("Nothing allocates after prepare, from the first note", "[czumarzu][robustness]")
{
  // First in the file, and on its own: nothing earlier has played. The
  // jumps' band-limited steps are the likeliest to be built late.
  auto synth = std::make_unique<Synth>();
  std::vector<float> l(256), r(256);
  auto p = steady(Wave::DoublePulse, 0.8f);
  p.line[1] = p.line[0];
  p.line[1].wave1 = Wave::ResoDoubleSaw;
  p.quality = Quality::Clean;
  p.sanitize();

  g_allocs = 0;
  synth->prepare(48000.);
  g_watch = true;
  const unsigned char on[3]{0x90, 60, 100}, off[3]{0x80, 60, 0};
  synth->midi(on, 3, p);
  for(int b = 0; b < 50; b++)
    synth->render(p, l.data(), r.data(), 256);
  // Through every quality, the line modes and modulations, the same rate
  // prepared again
  for(int q = 0; q < kQualityCount; q++)
    for(int m = 0; m < kModulationCount; m++)
    {
      p.quality = Quality(q);
      p.mode = LineMode::Line1And2;
      p.modulation = Modulation(m);
      for(int b = 0; b < 10; b++)
        synth->render(p, l.data(), r.data(), 256);
    }
  synth->prepare(48000.);
  synth->midi(off, 3, p);
  for(int b = 0; b < 20; b++)
    synth->render(p, l.data(), r.data(), 256);
  g_watch = false;
  CHECK(g_allocs == 0);
}

TEST_CASE("Every waveform is the chip's transfer function", "[czumarzu][chip]")
{
  upd933_ref::upd933 chip;
  for(const auto& w : kChipWaves)
  {
    for(int dcw : {0, 128, 512, 900, 1016})
    {
      chipVoice(chip, w, nullptr, dcw, 0);
      chip.m_voice[1].m_dcw_limit = 0x400;
      chip.m_voice[1].m_pitch_step = 0;

      const float pivot = 0.5f * (1.f - float(dcw) / 1024.f);
      const float k = 1.f + float(dcw) / 64.f;
      const Shape s = shapeOf(w.wave, vec4(pivot), vec4(k), vec4(1.f));

      double worst = 0.;
      for(int pos = 0; pos < 2048; pos++)
      {
        chip.m_voice[1].m_position = std::uint32_t(pos) << 20;
        // Normalised by the chip's level at DCA 127: its volume step 508
        const double ref = double(chip.update(1)) / (0.5 * std::pow(8192., 508. / 511.));
        const float x = float(pos) / 2048.f;
        // Away from the jumps, where one step of the chip's phase is a leap
        const float near = 3.f / 2048.f;
        bool skip = false;
        for(float b : {lane0(s.br[0]), lane0(s.br[1]), lane0(s.br[2]), 1.f, 0.f})
          skip |= std::abs(x - b) < near;
        if(skip)
          continue;
        worst = std::max(worst, std::abs(double(lane0(evaluate(s, vec4(x)))) - ref));
      }
      INFO("wave " << int(w.wave) << " dcw " << dcw);
      // The chip's 11-bit phase and 12-bit cosine, and its truncating divisions
      CHECK(worst < 0.02);
    }
  }
}

TEST_CASE("The polynomial sine and cosine are within 2.5e-7", "[czumarzu][adaa]")
{
  // Over a few turns, both signs. The polynomials are within 5.3e-8; the
  // rest is single precision's rounding, at -132 dB
  double worstS = 0., worstC = 0.;
  for(int i = -400000; i < 400000; i++)
  {
    const float t = float(i) / 100000.f + 1e-6f * float(i % 7);
    vec4 s, c;
    sincos2pi(vec4(t), s, c);
    worstS = std::max(worstS, std::abs(double(lane0(s)) - std::sin(2. * kPiD * double(t))));
    worstC = std::max(worstC, std::abs(double(lane0(c)) - std::cos(2. * kPiD * double(t))));
  }
  std::printf("sincos2pi: sin within %.2g, cos within %.2g\n", worstS, worstC);
  CHECK(worstS < 2.5e-7);
  CHECK(worstC < 2.5e-7);
}

TEST_CASE("The closed-form integral is exact", "[czumarzu][adaa]")
{
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> u01(0.f, 1.f);
  for(int w = 0; w < kWaveCount; w++)
  {
    for(int trial = 0; trial < 40; trial++)
    {
      const float pivot = std::max(kMinPivot, 0.5f * u01(rng));
      const float k = 1.f + 15.f * u01(rng);
      const float v = 0.5f + 3.f * u01(rng);
      const Shape s = shapeOf(Wave(w), vec4(pivot), vec4(k), vec4(v));

      // A stretch inside one segment
      const float x0 = 0.98f * u01(rng);
      const float lim = lane0(nextBreak(s, vec4(x0)));
      const float L = std::min(lim - x0, 0.02f) * u01(rng);
      const float m = x0 + 0.5f * L;
      const double got = lane0(integral(s, vec4(m), vec4(L)));

      // Simpson, in double, on the pointwise waveform
      const int n = 2000;
      double ref = 0.;
      for(int i = 0; i <= n; i++)
      {
        const float x = x0 + L * float(i) / n;
        const double y = lane0(evaluate(s, vec4(std::min(x, std::nextafter(lim, 0.f)))));
        ref += y * (i == 0 || i == n ? 1. : (i % 2 ? 4. : 2.));
      }
      ref *= double(L) / (3. * n);
      INFO("wave " << w << " pivot " << pivot << " k " << k << " L " << L);
      CHECK(std::abs(got - ref) <= 2e-6 * std::max(double(L), 1e-3) + 1e-8);
    }
  }
}

TEST_CASE("One sample's mean crosses breakpoints and wraps exactly", "[czumarzu][adaa]")
{
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u01(0.f, 1.f);
  for(int w = 0; w < kWaveCount; w++)
  {
    for(int w2 : {w, (w + 3) % kWaveCount})
    {
      for(int trial = 0; trial < 60; trial++)
      {
        const float dx = 0.002f + 0.06f * u01(rng);
        const float pivot = std::min(0.5f, std::max(4.f * dx, 0.5f * u01(rng)));
        const float k = std::min(1.f + 15.f * u01(rng), 0.45f / dx);
        const float v = 0.5f + 3.f * u01(rng);
        const float x0 = u01(rng) * 0.999f;
        const Shape a = shapeOf(Wave(w), vec4(pivot), vec4(k), vec4(v));
        const Shape b = shapeOf(Wave(w2), vec4(pivot), vec4(k), vec4(v));
        const double got = lane0(sampleMean(a, b, w == w2, vec4(x0), vec4(dx)));

        const int n = 20000;
        double ref = 0.;
        for(int i = 0; i < n; i++)
        {
          float x = x0 + dx * (float(i) + 0.5f) / n;
          const bool next = x >= 1.f;
          if(next)
            x -= 1.f;
          ref += lane0(evaluate(next ? b : a, vec4(x)));
        }
        ref /= n;
        INFO("wave " << w << "/" << w2 << " x0 " << x0 << " dx " << dx << " pivot " << pivot);
        // The midpoint rule's own error, at the jumps of the double pulse
        CHECK(std::abs(got - ref) < 5e-4);
      }
    }
  }
}

TEST_CASE("The half-band filter passes the audio band and stops its image", "[czumarzu][halfband]")
{
  Decimator2x d;
  d.prepare();

  // Sines at the oversampled rate: the decimated level is the response
  auto gainAt = [&](double f /* of the oversampled rate */) {
    d.reset();
    const int n = 8192;
    std::vector<float> out;
    for(int i = 0; i < n; i++)
    {
      const float a = float(std::sin(2. * kPiD * f * double(2 * i)));
      const float b = float(std::sin(2. * kPiD * f * double(2 * i + 1)));
      out.push_back(d.process(a, b));
    }
    // RMS: the decimated sine's samples miss its peaks
    double e = 0.;
    for(int i = n / 2; i < n; i++)
      e += double(out[std::size_t(i)]) * double(out[std::size_t(i)]);
    return 10. * std::log10(std::max(2. * e / double(n / 2), 1e-30));
  };

  double ripple = 0.;
  for(double f = 0.005; f <= 0.23; f += 0.005)
    ripple = std::max(ripple, std::abs(gainAt(f)));
  double stop = -300.;
  for(double f = 0.27; f < 0.5; f += 0.0037)
    stop = std::max(stop, gainAt(f));
  std::printf("half-band: passband ripple %.4f dB, stopband %.1f dB\n", ripple, stop);
  CHECK(ripple < 0.01);
  CHECK(stop < -100.);
}

TEST_CASE("Envelopes step, hold, release and end", "[czumarzu][envelope]")
{
  const double fs = 48000.;
  // Up to 1, down to 0.5 and hold, then down to 0 on release
  const EnvSpec spec = EnvSpec::make({{1.f, 1.f}, {0.6f, 0.5f}, {0.6f, 0.f}}, 1, 2);
  EnvState e;
  e.start(spec);
  for(int i = 0; i < 1000; i++)
    e.advance(spec, 1. / fs, kCtrl, true);
  CHECK(e.step == 1);
  CHECK(e.value == 0.5f);
  e.release(spec);
  CHECK(e.step == 2);

  // A full-scale move at rate r takes stepTime(r)
  int ticks = 0;
  while(!e.silent() && ticks < 1000000)
  {
    e.advance(spec, 1. / fs, kCtrl, false);
    ticks++;
  }
  const double took = ticks * kCtrl / fs;
  CHECK(std::abs(took - 0.5 * stepTime(0.6)) < 2. * kCtrl / fs);

  // No sustain point: runs to its end with the key held, holds there
  const EnvSpec free = EnvSpec::make({{1.f, 1.f}, {1.f, 0.3f}}, -1, 1);
  EnvState f;
  f.start(free);
  for(int i = 0; i < 100; i++)
    f.advance(free, 1. / fs, kCtrl, true);
  CHECK(f.done);
  CHECK(f.value == 0.3f);
  // Released at its end, with a level: falls to 0, never stays on
  f.release(free);
  for(int i = 0; i < 100; i++)
    f.advance(free, 1. / fs, kCtrl, false);
  CHECK(f.silent());

  // The ports round-trip
  const auto again = EnvSpec::from(spec.points(), spec.sustainItem(), spec.end, spec.loop, {});
  CHECK(again.sustain == spec.sustain);
  CHECK(again.end == spec.end);
  CHECK(again.level == spec.level);
  // and garbage falls back: steps of the wrong size, menus out of range, a
  // sustain after the end
  const auto bad = EnvSpec::from({1.f, 2.f}, 99, -4, false, spec);
  CHECK(bad.level == spec.level);
  CHECK(bad.end == 0);
  CHECK(bad.sustain == -1);
}

TEST_CASE("A looping envelope repeats while held, and ends when released",
          "[czumarzu][envelope]")
{
  const double fs = 48000.;
  // Up in 10 ms, down in 115 ms: a hit every 125 ms
  const float up = float(std::log(0.010 / kSlowest) / std::log(kFastest / kSlowest));
  const float down = float(std::log(0.115 / kSlowest) / std::log(kFastest / kSlowest));
  const EnvSpec spec = EnvSpec::make({{up, 1.f}, {down, 0.f}}, -1, 1, true);
  EnvState e;
  e.start(spec);
  std::vector<double> onsets;
  float prev = 0.f;
  for(int i = 0; i < int(1.0 * fs / kCtrl); i++)
  {
    e.advance(spec, 1. / fs, kCtrl, true);
    if(prev < 0.5f && e.value >= 0.5f)
      onsets.push_back(i * kCtrl / fs);
    prev = e.value;
  }
  REQUIRE(onsets.size() == 8);
  for(std::size_t i = 1; i < onsets.size(); i++)
    CHECK(std::abs(onsets[i] - onsets[i - 1] - 0.125) < 2. * kCtrl / fs);

  e.release(spec);
  for(int i = 0; i < int(0.3 * fs / kCtrl); i++)
    e.advance(spec, 1. / fs, kCtrl, false);
  CHECK(e.silent());

  // With a sustain point, the loop is the steps up to it; the release
  // leaves it for the step after
  const EnvSpec sus = EnvSpec::make({{up, 1.f}, {down, 0.2f}, {0.6f, 0.f}}, 1, 2, true);
  EnvState f;
  f.start(sus);
  int rises = 0;
  prev = 0.f;
  for(int i = 0; i < int(1.0 * fs / kCtrl); i++)
  {
    f.advance(sus, 1. / fs, kCtrl, true);
    rises += prev < 0.5f && f.value >= 0.5f;
    prev = f.value;
  }
  CHECK(rises >= 7);
  f.release(sus);
  CHECK(f.step == 2);

  // A loop of steps that take no time does not hang
  const EnvSpec flat = EnvSpec::make({{0.5f, 0.f}, {0.5f, 0.f}}, -1, 1, true);
  EnvState g;
  g.start(flat);
  for(int i = 0; i < 100; i++)
    g.advance(flat, 1. / fs, kCtrl, true);
  CHECK(g.value == 0.f);

}

TEST_CASE("CZumarzu is in tune", "[czumarzu][tuning]")
{
  for(double fs : {44100., 48000., 96000.})
  {
    for(Quality q : {Quality::Clean, Quality::Clean2x})
    {
      for(int note : {36, 57, 81})
      {
        Rig rig(fs);
        auto p = steady(Wave::Saw, 0.5f);
        p.quality = q;
        rig.on(note, 100, p);
        const auto x = tail(rig.run(1.2, p), std::size_t(0.2 * fs));
        const double f = 440. * std::exp2((note - 69) / 12.);
        const double coarse = estimateF0(x, fs, f * 0.8, f * 1.25);
        REQUIRE(std::abs(1200. * std::log2(coarse / f)) < 20.);
        const double got = measureFrequency(x, f, fs);
        INFO("fs " << fs << " note " << note << " quality " << int(q));
        CHECK(std::abs(1200. * std::log2(got / f)) < 0.5);
      }
    }
  }
}

TEST_CASE("CZumarzu sounds as the chip at low pitch", "[czumarzu][chip]")
{
  // Both at 40 kHz, a low note, where neither aliases: the harmonics must
  // match. The chip's A4 is its note 62, at 442 Hz.
  const double fs = upd933_ref::upd933::SAMPLE_RATE;
  const std::uint16_t chipNote = 45; // 442 * 2^(-17/12) = 165.6 Hz
  for(const auto& w : kChipWaves)
  {
    for(int dcw : {0, 384, 768})
    {
      for(const ChipWave* w2 : {(const ChipWave*)nullptr, &kChipWaves[1]})
      {
        upd933_ref::upd933 chip;
        chipVoice(chip, w, w2, dcw, std::uint16_t(chipNote << 9));
        const double f = double(chip.m_voice[1].m_pitch_step) * fs / 2147483648.;
        std::vector<float> ref(32768);
        for(auto& s : ref)
          s = float(chip.update(1)) / 4096.f;

        Rig rig(fs);
        auto p = steady(
            w.wave, float(dcw) / 1024.f,
            w2 ? SecondWave(int(w2->wave) + 1) : SecondWave::Off);
        const double semis = 69. + 12. * std::log2(f / 440.);
        const int midi = int(std::round(semis));
        p.tune = float(100. * (semis - midi));
        p.sanitize();
        rig.on(midi, 127, p);
        // The chip's full level is its volume step 508 of 511
        p.line[0].dca = EnvSpec::make({{1.f, 508.f / 511.f}}, 0, 1);
        // The synth's level: 0.25 per line, centred at -3 dB
        auto ours = tail(rig.run(1.2, p), std::size_t(0.3 * fs));
        for(auto& s : ours)
          s *= 4.f * std::sqrt(2.f);

        // Alternating waveforms repeat every two cycles
        const double fund = w2 ? f / 2. : f;
        const std::size_t n = std::size_t(std::round(24. * fs / fund));
        double worst = 0.;
        for(int h = 1; h <= 24; h++)
        {
          const double fh = fund * h;
          if(fh > 4000.)
            break;
          const double a = magnitudeAt(ref, 1000, n, fh, fs);
          const double b = magnitudeAt(ours, 1000, n, fh, fs);
          if(a < -40.) // under the chip's own quantisation noise, near enough
            continue;
          worst = std::max(worst, std::abs(a - b));
        }
        INFO("wave " << int(w.wave) << (w2 ? "+square" : "") << " dcw " << dcw);
        CHECK(worst < 0.5);
      }
    }
  }
}

TEST_CASE("The antialiasing keeps every waveform clean", "[czumarzu][aliasing]")
{
  // Measured, below 16 kHz, at the brightest DCW. The two waveforms that jump
  // are the hardest: their steps are band-limited separately.
  const double fs = 48000.;
  for(int w = 0; w < kWaveCount; w++)
  {
    for(int note : {72, 96})
    {
      double asr[4]{};
      for(Quality q : {Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live})
      {
        Rig rig(fs);
        auto p = steady(Wave(w), 0.992f);
        p.line[0].formant = 3.f;
        p.quality = q;
        rig.on(note, 127, p);
        const auto x = tail(rig.run(1.1, p), std::size_t(0.2 * fs));
        asr[int(q)] = aliasToSignal(x, 440. * std::exp2((note - 69) / 12.), fs, 16000.);
      }
      std::printf("aliasing, wave %d, note %d: Clean %.1f dB, Clean 2x %.1f dB, Clean 4x %.1f "
                  "dB, Live %.1f dB\n",
                  w, note, asr[0], asr[1], asr[2], asr[3]);
      INFO("wave " << w << " note " << note);
      const bool jumps = Wave(w) == Wave::DoublePulse || Wave(w) == Wave::ResoDoubleSaw;
      CHECK(asr[0] < (jumps ? -48. : -60.));
      CHECK(asr[1] < (jumps ? -68. : -80.));
      CHECK(asr[2] < (jumps ? -75. : -84.));
      // Live: as Clean but for the jumps, which it does not band-limit
      CHECK(asr[3] < (jumps ? -30. : -60.));
    }
  }
  // VPS higher: its second segment's slope grows with the height too
  for(float formant : {5.f, 8.f})
  {
    double asr[4]{};
    for(Quality q : {Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live})
    {
      Rig rig(fs);
      auto p = steady(Wave::Vps, 0.992f);
      p.line[0].formant = formant;
      p.quality = q;
      p.sanitize();
      rig.on(96, 127, p);
      const auto x = tail(rig.run(1.1, p), std::size_t(0.2 * fs));
      asr[int(q)] = aliasToSignal(x, 440. * std::exp2((96 - 69) / 12.), fs, 16000.);
    }
    std::printf("aliasing, VPS, formant %.0f, note 96: Clean %.1f dB, Clean 2x %.1f dB, Clean 4x "
                "%.1f dB, Live %.1f dB\n",
                double(formant), asr[0], asr[1], asr[2], asr[3]);
    INFO("formant " << formant);
    CHECK(asr[0] < -60.);
    CHECK(asr[1] < -80.);
    CHECK(asr[2] < -84.);
    CHECK(asr[3] < -60.);
  }
}

TEST_CASE("The triangle is exact, and the carried halves equal it", "[czumarzu][adaa]")
{
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> u01(0.f, 1.f);
  for(int w = 0; w < kWaveCount; w++)
  {
    for(int trial = 0; trial < 30; trial++)
    {
      const float dx = 0.002f + 0.05f * u01(rng);
      const float pivot = std::min(0.5f, std::max(8.f * dx, 0.5f * u01(rng)));
      const float k = std::min(1.f + 15.f * u01(rng), 0.2f / dx);
      const Shape a = shapeOf(Wave(w), vec4(pivot), vec4(k), vec4(1.5f));
      const Shape b = shapeOf(Wave((w + 5) % kWaveCount), vec4(pivot), vec4(k), vec4(1.5f));
      const float x0 = u01(rng) * 0.999f;
      const double got = lane0(sampleTriangle(a, b, false, vec4(x0), vec4(dx)));

      // The same by quadrature, cycles either side read with b
      const int n = 40000;
      double ref = 0.;
      for(int i = 0; i < n; i++)
      {
        const double t = -1. + 2. * (i + 0.5) / n; // in units of dx
        double x = x0 + t * dx;
        const bool other = x < 0. || x >= 1.;
        x -= std::floor(x);
        ref += (1. - std::abs(t)) * lane0(evaluate(other ? b : a, vec4(float(x))));
      }
      ref *= 2. / n;
      INFO("wave " << w << " x0 " << x0 << " dx " << dx);
      CHECK(std::abs(got - ref) < 5e-4);

      // Two intervals in a row: carry + falling is the triangle at the second
      const vec4 inv = vec4(1.f) / vec4(dx);
      const Halves h1
          = sampleInterval(a, b, false, vec4(x0 - dx < 0.f ? x0 : x0 - dx), vec4(dx), inv);
      const float x1 = (x0 - dx < 0.f ? x0 : x0 - dx) + dx;
      const Halves h2 = sampleInterval(a, b, false, vec4(x1), vec4(dx), inv);
      const double carried = lane0(h1.box / vec4(dx) - h1.falling + h2.falling);
      const double direct = lane0(sampleTriangle(a, b, false, vec4(x1), vec4(dx)));
      CHECK(std::abs(carried - direct) < 2e-5);
    }
  }
}

TEST_CASE("The step residual starts and ends at zero", "[czumarzu][adaa]")
{
  // Built by the synth's prepare(); 9 KB, not for the stack of every test
  const auto table = std::make_unique<StepResidual>();
  table->build();
  const auto& r = *table;
  const float h = float(StepResidual::kHalf);
  CHECK(r(-h) == 0.f);
  CHECK(std::abs(r(h)) < 1e-6f);
  CHECK(std::abs(r(-h + 0.001f)) < 1e-4f);
  CHECK(std::abs(r(h - 0.001f)) < 1e-4f);
  // Odd about the jump: the windowed sinc and the triangle are both centred
  for(float u : {0.3f, 0.7f, 1.5f, 2.5f})
    CHECK(std::abs(r(u) + r(-u)) < 1e-4f);
}

TEST_CASE("CZumarzu's response is flat across the band", "[czumarzu][adaa]")
{
  // A sine (saw at DCW 0): the triangle dulls the top octave, the
  // compensation gives it back
  for(Quality q : {Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live})
  {
    double lo = 1e9, hi = -1e9;
    for(int note : {45, 69, 93, 105, 112})
    {
      const double fs = 48000.;
      Rig rig(fs);
      auto p = steady(Wave::Saw, 0.f);
      p.quality = q;
      rig.on(note, 127, p);
      const auto x = tail(rig.run(0.6, p), std::size_t(0.2 * fs));
      const double f = 440. * std::exp2((note - 69) / 12.);
      // One line, centred, 0.25 per line: back to the waveform's level
      const double db = magnitudeAt(x, 0, 16384, f, fs) + 20. * std::log10(4. * std::sqrt(2.));
      lo = std::min(lo, db);
      hi = std::max(hi, db);
    }
    INFO("quality " << int(q));
    CHECK(lo > -1.);
    CHECK(hi < 1.);
  }
}

TEST_CASE("Alternating waveforms and two lines play", "[czumarzu][lines]")
{
  const double fs = 48000.;
  // Two different waveforms alternate: a period of two cycles
  {
    Rig rig(fs);
    auto p = steady(Wave::Saw, 0.6f, SecondWave::Square);
    rig.on(57, 100, p);
    const auto x = tail(rig.run(0.6, p), std::size_t(0.2 * fs));
    CHECK(magnitudeAt(x, 0, 8192, 110., fs) > -50.);
    Rig same(fs);
    auto q = steady(Wave::Saw, 0.6f);
    same.on(57, 100, q);
    const auto y = tail(same.run(0.6, q), std::size_t(0.2 * fs));
    CHECK(magnitudeAt(y, 0, 8192, 110., fs) < -80.);
  }
  // Every line mode and modulation stays finite and sounds
  for(int m = 0; m < kLineModeCount; m++)
    for(int mod = 0; mod < kModulationCount; mod++)
    {
      Rig rig(fs);
      auto p = steady(Wave::ResoTriangle, 0.7f);
      p.line[1] = p.line[0];
      p.line[1].wave1 = Wave::Square;
      p.mode = LineMode(m);
      p.modulation = Modulation(mod);
      p.spread = 0.5f;
      p.sanitize();
      rig.on(60, 100, p);
      std::vector<float> r;
      const auto l = rig.run(0.3, p, &r);
      INFO("mode " << m << " modulation " << mod);
      CHECK(allFinite(l));
      CHECK(allFinite(r));
      CHECK(*std::max_element(l.begin(), l.end()) > 0.01f);
    }
}

TEST_CASE("Voices are freed, stolen and held by the pedal", "[czumarzu][voices]")
{
  const double fs = 48000.;
  Rig rig(fs);
  Params p; // the default patch: two lines, DCA releasing in a few hundred ms
  p.sanitize();
  for(int n = 0; n < 24; n++)
    rig.on(40 + n, 100, p);
  rig.run(0.1, p);
  CHECK(rig.s->activeVoices() == kMaxVoices);
  for(int n = 0; n < 24; n++)
    rig.off(40 + n, p);
  rig.run(10., p);
  CHECK(rig.s->activeVoices() == 0);

  // Sustain pedal
  rig.send(0xB0, 64, 127, p);
  rig.on(60, 100, p);
  rig.off(60, p);
  rig.run(3., p);
  CHECK(rig.s->activeVoices() == 1);
  rig.send(0xB0, 64, 0, p);
  rig.run(10., p);
  CHECK(rig.s->activeVoices() == 0);

  // Mono: one voice, the last note wins
  p.play = PlayMode::Legato;
  rig.on(60, 100, p);
  rig.on(64, 100, p);
  rig.run(0.1, p);
  CHECK(rig.s->activeVoices() == 1);
  CHECK(rig.s->voices[0].note == 64);
  rig.off(64, p);
  CHECK(rig.s->voices[0].note == 60);
  rig.off(60, p);
  rig.run(10., p);
  CHECK(rig.s->activeVoices() == 0);
}

TEST_CASE("Any input: finite, bounded, no allocation", "[czumarzu][robustness]")
{
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> u(-1e6f, 1e6f);
  std::uniform_int_distribution<int> byte(0, 255);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  for(int trial = 0; trial < 40; trial++)
  {
    Rig rig(trial % 2 ? 44100. : 96000.);
    std::vector<float> l(512), r(512);
    Params p;
    auto pick = [&] {
      const int k = byte(rng) % 4;
      return k == 0 ? nan : k == 1 ? inf : u(rng);
    };
    auto envelope = [&](const EnvSpec& fallback) {
      std::vector<float> e(kEnvSize);
      for(auto& v : e)
        v = byte(rng) % 3 ? float(byte(rng)) / 255.f : pick();
      // Menus in range and out of it, a loop or not
      const int sus = byte(rng) % 2 ? byte(rng) % (kSteps + 1) : int(byte(rng)) - 64;
      const int end = byte(rng) % 2 ? byte(rng) % kSteps : int(byte(rng)) - 64;
      return EnvSpec::from(e, sus, end, byte(rng) & 1, fallback);
    };
    for(auto& line : p.line)
    {
      line.wave1 = Wave(byte(rng) % 20 - 4);
      line.wave2 = SecondWave(byte(rng) % 20 - 4);
      line.formant = pick();
      line.level = pick();
      line.dcwDepth = pick();
      line.pitchDepth = pick();
      line.dcwKeyFollow = pick();
      line.dcaKeyFollow = pick();
      line.dca = envelope(kDefaultDca);
      line.dcw = envelope(kDefaultDcw);
      line.dco = envelope(kDefaultDco);
    }
    // And a spec built in the code, past the menus' ranges
    if(trial % 4 == 3)
      p.line[0].dca = EnvSpec::make({{2.f, nan}, {-1.f, 0.5f}}, 12, -3, true);
    p.mode = LineMode(byte(rng) % 6);
    p.modulation = Modulation(byte(rng) % 5);
    p.play = PlayMode(byte(rng) % 4);
    p.tune = pick();
    p.octave = pick();
    p.detuneFine = pick();
    p.vibPeriod = pick();
    p.vibDepth = pick();
    p.vibDelay = pick();
    p.glide = pick();
    p.bendRange = pick();
    p.spread = pick();
    p.level = pick();
    p.voices = byte(rng) - 100;
    p.sanitize();

    g_allocs = 0;
    g_watch = true;
    for(int block = 0; block < 200; block++)
    {
      for(int m = 0; m < 4; m++)
      {
        const unsigned char msg[3]{(unsigned char)byte(rng), (unsigned char)byte(rng),
                                   (unsigned char)byte(rng)};
        rig.s->midi(msg, 1 + byte(rng) % 3, p);
      }
      // Every quality, and the line modes and modulations that change the
      // internal rate
      if(block % 25 == 0)
      {
        p.quality = Quality(byte(rng) % kQualityCount);
        p.modulation = Modulation(byte(rng) % kModulationCount);
        p.mode = LineMode(byte(rng) % kLineModeCount);
      }
      const int frames = 1 + byte(rng) % 512;
      rig.s->render(p, l.data(), r.data(), frames);
      for(int i = 0; i < frames; i++)
      {
        const auto ii = std::size_t(i);
        REQUIRE(ossia::safe_isfinite(l[ii]));
        REQUIRE(ossia::safe_isfinite(r[ii]));
        REQUIRE(std::abs(l[ii]) <= 1.f);
        REQUIRE(std::abs(r[ii]) <= 1.f);
      }
    }
    g_watch = false;
    CHECK(g_allocs == 0);

    // All notes off, then silence: nothing stays on
    for(int ch = 0; ch < 16; ch++)
    {
      rig.send((unsigned char)(0xB0 | ch), 123, 0, p);
      rig.send((unsigned char)(0xB0 | ch), 64, 0, p);
    }
    // The fuzzed envelopes can take minutes to release (eight steps of up to
    // 30 s, slower still with key follow): with the default patch's, every
    // released voice ends within a second
    p = Params{};
    p.sanitize();
    for(int i = 0; i < 200; i++)
      rig.s->render(p, l.data(), r.data(), 512);
    CHECK(rig.s->activeVoices() == 0);
  }
}

TEST_CASE("CZumarzu's cost", "[czumarzu][.bench]")
{
  for(Quality q : {Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live})
  {
    Rig rig(48000.);
    Params p;
    p.quality = q;
    p.line[1].wave1 = Wave::ResoTrapezoid;
    p.line[0].wave2 = SecondWave::Square;
    p.line[0].dca = EnvSpec::make({{1.f, 1.f}}, 0, 1);
    p.line[1].dca = p.line[0].dca;
    p.sanitize();
    for(int n = 0; n < kMaxVoices; n++)
      rig.on(40 + 3 * n, 100, p);
    std::vector<float> l(256), r(256);
    const int blocks = int(5. * 48000. / 256.);
    const auto t0 = std::chrono::steady_clock::now();
    for(int b = 0; b < blocks; b++)
      rig.s->render(p, l.data(), r.data(), 256);
    const double took
        = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const char* names[]{"Clean", "Clean 2x", "Clean 4x", "Live"};
    std::printf("16 voices, two lines, %s: %.2f %% of one core at 48 kHz\n", names[int(q)],
                100. * took / 5.);
  }
}

TEST_CASE("Spread is balanced, at any pitch", "[czumarzu][spread]")
{
  const double fs = 48000.;
  //! Left over right, in dB
  auto balance = [&](int note, LineMode mode, float spread, float level1, float level2) {
    Rig rig(fs);
    auto p = steady(Wave::Saw, 0.5f);
    p.mode = mode;
    p.line[1] = p.line[0];
    p.line[0].level = level1;
    p.line[1].level = level2;
    p.spread = spread;
    p.sanitize();
    rig.on(note, 100, p);
    std::vector<float> right;
    const auto left = rig.run(0.5, p, &right);
    double el = 0., er = 0.;
    for(std::size_t i = 4800; i < left.size(); i++)
    {
      el += double(left[i]) * left[i];
      er += double(right[i]) * right[i];
    }
    return 10. * std::log10(el / er);
  };
  // Equal power: a line at pan x has cos(x pi/2) on the left, sin on the right
  const double pan = 0.5 - 0.5 * 0.5; // line 1 at spread 0.5
  const double side = 20. * std::log10(std::cos(0.5 * kPiD * pan) / std::sin(0.5 * kPiD * pan));
  for(int note : {36, 60, 84})
  {
    INFO("note " << note);
    // One line alone is in the centre, whatever the spread
    CHECK(std::abs(balance(note, LineMode::Line1, 1.f, 1.f, 1.f)) < 0.05);
    CHECK(std::abs(balance(note, LineMode::Line2, 1.f, 1.f, 1.f)) < 0.05);
    // Two lines alike: both sides alike
    CHECK(std::abs(balance(note, LineMode::Line1Doubled, 1.f, 1.f, 1.f)) < 0.5);
    CHECK(std::abs(balance(note, LineMode::Line1And2, 1.f, 1.f, 1.f)) < 0.5);
    // One of the two lines silent: the other where the spread puts it
    CHECK(std::abs(balance(note, LineMode::Line1And2, 0.5f, 1.f, 0.f) - side) < 0.1);
    CHECK(std::abs(balance(note, LineMode::Line1And2, 0.5f, 0.f, 1.f) + side) < 0.1);
  }
  // Fully spread, line 2 at 0.3 of line 1: the right side is that much lower
  CHECK(std::abs(balance(60, LineMode::Line1And2, 1.f, 1.f, 0.3f) + 20. * std::log10(0.3)) < 0.1);
}
namespace
{
//! As the host's custom_mouse_event
struct Mouse
{
  enum button
  {
    no_button,
    left = (1 << 1),
    right = (1 << 2),
    middle = (1 << 3)
  };
  float x, y;
  enum button button = left;
};
Mouse ev(double x, double y, enum Mouse::button b = Mouse::left)
{
  return {float(x), float(y), b};
}

//! As the host does: the four ports' values
template <typename W>
void setPorts(W& w, const EnvSpec& e)
{
  w.values = {e.points(), e.sustainItem(), e.end, e.loop};
  w.sync();
}
}

TEST_CASE("A dragged envelope point moves as the mouse does", "[czumarzu][ui]")
{
  using W = StepEnvelopeWidget<DcwKind>;
  const double rateScale = W::kCol - 2. * W::kInset, levelScale = W::kPlotH - 6.;
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  int moves = 0;
  for(int trial = 0; trial < 500; trial++)
  {
    W w;
    EnvSpec e;
    e.end = 1 + int(u(rng) * 7.f);
    e.sustain = u(rng) < 0.6f ? int(u(rng) * float(e.end)) : -1;
    for(int i = 0; i < kSteps; i++)
    {
      e.rate[std::size_t(i)] = u(rng);
      e.level[std::size_t(i)] = (i > 0 && u(rng) < 0.25f) ? e.level[std::size_t(i - 1)] : u(rng);
    }
    setPorts(w, e);
    const EnvSpec before = w.spec;

    const int p = int(u(rng) * float(e.end + 1));
    const auto pi = std::size_t(p);
    // Pressed anywhere in the column: the point does not jump
    const double x0 = W::colX(p) + W::kCol * u(rng);
    const double y0 = W::kPlotY + W::kPlotH * u(rng);
    REQUIRE(w.mouse_press(ev(x0, y0)));
    CHECK(w.spec.rate == before.rate);
    CHECK(w.spec.level == before.level);
    double cx = x0, cy = y0;
    for(int k = 0; k < 30; k++)
    {
      cx += (u(rng) - 0.5) * 30.; // also out of the column, and the plot
      cy += (u(rng) - 0.5) * 30.;
      w.mouse_move(ev(cx, cy));
      moves++;
      INFO("trial " << trial << " step " << p << " cursor " << cx << ", " << cy);
      // By as much as the mouse, right slower and up louder, within 0..1
      const double er = std::clamp(before.rate[pi] - (cx - x0) / rateScale, 0., 1.);
      const double el = std::clamp(before.level[pi] - (cy - y0) / levelScale, 0., 1.);
      CHECK(std::abs(w.spec.rate[pi] - er) < 1e-5);
      CHECK(std::abs(w.spec.level[pi] - el) < 1e-5);
      // And nothing else moved
      for(int o = 0; o < kSteps; o++)
        if(o != p)
        {
          CHECK(w.spec.rate[std::size_t(o)] == before.rate[std::size_t(o)]);
          CHECK(w.spec.level[std::size_t(o)] == before.level[std::size_t(o)]);
        }
      CHECK(w.spec.end == before.end);
      CHECK(w.spec.sustain == before.sustain);
    }
    w.mouse_release(ev(cx, cy));
    CHECK(w.drag == -1);
  }
  CHECK(moves == 500 * 30);
}

TEST_CASE("The envelope graph edits the steps, the menus the rest", "[czumarzu][ui]")
{
  using W = StepEnvelopeWidget<DcwKind>;
  W w;
  int edits = 0, ends = 0, commits = 0;
  std::get<0>(w.transactions).update = [&](const std::vector<float>& v) {
    std::get<0>(w.values) = v;
    edits++;
  };
  std::get<2>(w.transactions).update = [&](const int& end) {
    std::get<2>(w.values) = end;
    ends++;
  };
  std::get<0>(w.transactions).commit = [&] { commits++; };
  setPorts(w, EnvSpec::make({{0.9f, 1.f}, {0.5f, 0.4f}, {0.5f, 0.f}}, 1, 2));
  const double y = W::kPlotY + 10.;
  auto at = [&](int i) { return W::colX(i) + W::kCol * 0.5; };

  // A click in a used column, without a move, changes nothing
  CHECK(w.mouse_press(ev(at(1), y)));
  w.mouse_release(ev(at(1), y));
  CHECK(edits == 0);
  CHECK(ends == 0);

  // A dimmed step, past the end: the end moves to it and its point drags,
  // in one gesture
  CHECK(w.mouse_press(ev(at(4), y)));
  CHECK(w.spec.end == 4);
  CHECK(std::get<2>(w.values) == 4);
  w.mouse_move(ev(at(4), y - 10.));
  CHECK(w.spec.end == 4);
  w.mouse_release(ev(at(4), y - 10.));
  CHECK(ends == 1);
  CHECK(edits == 1);
  CHECK(commits == 2);
  w.sync();
  CHECK(w.spec.end == 4);
  edits = 0;
  // Sustain, end and loop come from their menus
  std::get<1>(w.values) = 0;
  std::get<2>(w.values) = 5;
  std::get<3>(w.values) = true;
  w.sync();
  CHECK(w.spec.sustain == -1);
  CHECK(w.spec.end == 5);
  CHECK(w.spec.loop);
  CHECK(w.mouse_press(ev(at(5), y)));
  w.mouse_move(ev(at(5), y - 10.));
  w.mouse_release(ev(at(5), y - 10.));
  CHECK(edits == 1);
  CHECK(std::get<0>(w.values).size() == std::size_t(kEnvSize));
}

TEST_CASE("Live has no latency, Clean is 9 samples late", "[czumarzu][latency]")
{
  // A sine (saw at DCW 0) from a note at sample 0: its phase against the
  // ideal -cos(2 pi f n / fs) gives the delay, in samples
  const double fs = 48000.;
  const double f = 440.;
  auto delayOf = [&](Quality q) {
    Rig rig(fs);
    auto p = steady(Wave::Saw, 0.f);
    p.quality = q;
    p.tune = float(1200. * std::log2(f / 440.));
    p.sanitize();
    rig.on(69, 127, p);
    const auto x = rig.run(0.5, p);
    const std::size_t from = 4800, n = 9600;
    std::complex<double> got{}, ideal{};
    for(std::size_t i = 0; i < n; i++)
    {
      const double t = double(from + i);
      const auto w = std::polar(1., -2. * kPiD * f * t / fs);
      got += double(x[from + i]) * w;
      ideal += -std::cos(2. * kPiD * f * t / fs) * w;
    }
    double d = std::arg(ideal) - std::arg(got);
    d -= 2. * kPiD * std::round(d / (2. * kPiD));
    return d * fs / (2. * kPiD * f);
  };
  const double live = delayOf(Quality::Live);
  const double clean = delayOf(Quality::Clean);
  std::printf("latency: Live %.2f samples, Clean %.2f samples\n", live, clean);
  // Live never lags: the DC blocker and the compensation shelf lead the
  // phase a little (0.4 and 0.2 samples at 440 Hz). Clean is the jumps'
  // lookahead later.
  CHECK(live < 0.05);
  CHECK(live > -1.);
  CHECK(std::abs(clean - live - StepResidual::kHalf) < 0.05);
}

TEST_CASE("The 4x first stage keeps the band and stops what would fold into it",
          "[czumarzu][halfband]")
{
  Decimator4x d;
  d.prepare();
  auto gainAt = [&](double f) {
    d.reset();
    const int n = 8192;
    double e = 0.;
    for(int i = 0; i < n; i++)
    {
      const float a = float(std::sin(2. * kPiD * f * double(2 * i)));
      const float b = float(std::sin(2. * kPiD * f * double(2 * i + 1)));
      const float y = d.process(a, b);
      if(i >= n / 2)
        e += double(y) * double(y);
    }
    return 10. * std::log10(std::max(2. * e / double(n / 2), 1e-30));
  };
  double ripple = 0.;
  for(double f = 0.005; f <= 0.1125; f += 0.005)
    ripple = std::max(ripple, std::abs(gainAt(f)));
  double stop = -300.;
  for(double f = 0.3875; f < 0.5; f += 0.0037)
    stop = std::max(stop, gainAt(f));
  std::printf("4x first stage: passband ripple %.4f dB, stopband %.1f dB\n", ripple, stop);
  CHECK(ripple < 0.01);
  CHECK(stop < -100.);
}

TEST_CASE("The envelope graph follows the ports", "[czumarzu][ui]")
{
  // The host writes the values directly on a preset, an undo or automation
  using W = StepEnvelopeWidget<DcaKind>;
  W w;
  const auto a = EnvSpec::make({{0.9f, 1.f}, {0.4f, 0.f}}, 0, 1);
  const auto b = EnvSpec::make({{0.2f, 0.5f}, {0.7f, 0.8f}, {0.3f, 0.1f}}, 1, 2);
  setPorts(w, a);
  CHECK(w.spec.end == 1);
  setPorts(w, b);
  CHECK(w.spec.end == 2);
  CHECK(w.spec.sustain == 1);
  CHECK(w.spec.level[1] == 0.8f);
  // During a drag the dragged steps lead; the menus still follow
  REQUIRE(w.mouse_press(ev(W::colX(0) + 10., W::kPlotY + 5.)));
  setPorts(w, a);
  CHECK(w.spec.end == 1);
  CHECK(w.spec.level[1] == 0.8f);
  w.mouse_release(ev(0., 0.));
  w.sync();
  CHECK(w.spec.level[1] == 0.f);
}

namespace
{
//! The normalised rate whose full-scale move takes `s` seconds
float rateFor(double s)
{
  return float(std::log(s / kSlowest) / std::log(kFastest / kSlowest));
}

/*! Energy below `hi` Hz over all of it, in dB, zero padded. Rectangular for
 *  a sound that starts and ends in silence, else Hann windowed.
 */
double lowBand(const std::vector<float>& x, double fs, double hi, bool hann = false)
{
  std::size_t n = 1;
  while(n < 2 * x.size())
    n <<= 1;
  std::vector<std::complex<double>> a(n);
  for(std::size_t i = 0; i < x.size(); i++)
    a[i] = x[i]
           * (hann ? 0.5 - 0.5 * std::cos(2. * kPiD * double(i) / double(x.size())) : 1.);
  fft(a);
  double lo = 0., all = 0.;
  for(std::size_t i = 0; i < n / 2; i++)
  {
    const double e = std::norm(a[i]);
    all += e;
    if(double(i) * fs / double(n) <= hi)
      lo += e;
  }
  return 10. * std::log10(std::max(lo, 1e-30) / all);
}

double maxStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
  double m = 0.;
  for(std::size_t i = from + 1; i < to; i++)
    m = std::max(m, double(std::abs(x[i] - x[i - 1])));
  return m;
}
}

TEST_CASE("The rates' constants and exponential are exact", "[czumarzu][envelope]")
{
  CHECK(std::abs(kLnRateSpan - std::log(kFastest / kSlowest)) < 1e-15);
  for(double r = 0.; r <= 1.; r += 0.01)
    CHECK(std::abs(stepTime(r) / (kSlowest * std::pow(kFastest / kSlowest, r)) - 1.) < 1e-13);
  // The defaults are built at compile time, and sanitized: their speeds too
  static_assert(kDefaultDca.speed[0] > 0.);
  CHECK(std::abs(kDefaultDcw.speed[1] * stepTime(kDefaultDcw.rate[1]) - 1.) < 1e-12);
}

TEST_CASE("Specs built in the code are sanitized too", "[czumarzu][envelope]")
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const auto e = EnvSpec::make({{nan, 2.f}, {-1.f, 0.5f}, {0.3f, nan}}, 5, 11);
  CHECK(e.end == kSteps - 1);
  CHECK(e.sustain == 5);
  CHECK(e.rate[0] == 0.5f);
  CHECK(e.level[0] == 0.f);
  CHECK(e.rate[1] == 0.5f);
  CHECK(e.level[2] == 0.f);
  // A sustain past the end is none
  CHECK(EnvSpec::make({{1.f, 1.f}}, 3, 1).sustain == -1);
  CHECK(EnvSpec::make({{1.f, 1.f}}, -7, 1).sustain == -1);
  // and a line's sanitize sanitizes its envelopes
  LineParams l;
  l.dca.end = -4;
  l.dca.rate[2] = nan;
  l.sanitize();
  CHECK(l.dca.end == 0);
  CHECK(l.dca.rate[2] == 0.5f);
}

TEST_CASE("A NaN inside the synth does not stay there", "[czumarzu][robustness]")
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for(Quality q : {Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live})
  {
    Rig rig;
    auto p = steady(Wave::Saw, 0.5f);
    p.quality = q;
    p.sanitize();
    rig.on(60, 100, p);
    rig.run(0.1, p);
    // Into a filter's state, through the oscillator
    rig.s->groups[0].carry[0][0] = nan;
    std::vector<float> r;
    const auto l = rig.run(0.1, p, &r);
    INFO("quality " << int(q));
    CHECK(allFinite(l));
    CHECK(allFinite(r));
    CHECK(rig.s->activeVoices() == 0);
    // Every filter after the voices was reset: the next note sounds
    rig.on(64, 100, p);
    const auto y = tail(rig.run(0.3, p), std::size_t(0.1 * rig.fs));
    CHECK(allFinite(y));
    CHECK(*std::max_element(y.begin(), y.end()) > 0.05f);
  }
}

TEST_CASE("Prepared again at the same rate, the notes go on", "[czumarzu][voices]")
{
  // The host prepares again when its buffers grow, from the audio thread
  Rig a, b;
  auto p = steady(Wave::ResoSaw, 0.6f);
  p.line[0].dcw = EnvSpec::make({{0.6f, 0.9f}, {0.5f, 0.f}}, -1, 1); // moving
  p.sanitize();
  a.on(60, 100, p);
  b.on(60, 100, p);
  a.run(0.1, p);
  b.run(0.1, p);
  a.s->prepare(48000.);
  const auto x = a.run(0.2, p), y = b.run(0.2, p);
  CHECK(x == y);
  CHECK(a.s->activeVoices() == 1);
  // Another rate starts again
  a.s->prepare(44100.);
  CHECK(a.s->activeVoices() == 0);
}

TEST_CASE("Mono after Poly does not bring back a released key", "[czumarzu][voices]")
{
  Rig rig;
  Params p;
  p.sanitize();
  p.play = PlayMode::Mono;
  rig.on(60, 100, p);
  rig.run(0.05, p);
  // 60 released in Poly, 64 still held
  p.play = PlayMode::Poly;
  rig.on(64, 100, p);
  rig.off(60, p);
  rig.run(0.05, p);
  p.play = PlayMode::Mono;
  rig.on(67, 100, p);
  rig.run(0.05, p);
  rig.off(67, p);
  CHECK_FALSE(rig.s->voices[0].held);
  CHECK(rig.s->voices[0].note == 67);
  rig.off(64, p);
  rig.run(10., p);
  CHECK(rig.s->activeVoices() == 0);

  // Within Poly too: a key released there is out of the stack
  p.play = PlayMode::Legato;
  rig.on(50, 100, p);
  rig.run(0.05, p);
  p.play = PlayMode::Poly;
  rig.off(50, p);
  p.play = PlayMode::Legato;
  rig.on(52, 100, p);
  rig.off(52, p);
  CHECK_FALSE(rig.s->voices[0].held);
  rig.run(10., p);
  CHECK(rig.s->activeVoices() == 0);
}

TEST_CASE("All notes off, all sound off and reset all controllers", "[czumarzu][midi]")
{
  Params p;
  p.sanitize();
  auto anyGate = [](Rig& rig) {
    return std::any_of(rig.s->voices.begin(), rig.s->voices.end(), [](auto& v) {
      return v.state != Synth::Voice::Free && v.gate();
    });
  };
  for(int cc : {120, 123})
  {
    Rig rig;
    rig.send(0xB0, 64, 127, p);
    rig.on(60, 100, p);
    rig.off(60, p);
    rig.send(0xB0, (unsigned char)cc, 0, p);
    INFO("cc " << cc);
    CHECK_FALSE(anyGate(rig));
    // The pedal went up with them
    CHECK_FALSE(rig.s->channels[0].sustain);
    rig.on(62, 100, p);
    rig.off(62, p);
    CHECK_FALSE(anyGate(rig));
    rig.run(10., p);
    CHECK(rig.s->activeVoices() == 0);
  }

  Rig rig;
  rig.send(0xE0, 0, 127, p);  // bend up
  rig.send(0xD0, 100, 0, p);  // channel pressure
  rig.send(0xB0, 1, 127, p);  // mod wheel
  rig.send(0xB0, 64, 127, p); // pedal
  rig.on(65, 100, p);
  rig.send(0xA0, 65, 90, p); // its own pressure
  rig.off(65, p);
  rig.on(67, 100, p);
  rig.run(0.05, p);
  rig.send(0xB0, 121, 0, p);
  const auto& cs = rig.s->channels[0];
  CHECK(cs.bend == 0.f);
  CHECK(cs.pressure == 0.f);
  CHECK_FALSE(cs.sustain);
  CHECK(rig.s->modWheel == 0.f);
  for(const auto& v : rig.s->voices)
  {
    CHECK_FALSE(v.hasPressure);
    // The note held by the pedal alone is released, the key still down is not
    if(v.state == Synth::Voice::Active && v.note == 65)
      CHECK_FALSE(v.gate());
    if(v.state == Synth::Voice::Active && v.note == 67)
      CHECK(v.held);
  }
}

TEST_CASE("A note released under the pedal before its voice was free sounds",
          "[czumarzu][voices]")
{
  Params p;
  p.voices = 1;
  p.sanitize();
  {
    Rig rig;
    rig.on(60, 100, p);
    rig.run(0.05, p);
    rig.send(0xB0, 64, 127, p);
    rig.on(64, 100, p); // steals 60's voice
    REQUIRE(rig.s->voices[0].state == Synth::Voice::Stealing);
    rig.off(64, p);
    rig.run(0.05, p);
    const auto& v = rig.s->voices[0];
    CHECK(v.state == Synth::Voice::Active);
    CHECK(v.note == 64);
    CHECK(v.sustained);
    CHECK_FALSE(v.held);
    rig.send(0xB0, 64, 0, p);
    rig.run(10., p);
    CHECK(rig.s->activeVoices() == 0);
  }
  {
    // Without the pedal it is dropped
    Rig rig;
    rig.on(60, 100, p);
    rig.run(0.05, p);
    rig.on(64, 100, p);
    rig.off(64, p);
    rig.run(0.05, p);
    CHECK(rig.s->activeVoices() == 0);
  }
}

TEST_CASE("A slow glide at a high rate reaches its note", "[czumarzu][voices]")
{
  // 384 kHz, the longest glide: each tick's step is far under a float's ulp
  Rig rig(384000.);
  auto p = steady(Wave::Saw, 0.5f);
  p.play = PlayMode::Legato;
  p.glide = 2.f;
  p.sanitize();
  rig.on(48, 100, p);
  rig.run(0.01, p);
  rig.on(60, 100, p);
  rig.run(1., p);
  CHECK(rig.s->voices[0].pitch < 59.);
  rig.run(8., p);
  CHECK(rig.s->voices[0].pitch == 60.);
}

TEST_CASE("Ring modulation does not alias more than a line", "[czumarzu][aliasing]")
{
  // The product reaches twice as high: Clean runs it at twice the rate
  const double fs = 48000.;
  double asr[4]{};
  for(Quality q : {Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live})
  {
    Rig rig(fs);
    auto p = steady(Wave::Saw, 0.992f);
    p.mode = LineMode::Line1And2;
    p.line[1] = p.line[0];
    p.modulation = Modulation::Ring;
    p.detuneFine = 0.f;
    p.quality = q;
    p.sanitize();
    rig.on(96, 127, p);
    const auto x = tail(rig.run(1.1, p), std::size_t(0.2 * fs));
    asr[int(q)] = aliasToSignal(x, 440. * std::exp2((96 - 69) / 12.), fs, 16000.);
  }
  std::printf("aliasing, ring, note 96: Clean %.1f dB, Clean 2x %.1f dB, Clean 4x %.1f dB, "
              "Live %.1f dB\n",
              asr[0], asr[1], asr[2], asr[3]);
  CHECK(asr[0] < -80.);
  CHECK(asr[1] < -80.);
  CHECK(asr[2] < -84.);
  // Live stays at the rate, without latency
  CHECK(asr[3] < -60.);
}

TEST_CASE("A line's DC does not follow its envelope", "[czumarzu][dc]")
{
  // The pulses and the windows are far from centred: their mean comes and
  // goes with the DCA, a thump at every attack and release
  const double fs = 48000.;
  for(Wave w : {Wave::Pulse, Wave::ResoSaw, Wave::SawPulse, Wave::DoublePulse})
  {
    Rig rig(fs);
    auto p = steady(w, 0.992f);
    p.line[0].dca = EnvSpec::make({{rateFor(0.02), 1.f}, {rateFor(0.2), 0.f}}, 0, 1);
    p.sanitize();
    rig.on(60, 127, p);
    auto x = rig.run(0.3, p);
    rig.off(60, p);
    const auto rest = rig.run(0.6, p);
    x.insert(x.end(), rest.begin(), rest.end());
    const double db = lowBand(x, fs, 20.);
    std::printf("DC, wave %d: %.1f dB below 20 Hz\n", int(w), db);
    INFO("wave " << int(w));
    CHECK(db < -40.);
  }
  // Nor its DCW: the mean moves with the pivot, ramped with it
  Rig rig(fs);
  auto p = steady(Wave::Pulse, 0.f);
  p.line[0].dcw = EnvSpec::make({{rateFor(0.05), 0.99f}, {rateFor(0.05), 0.f}}, -1, 1, true);
  p.sanitize();
  rig.on(60, 127, p);
  const auto x = tail(rig.run(1., p), std::size_t(0.1 * fs));
  const double db = lowBand(x, fs, 20., true);
  std::printf("DC, pulse, DCW looping: %.1f dB below 20 Hz\n", db);
  CHECK(db < -40.);
}

TEST_CASE("A low cosine stays pure", "[czumarzu][adaa]")
{
  // A sine (saw at DCW 0) at note 24: the increments are tiny against the
  // phase, which single precision must still resolve
  for(auto [fs, q] : {std::pair{48000., Quality::Clean}, std::pair{192000., Quality::Clean4x}})
  {
    Rig rig(fs);
    auto p = steady(Wave::Saw, 0.f);
    p.quality = q;
    p.sanitize();
    rig.on(24, 127, p);
    const std::size_t n = fs > 100000. ? 1 << 17 : 1 << 15;
    const auto x = tail(rig.run(double(n) / fs + 0.3, p), std::size_t(0.3 * fs));
    const double f0 = 440. * std::exp2((24 - 69) / 12.);
    // Kaiser, beta 30: Blackman-Harris's sidelobes alone add up to -88 dB.
    // Its main lobe is 10 bins wide either side.
    std::vector<std::complex<double>> a(n);
    auto i0 = [](double z) {
      double sum = 1., term = 1.;
      for(int k = 1; k < 80; k++)
      {
        term *= (z / (2. * k)) * (z / (2. * k));
        sum += term;
      }
      return sum;
    };
    for(std::size_t i = 0; i < n; i++)
    {
      const double r = 2. * double(i) / double(n - 1) - 1.;
      a[i] = double(x[i]) * i0(30. * std::sqrt(1. - r * r)) / i0(30.);
    }
    fft(a);
    double on = 0., off = 0.;
    for(std::size_t i = 1; i < n / 2; i++)
    {
      const double f = double(i) * fs / double(n);
      if(f > 20000.)
        break;
      (std::abs(f - f0) <= 12. * fs / double(n) ? on : off) += std::norm(a[i]);
    }
    const double floor = 10. * std::log10(off / on);
    std::printf("cosine at note 24, %.0f Hz, quality %d: %.1f dB off the fundamental\n", fs,
                int(q), floor);
    INFO("fs " << fs);
    CHECK(floor < -100.);
  }
}

TEST_CASE("A change of quality fades instead of clicking", "[czumarzu][quality]")
{
  // A sine: its steps from one sample to the next are bounded. A change of
  // internal rate resets filters and delays: faded out and in, never a jump.
  const double fs = 48000.;
  const Quality order[]{Quality::Clean, Quality::Clean2x, Quality::Clean4x, Quality::Live,
                        Quality::Clean, Quality::Clean4x, Quality::Live, Quality::Clean2x};
  Rig rig(fs);
  auto p = steady(Wave::Saw, 0.f);
  p.sanitize();
  rig.on(69, 127, p);
  auto x = rig.run(0.2, p);
  const double steadyStep = maxStep(x, x.size() / 2, x.size());
  for(Quality q : order)
  {
    p.quality = q;
    const std::size_t from = x.size();
    const auto y = rig.run(0.05, p);
    x.insert(x.end(), y.begin(), y.end());
    INFO("to quality " << int(q));
    CHECK(maxStep(x, from - 1, x.size()) < 1.1 * steadyStep);
  }
  // And it does come back, at full level
  // One line in the centre: 0.25 at -3 dB
  const auto y = rig.run(0.1, p);
  CHECK(*std::max_element(y.begin(), y.end()) > 0.17f);
}

TEST_CASE("The envelope graph drags with the left button only", "[czumarzu][ui]")
{
  using W = StepEnvelopeWidget<DcwKind>;
  W w;
  int starts = 0, commits = 0, rollbacks = 0;
  auto& t = std::get<0>(w.transactions);
  t.start = [&] { starts++; };
  t.update = [&](const std::vector<float>& v) { std::get<0>(w.values) = v; };
  t.commit = [&] { commits++; };
  t.rollback = [&] { rollbacks++; };
  setPorts(w, EnvSpec::make({{0.9f, 1.f}, {0.5f, 0.4f}, {0.5f, 0.f}}, 1, 2));
  const double x = W::colX(1) + W::kCol * 0.5, y = W::kPlotY + 10.;

  CHECK_FALSE(w.mouse_press(ev(x, y, Mouse::right)));
  CHECK_FALSE(w.mouse_press(ev(x, y, Mouse::middle)));
  CHECK(w.drag == -1);
  CHECK(starts == 0);

  // A drag shows its step in the title, until released
  CHECK(w.mouse_press(ev(x, y)));
  CHECK(w.shown == 1);
  w.mouse_move(ev(x, y - 5.));
  w.mouse_release(ev(x, y - 5.));
  CHECK(w.shown == -1);
  CHECK(commits == 1);

  // The host resets the UI mid-drag: the edit is undone, not left open
  CHECK(w.mouse_press(ev(x, y)));
  w.mouse_move(ev(x, y - 5.));
  w.reset();
  CHECK(rollbacks == 1);
  CHECK(commits == 1);
  CHECK(w.drag == -1);
  CHECK(w.shown == -1);
  CHECK(starts == 2);
}

TEST_CASE("In Mono, the pedal holds the voices left from Poly", "[czumarzu][voices]")
{
  Params p;
  p.sanitize();
  Rig rig;
  // Voice 0 takes 64 (the voice Mono plays), another the 60 released later
  rig.on(64, 100, p);
  rig.on(60, 100, p);
  rig.run(0.05, p);
  REQUIRE(rig.s->voices[0].note == 64);
  int i = 1;
  while(i < kMaxVoices
        && (rig.s->voices[std::size_t(i)].state != Synth::Voice::Active
            || rig.s->voices[std::size_t(i)].note != 60))
    i++;
  REQUIRE(i < kMaxVoices);
  const auto& v = rig.s->voices[std::size_t(i)];
  REQUIRE(v.state == Synth::Voice::Active);

  rig.send(0xB0, 64, 127, p);
  p.play = PlayMode::Mono;
  rig.off(60, p);
  CHECK_FALSE(v.held);
  CHECK(v.sustained);
  // Up: released, then gone
  rig.send(0xB0, 64, 0, p);
  CHECK_FALSE(v.sustained);
  rig.off(64, p);
  rig.run(10., p);
  CHECK(rig.s->activeVoices() == 0);
}

TEST_CASE("The allocation counter sees every kind of allocation", "[czumarzu][robustness]")
{
  // Called through volatile pointers: an allocation the compiler sees whole
  // may be elided
  void* (*volatile newArray)(std::size_t) = &::operator new[];
  g_allocs = 0;
  g_watch = true;
  void* a = newArray(16);
  void* b = ::operator new(64, std::align_val_t{64});
  void* c = ::operator new[](64, std::align_val_t{64}, std::nothrow);
  void* d = ::operator new(16, std::nothrow);
  int expected = 4;
#if defined(__GLIBC__)
  void* (*volatile m)(std::size_t) = &std::malloc;
  void* (*volatile ca)(std::size_t, std::size_t) = &std::calloc;
  void* (*volatile re)(void*, std::size_t) = &std::realloc;
  void* e = re(ca(4, 4), 64);
  void* f = m(16);
  expected += 3;
#endif
  g_watch = false;
  CHECK(g_allocs == expected);
  ::operator delete[](a);
  ::operator delete(b, std::align_val_t{64});
  ::operator delete[](c, std::align_val_t{64});
  ::operator delete(d);
#if defined(__GLIBC__)
  std::free(e);
  std::free(f);
#endif
}
