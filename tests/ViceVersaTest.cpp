// ViceVersa, the vector synthesizer, at the DSP level: the mixing law against the
// Prophet VS's documented behaviour, the wave banks' band limits and reads,
// how much aliasing is left, the mix envelope's travel, the ladder filter,
// and that nothing a host or a preset sends makes it explode, allocate on the
// audio thread or keep a voice alive forever.
#include <ViceVersa/ViceVersa.hpp>

#include <catch2/catch_approx.hpp>
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

using namespace viceversa;

namespace
{
void fftc(std::vector<std::complex<double>>& a)
{
  viceversa::fft(a, false);
}

std::vector<double> power(const std::vector<float>& x, std::size_t n)
{
  std::vector<std::complex<double>> a(n);
  for(std::size_t i = 0; i < n; i++)
  {
    const double t = 2. * std::numbers::pi * double(i) / double(n);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2. * t)
                     - 0.01168 * std::cos(3. * t);
    a[i] = w * double(x[i]);
  }
  fftc(a);
  std::vector<double> p(n / 2);
  for(std::size_t i = 0; i < n / 2; i++)
    p[i] = std::norm(a[i]);
  return p;
}

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

double measureFrequency(const std::vector<float>& x, double f, double fs)
{
  const std::size_t n = std::size_t(std::round(8. * fs / f));
  const std::size_t gap = x.size() - n - 1;
  auto phaseAt = [&](std::size_t from) {
    std::complex<double> acc{};
    for(std::size_t i = 0; i < n; i++)
    {
      const double w = 0.5 - 0.5 * std::cos(2. * std::numbers::pi * double(i) / double(n));
      acc += w * double(x[from + i]) * std::polar(1., -2. * std::numbers::pi * f * double(from + i) / fs);
    }
    return std::arg(acc);
  };
  double d = phaseAt(gap) - phaseAt(0);
  d -= 2. * std::numbers::pi * std::round(d / (2. * std::numbers::pi));
  return f + d * fs / (2. * std::numbers::pi * double(gap));
}

//! Source A alone, the filter open and flat, a sustained level
Params plain(BankId bank, float frame)
{
  Params p;
  p.src[0].bank = bank;
  p.src[0].frame = frame;
  p.x = -1.f; // the A corner
  p.y = 0.f;
  p.vecEnvDepth = 0.f;
  p.pressureY = 0.f;
  p.timbreX = 0.f;
  p.cutoff = 20000.f;
  p.resonance = 0.f;
  p.filterEnv = 0.f;
  p.keyTrack = 0.f;
  p.velCutoff = 0.f;
  p.drive = 0.f;
  p.aA = 0.001f;
  p.aS = 1.f;
  p.velLevel = 0.f;
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
  void on(int note, int vel, const Params& p) { send(0x90, (unsigned char)note, (unsigned char)vel, p); }
  void off(int note, const Params& p) { send(0x80, (unsigned char)note, 0, p); }
  std::vector<float> run(double seconds, const Params& p)
  {
    const int n = std::max(1, int(seconds * fs));
    std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
    for(int i = 0; i < n; i += 256)
      s->render(p, l.data() + i, r.data() + i, std::min(256, n - i));
    return l;
  }
};

std::vector<float> tail(const std::vector<float>& x, std::size_t from)
{
  return {x.begin() + std::ptrdiff_t(from), x.end()};
}
}

// =========================================================================
TEST_CASE("Nothing allocates after prepare, from the first note", "[viceversa][robustness]")
{
  // First in the file, and on its own: nothing earlier has played
  auto synth = std::make_unique<Synth>();
  std::vector<float> l(256), r(256);
  Params p;
  p.lfos[0].target = ModTarget::X;
  p.lfos[1].target = ModTarget::Cutoff;
  p.lfos[1].shape = LfoShape::Random;
  p.drive = 0.8f;
  p.sanitize();

  synth->prepare(48000.);
  g_allocs = 0;
  g_watch = true;
  const unsigned char on[3]{0x90, 60, 100}, on2[3]{0x91, 67, 90};
  const unsigned char off[3]{0x80, 60, 0}, off2[3]{0x81, 67, 0};
  synth->midi(on, 3, p);
  synth->midi(on2, 3, p);
  for(int b = 0; b < 50; b++)
    synth->render(p, l.data(), r.data(), 256);
  // Every bank, each crossfaded from the one before, the noise among them
  for(int bank = 0; bank < kBankCount; bank++)
  {
    for(auto& s : p.src)
      s.bank = BankId(bank);
    for(int b = 0; b < 4; b++)
      synth->render(p, l.data(), r.data(), 256);
  }
  // Every LFO target, every play mode with its notes, the same rate again
  for(int t = 0; t < kModTargetCount; t++)
  {
    p.lfos[2].target = ModTarget(t);
    for(int b = 0; b < 2; b++)
      synth->render(p, l.data(), r.data(), 256);
  }
  for(int m = 0; m < kPlayModeCount; m++)
  {
    p.play = PlayMode(m);
    synth->midi(off, 3, p);
    synth->midi(on, 3, p);
    for(int b = 0; b < 10; b++)
      synth->render(p, l.data(), r.data(), 256);
  }
  synth->prepare(48000.);
  synth->midi(off, 3, p);
  synth->midi(off2, 3, p);
  for(int b = 0; b < 20; b++)
    synth->render(p, l.data(), r.data(), 256);
  g_watch = false;
  CHECK(g_allocs == 0);
}

TEST_CASE("The mixing law is the Prophet VS's", "[viceversa][mix]")
{
  auto check = [](Weights w, float a, float b, float c, float d) {
    CHECK(std::abs(w.a - a) < 1e-6f);
    CHECK(std::abs(w.b - b) < 1e-6f);
    CHECK(std::abs(w.c - c) < 1e-6f);
    CHECK(std::abs(w.d - d) < 1e-6f);
  };
  // Each corner alone, and the centre a quarter of each
  check(mixLinear(-1.f, 0.f), 1.f, 0.f, 0.f, 0.f);
  check(mixLinear(0.f, 1.f), 0.f, 1.f, 0.f, 0.f);
  check(mixLinear(1.f, 0.f), 0.f, 0.f, 1.f, 0.f);
  check(mixLinear(0.f, -1.f), 0.f, 0.f, 0.f, 1.f);
  check(mixLinear(0.f, 0.f), 0.25f, 0.25f, 0.25f, 0.25f);

  // Always 100 % in all; the equal-power law keeps the power instead
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-1.5f, 1.5f);
  for(int i = 0; i < 2000; i++)
  {
    const float x = u(rng), y = u(rng);
    const auto w = mixLinear(x, y);
    CHECK(std::abs(w.a + w.b + w.c + w.d - 1.f) < 1e-5f);
    CHECK(std::min({w.a, w.b, w.c, w.d}) >= 0.f);
    const auto e = mixEqualPower(x, y);
    CHECK(std::abs(e.a * e.a + e.b * e.b + e.c * e.c + e.d * e.d - 1.f) < 1e-5f);
    // Continuous: a small move, a small change
    const auto w2 = mixLinear(x + 1e-3f, y);
    CHECK(std::abs(w2.a - w.a) + std::abs(w2.b - w.b) + std::abs(w2.c - w.c)
              + std::abs(w2.d - w.d)
          < 5e-3f);
  }
}

TEST_CASE("Every mip level is band-limited", "[viceversa][bank]")
{
  for(const auto& bank : builtinBanks())
  {
    if(bank.noise())
      continue;
    for(int f : {0, kBankFrames / 2, kBankFrames - 1})
    {
      for(int l = 0; l < kLevels; l++)
      {
        const int n = levelSize(l);
        const float* t = bank.table(f, l) + 1;
        std::vector<std::complex<double>> a(static_cast<std::size_t>(n));
        for(int i = 0; i < n; i++)
          a[std::size_t(i)] = t[i];
        fftc(a);
        double in = 0., out = 0.;
        for(int k = 1; k < n / 2; k++)
          (k <= levelHarmonics(l) ? in : out) += std::norm(a[std::size_t(k)]);
        INFO("frame " << f << " level " << l);
        CHECK(out <= in * 1e-12 + 1e-20);
        // The guards wrap
        CHECK(bank.table(f, l)[0] == t[n - 1]);
        CHECK(t[n] == t[0]);
        CHECK(t[n + 1] == t[1]);
      }
    }
  }
}

