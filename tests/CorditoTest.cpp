// Cordito, the string synthesizer, at the DSP level: tuning, decay, expression,
// voices, and that nothing a host or a preset can send makes it explode,
// allocate on the audio thread or keep a voice alive forever.
#include <Cordito/Cordito.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <tuple>
#include <vector>

#include <ossia/math/safe_math.hpp>

// Counts the allocations made while `g_watch` is set
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

using namespace crd;

namespace
{
//! Params with nothing that moves the pitch on purpose
Params plain(Engine e)
{
  Params p;
  p.engine = e;
  p.exciter = bowed(e) ? Exciter::Bow : e == Engine::Struck ? Exciter::Hammer
                                                                      : Exciter::Pick;
  p.detune = 0.;
  p.spread = 0.;
  p.tension = 0.;
  p.stiffness = 0.;
  p.buzz = 0.;
  p.sanitize();
  return p;
}

struct Rig
{
  std::unique_ptr<Synth> s = std::make_unique<Synth>();
  double fs = 48000.;
  explicit Rig(double rate = 48000.)
  {
    s->prepare(rate);
    fs = s->sampleRate();
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
  //! Renders `seconds`, returns the left channel
  std::vector<double> run(double seconds, const Params& p, std::vector<double>* right = nullptr)
  {
    const int n = std::max(1, int(seconds * fs));
    std::vector<double> l, r;
    l.resize(std::size_t(n));
    r.resize(std::size_t(n));
    s->render(p, l.data(), r.data(), n);
    if(right)
      *right = r;
    return l;
  }
};

//! Fundamental from the autocorrelation around the expected period, refined
//! by a parabola through the peak
double estimateF0(const std::vector<double>& x, double fs, double expected)
{
  const int n = int(x.size());
  const double period = fs / expected;
  const int lo = std::max(2, int(period * 0.85));
  const int hi = std::min(n / 2, int(period * 1.15) + 2);
  auto ac = [&](int lag) {
    double s = 0., e1 = 0., e2 = 0.;
    for(int i = 0; i + lag < n; i++)
    {
      s += x[std::size_t(i)] * x[std::size_t(i + lag)];
      e1 += x[std::size_t(i)] * x[std::size_t(i)];
      e2 += x[std::size_t(i + lag)] * x[std::size_t(i + lag)];
    }
    return s / std::sqrt(e1 * e2 + 1e-30);
  };
  int best = lo;
  double bv = -2.;
  for(int l = lo; l <= hi; l++)
    if(double v = ac(l); v > bv)
    {
      bv = v;
      best = l;
    }
  const double a = ac(best - 1), b = ac(best), c = ac(best + 1);
  double d = (a - c) / (2. * (a - 2. * b + c));
  if(!ossia::safe_isfinite(d))
    d = 0.;
  return fs / (best + d);
}

double periodicity(const std::vector<double>& x, double fs, double f0)
{
  const int lag = int(std::lround(fs / f0));
  double s = 0., e1 = 0., e2 = 0.;
  for(std::size_t i = 0; i + std::size_t(lag) < x.size(); i++)
  {
    s += x[i] * x[i + std::size_t(lag)];
    e1 += x[i] * x[i];
    e2 += x[i + std::size_t(lag)] * x[i + std::size_t(lag)];
  }
  return s / std::sqrt(e1 * e2 + 1e-30);
}

double cents(double f, double ref)
{
  return 1200. * std::log2(f / ref);
}

double rms(const std::vector<double>& x, double fs, double t0, double t1)
{
  double s = 0.;
  const auto a = std::size_t(t0 * fs), b = std::min(x.size(), std::size_t(t1 * fs));
  for(auto i = a; i < b; i++)
    s += x[i] * x[i];
  return std::sqrt(s / double(std::max<std::size_t>(1, b - a)));
}

std::vector<double> slice(const std::vector<double>& x, double fs, double t0, double t1)
{
  return {x.begin() + std::ptrdiff_t(t0 * fs), x.begin() + std::ptrdiff_t(t1 * fs)};
}

bool allFinite(const std::vector<double>& x)
{
  for(double v : x)
    if(!ossia::safe_isfinite(v))
      return false;
  return true;
}
}

TEST_CASE("Cordito: the DC blockers in the loops are passive", "[synthimi][cordito]")
{
  // (1 - z^-1) / (1 - R z^-1) alone reaches 2 / (1 + R) at Nyquist: in a
  // loop, a string at the top of the range grew instead of decaying.
  for(double fs : {44100., 48000., 96000., 192000.})
    for(double fc : {1., 10.})
    {
      DcBlocker dc;
      dc.setCutoff(fc, fs);
      double peak = 0.;
      for(int i = 1; i <= 4096; i++)
        peak = std::max(peak, std::abs(dc.response(kPi * i / 4096.)));
      INFO("fs " << fs << " cutoff " << fc);
      CHECK(peak <= 1. + 1e-12);
    }

  Params p = plain(Engine::Pluck);
  p.tune = 24.;
  p.decay = 30.;
  p.brightness = 1.;
  Rig rig;
  rig.on(108, 100, p);
  const auto l = rig.run(6., p);
  CHECK(rms(l, rig.fs, 5., 6.) <= rms(l, rig.fs, 1., 2.));
}

TEST_CASE("Cordito: Coupling Detune chooses what the body pulls", "[synthimi][cordito]")
{
  // At 1 (the default), the notches are compensated at f0: near a body mode
  // every harmonic moves, by tens of cents. At 0, each partial is pulled by
  // the body's loading where it is, as on a real instrument: a few cents at
  // most at full Coupling, away from the modes. Between, between.
  auto pitch = [](int note, double detune) {
    Params p = plain(Engine::Pluck);
    p.body = Body::Guitar;
    p.coupling = 1.;
    p.bodyMix = 0.;
    p.couplingDetune = detune;
    p.sanitize();
    Rig rig;
    rig.on(note, 100, p);
    const auto l = rig.run(0.7, p);
    return cents(
        estimateF0(slice(l, rig.fs, 0.4, 0.7), rig.fs, midiToHz(note)), midiToHz(note));
  };
  for(int note : {58, 62, 66, 70, 74})
  {
    INFO("note " << note);
    CHECK(std::abs(pitch(note, 0.)) < 12.);
  }
  for(int note : {44, 55, 56})
  {
    const double on = pitch(note, 1.), half = pitch(note, 0.5), off = pitch(note, 0.);
    INFO("note " << note << ": 1: " << on << ", 0.5: " << half << ", 0: " << off << " cents");
    CHECK(std::abs(on - off) > 20.);
    CHECK(std::min(on, off) + 3. < half);
    CHECK(half < std::max(on, off) - 3.);
  }
}

TEST_CASE("Cordito: every engine plays in tune", "[synthimi][cordito]")
{
  // The loop's filters delay the wave: without their phase delay taken off
  // the lines, a waveguide is tens of cents flat in the upper register.
  // Bowed strings settle a little away from the fundamental of the loop
  // alone, as real ones do: the bow locks onto all the partials at once.
  for(double fs : {44100., 48000., 96000.})
    for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
      for(double bright : {0., 0.4, 0.6, 1.0})
        for(int note : {36, 48, 60, 72, 84, 96})
        {
          Params p = plain(e);
          p.brightness = bright;
          Rig rig{fs};
          rig.on(note, 100, p);
          const auto l = rig.run(0.45, p);
          const double c
              = cents(estimateF0(slice(l, fs, 0.3, 0.45), fs, midiToHz(note)), midiToHz(note));
          // A bowed string's pitch depends on how much sooner its upper
          // partials die, as a real one's does: exact at Brightness 1, up to
          // 16 cents sharp at C2 in between. Not calibrated yet.
          const double tolerance = bowed(e) ? 20. : 1.5;
          INFO("fs " << fs << " engine " << int(e) << " brightness " << bright << " note "
                     << note << ": " << c << " cents");
          CHECK(std::abs(c) < tolerance);
        }
}

TEST_CASE("Cordito: Tune, Fine and the pitch bend move the pitch", "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  p.tune = 7.;
  p.fine = -30.;
  {
    Rig rig;
    rig.on(60, 100, p);
    const auto l = rig.run(0.4, p);
    const double expected = midiToHz(67. - 0.3);
    CHECK(std::abs(cents(estimateF0(slice(l, rig.fs, 0.25, 0.4), rig.fs, expected), expected))
          < 1.5);
  }

  // Full bend up, on the note's own channel only: MPE without a mode switch
  p = plain(Engine::Pluck);
  p.bendRange = 12.;
  Rig rig;
  rig.on(60, 100, p, 2);
  rig.send(0xE3, 0x7F, 0x7F, p); // another channel: no effect on this note
  auto l = rig.run(0.4, p);
  CHECK(std::abs(cents(estimateF0(slice(l, rig.fs, 0.25, 0.4), rig.fs, midiToHz(60)), midiToHz(60)))
        < 1.5);
  rig.send(0xE2, 0x7F, 0x7F, p);
  l = rig.run(0.3, p);
  const double up = midiToHz(60. + 12. * (16383. / 8192. - 1.));
  CHECK(std::abs(cents(estimateF0(slice(l, rig.fs, 0.15, 0.3), rig.fs, up), up)) < 1.5);
}

TEST_CASE("Cordito: Decay is a T60, whatever the pitch and the sample rate", "[synthimi][cordito]")
{
  for(double fs : {48000., 96000.})
    for(Engine e : {Engine::Pluck, Engine::Struck})
      for(int note : {40, 60, 80})
      {
        Params p = plain(e);
        p.decay = 1.5;
        p.brightness = 1.;
        p.noise = 0.;
        Rig rig{fs};
        rig.on(note, 100, p);
        const auto l = rig.run(1.2, p);
        const double a = rms(l, fs, 0.2, 0.3), b = rms(l, fs, 1.0, 1.1);
        const double t60 = 60. / (20. * std::log10(a / b) / 0.8);
        INFO("fs " << fs << " engine " << int(e) << " note " << note << ": T60 " << t60);
        CHECK(t60 > 1.5 * 0.85);
        CHECK(t60 < 1.5 * 1.15);
      }
}

namespace
{
//! Amplitude of the component at f in a window, by the Goertzel algorithm
double goertzel(const std::vector<double>& x, double fs, double f, double t0, double t1)
{
  const auto a = std::size_t(t0 * fs), b = std::min(x.size(), std::size_t(t1 * fs));
  const double w = 2. * kPi * f / fs, c = 2. * std::cos(w);
  double s1 = 0., s2 = 0.;
  for(auto i = a; i < b; i++)
  {
    // Hann window, so that neighbouring partials do not leak in
    const double h = 0.5 - 0.5 * std::cos(2. * kPi * double(i - a) / double(b - a));
    const double s0 = x[i] * h + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0., s1 * s1 + s2 * s2 - c * s1 * s2)) / double(b - a);
}
}

