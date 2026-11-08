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
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <vector>

namespace
{
std::atomic<bool> g_watch{false};
std::atomic<int> g_allocs{0};
}
void* operator new(std::size_t n)
{
  if(g_watch.load(std::memory_order_relaxed))
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  if(void* p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc{};
}
void operator delete(void* p) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}

using namespace viceversa;

namespace
{
constexpr double kPi = 3.14159265358979323846;

void fftc(std::vector<std::complex<double>>& a)
{
  viceversa::fft(a, false);
}

std::vector<double> power(const std::vector<float>& x, std::size_t n)
{
  std::vector<std::complex<double>> a(n);
  for(std::size_t i = 0; i < n; i++)
  {
    const double t = 2. * kPi * double(i) / double(n);
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
      const double w = 0.5 - 0.5 * std::cos(2. * kPi * double(i) / double(n));
      acc += w * double(x[from + i]) * std::polar(1., -2. * kPi * f * double(from + i) / fs);
    }
    return std::arg(acc);
  };
  double d = phaseAt(gap) - phaseAt(0);
  d -= 2. * kPi * std::round(d / (2. * kPi));
  return f + d * fs / (2. * kPi * double(gap));
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
    x[std::size_t(i)] = float(0.8 * std::sin(2. * kPi * t) + 0.3 * std::cos(2. * kPi * 3. * t)
                              - 0.1 * std::sin(2. * kPi * 7. * t));
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
    const double ref = 0.8 * std::sin(2. * kPi * ph) + 0.3 * std::cos(2. * kPi * 3. * ph)
                       - 0.1 * std::sin(2. * kPi * 7. * ph);
    worst = std::max(worst, std::abs(double(b.read(0.f, 0, 0.f, ph)) - ref));
  }
  CHECK(worst < 2e-4);
}

TEST_CASE("The mip level is always safe and fades smoothly", "[viceversa][bank]")
{
  int prevLevel = 0;
  float prevBright = 1e9f;
  for(float dx = 1e-5f; dx < kSafeCycles; dx *= 1.001f)
  {
    int level;
    float frac;
    Bank::levelFor(dx, level, frac);
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
    const double g = std::tan(kPi * fc / fs);
    const vec4 G(float(g / (1. + g)));
    double e = 0.;
    const int n = 48000;
    for(int i = 0; i < n; i++)
    {
      const float x = 0.01f * float(std::sin(2. * kPi * f * i / fs));
      const float y = lad.process(vec4(x), G, vec4(k), vec4(1.f))[0];
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

  // Self-oscillating and driven hard: bounded
  Ladder lad;
  const vec4 G(0.2f);
  float peak = 0.f;
  for(int i = 0; i < 48000; i++)
  {
    const float x = i < 10 ? 1.f : 0.f;
    peak = std::max(peak, std::abs(lad.process(vec4(x), G, vec4(3.9f), vec4(4.f))[0]));
  }
  CHECK(std::isfinite(peak));
  CHECK(peak < 4.f);
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
      acc += double(v[i]) * std::polar(1., -2. * kPi * f * double(i) / fs);
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
  for(int trial = 0; trial < 20; trial++)
  {
    Rig rig(trial % 2 ? 44100. : 96000.);
    std::vector<float> l(512), r(512);
    Params p;
    auto pick = [&] { return byte(rng) % 4 == 0 ? nan : u(rng); };
    for(auto& s : p.src)
    {
      s.bank = BankId(byte(rng) % 20 - 3);
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
    for(auto& l : p.lfos)
    {
      l.target = ModTarget(byte(rng) % 20 - 3);
      l.shape = LfoShape(byte(rng) % 10 - 2);
      l.period = pick();
      l.depth = pick();
      l.phase = pick();
      l.delay = pick();
    }
    p.cutoff = pick();
    p.resonance = pick();
    p.drive = pick();
    p.filterEnv = pick();
    p.aA = pick();
    p.aR = pick();
    p.fD = pick();
    p.play = PlayMode(byte(rng) % 4);
    p.glide = pick();
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
      rig.s->render(p, l.data(), r.data(), 1 + byte(rng) % 512);
      for(float v : l)
      {
        REQUIRE(std::isfinite(v));
        REQUIRE(std::abs(v) <= 1.f);
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
        REQUIRE(std::isfinite(v));
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

  // Points: drag point 4 (at the centre) to C, the east corner
  CHECK(pad.mouse_press(VectorPad::cx(), VectorPad::cy()));
  pad.mouse_move(VectorPad::px(1.f), VectorPad::cy());
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
  CHECK(pad.mouse_press(VectorPad::buttonX() + button, VectorPad::buttonY(1) + button));
  CHECK(pad.target == VectorPad::Position);
  const auto before = envs.size();
  CHECK(pad.mouse_press(VectorPad::cx(), VectorPad::py(0.5f)));
  pad.mouse_move(VectorPad::px(-2.f), VectorPad::cy()); // outside: onto the edge
  pad.mouse_release(0., 0.);
  REQUIRE(moves.size() == 2);
  CHECK(std::abs(moves[0].y - 0.5f) < 1e-4f);
  CHECK(std::abs(moves[1].x + 1.f) < 1e-4f);
  CHECK(envs.size() == before);
  CHECK(commits == 2);
  // The first button: back to the points
  CHECK(pad.mouse_press(VectorPad::buttonX() + button, VectorPad::buttonY(0) + button));
  CHECK(pad.target == VectorPad::Points);

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
  // the target moves, and nothing else does
  auto halves = [&](ModTarget target, auto measure) {
    Rig rig(fs);
    auto p = plain(BankId::SineSaw, 0.5f);
    p.x = 0.f;
    for(int s = 1; s < kSources; s++)
      p.src[std::size_t(s)] = p.src[0];
    p.lfos[0] = LfoParams{target, LfoShape::Square, 0.4f, 1.f, 0.f, 0.f};
    p.sanitize();
    rig.on(57, 100, p);
    rig.run(0.05, p);
    const auto hi = measure(rig.run(0.1, p)); // first half: +1
    rig.run(0.1, p);
    const auto lo = measure(rig.run(0.1, p)); // second half: -1
    return std::pair{hi, lo};
  };
  auto rms = [](const std::vector<float>& v) {
    double e = 0.;
    for(float x : v)
      e += double(x) * x;
    return std::sqrt(e / double(v.size()));
  };
  // Zero crossings: the pitch
  auto crossings = [](const std::vector<float>& v) {
    int n = 0;
    for(std::size_t i = 1; i < v.size(); i++)
      n += (v[i - 1] < 0.f) != (v[i] < 0.f);
    return double(n);
  };
  {
    const auto [hi, lo] = halves(ModTarget::Pitch, crossings);
    CHECK(hi / lo == Catch::Approx(4.).epsilon(0.1)); // ±1 octave
  }
  {
    const auto [hi, lo] = halves(ModTarget::Level, rms);
    CHECK(lo < 0.01 * hi); // down to silence
  }
  {
    const auto [hi, lo] = halves(ModTarget::Off, rms);
    CHECK(hi == Catch::Approx(lo).epsilon(0.05));
  }
}