TEST_CASE("A cycle of any length is analysed exactly", "[viceversa][bank]")
{
  // 600 samples of three harmonics: not a power of two
  const int n = 600;
  std::vector<float> x(n);
  for(int i = 0; i < n; i++)
  {
    const double t = double(i) / n;
    x[std::size_t(i)] = float(0.8 * std::sin(2. * std::numbers::pi * t) + 0.3 * std::cos(2. * std::numbers::pi * 3. * t)
                              - 0.1 * std::sin(2. * std::numbers::pi * 7. * t));
  }
  const Spectrum s = analyse(x);
  CHECK(std::abs(s[0] - std::complex<double>(0., -0.8)) < 1e-6);
  CHECK(std::abs(s[2] - std::complex<double>(0.3, 0.)) < 1e-6);
  CHECK(std::abs(s[6] - std::complex<double>(0., 0.1)) < 1e-6);
  CHECK(std::abs(s[1]) < 1e-6);

  // And read back from the bank: the cubic read of an 8x table
  const Bank b(std::span<const Spectrum>(&s, 1));
  double worst = 0.;
  for(int i = 0; i < 5000; i++)
  {
    const float ph = float(i) / 5000.f;
    const double ref = 0.8 * std::sin(2. * std::numbers::pi * ph) + 0.3 * std::cos(2. * std::numbers::pi * 3. * ph)
                       - 0.1 * std::sin(2. * std::numbers::pi * 7. * ph);
    worst = std::max(worst, std::abs(double(b.read(0.f, Mip{}, ph)) - ref));
  }
  CHECK(worst < 2e-4);
}

TEST_CASE("The mip level is always safe and fades smoothly", "[viceversa][bank]")
{
  int prevLevel = 0;
  float prevBright = 1e9f;
  for(float dx = 1e-5f; dx < kSafeCycles; dx *= 1.001f)
  {
    const auto [level, frac] = Bank::levelFor(Bank::octavesOver(dx));
    // Every harmonic of the level read is under the safe limit
    CHECK(float(levelHarmonics(level)) * dx <= kSafeCycles * 1.0001f);
    REQUIRE(frac >= 0.f);
    REQUIRE(frac <= 1.f);
    // The effective top harmonic never jumps up as the pitch rises
    const float bright = std::log2(float(levelHarmonics(level))) - frac;
    CHECK(bright <= prevBright + 1e-3f);
    const bool smooth
        = prevBright == 1e9f || std::abs(bright - prevBright) < 0.02f + (prevLevel != level ? 0.02f : 0.f);
    CHECK(smooth);
    prevBright = bright;
    prevLevel = level;
  }
}

TEST_CASE("ViceVersa is in tune and does not alias", "[viceversa][aliasing]")
{
  for(double fs : {44100., 48000., 96000.})
  {
    for(int note : {33, 69, 93})
    {
      Rig rig(fs);
      auto p = plain(BankId::SineSaw, 1.f);
      rig.on(note, 127, p);
      const auto x = tail(rig.run(1.2, p), std::size_t(0.2 * fs));
      const double f = 440. * std::exp2((note - 69) / 12.);
      INFO("fs " << fs << " note " << note);
      CHECK(std::abs(1200. * std::log2(measureFrequency(x, f, fs) / f)) < 0.5);
      const double asr = aliasToSignal(x, f, fs, std::min(16000., 0.45 * fs));
      std::printf("viceversa saw at %.0f Hz, %g Hz: aliasing %.1f dB\n", f, fs, asr);
      CHECK(asr < -80.);
    }
  }
}

TEST_CASE("A glide crosses mip levels without clicks", "[viceversa][bank]")
{
  const double fs = 48000.;
  Rig rig(fs);
  auto p = plain(BankId::PulseWidth, 0.5f);
  p.play = PlayMode::Legato;
  p.glide = 1.5f;
  rig.on(30, 127, p);
  rig.run(0.2, p);
  rig.on(100, 127, p);
  const auto x = rig.run(1.5, p);
  // The largest jump of the second difference, against its typical size
  std::vector<double> d2;
  for(std::size_t i = 2; i < x.size(); i++)
    d2.push_back(std::abs(double(x[i]) - 2. * x[i - 1] + x[i - 2]));
  auto sorted = d2;
  std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(sorted.size() * 999 / 1000),
                   sorted.end());
  const double p999 = sorted[sorted.size() * 999 / 1000];
  const double mx = *std::max_element(d2.begin(), d2.end());
  CHECK(mx < 3. * p999);
}

TEST_CASE("The mix envelope travels, loops and releases", "[viceversa][vector]")
{
  VecEnvSpec s;
  s.x = {-1.f, 0.f, 1.f, 0.f, 0.f};
  s.y = {0.f, 1.f, 0.f, -1.f, 0.f};
  s.time = {0.1, 0.2, 0.1, 0.4};
  s.release = 0.05;

  // The points' port round-trips
  const auto again = VecEnvSpec::decode(s.points(), VecEnvSpec{});
  for(int i = 0; i < kVecPoints; i++)
  {
    CHECK(std::abs(again.x[std::size_t(i)] - s.x[std::size_t(i)]) < 1e-6f);
    CHECK(std::abs(again.y[std::size_t(i)] - s.y[std::size_t(i)]) < 1e-6f);
  }

  // Halfway from 0 to 1 at 50 ms, at 1 at 100 ms, at 2 at 300 ms
  VecEnvState e;
  e.start(s);
  e.advance(s, 0.05);
  CHECK(std::abs(e.x + 0.5f) < 1e-4f);
  CHECK(std::abs(e.y - 0.5f) < 1e-4f);
  e.advance(s, 0.25);
  CHECK(std::abs(e.x - 1.f) < 1e-4f);
  // To the end, and stays
  e.advance(s, 10.);
  CHECK(e.done);
  CHECK(e.x == s.x[4]);

  // The loop's menus: a span of at least one move, whatever they hold
  {
    VecEnvSpec m;
    m.setLoop(VecLoop(7), 9, -2);
    CHECK(m.loop == VecLoop::Off);
    CHECK(m.from == 3);
    CHECK(m.to == 4);
    m.setLoop(VecLoop::Round, 2, 1);
    CHECK(m.to == 3);
  }

  // Round between 1 and 3: never passes 3 towards 4 while held
  s.setLoop(VecLoop::Round, 1, 3);
  e.start(s);
  for(int i = 0; i < 2000; i++)
  {
    e.advance(s, 0.005);
    CHECK(!e.done);
  }
  // Released: to point 4 in the release time
  e.release();
  e.advance(s, 0.05);
  CHECK(std::abs(e.x - s.x[4]) < 1e-5f);
  CHECK(std::abs(e.y - s.y[4]) < 1e-5f);

  // Back and forth: 1, 2, 3, 2, 1, 2...
  s.loop = VecLoop::BackAndForth;
  e.start(s);
  e.advance(s, 0.1 + 0.2 + 0.1); // at 3
  CHECK(std::abs(e.y + 1.f) < 1e-4f);
  e.advance(s, 0.1); // back at 2
  CHECK(std::abs(e.x - 1.f) < 1e-4f);
  e.advance(s, 0.2); // back at 1
  CHECK(std::abs(e.y - 1.f) < 1e-4f);
}

TEST_CASE("The ladder filters at 24 dB per octave and stays bounded", "[viceversa][filter]")
{
  const double fs = 48000.;
  auto gainAt = [&](double f, double fc, float k) {
    Ladder lad;
    const double g = std::tan(std::numbers::pi * fc / fs);
    const auto c = Ladder::Coefs::make(vec4(float(g / (1. + g))), vec4(k));
    double e = 0.;
    const int n = 48000;
    for(int i = 0; i < n; i++)
    {
      const float x = 0.01f * float(std::sin(2. * std::numbers::pi * f * i / fs));
      // As the engine has it: drive 1, the passband given back
      const float y = lad.process(vec4(x), c)[0] * (1.f + 0.5f * k);
      if(i >= n / 2)
        e += double(y) * double(y);
    }
    return 10. * std::log10(2. * e / (n / 2) / 1e-4);
  };
  const double pass = gainAt(100., 1000., 0.f);
  const double stop = gainAt(8000., 1000., 0.f);
  CHECK(std::abs(pass) < 0.2);
  CHECK(stop - pass < -66.); // three octaves above, at 24 dB per octave
  // Resonance peaks at the cutoff
  CHECK(gainAt(1000., 1000., 3.5f) > gainAt(1000., 1000., 0.f) + 10.);

  // Full resonance oscillates by itself, at the cutoff, as long as it is
  // left; driven hard, it stays bounded. Below 4 it dies out.
  auto ring = [&](float k, float drive, double& hz) {
    Ladder lad;
    const double fc = 1000.;
    const double g = std::tan(std::numbers::pi * fc / fs);
    const auto c = Ladder::Coefs::make(vec4(float(g / (1. + g))), vec4(k));
    float peak = 0.f;
    double e = 0.;
    int crossings = 0;
    float prev = 0.f;
    const int n = 10 * 48000, from = n - 48000;
    for(int i = 0; i < n; i++)
    {
      const float x = i < 10 ? drive : 0.f;
      const float y = lad.process(vec4(x), c)[0];
      peak = std::max(peak, std::abs(y));
      if(i >= from)
      {
        e += double(y) * y;
        crossings += (prev < 0.f) != (y < 0.f);
      }
      prev = y;
    }
    hz = crossings / 2.;
    CHECK(ossia::safe_isfinite(peak));
    CHECK(peak < 2.f);
    return std::sqrt(e / 48000.);
  };
  double hz = 0.;
  const double still = ring(kMaxFeedback, 4.f, hz);
  CHECK(still > 0.05); // after ten seconds without input
  CHECK(hz == Catch::Approx(1000.).epsilon(0.05));
  CHECK(ring(kMaxFeedback, 0.001f, hz) > 0.05); // from a whisper too
  CHECK(ring(3.9f, 4.f, hz) < 1e-6);
}