TEST_CASE("Cordito: the fundamental decays as Decay asks, dark or bright", "[synthimi][cordito]")
{
  // Decay is the T60 of the fundamental. A one-pole loss filter kept dark
  // settings dark by raising its gain at DC, up to a clamp that then cut
  // the decay: C6 at Brightness 0.4 lasted 1.8 s of the 4 s asked.
  for(double decay : {4., 20.})
    for(int note : {48, 72, 84})
      for(double bright : {0., 0.4, 1.})
        for(double material : {0., 0.5})
        {
          Params p = plain(Engine::Pluck);
          p.decay = decay;
          p.brightness = bright;
          p.material = material;
          p.sanitize();
          Rig rig;
          rig.on(note, 100, p);
          const auto l = rig.run(2.4, p);
          const double f = midiToHz(note);
          const double a = goertzel(l, rig.fs, f, 0.3, 0.8), b = goertzel(l, rig.fs, f, 1.8, 2.3);
          const double t60 = 60. / (20. * std::log10(a / b) / 1.5);
          INFO("Decay " << decay << " note " << note << " brightness " << bright
                        << " material " << material << ": T60 " << t60);
          CHECK(t60 > decay * 0.8);
          CHECK(t60 < decay * 1.25);
        }
}

TEST_CASE("Cordito: high notes decay alike, semitone after semitone", "[synthimi][cordito]")
{
  // The fractional-delay reads lose high frequencies by an amount that
  // depends on each note's delay fraction: the T60 of a partial near 8 kHz
  // used to vary twofold between neighbouring keys. At Brightness 1 every
  // partial should decay as the fundamental does.
  for(double fs : {48000., 96000.})
  {
    std::vector<double> t60s;
    for(int note = 72; note < 84; note++)
    {
      Params p = plain(Engine::Pluck);
      p.decay = 4.;
      p.brightness = 1.;
      p.noise = 0.;
      Rig rig{fs};
      rig.on(note, 100, p);
      const auto l = rig.run(2.4, p);
      const double f0 = midiToHz(note);
      const double f = std::round(8000. / f0) * f0;
      const double a = goertzel(l, rig.fs, f, 0.3, 0.8), b = goertzel(l, rig.fs, f, 1.8, 2.3);
      t60s.push_back(60. / (20. * std::log10(a / b) / 1.5));
    }
    const auto [lo, hi] = std::minmax_element(t60s.begin(), t60s.end());
    INFO("fs " << fs << ": T60 near 8 kHz from " << *lo << " to " << *hi << " s");
    // What remains at 48 kHz is the 8-point interpolator's own loss, 0.1 %
    // per period at 8 kHz but compounded over 500-1000 periods a second
    CHECK(*lo > 4. * (fs < 60000. ? 0.5 : 0.7));
    CHECK(*hi < 4. * 1.3);
  }
}

TEST_CASE("Cordito: repeated notes on one key do not pile up", "[synthimi][cordito]")
{
  // A plectrum, a finger or a hammer touching a ringing string damps it
  for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Buzz})
  {
    Params p = plain(e);
    p.decay = 8.;
    p.release = Params::kReleaseFree;
    Rig single;
    single.on(45, 127, p);
    single.run(0.5, p);
    Rig rig;
    for(int i = 0; i < 64; i++)
    {
      rig.on(45, 127, p);
      rig.run(1. / 16., p);
    }
    INFO("engine " << int(e) << ": one note " << single.s->preLimiterPeak << ", 64 notes "
                   << rig.s->preLimiterPeak);
    CHECK(rig.s->preLimiterPeak < 3. * single.s->preLimiterPeak);
  }
}

TEST_CASE("Cordito: a released note dies within its Release and frees its voice", "[synthimi][cordito]")
{
  for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
  {
    Params p = plain(e);
    p.decay = 20.;
    p.release = 0.3;
    Rig rig;
    rig.on(52, 100, p);
    rig.run(0.5, p);
    REQUIRE(rig.s->activeVoices() == 1);
    rig.off(52, p);
    auto l = rig.run(0.3, p);
    // Damped, not cut
    CHECK(rms(l, rig.fs, 0., 0.05) > 1e-3);
    l = rig.run(2.5, p);
    INFO("engine " << int(e));
    CHECK(rig.s->activeVoices() == 0);
    CHECK(rms(l, rig.fs, 2.3, 2.5) < 1e-6);
  }
}

TEST_CASE("Cordito: dark, coupled strings die out and free their voices", "[synthimi][cordito]")
{
  // The loop's DC mode used to outlive the note by minutes on dark settings:
  // the loss filter's gain at DC reached its clamp. The voices never retired
  // and kept the sympathetic strings and the body ringing.
  for(Engine e : {Engine::Pluck, Engine::Buzz, Engine::Bow, Engine::Friction})
    for(double bright : {0., 0.33, 0.7})
    {
      Params p = plain(e);
      p.brightness = bright;
      p.material = 0.3;
      p.decay = 3.5;
      p.release = 0.6;
      p.body = Body::Guitar;
      p.coupling = 0.4;
      p.sympLevel = 0.5;
      p.sympTuning = SympTuning::Fifths;
      p.sympDecay = 4.;
      p.sanitize();
      Rig rig{22050.};
      for(int n : {48, 55, 60, 64})
        rig.on(n, 100, p);
      rig.run(0.5, p);
      for(int n : {48, 55, 60, 64})
        rig.off(n, p);
      const auto l = rig.run(12., p);
      INFO("engine " << int(e) << " brightness " << bright);
      CHECK(rig.s->activeVoices() == 0);
      CHECK(rms(l, rig.fs, 11.5, 12.) < 1e-6);
    }
}

TEST_CASE("Cordito: notes released after a switch of play mode stop", "[synthimi][cordito]")
{
  // Held under Poly, released under Mono or Legato (a preset change between
  // the note-on and the note-off): the note-off only looked at the mono
  // voice, and the other bows went on bowing forever.
  for(PlayMode from : {PlayMode::Poly, PlayMode::Mono, PlayMode::Legato})
    for(PlayMode to : {PlayMode::Poly, PlayMode::Mono, PlayMode::Legato})
    {
      Params a = plain(Engine::Bow);
      a.playMode = from;
      a.release = 0.2;
      Params b = a;
      b.playMode = to;
      Rig rig;
      for(int n : {48, 55, 60})
        rig.on(n, 100, a);
      rig.run(0.5, a);
      for(int n : {48, 55, 60})
        rig.off(n, b);
      const auto l = rig.run(4., b);
      INFO("from " << int(from) << " to " << int(to));
      CHECK(rig.s->activeVoices() == 0);
      CHECK(rms(l, rig.fs, 3.5, 4.) < 1e-6);
    }
}

TEST_CASE("Cordito: a panic stops everything, notes waiting on a voice included",
          "[synthimi][cordito]")
{
  // A note waiting on a voice that fades out started after All Notes Off or
  // All Sound Off, and went on bowing for good
  for(unsigned char cc : {120, 123})
  {
    Params p = plain(Engine::Bow);
    p.voices = 1;
    p.release = 0.1;
    Rig rig;
    rig.on(60, 100, p);
    rig.run(0.2, p);
    rig.on(64, 100, p);
    rig.run(0.002, p);
    rig.send(0xB0, cc, 0, p);
    const auto l = rig.run(10., p);
    INFO("CC " << int(cc));
    CHECK(rig.s->activeVoices() == 0);
    CHECK(rms(l, rig.fs, 9.5, 10.) < 1e-6);
  }
}

TEST_CASE("Cordito: prepare() again at the same rate keeps the notes", "[synthimi][cordito]")
{
  // The host calls prepare() again, on the audio thread, when its buffer
  // size grows: that wiped every voice, a click
  Params p = plain(Engine::Bow);
  Rig rig;
  rig.on(55, 100, p);
  const auto before = rig.run(0.5, p);
  rig.s->prepare(rig.fs);
  const auto after = rig.run(0.1, p);
  CHECK(rig.s->activeVoices() == 1);
  CHECK(rms(after, rig.fs, 0., 0.1) > 0.5 * rms(before, rig.fs, 0.4, 0.5));

  // And any rate is clamped to one it can run at
  for(double rate : {0., -1., 50., 1e9, std::numeric_limits<double>::quiet_NaN()})
  {
    Rig odd{rate};
    odd.on(60, 100, p);
    const auto l = odd.run(0.05, p);
    INFO("rate " << rate);
    CHECK(allFinite(l));
    CHECK(odd.s->sampleRate() >= 8000.);
    CHECK(odd.s->sampleRate() <= 384000.);
  }
}

TEST_CASE("Cordito: the newest notes sound when every voice is fading out",
          "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  p.voices = 2;
  Rig rig;
  for(int n = 60; n < 66; n++)
    rig.on(n, 100, p);
  rig.run(0.05, p);
  std::vector<int> sounding;
  for(int i = 0; i < kMaxVoices; i++)
    if(rig.s->voice(i).state == Voice::State::Active)
      sounding.push_back(rig.s->voice(i).note);
  std::sort(sounding.begin(), sounding.end());
  CHECK(sounding == std::vector<int>{64, 65});
}

TEST_CASE("Cordito: a key played again after lowering Voices keeps its voice",
          "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  p.release = Params::kReleaseFree;
  Rig rig;
  for(int n = 60; n < 66; n++)
    rig.on(n, 100, p);
  rig.run(0.05, p);
  const int before = rig.s->activeVoices();
  p.voices = 2;
  rig.on(65, 100, p);
  rig.run(0.05, p);
  CHECK(rig.s->activeVoices() == before);
}

TEST_CASE("Cordito: sympathetic strings turned off forget what rang", "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  p.sympLevel = 1.;
  p.sympTuning = SympTuning::Octaves;
  p.sympRoot = 48;
  p.sympDecay = 30.;
  p.release = 0.05;
  Rig rig;
  rig.on(60, 110, p);
  rig.run(0.3, p);
  rig.off(60, p);
  rig.run(0.5, p);
  p.sympLevel = 0.;
  rig.run(1., p);
  p.sympLevel = 1.;
  const auto l = rig.run(1., p);
  CHECK(rms(l, rig.fs, 0.5, 1.) < 1e-5);
}

TEST_CASE("Cordito: pressure reaches every engine, and a 1-byte message is ignored",
          "[synthimi][cordito]")
{
  // Auto drives an E-Bow's strength. It regulates the level it settles at,
  // so pressure shows in how fast the string swells.
  double level[2]{};
  for(int pressed = 0; pressed < 2; pressed++)
  {
    Params p = plain(Engine::Bow);
    p.exciter = Exciter::EBow;
    p.force = 0.2;
    Rig rig;
    rig.on(52, 100, p);
    rig.send(0xD0, pressed ? 127 : 0, 0, p);
    const auto l = rig.run(0.4, p);
    level[pressed] = rms(l, rig.fs, 0.1, 0.4);
  }
  INFO("swell: " << level[0] << " without pressure, " << level[1] << " with");
  CHECK(level[1] > 1.2 * level[0]);

  // A truncated channel pressure is not a pressure of 0
  Params p = plain(Engine::Bow);
  p.pressure = Pressure::BowSpeed;
  Rig a, b;
  a.on(52, 100, p);
  b.on(52, 100, p);
  const unsigned char truncated[1]{0xD0};
  b.s->midi(truncated, 1, p);
  const auto la = a.run(1., p), lb = b.run(1., p);
  CHECK(std::abs(rms(lb, b.fs, 0.5, 1.) / rms(la, a.fs, 0.5, 1.) - 1.) < 0.05);
}

