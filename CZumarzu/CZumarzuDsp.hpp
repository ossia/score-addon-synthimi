#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* A phase distortion synthesizer after the Casio CZ.
 *
 * Each voice has two lines, as a CZ tone: line 1 and line 2, played alone,
 * or line 1 with a detuned copy of itself (1+1') or with a detuned line 2
 * (1+2'). Each line alternates two waveforms from one cycle to the next and
 * has three eight-step envelopes: DCO (pitch), DCW (the amount of phase
 * distortion, the CZ's "filter") and DCA (level, which moves in decibels as
 * on the chip). Line 2 can be ring modulated by line 1, or its pitch jumped
 * by noise (0 or +32 semitones every 200 us, as on the chip).
 *
 * 16 voices in four groups of four SIMD lanes (kfr::vec<float, 4>), one lane
 * per voice. Envelopes, vibrato and the rest run every kCtrl samples and are
 * ramped; only the oscillators run per sample. The waveforms are
 * antialiased by the closed-form box filter of Dsp/PhaseMaps.hpp, and the
 * Clean 2x quality runs them at twice the rate through a half-band filter.
 * Each line's cycle mean is taken out before its DCA, so that the envelopes
 * do not move a DC with them.
 *
 * Nothing allocates after prepare().
 */

#include <CZumarzu/Dsp/Envelope.hpp>
#include <CZumarzu/Dsp/Halfband.hpp>
#include <CZumarzu/Dsp/PhaseMaps.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>

#if (defined(__SSE__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
#include <xmmintrin.h>
#define CZUMARZU_HAS_MXCSR 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define CZUMARZU_HAS_FPCR 1
#endif

namespace czumarzu
{
inline constexpr int kLanes = 4;
inline constexpr int kGroups = 4;
inline constexpr int kMaxVoices = kLanes * kGroups;
inline constexpr int kCtrl = 16; // output samples per control tick

// Stored by index in presets: append only
enum class SecondWave : int
{
  Off,
  Saw,
  Square,
  Pulse,
  DoubleSine,
  SawPulse,
  ResoSaw,
  ResoTriangle,
  ResoTrapezoid,
  DoublePulse,
  ResoPulse,
  ResoDoubleSaw,
  Vps
};
enum class LineMode : int
{
  Line1,
  Line2,
  Line1Doubled, // 1+1'
  Line1And2     // 1+2'
};
enum class Modulation : int
{
  Off,
  Ring,
  Noise
};
//! Clean: second-order antialiasing, band-limited jumps, 9 samples late.
//! With ring modulation it runs as Clean 2x: the product of the two lines
//! reaches twice as high, and would fold back from up to twice Nyquist.
//! Clean 2x, 4x: the same at two or four times the rate, then half-band
//! filters back to it.
//! Live: no latency: second-order antialiasing, jumps not band-limited. At the
//! rate, ring modulation included.
enum class Quality : int
{
  Clean,
  Clean2x,
  Clean4x,
  Live
};
enum class PlayMode : int
{
  Poly,
  Mono,
  Legato
};
enum class VibratoWave : int
{
  Triangle,
  SawUp,
  SawDown,
  Square,
  Sine
};

inline constexpr int kSecondWaveCount = 13;
// SecondWave is Off, then every Wave in order: LineParams::second()
static_assert(kSecondWaveCount == kWaveCount + 1);
static_assert(int(SecondWave::Vps) == int(Wave::Vps) + 1);
inline constexpr int kLineModeCount = 4;
inline constexpr int kModulationCount = 3;
inline constexpr int kQualityCount = 4;
inline constexpr int kPlayModeCount = 3;
inline constexpr int kVibratoWaveCount = 5;

//! By bit test: survives -ffinite-math-only
[[nodiscard]] inline bool finite(float x) noexcept
{
  std::uint32_t u;
  std::memcpy(&u, &x, sizeof(u));
  return (u & 0x7f800000u) != 0x7f800000u;
}
[[nodiscard]] inline float sane(float x, float lo, float hi, float fallback) noexcept
{
  return finite(x) ? std::clamp(x, lo, hi) : fallback;
}
template <typename E>
[[nodiscard]] E saneEnum(E e, int count) noexcept
{
  const int i = int(e);
  return (i >= 0 && i < count) ? e : E{};
}

struct LineParams
{
  Wave wave1{Wave::Saw};
  SecondWave wave2{SecondWave::Off};
  float formant{1.f};        // VPS height
  float level{1.f};          // [0, 1]
  float dcwKeyFollow{0.f};   // [0, 1]
  float dcaKeyFollow{0.f};   // [0, 1]
  float pitchDepth{12.f};    // semitones at DCO level 1
  float dcwDepth{1.f};       // DCW level 1 is this much DCW
  EnvSpec dca{kDefaultDca};
  EnvSpec dcw{kDefaultDcw};
  EnvSpec dco{kDefaultDco};

  void sanitize() noexcept
  {
    wave1 = saneEnum(wave1, kWaveCount);
    wave2 = saneEnum(wave2, kSecondWaveCount);
    formant = sane(formant, 0.5f, 8.f, 1.f);
    level = sane(level, 0.f, 1.f, 1.f);
    dcwKeyFollow = sane(dcwKeyFollow, 0.f, 1.f, 0.f);
    dcaKeyFollow = sane(dcaKeyFollow, 0.f, 1.f, 0.f);
    pitchDepth = sane(pitchDepth, -48.f, 48.f, 12.f);
    dcwDepth = sane(dcwDepth, 0.f, 1.f, 1.f);
    dca.sanitize();
    dcw.sanitize();
    dco.sanitize();
  }

  //! The waveform of the odd cycles; valid even if not sanitized
  [[nodiscard]] Wave second() const noexcept
  {
    const Wave w = wave2 == SecondWave::Off ? wave1 : Wave(int(wave2) - 1);
    return saneEnum(w, kWaveCount);
  }
};

struct Params
{
  std::array<LineParams, 2> line{};
  LineMode mode{LineMode::Line1And2};
  Modulation modulation{Modulation::Off};
  Quality quality{Quality::Clean};
  PlayMode play{PlayMode::Poly};

  float octave{0.f};       // octaves
  float tune{0.f};         // cents
  float detuneOctave{0.f}; // line 1' or 2', octaves
  float detuneNote{0.f};   // semitones
  float detuneFine{8.f};   // cents

  VibratoWave vibWave{VibratoWave::Triangle};
  float vibPeriod{0.18f};  // seconds
  float vibDelay{0.f};     // seconds
  float vibDepth{0.f};     // semitones

  float bendRange{2.f};    // semitones
  float glide{0.f};        // seconds
  float velLevel{0.5f};    // 1: 40 dB of velocity range
  float velDcw{0.3f};      // 1: velocity 0 has no DCW
  float pressureDcw{0.3f}; // added DCW at full pressure
  float spread{0.3f};      // line 1 left, line 2 right
  float level{0.7f};
  int voices{kMaxVoices};

  //! Each value in its port's range, or its port's default (CZumarzu.hpp)
  void sanitize() noexcept
  {
    for(auto& l : line)
      l.sanitize();
    mode = saneEnum(mode, kLineModeCount);
    modulation = saneEnum(modulation, kModulationCount);
    quality = saneEnum(quality, kQualityCount);
    play = saneEnum(play, kPlayModeCount);
    vibWave = saneEnum(vibWave, kVibratoWaveCount);
    octave = std::round(sane(octave, -3.f, 3.f, 0.f));
    tune = sane(tune, -100.f, 100.f, 0.f);
    detuneOctave = std::round(sane(detuneOctave, -3.f, 3.f, 0.f));
    detuneNote = std::round(sane(detuneNote, -11.f, 11.f, 0.f));
    detuneFine = sane(detuneFine, -100.f, 100.f, 8.f);
    vibPeriod = sane(vibPeriod, 0.01f, 10.f, 0.18f);
    vibDelay = sane(vibDelay, 0.f, 4.f, 0.f);
    vibDepth = sane(vibDepth, 0.f, 12.f, 0.f);
    bendRange = sane(bendRange, 0.f, 48.f, 2.f);
    glide = sane(glide, 0.f, 2.f, 0.f);
    velLevel = sane(velLevel, 0.f, 1.f, 0.5f);
    velDcw = sane(velDcw, 0.f, 1.f, 0.3f);
    pressureDcw = sane(pressureDcw, -1.f, 1.f, 0.3f);
    spread = sane(spread, 0.f, 1.f, 0.3f);
    level = sane(level, 0.f, 4.f, 0.7f);
    voices = std::clamp(voices, 1, kMaxVoices);
  }

  [[nodiscard]] bool twoLines() const noexcept
  {
    return mode == LineMode::Line1Doubled || mode == LineMode::Line1And2;
  }
  //! The parameters of oscillator o (0, or 1 in the two-line modes)
  [[nodiscard]] const LineParams& lineOf(int o) const noexcept
  {
    if(o == 0)
      return line[mode == LineMode::Line2 ? 1 : 0];
    return line[mode == LineMode::Line1Doubled ? 0 : 1];
  }
};

/*! Denormals flushed to zero while rendering, where the FPU does it: x86's
 *  MXCSR (FTZ and DAZ), AArch64's FPCR (FZ). Elsewhere (WebAssembly, ...)
 *  kHardware is false, and the synth flushes its filters' states itself.
 */
struct ScopedFlushDenormals
{
#if CZUMARZU_HAS_MXCSR
  static constexpr bool kHardware = true;
  unsigned int saved = _mm_getcsr();
  ScopedFlushDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
  ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#elif CZUMARZU_HAS_FPCR
  static constexpr bool kHardware = true;
  std::uint64_t saved = fpcr();
  ScopedFlushDenormals() noexcept { setFpcr(saved | (std::uint64_t(1) << 24)); }
  ~ScopedFlushDenormals() { setFpcr(saved); }
  static std::uint64_t fpcr() noexcept
  {
    std::uint64_t r;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(r));
    return r;
  }
  static void setFpcr(std::uint64_t r) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(r)); }
#else
  static constexpr bool kHardware = false;
  ScopedFlushDenormals() noexcept = default;
#endif
  ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
  ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
};