TEST_CASE("ViceVersa's vector moves the mix", "[viceversa][vector]")
{
  // At the A corner only A sounds; at C only C
  const double fs = 48000.;
  auto rms = [&](float x, float y) {
    Rig rig(fs);
    auto p = plain(BankId::SineSaw, 0.f);
    p.src[0].level = 1.f;
    for(int s = 1; s < kSources; s++)
    {
      p.src[std::size_t(s)].bank = BankId::SineSaw;
      p.src[std::size_t(s)].coarse = float(12 * s);
    }
    p.x = x;
    p.y = y;
    rig.on(45, 127, p);
    return tail(rig.run(0.5, p), 9600);
  };
  const auto a = rms(-1.f, 0.f);
  const auto c = rms(1.f, 0.f);
  auto level = [&](const std::vector<float>& v, double f) {
    std::complex<double> acc{};
    for(std::size_t i = 0; i < v.size(); i++)
      acc += double(v[i]) * std::polar(1., -2. * std::numbers::pi * f * double(i) / fs);
    return std::abs(acc) / double(v.size());
  };
  CHECK(level(a, 110.) > 100. * level(a, 440.));
  CHECK(level(c, 440.) > 100. * level(c, 110.));
}

TEST_CASE("ViceVersa's voices end, and any input is safe", "[viceversa][robustness]")
{
  std::mt19937 rng(77);
  std::uniform_real_distribution<float> u(-1e6f, 1e6f);
  std::uniform_int_distribution<int> byte(0, 255);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for(int trial = 0; trial < 20; trial++)
  {
    Rig rig(trial % 2 ? 44100. : 96000.);
    std::vector<float> l(512), r(512);
    Params p;
    auto pick = [&] {
      const int k = byte(rng) % 8;
      return k == 0 ? nan : k == 1 ? (byte(rng) % 2 ? inf : -inf) : u(rng);
    };
    for(auto& s : p.src)
    {
      s.bank = BankId(byte(rng) % 30 - 3);
      s.frame = pick();
      s.coarse = pick();
      s.fine = pick();
      s.level = pick();
    }
    p.x = pick();
    p.y = pick();
    p.law = MixLaw(byte(rng) % 3);
    std::vector<float> e(kVecEnvSize);
    for(auto& v : e)
      v = byte(rng) % 3 ? float(byte(rng)) / 255.f : pick();
    p.vecEnv = VecEnvSpec::decode(e, VecEnvSpec::centred());
    p.vecEnv.setLoop(VecLoop(byte(rng) % 5 - 1), byte(rng) % 7 - 1, byte(rng) % 7 - 1);
    for(auto& t : p.vecEnv.time)
      t = byte(rng) % 2 ? double(pick()) : 1e-3 * byte(rng);
    p.vecEnv.release = byte(rng) % 2 ? double(pick()) : 1e-3 * byte(rng);
    p.vecEnvDepth = pick();
    for(auto& l : p.lfos)
    {
      l.target = ModTarget(byte(rng) % 20 - 3);
      l.shape = LfoShape(byte(rng) % 10 - 2);
      // Half of them in range, so that they move things
      const bool real = byte(rng) % 2;
      l.period = real ? 0.02f + 0.01f * byte(rng) : pick();
      l.depth = real ? byte(rng) / 255.f : pick();
      l.phase = real ? byte(rng) / 255.f : pick();
      l.delay = real ? 0.002f * byte(rng) : pick();
    }
    p.frameEnv = pick();
    p.cutoff = pick();
    p.resonance = pick();
    p.drive = pick();
    p.filterEnv = pick();
    p.keyTrack = pick();
    p.aA = pick();
    p.aR = pick();
    p.fD = pick();
    p.play = PlayMode(byte(rng) % 4);
    p.glide = pick();
    p.bendRange = pick();
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
      // The spread across 0, and the loop's span moved under the voices
      if(block % 8 == 0)
      {
        p.spread = byte(rng) % 3 == 0 ? 0.f : byte(rng) / 255.f;
        p.vecEnv.setLoop(p.vecEnv.loop, byte(rng) % 4, byte(rng) % 5);
      }
      const int n = 1 + byte(rng) % 512;
      rig.s->render(p, l.data(), r.data(), n);
      for(int i = 0; i < n; i++)
      {
        REQUIRE(ossia::safe_isfinite(l[std::size_t(i)]));
        REQUIRE(ossia::safe_isfinite(r[std::size_t(i)]));
        REQUIRE(std::abs(l[std::size_t(i)]) <= 1.f);
        REQUIRE(std::abs(r[std::size_t(i)]) <= 1.f);
      }
    }
    g_watch = false;
    CHECK(g_allocs == 0);

    for(int ch = 0; ch < 16; ch++)
    {
      rig.send((unsigned char)(0xB0 | ch), 123, 0, p);
      rig.send((unsigned char)(0xB0 | ch), 64, 0, p);
    }
    p = Params{};
    p.sanitize();
    for(int i = 0; i < 200; i++)
      rig.s->render(p, l.data(), r.data(), 512);
    CHECK(rig.s->activeVoices() == 0);
  }

  // Every port's range is the sanitizer's, and NaN its default
  {
    Params q;
    q.vecEnv.time = {nan, 1e9, -1., 0.};
    q.vecEnv.release = std::numeric_limits<double>::infinity();
    q.lfos[0].period = 1000.f;
    q.lfos[0].delay = 100.f;
    q.glide = 100.f;
    q.aA = 100.f;
    q.sanitize();
    CHECK(q.vecEnv.time[0] == 1.);
    CHECK(q.vecEnv.time[1] == 20.);
    CHECK(q.vecEnv.time[2] == 0.001);
    CHECK(q.vecEnv.release == 0.3);
    CHECK(q.lfos[0].period == 20.f);
    CHECK(q.lfos[0].delay == 4.f);
    CHECK(q.glide == 2.f);
    CHECK(q.aA == 10.f);
  }
}

TEST_CASE("ViceVersa's cost", "[viceversa][.bench]")
{
  Rig rig(48000.);
  Params p;
  p.x = 0.f;
  p.y = 0.f; // all four sources heard
  p.aS = 1.f;
  p.sanitize();
  for(int n = 0; n < kMaxVoices; n++)
    rig.on(40 + 3 * n, 100, p);
  std::vector<float> l(256), r(256);
  const int blocks = int(5. * 48000. / 256.);
  const auto t0 = std::chrono::steady_clock::now();
  for(int b = 0; b < blocks; b++)
    rig.s->render(p, l.data(), r.data(), 256);
  const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("viceversa, 16 voices, four sources heard: %.2f %% of one core at 48 kHz\n",
              100. * took / 5.);
}