TEST_CASE("Cordito: the enumerations keep their order", "[synthimi][cordito]")
{
  // Presets store a combobox by its index: these may only ever be appended to
  using V = std::vector<std::string_view>;
  auto names = [](auto e) {
    const auto n = avnd::enum_names<decltype(e)>();
    return V(n.begin(), n.end());
  };
  CHECK(names(Engine{}) == V{"Pluck", "Struck", "Bow", "Buzz", "Friction"});
  CHECK(names(Exciter{}) == V{"Pick", "Finger", "Hammer", "Mallet", "Burst", "Bow", "EBow"});
  CHECK(
      names(SympTuning{})
      == V{"Off", "Octaves", "Fifths", "Major", "Minor", "Drone", "Chromatic", "Follow",
           "OctavesFollow", "FifthsFollow", "MajorFollow", "MinorFollow", "DroneFollow"});
  // What the combobox shows, entry for entry
  const decltype(Cordito::ins::symp_tuning)::range shown{};
  CHECK(
      V(std::begin(shown.values), std::end(shown.values))
      == V{"Off", "Octaves", "Fifths", "Major", "Minor", "Drone", "Chromatic", "Follow",
           "Octaves (follow)", "Fifths (follow)", "Major (follow)", "Minor (follow)",
           "Drone (follow)"});
  CHECK(int(SympTuning::Follow) == 7);
  CHECK(int(SympTuning::OctavesFollow) == 8);
  CHECK(int(SympTuning::DroneFollow) == 12);
  CHECK(
      names(Body{})
      == V{"None", "Guitar", "Violin", "Cello", "Harp", "Gourd", "Soundboard", "Box",
           "Metal"});
  CHECK(names(PlayMode{}) == V{"Poly", "Mono", "Legato"});
  CHECK(
      names(Pressure{}) == V{"Auto", "Force", "Brightness", "BowSpeed", "Mute", "Off"});
  CHECK(
      names(LfoTarget{})
      == V{"Off", "Pitch", "Level", "Pan", "Brightness", "Position", "BowSpeed", "Force",
           "Buzz"});
  CHECK(names(LfoShape{}) == V{"Sine", "Triangle", "SawUp", "SawDown", "Square", "Random"});
}

TEST_CASE("Cordito: the sustain pedal holds released notes", "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  p.release = 0.05;
  Rig rig;
  rig.on(60, 100, p);
  rig.send(0xB0, 64, 127, p);
  rig.off(60, p);
  auto l = rig.run(1., p);
  CHECK(rms(l, rig.fs, 0.9, 1.) > 1e-2);
  rig.send(0xB0, 64, 0, p);
  rig.run(1., p);
  CHECK(rig.s->activeVoices() == 0);
}

TEST_CASE("Cordito: voices are stolen, and a key played again re-excites its string",
          "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  Rig rig;
  rig.on(60, 100, p);
  rig.run(0.05, p);
  rig.on(60, 100, p);
  rig.run(0.05, p);
  CHECK(rig.s->activeVoices() == 1);

  for(int n = 0; n < 24; n++)
  {
    rig.on(40 + n, 90, p);
    rig.run(0.01, p);
  }
  rig.run(0.05, p);
  CHECK(rig.s->activeVoices() <= kMaxVoices);
  CHECK(rig.s->activeVoices() >= kMaxVoices - 1);

  p.voices = 4;
  Rig capped;
  for(int n = 0; n < 8; n++)
    capped.on(50 + n, 90, p);
  capped.run(0.05, p);
  CHECK(capped.s->activeVoices() <= 4);
}

TEST_CASE("Cordito: a key replayed after a preset change plays the new engine",
          "[synthimi][cordito]")
{
  // A string left ringing by the previous settings must not be re-excited
  // with its old engine: it rings out, and the note gets a voice of its own.
  for(PlayMode mode : {PlayMode::Poly, PlayMode::Mono, PlayMode::Legato})
  {
    Params bow = plain(Engine::Bow);
    bow.release = Params::kReleaseFree;
    bow.decay = 30.;
    bow.playMode = mode;
    Rig rig;
    rig.on(50, 100, bow);
    rig.run(0.3, bow);
    rig.off(50, bow);
    rig.run(0.1, bow);

    Params pluck = plain(Engine::Pluck);
    pluck.release = Params::kReleaseFree;
    pluck.playMode = mode;
    rig.on(50, 100, pluck);
    rig.run(0.05, pluck);
    bool found = false;
    for(int i = 0; i < kMaxVoices; i++)
    {
      const auto& v = rig.s->voice(i);
      if(v.state == Voice::State::Active && v.note == 50 && v.held)
      {
        found = true;
        INFO("play mode " << int(mode));
        CHECK(v.engine == Engine::Pluck);
        CHECK(v.exciter == Exciter::Pick);
      }
    }
    INFO("play mode " << int(mode));
    CHECK(found);
  }
}

TEST_CASE("Cordito: Legato slides to the next note without a second voice", "[synthimi][cordito]")
{
  Params p = plain(Engine::Bow);
  p.playMode = PlayMode::Legato;
  p.glide = 0.2;
  Rig rig;
  rig.on(55, 100, p);
  rig.run(0.5, p);
  rig.on(62, 100, p);
  const auto l = rig.run(1.2, p);
  CHECK(rig.s->activeVoices() == 1);
  CHECK(std::abs(cents(estimateF0(slice(l, rig.fs, 1.0, 1.2), rig.fs, midiToHz(62)), midiToHz(62)))
        < 6.);
}

TEST_CASE("Cordito: the bow speaks over a range of force, speed and position", "[synthimi][cordito]")
{
  for(double force : {0.2, 0.5, 0.8})
    for(double speed : {0.3, 0.6, 0.9})
      for(double position : {0.08, 0.13, 0.25})
      {
        Params p = plain(Engine::Bow);
        p.force = force;
        p.bowSpeed = speed;
        p.position = position;
        Rig rig;
        rig.on(48, 100, p);
        const auto l = rig.run(0.8, p);
        INFO("force " << force << " speed " << speed << " position " << position);
        REQUIRE(allFinite(l));
        CHECK(rms(l, rig.fs, 0.5, 0.8) > 1e-3);
        // Helmholtz motion: periodic, at the bow's own period (its pitch is
        // checked elsewhere)
        const auto w = slice(l, rig.fs, 0.5, 0.8);
        CHECK(periodicity(w, rig.fs, estimateF0(w, rig.fs, midiToHz(48))) > 0.8);
      }
}

TEST_CASE("Cordito: the E-Bow sustains while held, and stops on release", "[synthimi][cordito]")
{
  Params p = plain(Engine::Bow);
  p.exciter = Exciter::EBow;
  p.release = 0.2;
  Rig rig;
  rig.on(52, 100, p);
  const auto l = rig.run(3., p);
  CHECK(rms(l, rig.fs, 2.5, 3.) > 1e-2);
  // Regulated, not running into the loop's backstop clamp
  CHECK(rig.s->preLimiterPeak < 0.9);
  rig.off(52, p);
  rig.run(3., p);
  CHECK(rig.s->activeVoices() == 0);
}

TEST_CASE("Cordito: sympathetic strings ring on after the note", "[synthimi][cordito]")
{
  Params p = plain(Engine::Pluck);
  p.sympLevel = 1.;
  p.sympTuning = SympTuning::Octaves;
  p.sympRoot = 48;
  p.release = 0.05;
  Rig rig;
  rig.on(60, 110, p);
  rig.run(0.3, p);
  rig.off(60, p);
  rig.run(0.7, p);
  CHECK(rig.s->activeVoices() == 0);
  const auto l = rig.run(0.5, p);
  CHECK(rms(l, rig.fs, 0., 0.5) > 1e-4);
}

TEST_CASE("Cordito: String mixes the played strings, after the sympathetic ones", "[synthimi][cordito]")
{
  // Everything after the voices is linear below the limiter: the strings
  // alone and the sympathetic strings alone add up to the two together, and
  // String at 0 leaves the sympathetic ones exactly as they were
  Params p = plain(Engine::Pluck);
  p.sympLevel = 1.;
  p.sympTuning = SympTuning::Octaves;
  p.sympRoot = 48;
  p.body = Body::Guitar;
  const auto play = [](Params q) {
    q.sanitize();
    Rig rig;
    rig.on(60, 70, q);
    std::vector<double> r;
    auto l = rig.run(1., q, &r);
    l.insert(l.end(), r.begin(), r.end());
    return l;
  };
  const auto both = play(p);
  Params alone = p, halo = p;
  alone.sympLevel = 0.;
  halo.stringLevel = 0.;
  const auto a = play(alone), h = play(halo);
  double err = 0., energy = 0.;
  for(std::size_t i = 0; i < both.size(); i++)
  {
    err = std::max(err, std::abs(both[i] - a[i] - h[i]));
    energy += h[i] * h[i];
  }
  CHECK(err < 1e-9);
  CHECK(energy > 1e-3);
}

TEST_CASE("Cordito: Exciter is the excitation heard alone", "[synthimi][cordito]")
{
  for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
  {
    INFO(int(e));
    Params p;
    p.engine = e;
    p.exciter = bowed(e) ? Exciter::Bow : e == Engine::Struck ? Exciter::Hammer : Exciter::Pick;
    p.sanitize();
    const auto play = [](Params q, double string, double exciter) {
      q.stringLevel = string;
      q.exciterLevel = exciter;
      q.sanitize();
      Rig rig;
      rig.on(60, 100, q);
      std::vector<double> r;
      auto l = rig.run(1., q, &r);
      rig.off(60, q);
      auto tail = rig.run(1.5, q);
      l.insert(l.end(), tail.begin(), tail.end());
      return l;
    };
    const auto str = play(p, 1., 0.), exc = play(p, 0., 1.), sum = play(p, 1., 1.);
    // Heard as loud as the string it sets going, at middle C: by their
    // peaks, or a bow's rosin by its level while it bows
    const auto peak = [](const std::vector<double>& x) {
      double m = 0.;
      for(double y : x)
        m = std::max(m, std::abs(y));
      return m;
    };
    const double fs = 48000.;
    const double db = bowed(e) ? 20. * std::log10(rms(exc, fs, 0.2, 1.) / rms(str, fs, 0.2, 1.))
                               : 20. * std::log10(peak(exc) / peak(str));
    CHECK(std::abs(db) < 2.);
    // A pluck or a strike is a click, gone long before the string is; the
    // rosin goes with the bow
    CHECK(rms(exc, fs, 2., 2.5) < 1e-6);
    if(!bowed(e))
      CHECK(rms(exc, fs, 0.2, 1.) < 1e-6);
    // The excitation adds to the string, and changes none of its noise: the
    // rosin draws from a generator of its own
    double err = 0.;
    for(std::size_t i = 0; i < sum.size(); i++)
      err = std::max(err, std::abs(sum[i] - str[i] - exc[i]));
    CHECK(err < 1e-9);
  }
}