class Synth
{
public:
  //! Ramp of one oscillator parameter over a control tick, per lane. A tick
  //! starts from the last one's target, not from where the steps added up to
  //! with their rounding: a parameter that holds has a step of exactly 0.
  struct alignas(16) Ramp
  {
    std::array<float, kLanes> value{}, step{}, target{};
  };

  struct Voice
  {
    enum State : std::uint8_t
    {
      Free,
      Active,
      Stealing
    } state{Free};
    int note{60};
    int channel{0};
    float velocity{1.f};
    bool held{false};      // key down
    bool sustained{false}; // released under the pedal
    std::uint64_t stamp{0};
    //! Semitones, where the glide is. In double: in float, a slow glide at a
    //! high rate takes steps under the pitch's ulp and stalls short of it
    double pitch{60.};
    float target{60.f};
    double age{0.};        // seconds since the note started
    float pressure{0.f};   // polyphonic aftertouch
    bool hasPressure{false};
    std::array<std::array<EnvState, 3>, 2> env{}; // [osc][DCA, DCW, DCO]

    // Stealing: fading out before this note starts
    int pendingNote{-1};
    int pendingChannel{0};
    float pendingVelocity{0.f};
    bool pendingSustained{false}; // its key released under the pedal already
    float fade{1.f};

    //! What the control rate keeps from one tick to the next, per line:
    //! recomputed only when what it comes from moves
    struct Cache
    {
      // The envelopes' rates, with the key follow, at `key`
      float key{-1e9f}, dcaKf{-1.f}, dcwKf{-1.f};
      double dcaRate{0.}, dcwRate{0.};
      // The increment, at `semis` and `os`
      float semis{-1e9f};
      int os{0};
      float inc{0.f};
    };
    std::array<Cache, 2> cache{};

    [[nodiscard]] bool gate() const noexcept { return held || sustained; }
  };

  //! One line's output, StepResidual::kHalf samples late, while the
  //! band-limited steps of its jumps are added around it: a ring of kLate
  //! slots, which must hold the steps' 2 kHalf + 1 samples
  static constexpr int kLate = 32;
  static_assert(kLate >= 2 * StepResidual::kHalf + 1 && (kLate & (kLate - 1)) == 0);

  using u32x4 = kfr::vec<std::uint32_t, kLanes>;

  //! A line's cycle mean, and what it was computed from: while the DCW and
  //! the pitch hold, it does not change from one tick to the next
  struct alignas(16) Mean
  {
    vec4 dcw{-1.f}, dx{0.f}, v{0.f};
    Wave a{}, b{};
    int os{0};
    vec4 value{0.f};
  };

  struct alignas(16) Group
  {
    // [osc][lane]: the phase is the chip's: 31 bits of cycle, bit 31 says
    // which of the two waveforms this cycle plays
    std::array<std::array<std::uint32_t, kLanes>, 2> phase{};
    std::array<Ramp, 2> inc{}; // cycles per (internal) sample; its step is 0
    std::array<Ramp, 2> dcw{}; // [0, 0.992]
    std::array<Ramp, 2> amp{};
    std::array<std::uint32_t, kLanes> noise{}; // xorshift per lane
    std::array<float, kLanes> noiseUp{};       // 1: line 2 is 32 semitones up
    //! The rising side of the next sample's triangle, from this interval
    std::array<std::array<float, kLanes>, 2> carry{};
    //! [osc][sample % kLate][lane]
    std::array<std::array<std::array<float, kLanes>, kLate>, 2> late{};
    std::uint32_t count{0};
    std::array<Mean, 2> mean{};
    //! Internal samples still to render once its voices have ended: the
    //! steps left in `late`
    int drain{0};
    bool active{false};
  };

  struct ChannelState
  {
    float bend{0.f};
    float pressure{0.f};
    bool sustain{false};
  };