TEST_CASE("ViceVersa's spread is balanced in the centre, at any pitch",
          "[viceversa][spread]")
{
  const double fs = 48000.;
  auto levels = [&](int note, float x, float y, float& l, float& r) {
    Rig rig(fs);
    auto p = plain(BankId::SineSaw, 1.f);
    for(auto& s : p.src)
      s.bank = BankId::SineSaw, s.frame = 1.f;
    p.src[1].fine = 7.f; // detuned, as Wide Saws
    p.src[3].fine = -7.f;
    p.x = x;
    p.y = y;
    p.spread = 1.f;
    p.sanitize();
    rig.on(note, 100, p);
    const int n = int(0.5 * fs);
    std::vector<float> left(static_cast<std::size_t>(n)), right(static_cast<std::size_t>(n));
    for(int i = 0; i < n; i += 256)
      rig.s->render(p, left.data() + i, right.data() + i, std::min(256, n - i));
    double el = 0., er = 0.;
    for(int i = 4800; i < n; i++)
    {
      el += double(left[std::size_t(i)]) * left[std::size_t(i)];
      er += double(right[std::size_t(i)]) * right[std::size_t(i)];
    }
    l = float(10. * std::log10(el));
    r = float(10. * std::log10(er));
  };
  float l, r;
  for(int note : {36, 48, 60, 84})
  {
    levels(note, 0.f, 0.f, l, r);
    INFO("note " << note);
    CHECK(std::abs(l - r) < 0.5f);
  }
  // The point at A: to the left; at C: to the right
  levels(60, -1.f, 0.f, l, r);
  CHECK(l > r + 6.f);
  levels(60, 1.f, 0.f, l, r);
  CHECK(r > l + 6.f);
}
TEST_CASE("Every bank sounds, at its level, and the noise darkens", "[viceversa][bank]")
{
  // Each bank, frames first and last: finite, and the RMS of a sine of 0.7
  const double fs = 48000.;
  for(int b = 0; b < kBankCount; b++)
  {
    for(float frame : {0.f, 1.f})
    {
      Rig rig(fs);
      auto p = plain(BankId(b), frame);
      rig.on(60, 127, p);
      const auto x = tail(rig.run(0.6, p), std::size_t(0.2 * fs));
      double e = 0.;
      for(float v : x)
      {
        REQUIRE(ossia::safe_isfinite(v));
        e += double(v) * v;
      }
      const double db = 10. * std::log10(e / double(x.size()));
      INFO("bank " << b << " frame " << frame);
      CHECK(db > -30.);
      CHECK(db < -6.);
    }
  }

  // Noise: frame 0 is white, frame 1 has little left above 4 kHz
  auto highShare = [&](float frame) {
    Rig rig(fs);
    auto p = plain(BankId::Noise, frame);
    rig.on(60, 127, p);
    const auto x = tail(rig.run(1.0, p), std::size_t(0.2 * fs));
    const auto pw = power(x, 1 << 15);
    double hi = 0., all = 0.;
    for(std::size_t i = 1; i < pw.size(); i++)
    {
      all += pw[i];
      if(double(i) * fs / double(1 << 15) > 4000.)
        hi += pw[i];
    }
    return hi / all;
  };
  CHECK(highShare(0.f) > 0.6);
  CHECK(highShare(1.f) < 0.05);
}

TEST_CASE("The noise's colour is the same at any rate", "[viceversa][bank]")
{
  // The frequency under which half of the noise's power is, and its level,
  // averaged over eight spectra of 2^16 samples. Each colour is a one-pole
  // of a corner in Hz: a pole kept per sample would put it twice as high at
  // 96 kHz as at 48.
  struct Colour
  {
    double median, db;
  };
  auto colour = [](double fs, float frame) {
    Rig rig(fs);
    auto p = plain(BankId::Noise, frame);
    rig.on(60, 127, p);
    const std::size_t n = 1 << 16, segments = 8;
    const auto x
        = tail(rig.run(0.1 + double(segments * n) / fs, p), std::size_t(0.1 * fs));
    std::vector<double> pw(n / 2);
    for(std::size_t k = 0; k < segments; k++)
    {
      const auto seg = power(tail(x, k * n), n);
      for(std::size_t i = 0; i < pw.size(); i++)
        pw[i] += seg[i];
    }
    double all = 0.;
    for(std::size_t i = 1; i < pw.size(); i++)
      all += pw[i];
    double acc = 0., median = 0.;
    for(std::size_t i = 1; i < pw.size(); i++)
    {
      acc += pw[i];
      if(acc >= 0.5 * all)
      {
        median = double(i) * fs / double(n);
        break;
      }
    }
    double e = 0.;
    for(std::size_t i = 0; i < segments * n; i++)
      e += double(x[i]) * x[i];
    return Colour{median, 10. * std::log10(e / double(segments * n))};
  };
  for(float frame : {0.6f, 0.8f, 0.95f, 1.f})
  {
    const Colour ref = colour(48000., frame);
    for(double fs : {44100., 96000.})
    {
      const Colour c = colour(fs, frame);
      INFO("frame " << frame << " at " << fs << " Hz: half the power under " << c.median
                    << " Hz, " << c.db << " dB; at 48 kHz " << ref.median << " Hz, "
                    << ref.db << " dB");
      CHECK(std::abs(c.median / ref.median - 1.) < 0.1);
      CHECK(std::abs(c.db - ref.db) < 0.5);
    }
  }
}

TEST_CASE("A bank changed under a note crossfades instead of clicking", "[viceversa][bank]")
{
  // Two smooth waves, a sine and the Organ's first registration (its 2nd and
  // 4th harmonics), then both further along their frames, where the old bank
  // is read at its own frame: only a jump at the change would make a large
  // step
  const double fs = 48000.;
  const std::size_t fade = std::size_t(0.005 * fs);
  auto maxStep = [](const std::vector<float>& x, std::size_t from, std::size_t to) {
    double m = 0.;
    for(std::size_t i = std::max<std::size_t>(from, 1); i < to; i++)
      m = std::max(m, std::abs(double(x[i]) - x[i - 1]));
    return m;
  };
  for(float frame : {0.f, 0.4f})
  for(auto [a, b] : {std::pair{BankId::SineSaw, BankId::Organ},
                     std::pair{BankId::Organ, BankId::SineSaw}})
  {
    // The change at as many places in the waves' cycles
    for(int k = 0; k < 8; k++)
    {
      Rig rig(fs);
      auto p = plain(a, frame);
      rig.on(48, 127, p);
      auto x = rig.run((14400.5 + 37. * k) / fs, p);
      const std::size_t at = x.size();
      p.src[0].bank = b;
      const auto y = rig.run(0.2, p);
      x.insert(x.end(), y.begin(), y.end());
      const double before = maxStep(x, at - 4800, at);
      const double after = maxStep(x, at + 2 * fade, at + 2 * fade + 4800);
      const double across = maxStep(x, at, at + 2 * fade);
      INFO("frame " << frame << ", bank " << int(a) << " to " << int(b) << " after " << at
                    << " samples: steps "
                   << before << " before, " << across << " across, " << after << " after");
      CHECK(before > 0.001);
      CHECK(after > 0.001);
      CHECK(across < 1.25 * std::max(before, after));
    }
  }
}

TEST_CASE("Every bank has as many frames", "[viceversa][bank]")
{
  // A frame position is the same place in any bank: a crossfade reads the
  // old bank where the new one starts
  for(int b = 0; b < kBankCount; b++)
  {
    INFO("bank " << b);
    CHECK(builtinBanks()[std::size_t(b)].frames() == kBankFrames);
  }
}

TEST_CASE("A voice started during a bank's fade plays the new bank alone", "[viceversa][bank]")
{
  // One voice: the new note steals it, and starts 3 ms into the 5 ms fade.
  // Against a synth that had the new bank all along, it is the same note.
  // The first note is 40 dB down, so that what it leaves in the DC blocker
  // does not tell the two apart.
  const double fs = 48000.;
  auto play = [&](BankId first) {
    Rig rig(fs);
    auto p = plain(first, 0.6f);
    p.voices = 1;
    p.velLevel = 1.f;
    rig.on(48, 1, p);
    rig.run(0.3, p);
    p.src[0].bank = BankId::Organ;
    rig.on(60, 127, p);
    return rig.run(0.02, p);
  };
  const auto changed = play(BankId::SineSaw), ref = play(BankId::Organ);
  double diff = 0., peak = 0.;
  for(auto i = std::size_t(0.004 * fs); i < std::size_t(0.005 * fs); i++)
  {
    diff = std::max(diff, std::abs(double(changed[i]) - ref[i]));
    peak = std::max(peak, std::abs(double(ref[i])));
  }
  INFO("difference " << diff << ", peak " << peak);
  CHECK(peak > 0.05);
  CHECK(diff < 0.02 * peak);
}

TEST_CASE("A bank changed again during a fade gets there", "[viceversa][bank]")
{
  // The second change waits for the first fade, then fades in turn: after
  // both, the note is the last bank's, as if it had played it all along
  const double fs = 48000.;
  Rig rig(fs), ref(fs);
  auto p = plain(BankId::SineSaw, 0.5f), q = plain(BankId::Glass, 0.5f);
  rig.on(48, 127, p);
  ref.on(48, 127, q);
  rig.run(0.3, p);
  ref.run(0.3, q);
  p.src[0].bank = BankId::Organ;
  rig.run(0.001, p);
  ref.run(0.001, q);
  p.src[0].bank = BankId::Glass;
  const auto x = rig.run(0.1, p), y = ref.run(0.1, q);
  double diff = 0., peak = 0.;
  for(auto i = std::size_t(0.05 * fs); i < x.size(); i++)
  {
    diff = std::max(diff, std::abs(double(x[i]) - y[i]));
    peak = std::max(peak, std::abs(double(y[i])));
  }
  INFO("difference " << diff << ", peak " << peak);
  CHECK(peak > 0.05);
  CHECK(diff < 0.01 * peak);
}