TEST_CASE("Cordito: sympathetic strings resonate, they do not amplify", "[synthimi][cordito]")
{
  // A string in unison with a played note gains 1 / (1 - g): hundreds with a
  // long Symp. Decay. With Follow, which tunes the strings to the notes
  // played, raising the level used to drive the output into the limiter.
  auto loudness = [](SympTuning t, double level, double sympDecay, int root) {
    Params p = plain(Engine::Pluck);
    p.exciter = Exciter::Finger;
    p.decay = 7.;
    p.release = Params::kReleaseFree;
    p.sympTuning = t;
    p.sympLevel = level;
    p.sympDecay = sympDecay;
    p.sympRoot = root;
    p.sanitize();
    Rig rig;
    double rmsMax = 0.;
    for(int n : {root + 12, root + 16, root + 19, root + 24, root + 12, root + 19})
    {
      rig.on(n, 110, p);
      const auto l = rig.run(1., p);
      for(double t0 = 0.; t0 < 1.; t0 += 0.1)
        rmsMax = std::max(rmsMax, rms(l, rig.fs, t0, t0 + 0.1));
      rig.off(n, p);
    }
    return std::pair{rmsMax, rig.s->preLimiterPeak};
  };
  for(SympTuning t : {SympTuning::Follow, SympTuning::Octaves, SympTuning::Major,
                      SympTuning::Chromatic, SympTuning::Drone, SympTuning::OctavesFollow,
                      SympTuning::MajorFollow, SympTuning::DroneFollow})
    for(double sympDecay : {8., 30.})
      for(int root : {28, 36, 48})
      {
        const auto [dry, dryPeak] = loudness(t, 0., sympDecay, root);
        const auto [half, _] = loudness(t, 0.5, sympDecay, root);
        const auto [full, fullPeak] = loudness(t, 1., sympDecay, root);
        INFO("tuning " << int(t) << " decay " << sympDecay << " root " << root << ": "
                       << dry << " / " << half << " / " << full);
        CHECK(half >= dry * 0.99);
        CHECK(full >= half * 0.99);
        CHECK(20. * std::log10(full / dry) < 12.);
        // The same notes, ringing freely, are loud already: what the strings
        // add is what counts
        CHECK(fullPeak < 4. * dryPeak);
      }
}

TEST_CASE("Cordito: bridge coupling never makes a string grow", "[synthimi][cordito]")
{
  for(int b = 1; b <= int(Body::Metal); b++)
    for(int note : {28, 40, 52, 64, 76})
    {
      Params p = plain(Engine::Pluck);
      p.body = Body(b);
      p.coupling = 1.;
      p.decay = 30.;
      p.brightness = 1.;
      p.bodySize = 0.5 + 0.3 * (note % 5);
      p.sanitize();
      Rig rig;
      rig.on(note, 127, p);
      const auto l = rig.run(3., p);
      INFO("body " << b << " note " << note);
      REQUIRE(allFinite(l));
      CHECK(rms(l, rig.fs, 2.5, 3.) < rms(l, rig.fs, 0.1, 0.6));
      CHECK(rig.s->preLimiterPeak < 1.);
    }
}

TEST_CASE("Cordito: the default sound survives a mono sum", "[synthimi][cordito]")
{
  for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
  {
    Params p;
    p.engine = e;
    p.exciter = resolveExciter(e, Exciter::Pick);
    p.sanitize();
    Rig rig;
    rig.on(57, 100, p);
    std::vector<double> r;
    const auto l = rig.run(1., p, &r);
    std::vector<double> mono(l.size());
    for(std::size_t i = 0; i < l.size(); i++)
      mono[i] = 0.5 * (l[i] + r[i]);
    const double side = 0.5 * (rms(l, rig.fs, 0.1, 1.) + rms(r, rig.fs, 0.1, 1.));
    INFO("engine " << int(e));
    CHECK(20. * std::log10(rms(mono, rig.fs, 0.1, 1.) / side) > -3.);
  }
}

namespace
{
//! Level in dB of harmonic n of f0 in [t0, t1], the plucking position's
//! comb sin(n pi beta) divided out
double harmonicDb(const std::vector<double>& x, double fs, double f0, int n, double beta,
                  double t0, double t1)
{
  return 20. * std::log10(goertzel(x, fs, n * f0, t0, t1) / std::abs(std::sin(n * kPi * beta)));
}

//! The frequency of the strongest component within f +- span
double peakNear(const std::vector<double>& x, double fs, double f, double span, double t0, double t1)
{
  double best = f, bv = -1.;
  for(int i = 0; i <= 200; i++)
  {
    const double ff = f - span + 2. * span * i / 200.;
    if(const double v = goertzel(x, fs, ff, t0, t1); v > bv)
    {
      bv = v;
      best = ff;
    }
  }
  const double h = span / 100.;
  const double a = goertzel(x, fs, best - h, t0, t1), b = bv, c = goertzel(x, fs, best + h, t0, t1);
  const double d = 0.5 * (a - c) / (a - 2. * b + c);
  return ossia::safe_isfinite(d) ? best + d * h : best;
}

//! Energy of the harmonics from n0 to n1, in dB relative to the first three
double upperDb(const std::vector<double>& x, double fs, double f0, int n0, int n1, double t0, double t1)
{
  double lo = 0., hi = 0.;
  for(int n = 1; n <= n1; n++)
  {
    const double a = goertzel(x, fs, n * f0, t0, t1);
    if(n <= 3)
      lo += a * a;
    if(n >= n0)
      hi += a * a;
  }
  return 10. * std::log10(hi / lo);
}
}

TEST_CASE("Cordito: a pick plucks: its partials fall as 1/n", "[synthimi][cordito]")
{
  // A released string is a step in velocity waves: 1/n, -6 dB per octave.
  // The pick used to inject a velocity pulse, a strike: flat or rising up
  // to the plectrum's cut-off, then a cliff.
  Params p = plain(Engine::Pluck);
  p.exciter = Exciter::Pick;
  p.hardness = 1.;
  p.decay = 30.;
  p.brightness = 1.;
  p.noise = 0.;
  Rig rig;
  rig.on(48, 127, p);
  const auto l = rig.run(0.4, p);
  const double f0 = midiToHz(48), beta = rig.s->voice(0).position;
  const double h1 = harmonicDb(l, rig.fs, f0, 1, beta, 0.1, 0.35);
  for(int n : {2, 4, 8})
  {
    const double rel = harmonicDb(l, rig.fs, f0, n, beta, 0.1, 0.35) - h1;
    INFO("harmonic " << n << ": " << rel << " dB, 1/n is " << -20. * std::log10(n));
    CHECK(std::abs(rel + 20. * std::log10(n)) < 3.);
  }
}

TEST_CASE("Cordito: a harder note is a brighter one", "[synthimi][cordito]")
{
  // Jaffe and Smith's dynamics filter: velocity colours, not only scales
  for(Engine e : {Engine::Pluck, Engine::Struck})
    for(Exciter x : {Exciter::Pick, Exciter::Finger, Exciter::Burst, Exciter::Hammer})
    {
      if(resolveExciter(e, x) != x)
        continue;
      double db[2]{};
      for(int i = 0; i < 2; i++)
      {
        Params p = plain(e);
        p.exciter = x;
        p.noise = 0.;
        Rig rig;
        rig.on(48, i == 0 ? 20 : 127, p);
        db[i] = upperDb(rig.run(0.4, p), rig.fs, midiToHz(48), 10, 20, 0.05, 0.35);
      }
      INFO("engine " << int(e) << " exciter " << int(x) << ": upper partials " << db[0]
                     << " dB at velocity 20, " << db[1] << " at 127");
      CHECK(db[1] > db[0] + 3.);
    }
}

TEST_CASE("Cordito: the struck string sounds up to the top octave", "[synthimi][cordito]")
{
  // A hammer in contact for longer than the period filters out the
  // fundamental: C8 was 45 dB below C4
  for(Exciter x : {Exciter::Hammer, Exciter::Pick, Exciter::Mallet})
  {
    double level[2]{};
    int i = 0;
    for(int note : {60, 108})
    {
      Params p = plain(Engine::Struck);
      p.exciter = x;
      Rig rig;
      rig.on(note, 100, p);
      level[i++] = 20. * std::log10(rms(rig.run(0.5, p), rig.fs, 0.02, 0.5));
    }
    INFO("exciter " << int(x) << ": C4 " << level[0] << " dB, C8 " << level[1] << " dB");
    CHECK(std::abs(level[1] - level[0]) < 10.);
  }
}

TEST_CASE("Cordito: Stiffness is one stiff-string law in every engine", "[synthimi][cordito]")
{
  // Partial n at n f1 sqrt((1 + B n^2) / (1 + B)), B from the knob and the
  // pitch. The waveguide used to stretch a bass string by nothing and a
  // treble one by hundreds of cents, the modes by a law of their own.
  for(double fs : {48000., 96000.})
    for(double stiff : {0.5, 1.})
      for(int note : {36, 60, 84})
        for(Engine e : {Engine::Pluck, Engine::Struck})
        {
          Params p = plain(e);
          p.stiffness = stiff;
          p.decay = 8.;
          p.brightness = 1.;
          Rig rig{fs};
          rig.on(note, 100, p);
          const auto l = rig.run(1.2, p);
          const double f0 = midiToHz(note), B = inharmonicity(stiff, f0);
          const double f1 = peakNear(l, rig.fs, f0, 0.03 * f0, 0.1, 1.1);
          const double law = 1200. * std::log2(std::sqrt((1. + 64. * B) / (1. + B)));
          const double got = cents(peakNear(l, rig.fs, 8. * f1 * std::pow(2., law / 1200.),
                                            0.25 * f0, 0.1, 1.1),
                                   8. * f1);
          INFO("fs " << fs << " stiffness " << stiff << " note " << note << " engine "
                     << int(e) << ": partial 8 " << got << " cents, the law " << law);
          CHECK(std::abs(cents(f1, f0)) < 2.);
          CHECK(std::abs(got - law) < 0.25 * law + 5.);
        }
}

TEST_CASE("Cordito: the E-Bow sustains the fundamental", "[synthimi][cordito]")
{
  // Fed back whole, the string locked to its 3rd to 5th partials
  for(double fs : {48000., 96000.})
    for(int note : {40, 64, 88})
    {
      Params p = plain(Engine::Bow);
      p.exciter = Exciter::EBow;
      Rig rig{fs};
      rig.on(note, 100, p);
      const auto l = rig.run(3., p);
      const double f0 = midiToHz(note), h1 = goertzel(l, rig.fs, f0, 2.2, 3.);
      for(int n = 2; n <= 5; n++)
      {
        const double rel = 20. * std::log10(goertzel(l, rig.fs, n * f0, 2.2, 3.) / h1);
        INFO("fs " << fs << " note " << note << " harmonic " << n << ": " << rel << " dB");
        CHECK(rel < -10.);
      }
      CHECK(std::abs(cents(peakNear(l, rig.fs, f0, 0.03 * f0, 2.2, 3.), f0)) < 5.);
    }
}

TEST_CASE("Cordito: Buzz brightens, in tune, without killing the note", "[synthimi][cordito]")
{
  // Driven by the velocity wave and jumping by up to 17 samples in one, the
  // bridge contact made the note 13 to 25 dB quieter in 0.3 s, and darker.
  // A lossless bridge moves energy up, where the string loses it sooner: 9
  // or 10 dB at Buzz 1.
  for(int note : {52, 64})
  {
    double total[2]{}, high[2]{}, pitch[2]{};
    for(int i = 0; i < 2; i++)
    {
      Params p = plain(Engine::Buzz);
      p.buzz = i;
      p.decay = 6.;
      Rig rig;
      rig.on(note, 100, p);
      const auto l = rig.run(0.6, p);
      const double f0 = midiToHz(note);
      double lo = 0., hi = 0.;
      for(double f = 20.; f < 16000.; f += 10.)
      {
        const double a = goertzel(l, rig.fs, f, 0.02, 0.14);
        (f < 1000. ? lo : hi) += a * a;
      }
      total[i] = 20. * std::log10(rms(l, rig.fs, 0.25, 0.4));
      high[i] = 10. * std::log10(hi / lo);
      // Once the splash of the attack is over
      pitch[i] = cents(peakNear(l, rig.fs, f0, 0.06 * f0, 0.25, 0.6), f0);
    }
    INFO("note " << note << ": level " << total[0] << " -> " << total[1] << " dB, above 1 kHz "
                 << high[0] << " -> " << high[1] << " dB, pitch " << pitch[0] << " -> "
                 << pitch[1] << " cents");
    CHECK(high[1] > high[0] + 2.);
    CHECK(total[1] > total[0] - 11.);
    CHECK(std::abs(pitch[1]) < 8.);
  }
}