  /*! The first call, or one at another rate, resets everything. The host also
   *  calls it, from the audio thread, when its buffers grow: at the same
   *  rate the notes go on, and nothing is computed again.
   */
  void prepare(double rate) noexcept
  {
    const double r = (rate > 0. && finite(float(rate))) ? rate : 48000.;
    if(!built)
    {
      // None of these depend on the rate
      residual.build();
      dec[0].prepare();
      dec[1].prepare();
      dec4[0].prepare();
      dec4[1].prepare();
      built = true;
    }
    else if(r == fs)
      return;
    fs = r;
    for(auto& v : voices)
      v = Voice{};
    for(auto& g : groups)
      g = Group{};
    for(int gi = 0; gi < kGroups; gi++)
      for(int l = 0; l < kLanes; l++)
        groups[std::size_t(gi)].noise[std::size_t(l)]
            = 0x9E3779B9u * std::uint32_t(gi * kLanes + l + 1);
    channels = {};
    monoStack.clear();
    lastMono = false;
    lastTwo = false;
    tickLeft = 0;
    lfoPhase = 0.;
    modWheel = 0.f;
    noiseCount = 0;
    mode = -1;
    switchGain = 1.f;
    resetFilters();
    // 10 Hz DC blocker; the limiter's 1 ms attack, 100 ms release; 5 ms
    // fades around a change of internal rate
    dcCoef = float(1. - 2. * std::numbers::pi * 10. / fs);
    limAttack = float(1. - std::exp(-1. / (0.001 * fs)));
    limRelease = float(1. - std::exp(-1. / (0.1 * fs)));
    switchStep = float(1. / (0.005 * fs));
  }

  [[nodiscard]] double sampleRate() const noexcept { return fs; }

  [[nodiscard]] int activeVoices() const noexcept
  {
    int n = 0;
    for(auto& v : voices)
      n += v.state != Voice::Free;
    return n;
  }

  //! Peak of the output before the limiter in the last render, for tests
  float preLimiterPeak{0.f};

  void midi(const unsigned char* m, int n, const Params& p) noexcept
  {
    if(n < 1)
      return;
    followPlayMode(p);
    const int status = m[0] & 0xF0;
    const int ch = m[0] & 0x0F;
    const int d1 = n > 1 ? (m[1] & 0x7F) : 0;
    const int d2 = n > 2 ? (m[2] & 0x7F) : 0;
    auto& cs = channels[std::size_t(ch)];
    switch(status)
    {
      case 0x90:
        if(n < 3)
          return;
        if(d2 > 0)
          noteOn(d1, ch, float(d2) / 127.f, p);
        else
          noteOff(d1, ch, p);
        break;
      case 0x80:
        if(n >= 2)
          noteOff(d1, ch, p);
        break;
      case 0xA0:
        for(auto& v : voices)
          if(v.state != Voice::Free && v.note == d1 && v.channel == ch)
          {
            v.pressure = float(d2) / 127.f;
            v.hasPressure = true;
          }
        break;
      case 0xB0:
        controller(d1, d2, ch, p);
        break;
      case 0xD0:
        cs.pressure = float(d1) / 127.f;
        break;
      case 0xE0:
        if(n >= 3)
          cs.bend = float((d2 << 7 | d1) - 8192) / 8192.f;
        break;
      default:
        break;
    }
  }

  void render(const Params& p, float* l, float* r, int frames) noexcept
  {
    ScopedFlushDenormals ftz;
    followPlayMode(p);
    const int want = modeOf(p);
    if(mode < 0)
      switchMode(want);
    preLimiterPeak = 0.f;

    int done = 0;
    while(done < frames)
    {
      // Another internal rate or delay: once faded out with the old one
      if(want != mode && switchGain <= 0.f)
        switchMode(want);
      if(tickLeft == 0)
      {
        control(p, kCtrl);
        tickLeft = kCtrl;
      }
      const int n = std::min(frames - done, tickLeft);
      renderOscillators(p, n);
      compensate(n * os);

      for(int i = 0; i < n; i++)
      {
        float sl, sr;
        if(os == 4)
        {
          const auto j = std::size_t(4 * i);
          const float l0 = dec4[0].process(bufL[j], bufL[j + 1]);
          const float l1 = dec4[0].process(bufL[j + 2], bufL[j + 3]);
          const float r0 = dec4[1].process(bufR[j], bufR[j + 1]);
          const float r1 = dec4[1].process(bufR[j + 2], bufR[j + 3]);
          sl = dec[0].process(l0, l1);
          sr = dec[1].process(r0, r1);
        }
        else if(os == 2)
        {
          sl = dec[0].process(bufL[std::size_t(2 * i)], bufL[std::size_t(2 * i + 1)]);
          sr = dec[1].process(bufR[std::size_t(2 * i)], bufR[std::size_t(2 * i + 1)]);
        }
        else
        {
          sl = bufL[std::size_t(i)];
          sr = bufR[std::size_t(i)];
        }
        // Out before a change of internal rate, in after: the filters and
        // delays it resets would click. Before the DC blocker, whose memory
        // is longer than the fade
        switchGain = want != mode ? std::max(0.f, switchGain - switchStep)
                                  : std::min(1.f, switchGain + switchStep);
        sl *= switchGain;
        sr *= switchGain;

        // DC: what the lines' own means leave, as the steps of a note
        const float yl = sl - dcL + dcCoef * dcLy;
        const float yr = sr - dcR + dcCoef * dcRy;
        dcL = sl;
        dcR = sr;
        dcLy = yl;
        dcRy = yr;

        float ol = yl * p.level, or_ = yr * p.level;
        if(!finite(ol) || !finite(or_))
        {
          panic();
          ol = or_ = 0.f;
        }
        const float peak = std::max(std::abs(ol), std::abs(or_));
        preLimiterPeak = std::max(preLimiterPeak, peak);
        // Peak limiter: the reduction comes in over 1 ms and goes over
        // 100 ms; what passes meanwhile is soft clipped
        const float wanted = peak > kLimit ? kLimit / peak : 1.f;
        limGain += (wanted - limGain) * (wanted < limGain ? limAttack : limRelease);
        l[done + i] = softClip(ol * limGain);
        r[done + i] = softClip(or_ * limGain);
      }
      done += n;
      tickLeft -= n;
    }
    if constexpr(!ScopedFlushDenormals::kHardware)
      flushDenormals();
  }

  //! Everything silent and free, at once
  void panic() noexcept
  {
    for(int i = 0; i < kMaxVoices; i++)
      resetVoice(i);
    resetFilters();
    monoStack.clear();
  }

  // Exposed for tests
  std::array<Voice, kMaxVoices> voices{};
  std::array<Group, kGroups> groups{};
  std::array<ChannelState, 16> channels{};
  float modWheel{0.f};

private:
  static constexpr float kLimit = 0.97f;
  static constexpr float kPerOsc = 0.25f; // one note of two lines peaks at 0.5
  static constexpr float kNoiseUp = 6.349604207872798f; // 2^(32/12)
  //! Increments are multiples of 2^-24 cycle: the phase, 31 bits, then has 24
  //! significant ones, so x0 and x0 + dx are exact floats below 1, and the
  //! antialiasing's lengths with them. 0.003 Hz at 48 kHz, 0.6 cent at 8 Hz
  static constexpr float kIncQuantum = 1.f / 16777216.f;

  [[nodiscard]] static float quantizeInc(float x) noexcept
  {
    return std::max(kIncQuantum, std::round(x * 16777216.f) * kIncQuantum);
  }