TEST_CASE("A fade to or from the noise keeps the noise's colour moving", "[viceversa][bank]")
{
  // The colour follows a fast LFO on the frame: while the noise fades out,
  // its pole must still follow it, or it drifts on its last slope, out of
  // the unit circle
  const double fs = 48000.;
  auto rms = [](const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.;
    for(std::size_t i = from; i < to; i++)
      e += double(x[i]) * x[i];
    return std::sqrt(e / double(to - from));
  };
  for(auto [a, b] : {std::pair{BankId::Noise, BankId::SineSaw},
                     std::pair{BankId::SineSaw, BankId::Noise}})
  {
    for(int k = 0; k < 8; k++)
    {
      Rig rig(fs);
      auto p = plain(a, 0.5f);
      p.lfos[0].target = ModTarget::FrameA;
      p.lfos[0].shape = LfoShape::Triangle;
      p.lfos[0].period = 0.02f;
      p.lfos[0].depth = 1.f;
      p.sanitize();
      rig.on(48, 127, p);
      auto x = rig.run((14400.5 + 61. * k) / fs, p);
      const std::size_t at = x.size();
      p.src[0].bank = b;
      const auto y = rig.run(0.2, p);
      x.insert(x.end(), y.begin(), y.end());
      for(float v : x)
        REQUIRE(ossia::safe_isfinite(v));
      // The noise's level moves with its colour: against the loudest and
      // the average 10 ms of the steady parts
      auto loudest = [&](std::size_t from, std::size_t to) {
        double m = 0.;
        for(std::size_t i = from; i + 480 <= to; i += 240)
          m = std::max(m, rms(x, i, i + 480));
        return m;
      };
      const double before = loudest(at - 4800, at), after = loudest(at + 2400, at + 7200);
      const double quiet = std::min(rms(x, at - 4800, at), rms(x, at + 2400, at + 7200));
      const double across = rms(x, at, at + 480);
      INFO("bank " << int(a) << " to " << int(b) << " after " << at << " samples: rms "
                   << across << " across; loudest 10 ms " << before << " before, " << after
                   << " after");
      CHECK(across < 1.5 * std::max(before, after));
      CHECK(across > 0.25 * quiet);
    }
  }
}

TEST_CASE("The sampled banks keep their recorded spectra", "[viceversa][bank]")
{
  // Frame by frame, the table's harmonics are the stored magnitudes, scaled
  for(BankId id : {BankId::Brass, BankId::Strings, BankId::Voices, BankId::Winds})
  {
    const auto& bank = builtinBanks()[std::size_t(id)];
    const Spectrum want = banks::frame(id, 0.f);
    const int n = levelSize(0);
    const float* t = bank.table(0, 0) + 1;
    std::vector<std::complex<double>> a(static_cast<std::size_t>(n));
    for(int i = 0; i < n; i++)
      a[std::size_t(i)] = t[i];
    fftc(a);
    // Compare shapes: each harmonic against the fundamental
    const double ref = std::abs(a[1]) / std::max(1e-12, std::abs(want[0]));
    for(int h = 1; h <= 24; h++)
    {
      const double got = std::abs(a[std::size_t(h)]) * 2. / n;
      const double exp = std::abs(want[std::size_t(h - 1)]) * ref * 2. / n;
      INFO("bank " << int(id) << " harmonic " << h);
      CHECK(std::abs(got - exp) < 1e-3 + 1e-3 * exp);
    }
  }
}

//! What the host's painter sends a widget: crousti's custom_mouse_event
struct Click
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
Click click(double x, double y, enum Click::button b = Click::left)
{
  return {float(x), float(y), b};
}

TEST_CASE("The pad moves the envelope's points or the Position, and shows the voices",
          "[viceversa][ui]")
{
  VectorPad pad;
  std::vector<std::vector<float>> envs;
  std::vector<VectorPad::Point> moves;
  int starts = 0, commits = 0;
  auto& env = std::get<0>(pad.transactions);
  auto& pos = std::get<1>(pad.transactions);
  env.start = pos.start = [&] { starts++; };
  env.commit = pos.commit = [&] { commits++; };
  env.update = [&](const std::vector<float>& v) {
    envs.push_back(v);
    std::get<0>(pad.values) = v;
  };
  pos.update = [&](const VectorPad::Point& p) {
    moves.push_back(p);
    std::get<1>(pad.values) = p;
  };
  std::get<0>(pad.values) = VecEnvSpec::centred().points();
  pad.sync();

  // Not with the right button
  CHECK(!pad.mouse_press(click(VectorPad::cx(), VectorPad::cy(), Click::right)));
  CHECK(starts == 0);
  // Points: drag point 4 (at the centre) to C, the east corner. Grabbed off
  // its centre: it moves as the cursor does, without first jumping to it
  CHECK(pad.mouse_press(click(VectorPad::cx() + 4., VectorPad::cy() - 3.)));
  pad.mouse_move(VectorPad::cx() + 4., VectorPad::cy() - 3.);
  REQUIRE(!envs.empty());
  {
    const auto still = VecEnvSpec::decode(envs.back(), VecEnvSpec::centred());
    CHECK(std::abs(still.x[4]) < 1e-4f);
    CHECK(std::abs(still.y[4]) < 1e-4f);
  }
  pad.mouse_move(VectorPad::px(1.f) + 4., VectorPad::cy() - 3.);
  pad.mouse_release(0., 0.);
  REQUIRE(!envs.empty());
  const auto spec = VecEnvSpec::decode(envs.back(), VecEnvSpec::centred());
  CHECK(std::abs(spec.x[4] - 1.f) < 1e-4f);
  CHECK(moves.empty());
  CHECK(starts == 1);
  CHECK(commits == 1);

  // The second button: a press anywhere in the diamond puts the Position
  // there, a drag follows, and the envelope is left alone
  const double button = VectorPad::kButton * 0.5;
  CHECK(pad.mouse_press(click(VectorPad::buttonX() + button, VectorPad::buttonY(1) + button)));
  CHECK(pad.target == VectorPad::Position);
  const auto before = envs.size();
  CHECK(pad.mouse_press(click(VectorPad::cx(), VectorPad::py(0.5f))));
  pad.mouse_move(VectorPad::px(-2.f), VectorPad::cy()); // outside: onto the edge
  pad.mouse_release(0., 0.);
  REQUIRE(moves.size() == 2);
  CHECK(std::abs(moves[0].y - 0.5f) < 1e-4f);
  CHECK(std::abs(moves[1].x + 1.f) < 1e-4f);
  CHECK(envs.size() == before);
  CHECK(commits == 2);
  // The first button: back to the points
  CHECK(pad.mouse_press(click(VectorPad::buttonX() + button, VectorPad::buttonY(0) + button)));
  CHECK(pad.target == VectorPad::Points);

  // The host's reset, on transport stop, mid-drag: the gesture is undone,
  // and the voices' dots go
  int rollbacks = 0;
  env.rollback = pos.rollback = [&] { rollbacks++; };
  int repaints = 0;
  pad.update = [&] { repaints++; };
  pad.live.count = 3;
  REQUIRE(pad.mouse_press(click(VectorPad::px(1.f), VectorPad::cy())));
  pad.mouse_move(VectorPad::px(0.5f), VectorPad::cy());
  pad.reset();
  CHECK(rollbacks == 1);
  CHECK(pad.drag < 0);
  CHECK(pad.live.count == 0);
  CHECK(repaints > 0);
  CHECK(commits == 2);

  // The loop comes from its menus
  std::get<2>(pad.values) = int(VecLoop::BackAndForth);
  std::get<3>(pad.values) = 2;
  std::get<4>(pad.values) = 3; // "4"
  pad.sync();
  CHECK(pad.spec.loop == VecLoop::BackAndForth);
  CHECK(pad.spec.from == 2);
  CHECK(pad.spec.to == 4);

  // The sounding voices' points, as the processor sends them
  ViceVersa vv;
  vv.prepare({.frames = 256, .rate = 48000.});
  Params p = vv.params();
  p.x = 0.25f;
  p.y = -0.5f;
  p.vecEnvDepth = 0.f;
  vv.synth.midi(std::array<unsigned char, 3>{0x90, 60, 100}.data(), 3, p);
  float l[256], r[256];
  vv.synth.render(p, l, r, 256);
  const auto live = vv.synth.livePoints();
  REQUIRE(live.count == 1);
  CHECK(std::abs(live.x[0] - 0.25f) < 1e-4f);
  CHECK(std::abs(live.y[0] + 0.5f) < 1e-4f);
}