TEST_CASE("Cordito: a bow lifts over Bow Lift, and lets go of the string as it does",
          "[synthimi][cordito]")
{
  // A light bow on a muted low string. Its grip used to stay whole while its
  // speed faded: it clamped the string after note-off, then let go of it at
  // once 0.28 s later, a detuned swell louder than the note had been.
  for(Engine e : {Engine::Bow, Engine::Friction})
  for(int note : {28, 36})
  {
    Params p = plain(e);
    p.force = 0.05;
    p.velForce = 0.;
    p.mute = 0.7;
    p.release = Params::kReleaseFree;
    p.bowLift = 0.1;
    Rig rig;
    rig.on(note, 100, p);
    const auto held = rig.run(1., p);
    rig.off(note, p);
    const auto l = rig.run(0.6, p);
    const double h = rms(held, rig.fs, 0.5, 1.), a = rms(l, rig.fs, 0., 0.1),
                 b = rms(l, rig.fs, 0.2, 0.3);
    INFO("engine " << int(e) << " note " << note << ": held " << 20. * std::log10(h) << " dB, 0-0.1 s "
                 << 20. * std::log10(a) << " dB, 0.2-0.3 s " << 20. * std::log10(b) << " dB");
    // Dying away, not swelling
    CHECK(b < a);
    CHECK(b < 0.03 * std::max(h, a));
  }
  for(Engine e : {Engine::Bow, Engine::Friction})
  for(double lift : {0.05, 1.})
  {
    Params p = plain(e);
    p.bowLift = lift;
    Rig rig;
    rig.on(48, 100, p);
    rig.run(1., p);
    rig.off(48, p);
    rig.run(0.5 * lift, p);
    CHECK(rig.s->voice(0).bowing);
    rig.run(0.6 * lift, p);
    CHECK(!rig.s->voice(0).bowing);
  }
}

TEST_CASE("Cordito: a fast Bow Attack does not crackle", "[synthimi][cordito]")
{
  // The bow's envelope moved every 16 samples: over a 20 ms attack, 60 steps
  // of its speed, a click train 15 dB above a slow attack's on bass notes
  auto attackHf = [](Engine e, double attack) {
    Params p = plain(e);
    p.force = 0.05;
    p.velForce = 0.;
    p.noise = 0.;
    p.mute = 0.3;
    p.bowSpeed = 0.35;
    p.bowAttack = attack;
    Rig rig;
    rig.on(28, 100, p);
    const auto l = rig.run(0.15, p);
    double worst = 0.;
    for(std::size_t a = 2; a + 240 < l.size(); a += 240)
    {
      double e = 0.;
      for(std::size_t i = a; i < a + 240; i++)
      {
        const double d = l[i] - 2. * l[i - 1] + l[i - 2];
        e += d * d;
      }
      worst = std::max(worst, e);
    }
    return 10. * std::log10(worst + 1e-30);
  };
  for(Engine e : {Engine::Bow, Engine::Friction})
  {
    const double fast = attackHf(e, 0.02), slow = attackHf(e, 0.2);
    INFO("engine " << int(e) << ": high frequencies in the attack: " << fast << " dB at 20 ms, "
                   << slow << " dB at 200 ms");
    CHECK(fast < slow + 6.);
  }
}

TEST_CASE("Cordito: pressure glides from one MIDI value to the next", "[synthimi][cordito]")
{
  // Channel pressure arrives in steps, often coarse ones: what a voice reads
  // follows them over about 10 ms
  Params p = plain(Engine::Bow);
  Rig rig;
  rig.send(0xD0, 0, 0, p);
  rig.on(48, 100, p);
  rig.run(0.1, p);
  rig.send(0xD0, 127, 0, p);
  rig.run(0.002, p);
  const double early = rig.s->voice(0).pressure;
  rig.run(0.06, p);
  CHECK(early > 0.05);
  CHECK(early < 0.4);
  CHECK(rig.s->voice(0).pressure > 0.99);
}

TEST_CASE("Cordito: the middle of Force is a Helmholtz motion, Bow Speed its loudness",
          "[synthimi][cordito]")
{
  // Friction: Force spans a share of Schelleng's maximum for the bow's speed and
  // position: its middle is the regular stick-slip motion, a sawtooth whose
  // fundamental dominates, wherever the bow is and however fast it goes.
  for(double position : {0.08, 0.13, 0.25})
    for(int note : {48, 60, 72})
    {
      double level[2]{};
      int i = 0;
      for(double speed : {0.2, 0.9})
      {
        Params p = plain(Engine::Friction);
        p.force = 0.5;
        p.velForce = 0.;
        p.position = position;
        p.bowSpeed = speed;
        p.noise = 0.;
        Rig rig;
        rig.on(note, 100, p);
        const auto l = rig.run(1.5, p);
        const auto steady = slice(l, rig.fs, 0.9, 1.5);
        const double f0 = midiToHz(note);
        INFO("position " << position << " note " << note << " speed " << speed);
        CHECK(periodicity(steady, rig.fs, f0) > 0.9);
        CHECK(goertzel(l, rig.fs, f0, 0.9, 1.5) > goertzel(l, rig.fs, 2. * f0, 0.9, 1.5));
        level[i++] = 20. * std::log10(rms(l, rig.fs, 0.9, 1.5));
      }
      INFO("position " << position << " note " << note << ": " << level[0] << " dB at speed 0.2, "
                       << level[1] << " dB at 0.9");
      CHECK(level[1] > level[0] + 6.);
    }
}

namespace
{
//! A bowed note, held: a tone steady enough to measure what an LFO does to it
Params steadyBow()
{
  Params p = plain(Engine::Bow);
  p.decay = 8.;
  p.noise = 0.;
  p.velForce = 0.;
  return p;
}

//! The pitch, in cents from f0, every `hop` seconds over [t0, t1]
std::vector<double> pitchTrack(const std::vector<double>& x, double fs, double f0, double t0,
                               double t1, double hop)
{
  std::vector<double> c;
  for(double t = t0; t + 0.04 <= t1; t += hop)
    c.push_back(cents(peakNear(x, fs, f0, 0.1 * f0, t, t + 0.04), f0));
  return c;
}
}

TEST_CASE("Cordito: LFOs that move nothing change nothing", "[synthimi][cordito]")
{
  // Their settings with every target Off, against no LFO at all: the same
  // output, sample for sample, whatever the engine
  for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
  {
    Params a = plain(e);
    Params b = a;
    for(auto& l : b.lfos)
      l = {LfoTarget::Off, LfoShape::Random, 0.05, 1., 0.};
    Rig ra, rb;
    ra.on(55, 100, a);
    rb.on(55, 100, b);
    const auto la = ra.run(0.5, a), lb = rb.run(0.5, b);
    INFO("engine " << int(e));
    CHECK(la == lb);
  }
}

TEST_CASE("Cordito: an LFO on Pitch is a vibrato that waits its Delay, each note", "[synthimi][cordito]")
{
  // Depth 0.5 on Pitch: half a semitone either way
  Params p = steadyBow();
  p.lfos[0] = {LfoTarget::Pitch, LfoShape::Sine, 0.25, 0.5, 0.};
  Rig rig;
  rig.on(60, 100, p);
  const auto l = rig.run(2., p);
  const double f0 = midiToHz(60);
  const auto c = pitchTrack(l, rig.fs, f0, 1., 2., 0.01);
  const auto [lo, hi] = std::minmax_element(c.begin(), c.end());
  INFO("pitch from " << *lo << " to " << *hi << " cents");
  CHECK(*hi - *lo > 70.);
  CHECK(*hi - *lo < 130.);
  // Four cycles a second: as many rising crossings of the mean
  double mean = 0.;
  for(double x : c)
    mean += x / double(c.size());
  int rising = 0;
  for(std::size_t i = 1; i < c.size(); i++)
    rising += c[i - 1] < mean && c[i] >= mean;
  CHECK(rising >= 3);
  CHECK(rising <= 5);

  // With a Delay of 1 s, the first second holds still, and it rises after
  auto still = [&](const std::vector<double>& x, double fs, double t0, double t1) {
    const auto c = pitchTrack(x, fs, f0, t0, t1, 0.02);
    const auto [a, b] = std::minmax_element(c.begin(), c.end());
    return *b - *a;
  };
  p.lfos[0].delay = 1.;
  Rig late;
  late.on(60, 100, p);
  const auto l2 = late.run(2.4, p);
  INFO("delayed: " << still(l2, late.fs, 0.3, 0.95) << " cents in its first second, "
                   << still(l2, late.fs, 1.6, 2.4) << " after 1.6 s");
  CHECK(still(l2, late.fs, 0.3, 0.95) < 6.);
  CHECK(still(l2, late.fs, 1.6, 2.4) > 70.);

  // A key played again on its ringing string is a new note: its vibrato
  // waits again (the LFOs only restarted with a fresh voice)
  late.on(60, 100, p);
  const auto again = late.run(1., p);
  INFO("played again: " << still(again, late.fs, 0.3, 0.95) << " cents");
  CHECK(still(again, late.fs, 0.3, 0.95) < 6.);

  // So is each note of Mono, on its one voice
  Params m = p;
  m.playMode = PlayMode::Mono;
  Rig mono;
  mono.on(60, 100, m);
  mono.run(2., m);
  mono.on(62, 100, m);
  const auto next = mono.run(1., m);
  const double f2 = midiToHz(62);
  const auto c2 = pitchTrack(next, mono.fs, f2, 0.3, 0.95, 0.02);
  const auto [lo2, hi2] = std::minmax_element(c2.begin(), c2.end());
  INFO("next Mono note: " << *hi2 - *lo2 << " cents");
  CHECK(*hi2 - *lo2 < 6.);
}