  //! Continuous with a slope of 1 at 0.9, bounded by 1
  [[nodiscard]] static float softClip(float x) noexcept
  {
    const float a = std::abs(x);
    if(a <= 0.9f)
      return x;
    return std::copysign(std::min(1.f, 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.f)), x);
  }

  //! The filters and delays after the voices, at rest
  void resetFilters() noexcept
  {
    for(auto& d : dec)
      d.reset();
    for(auto& d : dec4)
      d.reset();
    eqL = eqR = {};
    dcL = dcR = dcLy = dcRy = 0.f;
    for(auto& g : groups)
    {
      g.late = {};
      g.drain = 0;
    }
    limGain = 1.f;
  }

  //! Without the FPU's help: the recursive states, which decay towards
  //! denormals in silence, to zero. Once per render.
  void flushDenormals() noexcept
  {
    for(auto& d : dec)
      d.flushDenormals();
    for(auto& d : dec4)
      d.flushDenormals();
    for(float* x : {&eqL[0], &eqL[1], &eqR[0], &eqR[1], &dcL, &dcR, &dcLy, &dcRy})
      if(std::abs(*x) < 1e-20f)
        *x = 0.f;
  }

  /*! What renders: the internal rate (oversampling) times 2, plus 1 for Live,
   *  which has no delay
   */
  [[nodiscard]] static int modeOf(const Params& p) noexcept
  {
    const bool ring = p.twoLines() && p.modulation == Modulation::Ring;
    const int over = p.quality == Quality::Clean4x                               ? 4
                     : p.quality == Quality::Clean2x
                             || (p.quality == Quality::Clean && ring) ? 2
                                                                      : 1;
    return 2 * over + (p.quality == Quality::Live ? 1 : 0);
  }

  //! From now on, mode m: at once on the first render, else faded out
  void switchMode(int m) noexcept
  {
    const int over = m / 2;
    if(mode >= 0 && over != os)
    {
      // Increments and steps are per internal sample: the rest of the tick
      // goes on at the new rate
      const float ratio = float(os) / float(over);
      for(auto& g : groups)
        for(int o = 0; o < 2; o++)
        {
          const auto oi = std::size_t(o);
          for(int l = 0; l < kLanes; l++)
          {
            const auto li = std::size_t(l);
            auto& inc = g.inc[oi];
            inc.value[li] = inc.target[li] = quantizeInc(inc.value[li] * ratio);
            g.dcw[oi].step[li] *= ratio;
            g.amp[oi].step[li] *= ratio;
          }
        }
    }
    os = over;
    live = (m & 1) != 0;
    mode = m;
    for(auto& g : groups)
      g.carry = {};
    resetFilters();
    noiseCount = 0;
  }

  // ---- voices ----------------------------------------------------------
  Voice& voice(int i) noexcept { return voices[std::size_t(i)]; }

  //! The voice free and its lane at rest; what `late` still holds of it plays
  //! out, which a steal's fade ends in
  void resetVoice(int i) noexcept
  {
    auto& v = voice(i);
    v = Voice{};
    auto& g = groups[std::size_t(i / kLanes)];
    const auto l = std::size_t(i % kLanes);
    for(int o = 0; o < 2; o++)
    {
      g.phase[std::size_t(o)][l] = 0;
      g.carry[std::size_t(o)][l] = 0.f;
      for(auto* r : {&g.inc[std::size_t(o)], &g.dcw[std::size_t(o)], &g.amp[std::size_t(o)]})
      {
        r->value[l] = 0.f;
        r->step[l] = 0.f;
        r->target[l] = 0.f;
      }
    }
    g.noiseUp[l] = 0.f;
  }

  void startVoice(int i, int note, int ch, float vel, const Params& p, bool fresh) noexcept
  {
    auto& v = voice(i);
    if(fresh)
      resetVoice(i);
    v.state = Voice::Active;
    v.note = note;
    v.channel = ch;
    v.velocity = vel;
    v.held = true;
    v.sustained = false;
    v.stamp = ++stamp;
    v.target = float(note);
    if(fresh || p.play == PlayMode::Poly)
      v.pitch = note;
    v.age = 0.;
    v.pendingNote = -1;
    v.pendingSustained = false;
    v.fade = 1.f;
    for(int o = 0; o < 2; o++)
    {
      const auto& lp = p.lineOf(o);
      v.env[std::size_t(o)][0].start(lp.dca);
      v.env[std::size_t(o)][1].start(lp.dcw);
      v.env[std::size_t(o)][2].start(lp.dco);
    }
    // Sample accurate: the rest of this tick plays the new note
    if(tickLeft > 0)
      controlVoice(i, p, tickLeft, false);
  }

  void releaseVoice(Voice& v, const Params& p) noexcept
  {
    if(v.state != Voice::Active)
      return;
    for(int o = 0; o < 2; o++)
    {
      const auto& lp = p.lineOf(o);
      v.env[std::size_t(o)][0].release(lp.dca);
      v.env[std::size_t(o)][1].release(lp.dcw);
      v.env[std::size_t(o)][2].release(lp.dco);
    }
  }

  void noteOn(int note, int ch, float vel, const Params& p) noexcept
  {
    if(p.play != PlayMode::Poly)
    {
      monoNoteOn(note, ch, vel, p);
      return;
    }
    // The same key again, on the same channel: retrigger where it is
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Active && v.note == note && v.channel == ch)
      {
        startVoice(i, note, ch, vel, p, false);
        return;
      }
    }
    const int limit = p.voices;
    int freeIdx = -1, releasedIdx = -1, oldestIdx = -1;
    int sounding = 0;
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Free)
      {
        if(freeIdx < 0)
          freeIdx = i;
        continue;
      }
      sounding++;
      if(v.state != Voice::Active)
        continue;
      if(!v.gate() && (releasedIdx < 0 || v.stamp < voice(releasedIdx).stamp))
        releasedIdx = i;
      if(oldestIdx < 0 || v.stamp < voice(oldestIdx).stamp)
        oldestIdx = i;
    }
    if(freeIdx >= 0 && sounding < limit)
    {
      startVoice(freeIdx, note, ch, vel, p, true);
      return;
    }
    int steal = releasedIdx >= 0 ? releasedIdx : oldestIdx;
    if(steal < 0)
    {
      // Every voice is already fading out: the newest note wins the newest
      for(int i = 0; i < kMaxVoices; i++)
        if(voice(i).state == Voice::Stealing && (steal < 0 || voice(i).stamp > voice(steal).stamp))
          steal = i;
      if(steal < 0)
        return;
    }
    auto& v = voice(steal);
    v.state = Voice::Stealing;
    v.pendingNote = note;
    v.pendingChannel = ch;
    v.pendingVelocity = vel;
    v.pendingSustained = false;
    v.stamp = ++stamp;
  }

  void noteOff(int note, int ch, const Params& p) noexcept
  {
    // In any mode: a key left in the stack would bring the mono voice back
    // to it, held for good
    monoStack.remove(note, ch);
    const bool pedal = channels[std::size_t(ch)].sustain;
    for(auto& v : voices)
      if(v.state == Voice::Stealing && v.pendingNote == note && v.pendingChannel == ch)
      {
        // Released before it started: under the pedal it still sounds, else
        // the fade just ends
        if(pedal)
          v.pendingSustained = true;
        else
          v.pendingNote = -1;
      }
    if(p.play != PlayMode::Poly)
    {
      monoNoteOff(note, ch, p);
      return;
    }
    for(auto& v : voices)
    {
      if(v.state == Voice::Active && v.note == note && v.channel == ch && v.held)
      {
        v.held = false;
        if(pedal)
          v.sustained = true;
        else
          releaseVoice(v, p);
      }
    }
  }

  void controller(int cc, int value, int ch, const Params& p) noexcept
  {
    auto& cs = channels[std::size_t(ch)];
    switch(cc)
    {
      case 1:
        modWheel = float(value) / 127.f;
        break;
      case 64:
        setSustain(ch, value >= 64, p);
        break;
      case 120: // all sound off
      case 123: // all notes off
        // On this channel, the pedal's notes too; a note waiting for a
        // stolen voice is dropped
        cs.sustain = false;
        for(int i = 0; i < kMaxVoices; i++)
        {
          auto& v = voice(i);
          if(v.state == Voice::Stealing && v.pendingChannel == ch)
            v.pendingNote = -1;
          if(v.state == Voice::Free || v.channel != ch)
            continue;
          if(cc == 120)
            resetVoice(i);
          else
          {
            v.held = v.sustained = false;
            releaseVoice(v, p);
          }
        }
        monoStack.clear();
        break;
      case 121: // reset all controllers: what the playing moved, not the patch
        cs.bend = 0.f;
        cs.pressure = 0.f;
        modWheel = 0.f;
        for(auto& v : voices)
          if(v.channel == ch)
          {
            v.pressure = 0.f;
            v.hasPressure = false;
          }
        setSustain(ch, false, p);
        break;
      default:
        break;
    }
  }

  void setSustain(int ch, bool down, const Params& p) noexcept
  {
    channels[std::size_t(ch)].sustain = down;
    if(down)
      return;
    for(auto& v : voices)
    {
      // Released under the pedal before its voice was free: now just released
      if(v.state == Voice::Stealing && v.pendingChannel == ch && v.pendingSustained)
        v.pendingNote = -1;
      if(v.channel == ch && v.sustained)
      {
        v.sustained = false;
        if(!v.held)
          releaseVoice(v, p);
      }
    }
  }

  // Mono and Legato: one voice, the last note held wins
  struct MonoStack
  {
    std::array<std::pair<int, int>, 16> notes{}; // note, channel
    int size{0};
    void clear() noexcept { size = 0; }
    void push(int n, int c) noexcept
    {
      remove(n, c);
      if(size == int(notes.size()))
      {
        std::copy(notes.begin() + 1, notes.end(), notes.begin());
        size--;
      }
      notes[std::size_t(size++)] = {n, c};
    }
    void remove(int n, int c) noexcept
    {
      for(int i = 0; i < size; i++)
        if(notes[std::size_t(i)] == std::pair{n, c})
        {
          std::copy(notes.begin() + i + 1, notes.begin() + size, notes.begin() + i);
          size--;
          return;
        }
    }
  } monoStack;

  //! Into or out of Poly: the keys held so far are not the stack's
  void followPlayMode(const Params& p) noexcept
  {
    const bool mono = p.play != PlayMode::Poly;
    if(mono != lastMono)
      monoStack.clear();
    lastMono = mono;
  }

  void monoNoteOn(int note, int ch, float vel, const Params& p) noexcept
  {
    const bool wasHeld = monoStack.size > 0;
    monoStack.push(note, ch);
    // Voices left from Poly are released into the background
    for(int i = 1; i < kMaxVoices; i++)
      if(voice(i).state == Voice::Active && voice(i).held)
      {
        voice(i).held = false;
        releaseVoice(voice(i), p);
      }
    auto& v = voice(0);
    if(v.state == Voice::Free)
    {
      startVoice(0, note, ch, vel, p, true);
      return;
    }
    if(p.play == PlayMode::Legato && wasHeld && v.state == Voice::Active)
    {
      // Slide without re-exciting: the glide, or the next tick, takes it there
      v.note = note;
      v.channel = ch;
      v.target = float(note);
      v.held = true;
      return;
    }
    startVoice(0, note, ch, vel, p, false);
  }

  //! The note is already out of the stack
  void monoNoteOff(int note, int ch, const Params& p) noexcept
  {
    auto& v = voice(0);
    // Notes left from Poly
    for(int i = 1; i < kMaxVoices; i++)
    {
      auto& w = voice(i);
      if(w.state == Voice::Active && w.note == note && w.channel == ch && w.held)
      {
        w.held = false;
        releaseVoice(w, p);
      }
    }
    if(v.state != Voice::Active || v.note != note || v.channel != ch)
      return;
    if(monoStack.size > 0)
    {
      // Back to the newest still held, as a legato slide
      const auto [n, c] = monoStack.notes[std::size_t(monoStack.size - 1)];
      v.note = n;
      v.channel = c;
      v.target = float(n);
      return;
    }
    v.held = false;
    if(channels[std::size_t(ch)].sustain)
      v.sustained = true;
    else
      releaseVoice(v, p);
  }

  // ---- control rate ----------------------------------------------------
  void control(const Params& p, int n) noexcept
  {
    // Vibrato, shared by every voice
    lfoPhase += double(n) / (double(p.vibPeriod) * fs);
    lfoPhase -= std::floor(lfoPhase);
    lfoValue = vibrato(p.vibWave, float(lfoPhase));
    // The glide's one-pole over a tick, the same for every voice
    glideCoef = p.glide > 0.f ? 1. - std::exp(-double(n) / (fs * double(p.glide) / 3.)) : 1.;

    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Stealing && v.fade <= 0.f)
      {
        // Faded out: the note it waited for starts now
        const int pn = v.pendingNote, pc = v.pendingChannel;
        const float pv = v.pendingVelocity;
        const bool ps = v.pendingSustained;
        if(pn >= 0)
        {
          startVoice(i, pn, pc, pv, p, true);
          if(ps)
          {
            v.held = false;
            v.sustained = true;
          }
        }
        else
          resetVoice(i);
      }
      if(v.state != Voice::Free)
        controlVoice(i, p, n, true);
    }
    for(int gi = 0; gi < kGroups; gi++)
    {
      auto& g = groups[std::size_t(gi)];
      bool any = false;
      for(int l = 0; l < kLanes; l++)
        any |= voices[std::size_t(gi * kLanes + l)].state != Voice::Free;
      // The last steps written reach kHalf samples ahead, and come out
      // kHalf later
      if(g.active && !any)
        g.drain = 2 * StepResidual::kHalf + 1;
      g.active = any;
    }
  }

  [[nodiscard]] static float vibrato(VibratoWave w, float ph) noexcept
  {
    switch(w)
    {
      default:
      case VibratoWave::Triangle:
        return 1.f - 4.f * std::abs(ph - 0.5f);
      case VibratoWave::SawUp:
        return 2.f * ph - 1.f;
      case VibratoWave::SawDown:
        return 1.f - 2.f * ph;
      case VibratoWave::Square:
        return ph < 0.5f ? 1.f : -1.f;
      case VibratoWave::Sine:
        return std::sin(kTwoPi * ph);
    }
  }

  /*! The ramps of voice i towards where it must be `n` output samples later.
   *  `tickStart`: at the start of a tick, else a note restarted within one,
   *  whose voice has had its glide for this tick already.
   */
  void controlVoice(int i, const Params& p, int n, bool tickStart) noexcept
  {
    auto& v = voice(i);
    auto& g = groups[std::size_t(i / kLanes)];
    g.active = true;
    const auto l = std::size_t(i % kLanes);
    const double fsi = fs * os;
    const int ni = n * os;
    const auto& cs = channels[std::size_t(v.channel)];

    v.age += double(n) / fs;

    // Glide, one-pole in semitones, until within a hundredth of a cent
    if(p.glide > 0.f && p.play != PlayMode::Poly)
    {
      if(tickStart)
      {
        v.pitch += (double(v.target) - v.pitch) * glideCoef;
        if(std::abs(double(v.target) - v.pitch) < 1e-4)
          v.pitch = v.target;
      }
    }
    else
      v.pitch = v.target;

    float vibAmt = p.vibDepth + 0.5f * modWheel;
    if(p.vibDelay > 0.f)
      vibAmt *= std::clamp(float((v.age - p.vibDelay) / (0.5 * p.vibDelay)), 0.f, 1.f);

    const float pressure = v.hasPressure ? v.pressure : cs.pressure;
    const float pitch = float(v.pitch);
    const float base = pitch + 12.f * p.octave + 0.01f * p.tune + cs.bend * p.bendRange
                       + vibAmt * lfoValue;
    // The key follow follows the glide: a legato slide moves the timbre with it
    const float key = pitch - 60.f;

    const bool steal = v.state == Voice::Stealing;
    if(steal)
    {
      // 3 ms to silence
      v.fade -= float(n) / float(0.003 * fs);
      if(v.fade < 0.f)
        v.fade = 0.f;
    }

    bool silent = true;
    // Line 2's envelopes only run when it plays
    const int oscs = p.twoLines() ? 2 : 1;
    for(int o = 0; o < oscs; o++)
    {
      const auto oi = std::size_t(o);
      auto& env = v.env[oi];
      auto& c = v.cache[oi];
      const auto& lp = p.lineOf(o);

      // Envelopes, with key follow on their rates
      if(c.key != key || c.dcaKf != lp.dcaKeyFollow || c.dcwKf != lp.dcwKeyFollow)
      {
        c.key = key;
        c.dcaKf = lp.dcaKeyFollow;
        c.dcwKf = lp.dcwKeyFollow;
        c.dcaRate = std::exp2(double(lp.dcaKeyFollow * key) / 24.) / fs;
        c.dcwRate = std::exp2(double(lp.dcwKeyFollow * key) / 24.) / fs;
      }
      env[0].advance(lp.dca, c.dcaRate, n, v.gate());
      env[1].advance(lp.dcw, c.dcwRate, n, v.gate());
      env[2].advance(lp.dco, 1. / fs, n, v.gate());
      if(!env[0].silent())
        silent = false;

      float semis = base + lp.pitchDepth * env[2].value;
      if(o == 1)
        semis += 12.f * p.detuneOctave + p.detuneNote + 0.01f * p.detuneFine;
      if(c.semis != semis || c.os != os)
      {
        c.semis = semis;
        c.os = os;
        const float hz = 440.f * std::exp2((std::clamp(semis, -24.f, 160.f) - 69.f) / 12.f);
        c.inc = quantizeInc(std::clamp(float(hz / fsi), kIncQuantum, 0.45f));
      }

      float dcwScale = lp.dcwDepth * (1.f - p.velDcw * (1.f - v.velocity));
      dcwScale *= std::clamp(1.f - lp.dcwKeyFollow * std::max(0.f, key + 24.f) / 72.f, 0.f, 1.f);
      const float dcw = std::clamp(
          env[1].value * dcwScale + p.pressureDcw * pressure, 0.f, 0.992f);

      // The chip's DCA, linear in decibels, and the velocity's range below
      // it: one exponential
      const float lvl = env[0].value;
      float amp = lvl > 0.f ? std::exp(
                      -kNepersPerDb
                      * (kDcaRangeDb * (1.f - lvl) + 40.f * p.velLevel * (1.f - v.velocity)))
                            : 0.f;
      amp *= lp.level * v.fade;

      auto ramp = [&](Ramp& r, float target) {
        if(tickStart)
          r.value[l] = r.target[l];
        r.target[l] = target;
        r.step[l] = (target - r.value[l]) / float(ni);
      };
      // The pitch jumps, as on the chip; the others are ramped
      g.inc[oi].value[l] = g.inc[oi].target[l] = c.inc;
      g.inc[oi].step[l] = 0.f;
      ramp(g.dcw[oi], dcw);
      ramp(g.amp[oi], amp);
    }

    if(!steal && silent && !v.gate())
      resetVoice(i);
  }

  // ---- audio rate ------------------------------------------------------
  /*! The second-order antialiasing is a triangle two samples wide: its
   *  response, sinc^2, is -1.8 dB at a quarter of the rate and -5.4 dB at
   *  0.42. A first-order shelf on the mix gives it back, causal, so without
   *  latency: g (1 - q/z) / (1 - p/z), fitted to +-0.07 dB up to 0.42 of the
   *  rate, or +-0.001 dB up to 0.21 at twice the rate. The same for every
   *  voice, so done once, on the sum.
   */
  void compensate(int ni) noexcept
  {
    // Fitted up to 0.42, 0.21 and 0.105 of the rate: within 0.07, 0.001 and
    // 0.0004 dB
    const float p = os == 4 ? -0.325f : os == 2 ? -0.31f : -0.405f;
    const float q = os == 4 ? -0.02f : os == 2 ? -0.015f : -0.065f;
    const float g = os == 4 ? 1.29902f : os == 2 ? 1.29064f : 1.31925f;
    for(int i = 0; i < ni; i++)
    {
      const auto ii = std::size_t(i);
      const float l = bufL[ii], r = bufR[ii];
      const float yl = g * (l - q * eqL[0]) + p * eqL[1];
      const float yr = g * (r - q * eqR[0]) + p * eqR[1];
      eqL = {l, yl};
      eqR = {r, yr};
      bufL[ii] = yl;
      bufR[ii] = yr;
    }
  }

  //! One line, as it renders for this call: what does not change per sample
  struct Line
  {
    Wave a, b;
    int window;
    bool same; // a single shape: the same waveform, or two under one window
    vec4 span; // the samples the pivot must span
    vec4 v;    // VPS's height
    int kind;  // 0: Live; 1: late; 2: late, with its jumps band-limited
  };

  /*! One oscillator of a group, over a render call (at most a tick). While
   *  the DCW holds, its shapes are built once, and selected for the lanes'
   *  parities only when one flips.
   */
  struct Plan
  {
    vec4 dx, inv;    // the increment without noise, and its inverse
    vec4 dc, dcStep; // the line's cycle mean, ramped as the DCW
    Shape a, b;      // the line's two shapes
    Shape here, next;
    u32x4 parity{2u};     // the lanes' parities `here` was selected for
    bool skip{false};     // every lane silent: only its delay plays out
    bool steady{false};   // the DCW holds
  };

  /*! The line's two shapes at DCW `dcw`. The limits come from the increment
   *  without noise, which jumps faster than a cycle: the pivot and the
   *  resonance would jump with it.
   */
  static void lineShapes(
      const Line& line, vec4 dcw, vec4 dx, vec4 inv, vec4 maxReso, Shape& a, Shape& b) noexcept
  {
    // The chip's DCW limit: the pivot stays four samples wide, more for the
    // waveforms whose phase moves faster than the saw's
    const vec4 one(1.f), half(0.5f);
    const vec4 pivot = kfr::min(
        half, kfr::max(half * (one - dcw), kfr::max(line.span * dx, vec4(kMinPivot))));
    vec4 k = one + vec4(16.f) * (one - vec4(2.f) * pivot);
    k = kfr::max(one, kfr::min(k, maxReso * inv));
    // VPS's height, so that neither segment's phase moves faster than the
    // saw's over four samples: the pivot's limit only holds that for the
    // first, and only below its cap of 0.5
    const vec4 B(0.125f);
    const vec4 v = kfr::min(line.v, kfr::min(B * pivot * inv, one + B * (one - pivot) * inv));
    a = lineShape(line.a, line.window, pivot, k, v);
    if(!line.same)
      b = lineShape(line.b, line.window, pivot, k, v);
  }

  //! The line alternates its two shapes: its DC is their means' mean
  [[nodiscard]] static vec4 lineMean(const Line& line, const Shape& a, const Shape& b) noexcept
  {
    return line.same ? cycleMean(a) : vec4(0.5f) * (cycleMean(a) + cycleMean(b));
  }

  void plan(Group& g, std::size_t o, const Line& line, vec4 maxReso, int ni, Plan& pl) noexcept
  {
    pl.dx = kfr::max(vec4(kIncQuantum), kfr::read<4, true>(g.inc[o].value.data()));
    pl.inv = vec4(1.f) / pl.dx;
    const vec4 zero(0.f);
    pl.skip = kfr::all(kfr::read<4, true>(g.amp[o].value.data()) == zero)
              && kfr::all(kfr::read<4, true>(g.amp[o].step.data()) == zero);
    if(pl.skip)
    {
      // Silent: the phases go on, all at once, and the triangle starts anew
      auto& ph = g.phase[o];
      kfr::write<true>(
          ph.data(), kfr::read<4, true>(ph.data()) + phaseSteps(pl.dx) * u32x4(std::uint32_t(ni)));
      g.carry[o] = {};
      return;
    }
    const vec4 dcw = kfr::read<4, true>(g.dcw[o].value.data());
    pl.steady = kfr::all(kfr::read<4, true>(g.dcw[o].step.data()) == zero);
    pl.parity = u32x4(2u);
    lineShapes(line, dcw, pl.dx, pl.inv, maxReso, pl.a, pl.b);
    pl.dcStep = zero;
    auto& mean = g.mean[o];
    if(pl.steady)
    {
      if(!(kfr::all(mean.dcw == dcw) && kfr::all(mean.dx == pl.dx) && kfr::all(mean.v == line.v)
           && mean.a == line.a && mean.b == line.b && mean.os == os))
        mean = {dcw, pl.dx, line.v, line.a, line.b, os, lineMean(line, pl.a, pl.b)};
      pl.dc = mean.value;
    }
    else
    {
      pl.dc = lineMean(line, pl.a, pl.b);
      // From here to where the DCW ends the tick
      Shape a, b;
      lineShapes(line, kfr::read<4, true>(g.dcw[o].target.data()), pl.dx, pl.inv, maxReso, a, b);
      pl.dcStep = (lineMean(line, a, b) - pl.dc) / vec4(float(tickLeft * os));
    }
  }

  //! The phase's steps for an increment: exact, as it is a multiple of 2^-24
  [[nodiscard]] static u32x4 phaseSteps(vec4 dx) noexcept
  {
    return kfr::bitcast<std::uint32_t>(kfr::cast<std::int32_t>(dx * vec4(2147483648.f)));
  }

  //! What a silent oscillator still plays: its delay's last samples
  [[nodiscard]] static vec4 drain(Group& g, std::size_t o) noexcept
  {
    auto& out = g.late[o][(g.count - std::uint32_t(StepResidual::kHalf)) & (kLate - 1)];
    const vec4 r = kfr::read<4, true>(out.data());
    out.fill(0.f);
    return r;
  }

  //! One sample of oscillator o of a group, times its level
  vec4 sample(Group& g, std::size_t o, const Line& line, Plan& pl, bool noised, vec4 maxReso) noexcept
  {
    if(pl.skip)
      return line.kind == 0 ? vec4(0.f) : drain(g, o);

    // 31 bits of cycle, exact in a float as the increments are quantized;
    // bit 31 the cycle's waveform
    const u32x4 ph = kfr::read<4, true>(g.phase[o].data());
    const vec4 x0 = kfr::cast<float>(kfr::bitcast<std::int32_t>(ph & u32x4(0x7fffffffu)))
                    * vec4(4.656612873077393e-10f);
    const u32x4 parity = ph >> 31;

    vec4 dx = pl.dx, inv = pl.inv;
    if(noised)
    {
      // Kept a multiple of the quantum, as the base increment
      const vec4 up = vec4(1.f) + vec4(kNoiseUp - 1.f) * kfr::read<4, true>(g.noiseUp.data());
      dx = kfr::min(vec4(0.45f), dx * up);
      dx = kfr::floor(dx * vec4(16777216.f) + vec4(0.5f)) * vec4(kIncQuantum);
      inv = vec4(1.f) / dx;
    }
    if(!pl.steady)
    {
      lineShapes(line, kfr::read<4, true>(g.dcw[o].value.data()), pl.dx, pl.inv, maxReso, pl.a, pl.b);
      pl.parity = u32x4(2u);
    }

    const Shape* here = &pl.a;
    const Shape* next = &pl.a;
    if(!line.same)
    {
      if(!kfr::all(parity == pl.parity))
      {
        const auto m = kfr::cast<float>(kfr::bitcast<std::int32_t>(parity)) > vec4(0.5f);
        pl.here = selectShape(m, pl.b, pl.a);
        pl.next = selectShape(m, pl.a, pl.b);
        pl.parity = parity;
      }
      here = &pl.here;
      next = &pl.next;
    }

    vec4 out;
    switch(line.kind)
    {
      case 0:
        out = oscillatorT<false, false>(g, o, *here, *next, line.same, x0, dx, inv, pl.dc);
        break;
      case 1:
        out = oscillatorT<false, true>(g, o, *here, *next, line.same, x0, dx, inv, pl.dc);
        break;
      default:
        out = oscillatorT<true, true>(g, o, *here, *next, line.same, x0, dx, inv, pl.dc);
        break;
    }
    if(!pl.steady)
    {
      advance(g.dcw[o]);
      pl.dc += pl.dcStep;
    }
    advance(g.amp[o]);
    return out;
  }

  void renderOscillators(const Params& p, int n) noexcept
  {
    const int ni = n * os;
    std::fill_n(accL.begin(), ni, vec4(0.f));
    std::fill_n(accR.begin(), ni, vec4(0.f));

    const bool two = p.twoLines();
    if(two && !lastTwo)
      // Line 2 did not render: its delay holds an old tail
      for(auto& g : groups)
      {
        g.late[1] = {};
        g.carry[1] = {};
      }
    lastTwo = two;
    const int oscs = two ? 2 : 1;

    Line lines[2];
    for(int o = 0; o < oscs; o++)
    {
      const auto& lp = p.lineOf(o);
      auto& line = lines[o];
      line.a = lp.wave1;
      line.b = lp.second();
      line.window = lineWindow(line.a, line.b);
      line.same = line.a == line.b || (resonant(line.a) && resonant(line.b));
      line.span = kfr::max(pivotSamples(line.a, vec4(lp.formant)), pivotSamples(line.b, vec4(lp.formant)));
      line.v = vec4(lp.formant);
      const bool jumps = line.a == Wave::DoublePulse || line.b == Wave::DoublePulse
                         || line.window == int(Wave::ResoDoubleSaw);
      line.kind = live ? 0 : jumps ? 2 : 1;
    }
    const bool ring = two && p.modulation == Modulation::Ring;
    const bool noise = two && p.modulation == Modulation::Noise;

    // The resonance's peak: below 14.4 kHz at 48 kHz in Clean, as its
    // sidebands spread further up; at 2x up to 0.45 of the output rate, the
    // half-band filter removing what spreads past Nyquist
    const vec4 maxReso(os == 4 ? 0.1125f : os == 2 ? 0.225f : kMaxResoCycles);

    // The noise pitch is drawn anew every 200 us
    const int noisePeriod = std::max(1, int(std::lround(0.0002 * fs * os)));

    /* Spread: line 1 to the left, line 2 (or 1') to the right, equal power;
     * one line alone stays in the centre. Each voice is stereo, so a single
     * note is as wide as a chord, and nothing depends on the pitch: with
     * 1 + 1' both sides are the same line. */
    constexpr float quarterTurn = 0.5f * kPi;
    const float pan0 = two ? 0.5f - 0.5f * p.spread : 0.5f;
    const float pan1 = 0.5f + 0.5f * p.spread;
    const vec4 g0l(kPerOsc * std::cos(quarterTurn * pan0));
    const vec4 g0r(kPerOsc * std::sin(quarterTurn * pan0));
    const vec4 g1l(kPerOsc * std::cos(quarterTurn * pan1));
    const vec4 g1r(kPerOsc * std::sin(quarterTurn * pan1));

    for(auto& g : groups)
    {
      if(!g.active && g.drain <= 0)
        continue;
      Plan plans[2];
      for(int o = 0; o < oscs; o++)
        plan(g, std::size_t(o), lines[o], maxReso, ni, plans[o]);

      int nc = noiseCount;
      for(int s = 0; s < ni; s++)
      {
        if(noise && nc == 0)
          for(int l = 0; l < kLanes; l++)
          {
            auto& x = g.noise[std::size_t(l)];
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            g.noiseUp[std::size_t(l)] = float(x >> 31);
          }
        if(++nc >= noisePeriod)
          nc = 0;

        const auto si = std::size_t(s);
        const vec4 out0 = sample(g, 0, lines[0], plans[0], false, maxReso);
        if(two)
        {
          vec4 out1 = sample(g, 1, lines[1], plans[1], noise, maxReso);
          if(ring)
            out1 = out1 * out0;
          accL[si] += out0 * g0l + out1 * g1l;
          accR[si] += out0 * g0r + out1 * g1r;
        }
        else
        {
          accL[si] += out0 * g0l;
          accR[si] += out0 * g0r;
        }
        g.count++;
      }
      if(!g.active)
        g.drain = std::max(0, g.drain - ni);
    }
    noiseCount = (noiseCount + ni) % noisePeriod;

    // The voices' lanes, summed once for every group
    for(int s = 0; s < ni; s++)
    {
      bufL[std::size_t(s)] = kfr::hadd(accL[std::size_t(s)]);
      bufR[std::size_t(s)] = kfr::hadd(accR[std::size_t(s)]);
    }
  }

  static void advance(Ramp& r) noexcept
  {
    kfr::write<true>(
        r.value.data(), kfr::read<4, true>(r.value.data()) + kfr::read<4, true>(r.step.data()));
  }

  /*! One sample of oscillator o of a group, less the line's mean `dc`, times
   *  its level. FindJumps: band-limit the jumps; Late: through the delay
   *  that needs.
   */
  template <bool FindJumps, bool Late>
  vec4 oscillatorT(
      Group& g, std::size_t o, const Shape& here, const Shape& next, bool same, vec4 x0,
      vec4 dx, vec4 inv, vec4 dc) noexcept
  {
    // Second-order antialiasing: this sample's triangle, from the carry of
    // the previous interval and the falling side over this one
    Jumps jumps;
    const Halves h = sampleInterval<FindJumps>(here, next, same, x0, dx, inv, &jumps);
    auto& carry = g.carry[o];
    // Without its mean: what the DCA moves then has no DC to thump with
    const vec4 y = kfr::read<4, true>(carry.data()) + h.falling - dc;
    kfr::write<true>(carry.data(), h.box * inv - h.falling);

    auto& ph = g.phase[o];
    kfr::write<true>(ph.data(), kfr::read<4, true>(ph.data()) + phaseSteps(dx));

    const vec4 amp = kfr::read<4, true>(g.amp[o].value.data());
    if constexpr(!Late)
      return y * amp;
    auto& late = g.late[o];
    const std::uint32_t n = g.count;
    auto slot = [&](int k) -> std::array<float, kLanes>& {
      return late[(n + std::uint32_t(k)) & (kLate - 1)];
    };
    kfr::write<true>(slot(0).data(), kfr::read<4, true>(slot(0).data()) + y * amp);

    if constexpr(FindJumps)
    {
      // Each jump's band-limited step, over the samples around it
      alignas(16) std::array<float, kLanes> at, height;
      for(int j = 0; j < 2; j++)
      {
        if(!kfr::any(kfr::abs(jumps.height[j]) > vec4(1e-6f)))
          continue;
        kfr::write<true>(at.data(), jumps.at[j]);
        kfr::write<true>(height.data(), jumps.height[j] * amp);
        for(int l = 0; l < kLanes; l++)
        {
          const auto li = std::size_t(l);
          if(height[li] == 0.f)
            continue;
          for(int k = -StepResidual::kHalf + 1; k <= StepResidual::kHalf; k++)
            slot(k)[li] += height[li] * residual(float(k) - at[li]);
        }
      }
    }

    // Late by the steps' reach back
    auto& out = slot(-StepResidual::kHalf);
    const vec4 r = kfr::read<4, true>(out.data());
    out.fill(0.f);
    return r;
  }

  StepResidual residual;
  bool built{false};
  double fs{48000.};
  int os{1};        // the internal rate, in output samples
  bool live{false}; // no delay, jumps not band-limited
  int mode{-1};     // modeOf() of what renders; -1 before the first render
  float switchGain{1.f};
  float switchStep{1.f / 240.f};
  bool lastMono{false};
  bool lastTwo{false};
  std::array<vec4, 4 * kCtrl> accL{}, accR{};
  std::array<float, 4 * kCtrl> bufL{}, bufR{};
  Decimator2x dec[2];
  Decimator4x dec4[2];
  std::uint64_t stamp{0};
  int tickLeft{0};
  int noiseCount{0};
  double lfoPhase{0.};
  float lfoValue{0.f};
  double glideCoef{1.};
  float dcL{0.f}, dcR{0.f}, dcLy{0.f}, dcRy{0.f};
  std::array<float, 2> eqL{}, eqR{}; // the compensation's last input and output
  float dcCoef{0.9987f};
  float limGain{1.f};
  float limAttack{0.02f};
  float limRelease{0.0002f};
};
}