TEST_CASE("Two LFOs move the point over the diamond, not along a line", "[viceversa][lfo]")
{
  const double fs = 48000.;
  // Where the voice's point goes over many cycles, for a shape
  // LFO 1 on X, LFO 2 on Y, `phase` cycles apart
  auto trace = [&](LfoShape shape, float phase = 0.25f) {
    Rig rig(fs);
    auto p = plain(BankId::SineSaw, 0.f);
    p.x = 0.f; // from the centre
    p.lfos[0] = LfoParams{ModTarget::X, shape, 0.05f, 1.f, 0.f, 0.f};
    p.lfos[1] = LfoParams{ModTarget::Y, shape, 0.05f, 1.f, phase, 0.f};
    p.sanitize();
    rig.on(60, 100, p);
    std::vector<std::array<float, 2>> pts;
    for(int i = 0; i < 400; i++)
    {
      rig.run(0.0071, p); // not a divisor of the period: every phase is seen
      const auto live = rig.s->livePoints();
      REQUIRE(live.count == 1);
      pts.push_back({live.x[0], live.y[0]});
    }
    return pts;
  };
  // On a line through the centre, x and y are proportional: |corr| = 1
  auto corr = [](const std::vector<std::array<float, 2>>& pts) {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, n = double(pts.size());
    for(auto [x, y] : pts)
    {
      sx += x;
      sy += y;
      sxx += x * x;
      syy += y * y;
      sxy += x * y;
    }
    const double cxy = sxy / n - sx * sy / n / n;
    return cxy / std::sqrt((sxx / n - sx * sx / n / n) * (syy / n - sy * sy / n / n));
  };
  // Sine: a circle, clipped by the diamond; triangle: the diamond's edge
  CHECK(std::abs(corr(trace(LfoShape::Sine))) < 0.2);
  const auto tri = trace(LfoShape::Triangle);
  CHECK(std::abs(corr(tri)) < 0.2);
  for(auto [x, y] : tri)
    CHECK(std::abs(std::abs(x) + std::abs(y) - 1.f) < 0.05f);
  // Random: anywhere
  const auto rnd = trace(LfoShape::Random);
  CHECK(std::abs(corr(rnd)) < 0.3);
  int quadrants[4]{};
  for(auto [x, y] : rnd)
    quadrants[(x > 0.f ? 1 : 0) + (y > 0.f ? 2 : 0)]++;
  for(int q : quadrants)
    CHECK(q > 40);

  // In phase, a line; half a cycle apart, the other diagonal. Two random
  // LFOs are independent at any phase
  CHECK(corr(trace(LfoShape::Sine, 0.f)) > 0.99);
  CHECK(corr(trace(LfoShape::Triangle, 0.5f)) < -0.99);
  CHECK(std::abs(corr(trace(LfoShape::Random, 0.f))) < 0.3);
  CHECK(std::abs(corr(trace(LfoShape::Random, 0.6f))) < 0.3);
}

TEST_CASE("Each LFO target moves what it names", "[viceversa][lfo]")
{
  const double fs = 48000.;
  // One LFO on `target`, a square at depth 1: its two halves differ in what
  // the target moves, and nothing else does. The four sources are alike,
  // unless `setup` makes them differ
  auto halves = [&](ModTarget target, auto measure, auto setup) {
    Rig rig(fs);
    auto p = plain(BankId::SineSaw, 0.5f);
    p.x = 0.f;
    for(int s = 1; s < kSources; s++)
      p.src[std::size_t(s)] = p.src[0];
    p.lfos[0] = LfoParams{target, LfoShape::Square, 0.4f, 1.f, 0.f, 0.f};
    setup(p);
    p.sanitize();
    rig.on(57, 100, p);
    rig.run(0.05, p);
    const auto hi = measure(rig.run(0.1, p)); // first half: +1
    rig.run(0.1, p);
    const auto lo = measure(rig.run(0.1, p)); // second half: -1
    return std::pair{hi, lo};
  };
  auto same = [](Params&) {};
  auto rms = [](const std::vector<float>& v) {
    double e = 0.;
    for(float x : v)
      e += double(x) * x;
    return std::sqrt(e / double(v.size()));
  };
  // Zero crossings: the pitch, or the harmonic sounding
  auto crossings = [](const std::vector<float>& v) {
    int n = 0;
    for(std::size_t i = 1; i < v.size(); i++)
      n += (v[i - 1] < 0.f) != (v[i] < 0.f);
    return double(n);
  };
  // What is above 2 kHz, against the whole
  auto bright = [&](const std::vector<float>& v) {
    const auto pw = power(v, 4096);
    double hi = 0., all = 0.;
    for(std::size_t i = 1; i < pw.size(); i++)
    {
      all += pw[i];
      if(double(i) * fs / 4096. > 2000.)
        hi += pw[i];
    }
    return hi / all;
  };
  {
    const auto [hi, lo] = halves(ModTarget::Pitch, crossings, same);
    CHECK(hi / lo == Catch::Approx(4.).epsilon(0.1)); // ±1 octave
  }
  {
    const auto [hi, lo] = halves(ModTarget::Level, rms, same);
    CHECK(lo < 0.01 * hi); // down to silence
  }
  {
    const auto [hi, lo] = halves(ModTarget::Off, rms, same);
    CHECK(hi == Catch::Approx(lo).epsilon(0.05));
  }
  {
    // Cutoff ±4 octaves around 1 kHz
    const auto [hi, lo] = halves(ModTarget::Cutoff, bright, [](Params& p) {
      for(auto& s : p.src)
        s.frame = 1.f; // a saw
      p.cutoff = 1000.f;
    });
    CHECK(hi > 30. * lo);
  }
  {
    // X: from the A corner to C, two octaves apart; Y: from B to D, likewise
    auto spread = [](Params& p) {
      const float coarse[kSources] = {0.f, 12.f, 24.f, -12.f};
      for(int s = 0; s < kSources; s++)
        p.src[std::size_t(s)].coarse = coarse[s];
      for(auto& s : p.src)
        s.frame = 0.f; // sines
    };
    const auto [xhi, xlo] = halves(ModTarget::X, crossings, spread);
    CHECK(xhi / xlo == Catch::Approx(4.).epsilon(0.1)); // C (24) over A (0)
    const auto [yhi, ylo] = halves(ModTarget::Y, crossings, spread);
    CHECK(yhi / ylo == Catch::Approx(4.).epsilon(0.1)); // B (12) over D (-12)
  }
  // Each source's own: heard alone, at its corner, it moves; the others not
  const float cornerX[kSources] = {-1.f, 0.f, 1.f, 0.f};
  const float cornerY[kSources] = {0.f, 1.f, 0.f, -1.f};
  for(int which = 0; which < kSources; which++)
  {
    for(int heard = 0; heard < kSources; heard++)
    {
      auto corner = [&](BankId bank, float frame) {
        return [&, bank, frame](Params& p) {
          p.x = cornerX[heard];
          p.y = cornerY[heard];
          for(auto& s : p.src)
          {
            s.bank = bank;
            s.frame = frame;
          }
        };
      };
      INFO("target source " << which << ", heard " << heard);
      const auto [phi, plo] = halves(
          ModTarget(int(ModTarget::PitchA) + which), crossings, corner(BankId::SineSaw, 0.f));
      if(which == heard)
        CHECK(phi / plo == Catch::Approx(4.).epsilon(0.1));
      else
        CHECK(phi / plo == Catch::Approx(1.).epsilon(0.02));
      // Frame: one harmonic at a time, the 24th, then the 1st
      const auto [fhi, flo] = halves(
          ModTarget(int(ModTarget::FrameA) + which), crossings,
          corner(BankId::HarmonicSweep, 0.5f));
      if(which == heard)
        CHECK(fhi / flo > 15.);
      else
        CHECK(fhi / flo == Catch::Approx(1.).epsilon(0.02));
    }
  }
}

namespace
{
//! A sine at `cycles` per sample's amplitude in x, through a Hann window
double amplitudeAt(const std::vector<float>& x, double cycles)
{
  std::complex<double> acc{};
  double wsum = 0.;
  const std::size_t n = x.size();
  for(std::size_t i = 0; i < n; i++)
  {
    const double w = 0.5 - 0.5 * std::cos(2. * std::numbers::pi * double(i) / double(n));
    acc += w * double(x[i]) * std::polar(1., -2. * std::numbers::pi * cycles * double(i));
    wsum += w;
  }
  return 2. * std::abs(acc) / wsum;
}

double rmsOf(const std::vector<float>& v, std::size_t from = 0)
{
  double e = 0.;
  for(std::size_t i = from; i < v.size(); i++)
    e += double(v[i]) * v[i];
  return std::sqrt(e / double(std::max<std::size_t>(1, v.size() - from)));
}

void cc(Rig& rig, int number, int value, const Params& p)
{
  rig.send(0xB0, (unsigned char)number, (unsigned char)value, p);
}
}