TEST_CASE("Cordito: an LFO on Level is a tremolo that never retires the voice",
          "[synthimi][cordito]")
{
  // Depth 0.5 on Level: 6 dB either way, so 12 dB from trough to peak
  Params p = steadyBow();
  p.lfos[0] = {LfoTarget::Level, LfoShape::Sine, 0.5, 0.5, 0.};
  Rig rig;
  rig.on(60, 100, p);
  const auto l = rig.run(3., p);
  double lo = 1e9, hi = 0.;
  for(double t = 1.; t + 0.02 <= 3.; t += 0.01)
  {
    const double r = rms(l, rig.fs, t, t + 0.02);
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  const double swing = 20. * std::log10(hi / lo);
  INFO("tremolo swing " << swing << " dB");
  CHECK(swing > 9.);
  CHECK(swing < 15.);

  // Its deepest troughs, on a quiet note, are 12 dB down: still sounding
  Params q = steadyBow();
  q.lfos[0] = {LfoTarget::Level, LfoShape::Square, 2., 1., 0.};
  Rig quiet;
  quiet.on(60, 20, q);
  quiet.run(3., q);
  CHECK(quiet.s->activeVoices() == 1);
}

TEST_CASE("Cordito: an LFO on Pan moves the voice from side to side", "[synthimi][cordito]")
{
  Params p = steadyBow();
  p.spread = 0.;
  p.lfos[0] = {LfoTarget::Pan, LfoShape::Square, 1., 1., 0.};
  Rig rig;
  rig.on(60, 100, p);
  std::vector<double> r;
  const auto l = rig.run(2., p, &r);
  // Square at depth 1: the first half of each second fully right, the other
  // fully left (past the 16-sample ramps at the edges)
  CHECK(rms(l, rig.fs, 1.1, 1.4) < 0.01 * rms(r, rig.fs, 1.1, 1.4));
  CHECK(rms(r, rig.fs, 1.6, 1.9) < 0.01 * rms(l, rig.fs, 1.6, 1.9));
}

TEST_CASE("Cordito: every engine plays in tune at its default controls, at every rate",
          "[synthimi][cordito]")
{
  // The other tests simplify the controls (no stiffness, no detune): at the
  // defaults, the bow at 48 kHz sounded around the octave, its fundamental
  // gone, as the stretch of the stiff string pushed it off
  for(double fs : {44100., 48000., 96000.})
    for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
      for(int note : {48, 60, 72})
      {
        Params p;
        p.engine = e;
        p.exciter = resolveExciter(e, Exciter::Bow);
        p.sanitize();
        Rig rig{fs};
        rig.on(note, 100, p);
        const auto l = rig.run(1.6, p);
        const double f0 = midiToHz(note);
        const double h1 = goertzel(l, rig.fs, f0, 0.8, 1.6), h2 = goertzel(l, rig.fs, 2. * f0, 0.8, 1.6);
        const double c = cents(peakNear(l, rig.fs, f0, 0.05 * f0, 0.8, 1.6), f0);
        INFO("fs " << fs << " engine " << int(e) << " note " << note << ": H2/H1 "
                   << 20. * std::log10(h2 / h1) << " dB, " << c << " cents");
        // A bow that fell into another regime sounds its octave; a pluck or a
        // strike near the bridge just excites its second partial more
        if(bowed(e))
          CHECK(h1 > h2);
        CHECK(std::abs(c) < 10.);
      }
}

TEST_CASE("Cordito: the exciters sound alike at every sample rate", "[synthimi][cordito]")
{
  // Their noise was lowpassed by a coefficient per sample: more of it went
  // through as the rate rose, and its level and colour changed
  for(Engine e : {Engine::Pluck, Engine::Struck})
    for(Exciter x : {Exciter::Pick, Exciter::Burst, Exciter::Hammer})
    {
      if(resolveExciter(e, x) != x)
        continue;
      double level[3]{};
      int i = 0;
      for(double fs : {48000., 96000., 192000.})
      {
        Params p = plain(e);
        p.exciter = x;
        p.noise = 1.;
        Rig rig{fs};
        rig.on(52, 100, p);
        level[i++] = 20. * std::log10(rms(rig.run(0.2, p), rig.fs, 0.002, 0.2));
      }
      INFO("engine " << int(e) << " exciter " << int(x) << ": " << level[0] << " / " << level[1]
                     << " / " << level[2] << " dB at 48 / 96 / 192 kHz");
      CHECK(std::abs(level[1] - level[0]) < 1.5);
      CHECK(std::abs(level[2] - level[0]) < 1.5);
    }
}

TEST_CASE("Cordito: hammering one key a thousand times stays bounded", "[synthimi][cordito]")
{
  for(Engine e : {Engine::Pluck, Engine::Struck, Engine::Bow, Engine::Buzz, Engine::Friction})
  {
    Params p = plain(e);
    p.force = 1.;
    p.velForce = 0.;
    Rig rig;
    bool finite = true;
    for(int i = 0; i < 1000; i++)
    {
      rig.on(45, 127, p);
      finite &= allFinite(rig.run(0.02, p));
    }
    INFO("engine " << int(e) << " pre-limiter peak " << rig.s->preLimiterPeak);
    CHECK(finite);
    CHECK(rig.s->preLimiterPeak < 8.);
  }
}

namespace
{
//! Sympathetic strings alone: the played ones drive them but are not heard
Params following(SympTuning t)
{
  Params p = plain(Engine::Pluck);
  p.sympLevel = 1.;
  p.sympTuning = t;
  p.sympDecay = 8.;
  p.stringLevel = 0.;
  p.release = 0.05;
  p.sanitize();
  return p;
}

//! The notes the strings in use are tuned to, or are being retuned to
std::vector<double> tuned(const Rig& rig)
{
  const auto& bank = rig.s->sympathetic();
  std::vector<double> v;
  for(int i = 0; i < bank.count; i++)
  {
    const auto& s = bank.strings[std::size_t(i)];
    v.push_back(s.retune ? s.next : s.note);
  }
  std::sort(v.begin(), v.end());
  return v;
}

//! Both sides: the strings alternate between them
std::vector<double> mono(Rig& rig, double seconds, const Params& p)
{
  std::vector<double> r;
  auto l = rig.run(seconds, p, &r);
  for(std::size_t i = 0; i < l.size(); i++)
    l[i] += r[i];
  return l;
}

bool has(const std::vector<double>& v, double note)
{
  return std::find(v.begin(), v.end(), note) != v.end();
}

constexpr SympTuning kFollowing[]{
    SympTuning::OctavesFollow, SympTuning::FifthsFollow, SympTuning::MajorFollow,
    SympTuning::MinorFollow, SympTuning::DroneFollow};
}

TEST_CASE("Cordito: the tunings on Symp. Root are where they were", "[synthimi][cordito]")
{
  using V = std::vector<double>;
  auto table = [](SympTuning t, int root) {
    Params p = following(t);
    p.sympRoot = root;
    Rig rig;
    // Keys change nothing to them
    rig.on(50, 100, p);
    rig.run(0.05, p);
    rig.on(61, 100, p);
    rig.run(0.05, p);
    CHECK(rig.s->sympathetic().norm == rig.s->sympathetic().count);
    return tuned(rig);
  };
  CHECK(table(SympTuning::Off, 36).empty());
  CHECK(table(SympTuning::Octaves, 36) == V{36, 48, 60, 72});
  CHECK(table(SympTuning::Fifths, 40) == V{40, 47, 52, 59, 64, 71});
  CHECK(table(SympTuning::Major, 36) == V{36, 40, 43, 48, 52, 55, 60});
  CHECK(table(SympTuning::Minor, 45) == V{45, 48, 52, 57, 60, 64, 69});
  CHECK(table(SympTuning::Drone, 36) == V{24, 31, 36, 36.04, 43, 48});
  CHECK(table(SympTuning::Chromatic, 36) == V{36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47});
  CHECK(table(SympTuning::Follow, 36) == V{50, 61});
}

TEST_CASE("Cordito: one key in a (follow) tuning is that tuning on the key", "[synthimi][cordito]")
{
  using V = std::vector<double>;
  auto render = [](SympTuning t, int root, int note, V* table = nullptr) {
    Params p = following(t);
    p.sympRoot = root;
    Rig rig;
    rig.on(note, 100, p);
    std::vector<double> r;
    auto l = rig.run(1., p, &r);
    rig.off(note, p);
    const auto l2 = rig.run(0.5, p);
    l.insert(l.end(), r.begin(), r.end());
    l.insert(l.end(), l2.begin(), l2.end());
    if(table)
      *table = tuned(rig);
    return l;
  };
  for(int note : {48, 57, 60})
  {
    INFO("note " << note);
    V fixed, follow;
    // The same strings in the same places: the same sound to the last bit,
    // and Symp. Root has no say in it
    for(auto [a, b] : {std::pair{SympTuning::Octaves, SympTuning::OctavesFollow},
                       std::pair{SympTuning::Fifths, SympTuning::FifthsFollow},
                       std::pair{SympTuning::Major, SympTuning::MajorFollow},
                       std::pair{SympTuning::Minor, SympTuning::MinorFollow}})
    {
      INFO("tuning " << int(b));
      const auto ref = render(a, note, note, &fixed);
      CHECK(rms(ref, 48000., 0.5, 1.) > 1e-5);
      CHECK(render(b, 24, note, &follow) == ref);
      CHECK(follow == fixed);
      CHECK(render(b, 60, note) == ref);
    }
    // Drone has the same notes, the unisons first
    render(SympTuning::Drone, note, note, &fixed);
    const auto low = render(SympTuning::DroneFollow, 24, note, &follow);
    CHECK(follow == fixed);
    CHECK(render(SympTuning::DroneFollow, 60, note) == low);
  }
}

TEST_CASE("Cordito: the (follow) tunings follow the keys", "[synthimi][cordito]")
{
  // The third of F is heard once F is played, in Major (follow) and not in
  // Major on a root of C
  auto thirdOfF = [](SympTuning t) {
    Params p = following(t);
    p.sympRoot = 48;
    Rig rig;
    rig.on(48, 110, p);
    const auto a = mono(rig, 1., p);
    rig.off(48, p);
    rig.on(53, 110, p);
    const auto b = mono(rig, 1., p);
    const double f = 220.;
    return std::pair{goertzel(a, rig.fs, f, 0.5, 1.), goertzel(b, rig.fs, f, 0.5, 1.)};
  };
  const auto [fixedBefore, fixedAfter] = thirdOfF(SympTuning::Major);
  const auto [before, after] = thirdOfF(SympTuning::MajorFollow);
  INFO(fixedBefore << " " << fixedAfter << " / " << before << " " << after);
  CHECK(before == fixedBefore);
  CHECK(after > 10. * before);
  CHECK(after > 10. * fixedAfter);

  // Minor (follow) has the minor third instead
  {
    Params p = following(SympTuning::MinorFollow);
    Rig rig;
    rig.on(53, 110, p);
    const auto l = mono(rig, 1., p);
    CHECK(
        goertzel(l, rig.fs, 440. * std::exp2((56 - 69) / 12.), 0.5, 1.)
        > 10. * goertzel(l, rig.fs, 220., 0.5, 1.));
  }
}

TEST_CASE("Cordito: (follow) gives the strings to every key held", "[synthimi][cordito]")
{
  Params p = following(SympTuning::MajorFollow);
  Rig rig;
  for(int n : {60, 64, 67})
  {
    rig.on(n, 100, p);
    rig.run(0.2, p);
  }
  auto t = tuned(rig);
  INFO("strings " << t.size());
  // Every string in use, each on a note of its own
  CHECK(t.size() == std::size_t(kMaxSymp));
  CHECK(std::adjacent_find(t.begin(), t.end()) == t.end());
  // Each key has its unison, third, fifth and octave: the far intervals of
  // the oldest keys are what did not fit
  for(int n : {60, 64, 67})
    for(int i : {0, 4, 7, 12})
    {
      INFO(n << " + " << i);
      CHECK(has(t, n + i));
    }
  CHECK(has(t, 67 + 16));
  CHECK(!has(t, 60 + 24));

  // Octaves fit whole
  Params q = following(SympTuning::OctavesFollow);
  Rig oct;
  for(int n : {48, 55, 52})
    oct.on(n, 100, q);
  oct.run(0.1, q);
  CHECK(tuned(oct) == std::vector<double>{48, 52, 55, 60, 64, 67, 72, 76, 79, 84, 88, 91});
  // Notes out of the strings' range are not given one
  Rig high;
  high.on(120, 100, q);
  high.run(0.1, q);
  CHECK(tuned(high).empty());
}

TEST_CASE("Cordito: (follow) leaves ringing strings alone, and fades those it retunes",
          "[synthimi][cordito]")
{
  Params p = following(SympTuning::MajorFollow);
  Rig rig;
  const auto& bank = rig.s->sympathetic();
  auto slotOf = [&](double note) {
    for(int i = 0; i < bank.count; i++)
      if(bank.strings[std::size_t(i)].note == note)
        return i;
    return -1;
  };
  rig.on(48, 110, p);
  rig.run(0.5, p);
  rig.off(48, p);
  // Released: nothing is retuned, the strings ring on
  const auto held = tuned(rig);
  const auto tail = rig.run(1., p);
  CHECK(tuned(rig) == held);
  CHECK(rms(tail, rig.fs, 0.5, 1.) > 1e-5);

  // G shares its unison and its octave with the fifths of C: those strings
  // stay where they are, with what rings in them
  const int g = slotOf(55), g2 = slotOf(67);
  REQUIRE(g >= 0);
  REQUIRE(g2 >= 0);
  const double ringing = SympBank::loudness(bank.strings[std::size_t(g2)]);
  REQUIRE(ringing > SympBank::kSilent);
  rig.on(55, 110, p);
  rig.run(1. / rig.fs, p);
  CHECK(slotOf(55) == g);
  CHECK(slotOf(67) == g2);
  CHECK(SympBank::loudness(bank.strings[std::size_t(g2)]) > 0.5 * ringing);
  CHECK(bank.strings[std::size_t(g2)].gain == 1.);
  // C's 7 strings and the 5 more that G needed
  CHECK(bank.count == kMaxSymp);
  for(int n : {48, 52, 55, 60, 64, 67, 72, 59, 62, 71, 74, 79})
    CHECK(has(tuned(rig), n));
  rig.run(0.5, p);
  rig.off(55, p);

  // Every string is taken: the next keys retune ringing ones. A string never
  // changes its note while it is heard: either it has faded out, or it was
  // silent when the key came.
  std::array<double, kMaxSymp> note{}, gain{};
  std::array<bool, kMaxSymp> silent{};
  int retuned = 0;
  bool clean = true, finite = true;
  double peak = 0.;
  const int keys[]{50, 57, 49, 61, 54, 51};
  for(int k = 0; k < 6; k++)
  {
    rig.on(keys[k], 110, p);
    for(int i = 0; i < int(0.08 * rig.fs); i++)
    {
      for(int j = 0; j < kMaxSymp; j++)
      {
        note[std::size_t(j)] = bank.strings[std::size_t(j)].note;
        gain[std::size_t(j)] = bank.strings[std::size_t(j)].gain;
        silent[std::size_t(j)]
            = i == 0 && SympBank::loudness(bank.strings[std::size_t(j)]) < SympBank::kSilent;
      }
      const auto l = rig.run(1. / rig.fs, p);
      finite = finite && ossia::safe_isfinite(l[0]);
      peak = std::max(peak, std::abs(l[0]));
      for(int j = 0; j < kMaxSymp; j++)
        if(bank.strings[std::size_t(j)].note != note[std::size_t(j)])
        {
          retuned += silent[std::size_t(j)] ? 0 : 1;
          clean = clean && (silent[std::size_t(j)] || gain[std::size_t(j)] <= 2. * bank.fadeStep);
        }
    }
    rig.off(keys[k], p);
    INFO("key " << keys[k]);
    for(int i : {0, 4, 7, 12, 16, 19, 24})
      CHECK(has(tuned(rig), keys[k] + i));
  }
  CHECK(retuned >= 6);
  CHECK(clean);
  CHECK(finite);
  CHECK(peak < 1.);
  // Fades end: every string is back at its full level
  rig.run(0.1, p);
  for(int j = 0; j < bank.count; j++)
  {
    CHECK(bank.strings[std::size_t(j)].gain == 1.);
    CHECK(!bank.strings[std::size_t(j)].retune);
  }
}

TEST_CASE("Cordito: (follow) takes over the strings of the tuning before it", "[synthimi][cordito]")
{
  Params p = following(SympTuning::Major);
  p.sympRoot = 48;
  Rig rig;
  rig.on(48, 110, p);
  rig.run(0.5, p);
  rig.off(48, p);
  // No key down: the strings stay as they are, and ring
  p.sympTuning = SympTuning::MinorFollow;
  const auto l = rig.run(0.5, p);
  CHECK(tuned(rig) == std::vector<double>{48, 52, 55, 60, 64, 67, 72});
  CHECK(rms(l, rig.fs, 0.2, 0.5) > 1e-5);
  // A tuning chosen while keys are down builds on them at once
  rig.on(50, 110, p);
  rig.on(57, 110, p);
  p.sympTuning = SympTuning::OctavesFollow;
  rig.run(0.2, p);
  for(int n : {50, 57, 62, 69, 74, 81, 86, 93})
    CHECK(has(tuned(rig), n));
  // And Symp. Decay still reaches the strings
  const double before = rig.s->sympathetic().strings[0].loss.b;
  p.sympDecay = 1.;
  rig.run(0.01, p);
  CHECK(rig.s->sympathetic().strings[0].loss.b != before);
  // Back on a root: that tuning, whole
  p.sympTuning = SympTuning::Octaves;
  p.sympRoot = 36;
  rig.run(0.2, p);
  CHECK(tuned(rig) == std::vector<double>{36, 48, 60, 72});
  CHECK(rig.s->sympathetic().norm == 4);
}

TEST_CASE("Cordito: (follow) under a storm of notes stays finite, bounded, in place",
          "[synthimi][cordito]")
{
  for(SympTuning t : kFollowing)
  {
    Params p = following(t);
    p.sympLevel = 4.;
    p.sympDecay = 30.;
    p.stringLevel = 1.;
    Rig rig;
    Rng rng;
    bool finite = true;
    double peak = 0.;
    std::vector<double> l(64), r(64);
    g_allocs = 0;
    g_watch = true;
    for(int block = 0; block < 3000; block++)
    {
      // Several keys a block at times, on a few channels, with pedals,
      // panics and tunings changing under them
      for(int m = int(rng.next() % 3); m > 0; m--)
      {
        const unsigned u = rng.next() % 100;
        const auto ch = (unsigned char)(rng.next() % 3);
        const auto note = (unsigned char)(rng.next() % 128);
        if(u < 55)
          rig.send(0x90 | ch, note, (unsigned char)(rng.next() % 128), p);
        else if(u < 95)
          rig.send(0x80 | ch, note, 0, p);
        else if(u < 97)
          rig.send(0xB0 | ch, 64, (unsigned char)(rng.next() % 128), p);
        else if(u < 99)
          rig.send(0xB0 | ch, 123, 0, p);
        else
          rig.send(0xB0 | ch, 120, 0, p);
      }
      if(block % 500 == 499)
        p.sympTuning = kFollowing[rng.next() % 5];
      if(block % 700 == 699)
        p.sympDecay = 0.5 + 29.5 * rng.uniform();
      rig.s->render(p, l.data(), r.data(), 64);
      for(int i = 0; i < 64; i++)
      {
        finite = finite && ossia::safe_isfinite(l[std::size_t(i)])
                 && ossia::safe_isfinite(r[std::size_t(i)]);
        peak = std::max({peak, std::abs(l[std::size_t(i)]), std::abs(r[std::size_t(i)])});
      }
    }
    g_watch = false;
    INFO("tuning " << int(t) << " peak " << peak);
    CHECK(finite);
    CHECK(peak <= 1.);
    CHECK(g_allocs == 0);
    const auto& bank = rig.s->sympathetic();
    CHECK(bank.count <= kMaxSymp);
    CHECK(bank.keyCount <= SympBank::kMaxKeys);
    // Left alone, the bank dies out
    for(int ch = 0; ch < 3; ch++)
    {
      rig.send(0xB0 | ch, 64, 0, p);
      rig.send(0xB0 | ch, 123, 0, p);
    }
    p.sympDecay = 0.5;
    rig.run(8., p);
    const auto tail = rig.run(0.1, p);
    CHECK(rms(tail, rig.fs, 0., 0.1) < 1e-6);
    for(int i = 0; i < bank.count; i++)
      CHECK(ossia::safe_isfinite(bank.strings[std::size_t(i)].loss.y));
  }
}

namespace
{
//! Bank and wheel: `on` whether the strings follow it
Params bending(SympTuning t, bool on)
{
  Params p = following(t);
  p.sympRoot = 48;
  p.bendRange = 2.;
  p.sympBend = on;
  p.sanitize();
  return p;
}

void wheel(Rig& rig, double x, const Params& p, int ch = 0)
{
  const int v = std::clamp(int(std::lround(8192. + 8192. * x)), 0, 16383);
  rig.send((unsigned char)(0xE0 | ch), (unsigned char)(v & 0x7F), (unsigned char)(v >> 7), p);
}
}

TEST_CASE("Cordito: Symp. Bend lets the wheel bend the sympathetic strings", "[synthimi][cordito]")
{
  // A note left to ring in the bank alone, then the wheel: on a root, on the
  // notes played, on the keys held
  for(SympTuning t : {SympTuning::Octaves, SympTuning::Follow, SympTuning::OctavesFollow})
    for(bool on : {false, true})
      for(double x : {1., -0.5})
      {
        Params p = bending(t, on);
        Rig rig;
        rig.on(48, 110, p);
        rig.run(0.5, p);
        rig.off(48, p);
        rig.run(0.3, p);
        const auto before = mono(rig, 0.5, p);
        wheel(rig, x, p);
        const auto after = mono(rig, 1., p);
        const double st = p.bendRange * std::clamp(std::lround(8192. + 8192. * x), 0l, 16383l) / 8192.
                          - p.bendRange;
        INFO("tuning " << int(t) << (on ? " on" : " off") << " wheel " << x);
        for(int note : {48, 60})
        {
          if(t == SympTuning::Follow && note != 48)
            continue;
          const double f = 440. * std::exp2((note - 69) / 12.);
          const double moved = f * std::exp2(st / 12.);
          CHECK(std::abs(cents(peakNear(before, rig.fs, f, 0.02 * f, 0.1, 0.5), f)) < 3.);
          const double there = on ? moved : f, gone = on ? f : moved;
          INFO("note " << note << ": "
                       << cents(peakNear(after, rig.fs, there, 0.02 * there, 0.3, 1.), there)
                       << " cents");
          CHECK(std::abs(cents(peakNear(after, rig.fs, there, 0.02 * there, 0.3, 1.), there)) < 3.);
          CHECK(
              goertzel(after, rig.fs, there, 0.3, 1.) > 10. * goertzel(after, rig.fs, gone, 0.3, 1.));
        }
        // Still ringing: the bend moved the sound, it did not restart it
        CHECK(rms(after, rig.fs, 0.5, 1.) > 0.3 * rms(before, rig.fs, 0., 0.5));
      }
}

TEST_CASE("Cordito: without Symp. Bend the wheel leaves the bank as it is", "[synthimi][cordito]")
{
  for(SympTuning t : {SympTuning::Major, SympTuning::Follow, SympTuning::MajorFollow})
  {
    INFO("tuning " << int(t));
    // Off: the strings never leave the delay they were tuned with
    {
      Params p = bending(t, false);
      Rig rig;
      const auto& bank = rig.s->sympathetic();
      bool still = true;
      for(int k = 0; k < 40; k++)
      {
        if(k % 4 == 0)
          rig.on(48 + k / 4, 100, p);
        wheel(rig, std::sin(0.7 * k), p);
        rig.run(0.02, p);
        for(int i = 0; i < bank.count; i++)
        {
          const auto& s = bank.strings[std::size_t(i)];
          still = still && s.cur == s.d && s.glide == 0 && s.m == int(s.d);
        }
      }
      CHECK(still);
      CHECK(bank.bend == 0.);
    }
    // On, with the wheel at rest: the same sound to the last bit, and the
    // same again once the wheel is back from a bend
    auto render = [&](bool on, bool excursion) {
      Params p = bending(t, on);
      Rig rig;
      rig.on(48, 100, p);
      rig.run(0.2, p);
      if(excursion)
      {
        wheel(rig, 0.8, p);
        rig.run(0.2, p);
        wheel(rig, 0., p);
        rig.run(0.2, p);
        // Back at rest: the strings are tuned as they were
        const auto& bank = rig.s->sympathetic();
        for(int i = 0; i < bank.count; i++)
        {
          const auto& s = bank.strings[std::size_t(i)];
          CHECK(s.cur == s.d);
          CHECK(s.glide == 0);
        }
        CHECK(bank.bend == 0.);
        return std::vector<double>{};
      }
      rig.off(48, p);
      rig.on(55, 100, p);
      std::vector<double> r;
      auto l = rig.run(0.5, p, &r);
      l.insert(l.end(), r.begin(), r.end());
      return l;
    };
    CHECK(render(true, false) == render(false, false));
    render(true, true);
  }
}

TEST_CASE("Cordito: a bend slides the sympathetic strings, without a click or an allocation",
          "[synthimi][cordito]")
{
  for(SympTuning t : {SympTuning::Fifths, SympTuning::FifthsFollow})
  {
    INFO("tuning " << int(t));
    Params p = bending(t, true);
    Rig rig;
    const auto& bank = rig.s->sympathetic();
    rig.on(48, 110, p);
    rig.run(0.5, p);
    rig.off(48, p);
    rig.run(0.5, p);
    auto roughness = [&](double seconds, double* slide) {
      // The largest step of the second difference, which a break in the
      // sound shows in, and of a string's delay
      double worst = 0., a = 0., b = 0.;
      std::array<double, kMaxSymp> cur{};
      const int n = int(seconds * rig.fs);
      for(int i = 0; i < n; i++)
      {
        for(int j = 0; j < bank.count; j++)
          cur[std::size_t(j)] = bank.strings[std::size_t(j)].cur;
        double xl = 0., xr = 0.;
        rig.s->render(p, &xl, &xr, 1);
        const double x = xl + xr;
        if(i >= 2)
          worst = std::max(worst, std::abs(x - 2. * a + b));
        b = a;
        a = x;
        if(slide)
          for(int j = 0; j < bank.count; j++)
            *slide = std::max(*slide, std::abs(bank.strings[std::size_t(j)].cur - cur[std::size_t(j)]));
      }
      return worst;
    };
    const double calm = roughness(0.1, nullptr);
    REQUIRE(calm > 0.);
    // The wheel thrown from one end to the other, as a controller sends it:
    // in one step
    double slide = 0.;
    g_allocs = 0;
    g_watch = true;
    wheel(rig, 1., p);
    double rough = roughness(0.05, &slide);
    wheel(rig, -1., p);
    rough = std::max(rough, roughness(0.05, &slide));
    wheel(rig, 0., p);
    rough = std::max(rough, roughness(0.05, &slide));
    g_watch = false;
    INFO("calm " << calm << " bending " << rough << " slide " << slide);
    CHECK(g_allocs == 0);
    // A delay moves by a fraction of a sample per sample, never by a jump
    CHECK(slide > 0.);
    CHECK(slide < 0.5);
    CHECK(rough < 3. * calm);
  }

  // Any wheel, any range, on any channel, the control flipped under it
  for(SympTuning t : {SympTuning::Chromatic, SympTuning::Follow, SympTuning::DroneFollow})
  {
    Params p = bending(t, true);
    p.sympLevel = 4.;
    p.stringLevel = 1.;
    Rig rig;
    Rng rng;
    bool finite = true, inside = true;
    double peak = 0.;
    std::vector<double> l(64), r(64);
    const auto& bank = rig.s->sympathetic();
    g_allocs = 0;
    g_watch = true;
    for(int block = 0; block < 3000; block++)
    {
      const unsigned u = rng.next() % 100;
      const int ch = int(rng.next() % 3);
      if(u < 30)
        wheel(rig, 2. * rng.uniform() - 1., p, ch);
      else if(u < 45)
        rig.on(int(rng.next() % 128), 100, p, ch);
      else if(u < 55)
        rig.off(int(rng.next() % 128), p, ch);
      if(block % 200 == 199)
        p.bendRange = double(rng.next() % 49);
      if(block % 330 == 329)
        p.sympBend = !p.sympBend;
      rig.s->render(p, l.data(), r.data(), 64);
      for(int i = 0; i < 64; i++)
      {
        finite = finite && ossia::safe_isfinite(l[std::size_t(i)])
                 && ossia::safe_isfinite(r[std::size_t(i)]);
        peak = std::max({peak, std::abs(l[std::size_t(i)]), std::abs(r[std::size_t(i)])});
      }
      for(int i = 0; i < bank.count; i++)
      {
        const auto& s = bank.strings[std::size_t(i)];
        inside = inside && s.cur >= 2. && s.m + 2 <= s.line.mask;
      }
    }
    g_watch = false;
    INFO("tuning " << int(t) << " peak " << peak);
    CHECK(finite);
    CHECK(inside);
    CHECK(peak <= 1.);
    CHECK(g_allocs == 0);
  }
}

TEST_CASE("Cordito: any port value, any MIDI: finite, bounded, no allocation", "[synthimi][cordito]")
{
  // Through the processor itself, so its clamping of the ports is covered:
  // ControlInlet::setValue does not clamp, and presets or OSC can hold NaN.
  auto k = std::make_unique<Cordito>();
  constexpr int frames = 64;
  std::vector<double> l(frames), r(frames);
  double* outs[2]{l.data(), r.data()};
  k->outputs.audio.samples = outs;
  k->prepare(halp::setup{.input_channels = 0, .output_channels = 2, .frames = frames, .rate = 48000.});
  k->inputs.midi.midi_messages.reserve(64);

  Rng rng;
  auto wild = [&](double lo, double hi) -> float {
    const float u = rng.uniform();
    if(u < 0.03f)
      return std::numeric_limits<float>::quiet_NaN();
    if(u < 0.05f)
      return std::numeric_limits<float>::infinity();
    if(u < 0.07f)
      return -std::numeric_limits<float>::infinity();
    return float(lo + (hi - lo) * (1.4 * rng.uniform() - 0.2));
  };

  // The probe itself sees allocations
  g_allocs = 0;
  g_watch = true;
  delete new int{};
  g_watch = false;
  REQUIRE(g_allocs == 1);

  bool finite = true, bounded = true;
  g_allocs = 0;
  g_watch = true;
  for(int block = 0; block < 6000; block++)
  {
    auto& in = k->inputs;
    if(block % 50 == 0)
    {
      in.engine.value = Engine(int(rng.next() % 5));
      in.exciter.value = Exciter(int(rng.next() % 8));
      in.symp_tuning.value = SympTuning(int(rng.next() % 14));
      in.body.value = Body(int(rng.next() % 10));
      in.play_mode.value = PlayMode(int(rng.next() % 4));
      in.pressure.value = Pressure(int(rng.next() % 7));
      in.modes.value = int(rng.next() % 80) - 10;
      in.symp_root.value = int(rng.next() % 100);
      in.symp_bend.value = rng.next() % 2 == 0;
      in.voices.value = int(rng.next() % 20) - 2;
      in.bend_range.value = int(rng.next() % 60);
      for(auto [target, shape, period, depth, delay] :
          {std::tuple{&in.lfo1_target.value, &in.lfo1_shape.value, &in.lfo1_period.value,
                      &in.lfo1_depth.value, &in.lfo1_delay.value},
           std::tuple{&in.lfo2_target.value, &in.lfo2_shape.value, &in.lfo2_period.value,
                      &in.lfo2_depth.value, &in.lfo2_delay.value},
           std::tuple{&in.lfo3_target.value, &in.lfo3_shape.value, &in.lfo3_period.value,
                      &in.lfo3_depth.value, &in.lfo3_delay.value},
           std::tuple{&in.lfo4_target.value, &in.lfo4_shape.value, &in.lfo4_period.value,
                      &in.lfo4_depth.value, &in.lfo4_delay.value}})
      {
        *target = LfoTarget(int(rng.next() % 10));
        *shape = LfoShape(int(rng.next() % 7));
        *period = wild(0.025, 20.);
        *depth = wild(0., 1.);
        *delay = wild(0., 4.);
      }
      for(auto* f :
          {&in.hardness.value, &in.position.value, &in.force.value, &in.noise.value,
           &in.brightness.value, &in.stiffness.value, &in.tension.value, &in.material.value,
           &in.buzz.value, &in.bow_speed.value, &in.mute.value, &in.damper_noise.value,
           &in.body_mix.value, &in.coupling.value, &in.vel_force.value,
           &in.spread.value, &in.coupling_detune.value, &in.string_level.value,
           &in.exciter_level.value})
        *f = wild(0., 1.);
      in.tune.value = wild(-24., 24.);
      in.fine.value = wild(-100., 100.);
      in.detune.value = wild(0., 25.);
      in.symp_level.value = wild(0., 4.);
      in.body_size.value = wild(0.5, 2.);
      in.level.value = wild(0., 2.);
      in.decay.value = wild(0.05, 30.);
      in.bow_attack.value = wild(0.005, 2.);
    in.bow_lift.value = wild(0.005, 4.);
      in.release.value = wild(0.01, 10.);
      in.symp_decay.value = wild(0.5, 30.);
      in.glide.value = wild(0., 2.);
    }
    in.midi.midi_messages.clear();
    for(int m = 0; m < int(rng.next() % 4); m++)
    {
      static constexpr unsigned char status[]{0x90, 0x80, 0xE0, 0xD0, 0xA0, 0xB0};
      halp::midi_msg msg;
      msg.bytes = {
          (unsigned char)(status[rng.next() % 6] | (rng.next() % 4)),
          (unsigned char)(rng.next() % 128), (unsigned char)(rng.next() % 128)};
      if(rng.uniform() < 0.1f)
        msg.bytes[1] = rng.uniform() < 0.5f ? 64 : 74;
      msg.timestamp = int(rng.next() % (frames + 20)) - 10;
      in.midi.midi_messages.push_back(msg);
    }
    (*k)(halp::tick{frames});
    for(int i = 0; i < frames; i++)
    {
      finite &= ossia::safe_isfinite(l[std::size_t(i)]) && ossia::safe_isfinite(r[std::size_t(i)]);
      bounded &= std::abs(l[std::size_t(i)]) <= 1. && std::abs(r[std::size_t(i)]) <= 1.;
    }
  }
  g_watch = false;
  CHECK(finite);
  CHECK(bounded);
  CHECK(g_allocs == 0);
}