TEST_CASE("The lanes' read is the scalar read's", "[viceversa][bank]")
{
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  for(BankId id : {BankId::SineSaw, BankId::Sync, BankId::Strings})
  {
    const auto& b = builtinBanks()[std::size_t(id)];
    double worst = 0.;
    for(int trial = 0; trial < 20000; trial++)
    {
      std::array<Mip, kLanes> mip;
      alignas(16) std::array<float, kLanes> frame, x;
      for(std::size_t l = 0; l < kLanes; l++)
      {
        mip[l] = {int(u(rng) * kLevels) % kLevels, u(rng) < 0.5f ? 0.f : u(rng)};
        frame[l] = u(rng) * float(b.frames() - 1);
        x[l] = u(rng) * 0.999999f;
      }
      if(trial % 7 == 0)
        frame[0] = float(b.frames() - 1); // the last frame, alone
      Bank::Lanes t;
      b.lanes(t, mip);
      const vec4 got = Bank::read(t, kfr::read<kLanes, true>(frame.data()),
                                  kfr::read<kLanes, true>(x.data()));
      for(std::size_t l = 0; l < kLanes; l++)
      {
        const Mip m{mip[l].level, mip[l].level >= kLevels - 1 ? 0.f : mip[l].frac};
        worst = std::max(worst, double(std::abs(got[l] - b.read(frame[l], m, x[l]))));
      }
    }
    INFO("bank " << int(id));
    CHECK(worst < 2e-6);
  }
}

TEST_CASE("The mip levels keep the top of the spectrum", "[viceversa][bank]")
{
  // The saw: each harmonic read at a note's level is the full table's, at
  // 12 to 14 kHz too, except over the last sixth of an octave before a
  // level switches, where it fades to the next
  const double fs = 48000.;
  const auto& bank = builtinBanks()[std::size_t(BankId::SineSaw)];
  const int last = bank.frames() - 1;
  const int n0 = levelSize(0);
  std::vector<std::complex<double>> a(static_cast<std::size_t>(n0));
  for(int i = 0; i < n0; i++)
    a[std::size_t(i)] = bank.table(last, 0)[i + 1];
  fftc(a);
  int full = 0, faded = 0;
  for(double note = 57.; note <= 93.; note += 0.25)
  {
    const double f0 = 440. * std::exp2((note - 69.) / 12.);
    const float dx = float(f0 / fs);
    const Mip mip = Bank::levelFor(Bank::octavesOver(dx));
    (mip.frac > 0.f ? faded : full)++;
    if(mip.frac > 0.f)
      continue;
    // The highest harmonic at 12 to 14 kHz that the level has
    const int h = std::min(levelHarmonics(mip.level), int(14000. / f0));
    if(h * f0 < 12000.)
      continue;
    std::vector<float> x(16384);
    Bank::Lanes t;
    bank.lanes(t, {mip, mip, mip, mip});
    float ph = 0.f;
    for(auto& v : x)
    {
      v = Bank::read(t, vec4(float(last)), vec4(ph))[0];
      ph += dx;
      ph -= ph >= 1.f ? 1.f : 0.f;
    }
    const double want = std::abs(a[std::size_t(h)]) * 2. / n0;
    const double got = amplitudeAt(x, double(h) * dx);
    INFO("note " << note << " harmonic " << h << " at " << h * f0 << " Hz");
    CHECK(std::abs(20. * std::log10(got / want)) < 1.);
  }
  // Five sixths of the notes read one level only
  CHECK(double(full) / double(full + faded) > 0.8);
  // A4: up to its 32nd harmonic, 14 kHz, as it was not
  CHECK(Bank::levelFor(Bank::octavesOver(440.f / 48000.f)).frac == 0.f);
}

TEST_CASE("The drive does not alias", "[viceversa][aliasing]")
{
  // A saw at full drive, the filter open: the saturator's harmonics fold
  // back from twice the rate
  for(int note : {72, 84, 96})
  {
    const double fs = 48000.;
    Rig rig(fs);
    auto p = plain(BankId::SineSaw, 1.f);
    p.drive = 1.f;
    rig.on(note, 127, p);
    const auto x = tail(rig.run(1.2, p), std::size_t(0.2 * fs));
    const double f = 440. * std::exp2((note - 69) / 12.);
    const double asr = aliasToSignal(x, f, fs, 16000.);
    std::printf("viceversa saw at %.0f Hz, drive 1: aliasing %.1f dB\n", f, asr);
    INFO("note " << note);
    CHECK(asr < (note == 96 ? -54. : note == 84 ? -57. : -60.));
  }
}

TEST_CASE("The filter's sweep, the frames' too, are smooth", "[viceversa][filter]")
{
  // A fast frame sweep moves the frame position every sample, not every
  // control period
  const double fs = 48000.;
  Rig rig(fs);
  auto p = plain(BankId::PulseWidth, 0.5f);
  p.lfos[0] = LfoParams{ModTarget::FrameA, LfoShape::Sine, 0.05f, 0.5f, 0.f, 0.f};
  p.sanitize();
  rig.on(48, 127, p);
  rig.run(0.01, p);
  std::vector<float> pos;
  float l, r;
  for(int i = 0; i < 4800; i++)
  {
    rig.s->render(p, &l, &r, 1);
    pos.push_back(rig.s->groups[0].frame[0].value[0]);
  }
  std::vector<double> d;
  for(std::size_t i = 1; i < pos.size(); i++)
    d.push_back(std::abs(double(pos[i]) - pos[i - 1]));
  auto sorted = d;
  std::sort(sorted.begin(), sorted.end());
  const double median = sorted[sorted.size() / 2];
  CHECK(median > 0.);
  CHECK(sorted.back() < 3. * median + 1e-3);
}

TEST_CASE("A new voice's filter starts where it is", "[viceversa][filter]")
{
  // A voice ends; the next in its lane, far higher with the key tracking,
  // does not ramp its cutoff from the last one's
  Rig rig(48000.);
  auto p = plain(BankId::SineSaw, 1.f);
  p.cutoff = 500.f;
  p.keyTrack = 1.f;
  p.aR = 0.01f;
  rig.on(30, 127, p);
  rig.run(0.1, p);
  rig.off(30, p);
  rig.run(0.5, p);
  REQUIRE(rig.s->activeVoices() == 0);
  rig.on(100, 127, p);
  float l, r;
  rig.s->render(p, &l, &r, 1);
  CHECK(rig.s->groups[0].G.step[0] == 0.f);
  CHECK(rig.s->groups[0].G.value[0] > 0.1f);
  CHECK(rig.s->groups[0].k.step[0] == 0.f);
}

TEST_CASE("prepare() at the same rate keeps what sounds", "[viceversa][robustness]")
{
  const double fs = 48000.;
  Rig rig(fs);
  auto p = plain(BankId::SineSaw, 0.f); // a sine
  rig.on(45, 127, p);
  const auto before = rig.run(0.2, p);
  REQUIRE(rig.s->activeVoices() == 1);
  // The host's buffers grew: same rate, so the note goes on, unbroken
  rig.s->prepare(fs);
  CHECK(rig.s->activeVoices() == 1);
  const auto after = rig.run(0.05, p);
  double step = 0.;
  for(std::size_t i = 1; i < after.size(); i++)
    step = std::max(step, std::abs(double(after[i]) - after[i - 1]));
  CHECK(std::abs(double(after[0]) - before.back()) < 1.5 * step);
  CHECK(rmsOf(after) > 0.1);
  // A new rate starts afresh
  rig.s->prepare(96000.);
  CHECK(rig.s->activeVoices() == 0);
  CHECK(rig.s->sampleRate() == 96000.);
}

TEST_CASE("A note released in another play mode leaves no voice held",
          "[viceversa][robustness]")
{
  auto p = plain(BankId::SineSaw, 0.f);
  p.aR = 0.05f;
  // Held in Mono, released in Poly, then Mono again
  {
    Rig rig;
    p.play = PlayMode::Mono;
    rig.on(60, 100, p);
    rig.run(0.05, p);
    p.play = PlayMode::Poly;
    rig.off(60, p);
    rig.run(0.05, p);
    p.play = PlayMode::Mono;
    rig.on(64, 100, p);
    rig.run(0.05, p);
    rig.off(64, p);
    rig.run(0.5, p);
    CHECK(rig.s->activeVoices() == 0);
  }
  // Held across Mono, Poly, Mono, released in Mono
  {
    Rig rig;
    p.play = PlayMode::Mono;
    rig.on(60, 100, p);
    rig.run(0.05, p);
    p.play = PlayMode::Poly;
    rig.run(0.05, p);
    p.play = PlayMode::Mono;
    rig.on(64, 100, p);
    rig.run(0.05, p);
    rig.off(64, p);
    rig.off(60, p);
    rig.run(0.5, p);
    CHECK(rig.s->activeVoices() == 0);
  }
}

TEST_CASE("A note off in Mono releases a Poly voice of its own channel only",
          "[viceversa][robustness]")
{
  auto p = plain(BankId::SineSaw, 0.f);
  Rig rig;
  // Voice 0 takes 64, the others the two 60s, one per channel
  rig.send(0x90, 64, 100, p);
  rig.send(0x90, 60, 100, p);
  rig.send(0x91, 60, 100, p);
  rig.run(0.05, p);
  auto find = [&](int ch) -> const Synth::Voice* {
    for(int i = 1; i < kMaxVoices; i++)
    {
      const auto& v = rig.s->voices[std::size_t(i)];
      if(v.state == Synth::Voice::Active && v.note == 60 && v.channel == ch)
        return &v;
    }
    return nullptr;
  };
  REQUIRE(find(0));
  REQUIRE(find(1));

  p.play = PlayMode::Mono;
  rig.send(0x81, 60, 0, p);
  rig.run(0.05, p);
  CHECK(find(0)->held);
  CHECK_FALSE(find(1)->held);
  CHECK(find(0)->amp.stage != Adsr::Release);
  CHECK(find(1)->amp.stage == Adsr::Release);
}

TEST_CASE("All notes off and reset all controllers lift the pedal", "[viceversa][robustness]")
{
  const double fs = 48000.;
  auto p = plain(BankId::SineSaw, 0.f);
  // CC 120 and 123: a note after them ends when released
  for(int number : {120, 123})
  {
    Rig rig(fs);
    cc(rig, 64, 127, p);
    cc(rig, number, 0, p);
    rig.on(60, 100, p);
    rig.run(0.01, p);
    rig.off(60, p);
    INFO("CC " << number);
    CHECK(!rig.s->voices[0].gate());
  }
  // CC 121: the pedal's notes released, the bend back to none
  Rig rig(fs);
  cc(rig, 64, 127, p);
  rig.on(62, 100, p);
  rig.off(62, p);
  REQUIRE(rig.s->voices[0].gate());
  rig.send(0xE0, 0x7F, 0x7F, p);
  cc(rig, 121, 0, p);
  CHECK(!rig.s->voices[0].gate());
  rig.run(1., p);
  rig.on(69, 127, p);
  const auto x = tail(rig.run(0.5, p), 4800);
  CHECK(std::abs(1200. * std::log2(measureFrequency(x, 440., fs) / 440.)) < 1.);
  // And a new note is not held by the pedal any more
  rig.off(69, p);
  CHECK(!rig.s->voices[0].gate());
}

TEST_CASE("A note released while it waits for its voice, pedal down, still sounds",
          "[viceversa][robustness]")
{
  Rig rig(48000.);
  auto p = plain(BankId::SineSaw, 0.f);
  p.voices = 1;
  p.sanitize();
  rig.on(60, 100, p);
  rig.run(0.05, p);
  cc(rig, 64, 127, p);
  rig.on(62, 100, p); // steals the only voice
  rig.off(62, p);     // before it got it
  rig.run(0.05, p);
  const auto& v = rig.s->voices[0];
  CHECK(v.state == Synth::Voice::Active);
  CHECK(v.note == 62);
  CHECK(v.sustained);
  cc(rig, 64, 0, p);
  CHECK(!v.gate());
  // Released before the steal with the pedal lifted too: dropped
  rig.on(64, 100, p);
  cc(rig, 64, 127, p);
  rig.off(64, p);
  cc(rig, 64, 0, p);
  rig.run(0.05, p);
  CHECK(rig.s->voices[0].note != 64);
}

TEST_CASE("A long glide at a high rate gets there", "[viceversa][robustness]")
{
  const double fs = 384000.;
  Rig rig(fs);
  auto p = plain(BankId::SineSaw, 0.f);
  p.play = PlayMode::Mono;
  p.glide = 2.f;
  p.sanitize();
  rig.on(60, 100, p);
  rig.run(0.01, p);
  rig.on(72, 100, p);
  rig.run(10., p);
  CHECK(rig.s->voices[0].pitch == 72.);
}

TEST_CASE("The vector's loop span can move under a voice", "[viceversa][vector]")
{
  VecEnvSpec s;
  s.x = {-1.f, 0.f, 1.f, 0.f, 0.5f};
  s.y = {0.f, 1.f, 0.f, -1.f, 0.f};
  s.time = {0.1, 0.1, 0.1, 0.1};
  // Back and forth over 0..2, the voice heading down; then 2..4
  s.setLoop(VecLoop::BackAndForth, 0, 2);
  VecEnvState e;
  e.start(s);
  e.advance(s, 0.25); // past 2, on its way back to 1
  s.setLoop(VecLoop::BackAndForth, 2, 4);
  bool moved = false;
  float lastX = e.x;
  for(int i = 0; i < 400; i++)
  {
    e.advance(s, 0.01);
    CHECK(e.a >= 0);
    CHECK(e.a <= 4);
    CHECK(e.b >= 0);
    CHECK(e.b <= 4);
    moved |= i > 200 && e.x != lastX;
    lastX = e.x;
  }
  CHECK(moved);
  CHECK(e.a >= 2);
  CHECK(e.b >= 2);
  // Over 0..4, heading up to 4; then 0..2: back down into it
  s.setLoop(VecLoop::BackAndForth, 0, 4);
  e.start(s);
  e.advance(s, 0.35);
  s.setLoop(VecLoop::BackAndForth, 0, 2);
  for(int i = 0; i < 400; i++)
  {
    e.advance(s, 0.01);
    CHECK(e.b <= 4);
  }
  CHECK(e.a <= 2);
  CHECK(e.b <= 2);
  // Round, beyond its new end: back to its start, not on to 4 and done
  s.setLoop(VecLoop::Round, 0, 4);
  e.start(s);
  e.advance(s, 0.35);
  s.setLoop(VecLoop::Round, 0, 2);
  e.advance(s, 1.);
  CHECK(!e.done);
  CHECK(e.b <= 2);
}

TEST_CASE("The spread crosses 0 without a click", "[viceversa][spread]")
{
  const double fs = 48000.;
  Rig rig(fs);
  auto p = plain(BankId::SineSaw, 0.f); // sines
  for(auto& s : p.src)
    s.bank = BankId::SineSaw;
  p.x = 0.f;
  p.spread = 0.f;
  p.sanitize();
  rig.on(48, 127, p);
  std::vector<float> l(static_cast<std::size_t>(fs * 0.4)), r(l.size());
  const std::size_t half = l.size() / 2;
  for(std::size_t i = 0; i < l.size(); i += 256)
  {
    if(i >= half)
      p.spread = 0.01f;
    rig.s->render(p, l.data() + i, r.data() + i, int(std::min<std::size_t>(256, l.size() - i)));
  }
  // The largest step from a sample to the next: where the spread left 0,
  // and elsewhere
  for(auto* x : {&l, &r})
  {
    double elsewhere = 0., there = 0.;
    for(std::size_t i = 4800; i < x->size(); i++)
    {
      const double d = std::abs(double((*x)[i]) - (*x)[i - 1]);
      double& m = i >= half && i < half + 256 ? there : elsewhere;
      m = std::max(m, d);
    }
    CHECK(there < 1.1 * elsewhere);
  }
}

TEST_CASE("Unconnected, the processor counts nothing", "[viceversa][ui]")
{
  ViceVersa vv;
  vv.prepare({.frames = 512, .rate = 48000.});
  std::vector<float> l(512), r(512);
  float* ch[2]{l.data(), r.data()};
  vv.outputs.audio.samples = ch;
  for(int i = 0; i < 1000; i++)
    vv({.frames = 512});
  CHECK(vv.sinceSent == 0);
  CHECK(vv.sinceResent == 0);
  // Connected: a message twice a second, even when nothing moves
  int sent = 0;
  vv.send_message = [&](LivePoints) { sent++; };
  for(int i = 0; i < 188; i++) // two seconds
    vv({.frames = 512});
  CHECK(sent >= 3);
  CHECK(sent <= 4);
}

TEST_CASE("In Mono, the pedal holds the voices left from Poly", "[viceversa][robustness]")
{
  auto p = plain(BankId::SineSaw, 0.f);
  p.aR = 0.05f;
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

  rig.send(0xB0, 64, 127, p);
  p.play = PlayMode::Mono;
  rig.off(60, p);
  CHECK_FALSE(v.held);
  CHECK(v.sustained);
  CHECK(v.amp.stage != Adsr::Release);
  // Up: released, then gone
  rig.send(0xB0, 64, 0, p);
  CHECK(v.amp.stage == Adsr::Release);
  rig.off(64, p);
  rig.run(2., p);
  CHECK(rig.s->activeVoices() == 0);
}

TEST_CASE("The allocation counter sees every kind of allocation", "[viceversa][robustness]")
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
