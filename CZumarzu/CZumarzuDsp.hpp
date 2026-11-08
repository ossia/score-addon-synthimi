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

#if (defined(__SSE__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
#include <xmmintrin.h>
#define CZUMARZU_HAS_MXCSR 1
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
//! Clean 2x, 4x: the same at two or four times the rate, then half-band
//! filters back to it.
//! Live: no latency: second-order antialiasing, jumps not band-limited.
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
inline constexpr int kLineModeCount = 4;
inline constexpr int kModulationCount = 3;
inline constexpr int kQualityCount = 4;
inline constexpr int kPlayModeCount = 3;
inline constexpr int kVibratoWaveCount = 5;

//! By bit test: survives -ffinite-math-only
inline bool finite(float x) noexcept
{
  std::uint32_t u;
  std::memcpy(&u, &x, sizeof(u));
  return (u & 0x7f800000u) != 0x7f800000u;
}
inline float sane(float x, float lo, float hi, float fallback) noexcept
{
  return finite(x) ? std::clamp(x, lo, hi) : fallback;
}
template <typename E>
E saneEnum(E e, int count) noexcept
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
  EnvSpec dca{defaultDca()};
  EnvSpec dcw{defaultDcw()};
  EnvSpec dco{defaultDco()};

  static EnvSpec defaultDca() { return EnvSpec::make({{0.95f, 1.f}, {0.62f, 0.f}}, 0, 1); }
  static EnvSpec defaultDcw()
  {
    return EnvSpec::make({{0.9f, 0.7f}, {0.45f, 0.35f}, {0.6f, 0.f}}, 1, 2);
  }
  static EnvSpec defaultDco() { return EnvSpec::make({{1.f, 0.f}}, -1, 0); }

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
  }

  Wave second() const noexcept
  {
    return wave2 == SecondWave::Off ? wave1 : Wave(int(wave2) - 1);
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
    detuneFine = sane(detuneFine, -100.f, 100.f, 0.f);
    vibPeriod = sane(vibPeriod, 0.01f, 100.f, 0.18f);
    vibDelay = sane(vibDelay, 0.f, 10.f, 0.f);
    vibDepth = sane(vibDepth, 0.f, 12.f, 0.f);
    bendRange = sane(bendRange, 0.f, 48.f, 2.f);
    glide = sane(glide, 0.f, 10.f, 0.f);
    velLevel = sane(velLevel, 0.f, 1.f, 0.5f);
    velDcw = sane(velDcw, 0.f, 1.f, 0.3f);
    pressureDcw = sane(pressureDcw, -1.f, 1.f, 0.3f);
    spread = sane(spread, 0.f, 1.f, 0.3f);
    level = sane(level, 0.f, 2.f, 0.7f);
    voices = std::clamp(voices, 1, kMaxVoices);
  }

  bool twoLines() const noexcept
  {
    return mode == LineMode::Line1Doubled || mode == LineMode::Line1And2;
  }
  //! The parameters of oscillator o (0, or 1 in the two-line modes)
  const LineParams& lineOf(int o) const noexcept
  {
    if(o == 0)
      return line[mode == LineMode::Line2 ? 1 : 0];
    return line[mode == LineMode::Line1Doubled ? 0 : 1];
  }
};

struct ScopedFlushDenormals
{
#if CZUMARZU_HAS_MXCSR
  unsigned int saved = _mm_getcsr();
  ScopedFlushDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
  ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#endif
};


class Synth
{
public:
  //! Ramp of one oscillator parameter over a control tick, per lane
  struct alignas(16) Ramp
  {
    std::array<float, kLanes> value{}, step{};
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
    float pitch{60.f};     // semitones, where the glide is
    float target{60.f};
    double age{0.};        // seconds since the note started
    float pressure{0.f};   // polyphonic aftertouch
    bool hasPressure{false};
    std::array<std::array<EnvState, 3>, 2> env{}; // [osc][DCA, DCW, DCO]

    // Stealing: fading out before this note starts
    int pendingNote{-1};
    int pendingChannel{0};
    float pendingVelocity{0.f};
    float fade{1.f};

    bool gate() const noexcept { return held || sustained; }
  };

  struct alignas(16) Group
  {
    // [osc][lane]: the phase is the chip's: 31 bits of cycle, bit 31 says
    // which of the two waveforms this cycle plays
    std::array<std::array<std::uint32_t, kLanes>, 2> phase{};
    std::array<Ramp, 2> inc{}; // cycles per (internal) sample
    std::array<Ramp, 2> dcw{}; // [0, 0.992]
    std::array<Ramp, 2> amp{};
    std::array<std::uint32_t, kLanes> noise{}; // xorshift per lane
    std::array<float, kLanes> noiseUp{};       // 1: line 2 is 32 semitones up
    //! The rising side of the next sample's triangle, from this interval
    std::array<std::array<float, kLanes>, 2> carry{};
    //! Each line's output, StepResidual::kHalf samples late, while the
    //! band-limited steps of its jumps are added around them:
    //! [osc][sample & 31][lane]
    std::array<std::array<std::array<float, kLanes>, 32>, 2> late{};
    std::uint32_t count{0};
    bool active{false};
  };

  void prepare(double rate) noexcept
  {
    fs = (rate > 0. && finite(float(rate))) ? rate : 48000.;
    dec[0].prepare();
    dec[1].prepare();
    dec4[0].prepare();
    dec4[1].prepare();
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
    tickLeft = 0;
    lfoPhase = 0.;
    modWheel = 0.f;
    dcL = dcR = dcLy = dcRy = 0.f;
    eqL = eqR = {};
    limGain = 1.f;
    // 10 Hz DC blocker, 100 ms limiter release
    dcCoef = float(1. - 2. * 3.14159265358979 * 10. / fs);
    limRelease = float(1. - std::exp(-1. / (0.1 * fs)));
    lastQuality = -1;
    noiseCount = 0;
  }

  double sampleRate() const noexcept { return fs; }

  int activeVoices() const noexcept
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
        if(d1 == 1)
          modWheel = float(d2) / 127.f;
        else if(d1 == 64)
        {
          const bool down = d2 >= 64;
          cs.sustain = down;
          if(!down)
            for(auto& v : voices)
              if(v.channel == ch && v.sustained)
              {
                v.sustained = false;
                if(!v.held)
                  releaseVoice(v, p);
              }
        }
        else if(d1 == 120 || d1 == 123)
        {
          // On this channel; a note waiting for a stolen voice is dropped too
          for(int i = 0; i < kMaxVoices; i++)
          {
            auto& v = voices[std::size_t(i)];
            if(v.state == Voice::Stealing && v.pendingChannel == ch)
              v.pendingNote = -1;
            if(v.state == Voice::Free || v.channel != ch)
              continue;
            if(d1 == 120)
              resetVoice(i);
            else
            {
              v.held = v.sustained = false;
              releaseVoice(v, p);
            }
          }
          monoStack.clear();
        }
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
    if(int(p.quality) != lastQuality)
    {
      // The internal rate changes: increments are per internal sample
      lastQuality = int(p.quality);
      dec[0].reset();
      dec[1].reset();
      dec4[0].reset();
      dec4[1].reset();
      eqL = eqR = {};
      for(auto& g : groups)
        g.late = {};
      tickLeft = 0;
    }
    const int os = oversampling(p.quality);
    preLimiterPeak = 0.f;

    int done = 0;
    while(done < frames)
    {
      if(tickLeft == 0)
      {
        control(p, kCtrl);
        tickLeft = kCtrl;
      }
      const int n = std::min(frames - done, tickLeft);
      renderOscillators(p, os, n);
      compensate(os, n * os);

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
        // DC: the windowed waveforms are not centred
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
        // Peak limiter: instant attack, 100 ms release, then a hard clamp
        if(peak * limGain > kLimit)
          limGain = kLimit / peak;
        else
          limGain += (1.f - limGain) * limRelease;
        l[done + i] = std::clamp(ol * limGain, -1.f, 1.f);
        r[done + i] = std::clamp(or_ * limGain, -1.f, 1.f);
      }
      done += n;
      tickLeft -= n;
    }
  }

  //! Everything silent and free, at once
  void panic() noexcept
  {
    for(int i = 0; i < kMaxVoices; i++)
      resetVoice(i);
    dec[0].reset();
    dec[1].reset();
    dcL = dcR = dcLy = dcRy = 0.f;
    monoStack.clear();
  }

  // Exposed for tests
  std::array<Voice, kMaxVoices> voices{};
  std::array<Group, kGroups> groups{};

private:
  static constexpr float kLimit = 0.97f;
  static constexpr float kPerOsc = 0.25f; // one note of two lines peaks at 0.5
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kNoiseUp = 6.349604207872798f; // 2^(32/12)

  struct ChannelState
  {
    float bend{0.f};
    float pressure{0.f};
    bool sustain{false};
  };

  // ---- voices ----------------------------------------------------------
  Voice& voice(int i) noexcept { return voices[std::size_t(i)]; }

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
      for(auto& slot : g.late[std::size_t(o)])
        slot[l] = 0.f;
      for(auto* r : {&g.inc[std::size_t(o)], &g.dcw[std::size_t(o)], &g.amp[std::size_t(o)]})
      {
        r->value[l] = 0.f;
        r->step[l] = 0.f;
      }
    }
    g.noiseUp[l] = 0.f;
  }

  void startVoice(int i, int note, int ch, float vel, const Params& p, bool fresh) noexcept
  {
    auto& v = voice(i);
    if(fresh)
    {
      resetVoice(i);
      v.pitch = float(note);
    }
    v.state = Voice::Active;
    v.note = note;
    v.channel = ch;
    v.velocity = vel;
    v.held = true;
    v.sustained = false;
    v.stamp = ++stamp;
    v.target = float(note);
    v.age = 0.;
    v.pendingNote = -1;
    v.fade = 1.f;
    if(fresh || p.play == PlayMode::Poly)
      v.pitch = float(note);
    for(int o = 0; o < 2; o++)
    {
      const auto& lp = p.lineOf(o);
      v.env[std::size_t(o)][0].start(lp.dca);
      v.env[std::size_t(o)][1].start(lp.dcw);
      v.env[std::size_t(o)][2].start(lp.dco);
    }
    // Sample accurate: the rest of this tick plays the new note
    if(tickLeft > 0)
      controlVoice(i, p, tickLeft);
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
    v.stamp = ++stamp;
  }

  void noteOff(int note, int ch, const Params& p) noexcept
  {
    if(p.play != PlayMode::Poly)
    {
      monoNoteOff(note, ch, p);
      return;
    }
    for(auto& v : voices)
    {
      if(v.state == Voice::Stealing && v.pendingNote == note && v.pendingChannel == ch)
        v.pendingNote = -1; // released before it started: the fade just ends
      if(v.state == Voice::Active && v.note == note && v.channel == ch && v.held)
      {
        v.held = false;
        if(channels[std::size_t(ch)].sustain)
          v.sustained = true;
        else
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
      // Slide without re-exciting
      v.note = note;
      v.channel = ch;
      v.target = float(note);
      if(p.glide <= 0.f)
        v.pitch = v.target;
      v.held = true;
      return;
    }
    startVoice(0, note, ch, vel, p, false);
    v.target = float(note);
    if(p.glide <= 0.f)
      v.pitch = v.target;
  }

  void monoNoteOff(int note, int ch, const Params& p) noexcept
  {
    monoStack.remove(note, ch);
    auto& v = voice(0);
    // Notes left from Poly
    for(int i = 1; i < kMaxVoices; i++)
      if(voice(i).state == Voice::Active && voice(i).note == note && voice(i).held)
      {
        voice(i).held = false;
        releaseVoice(voice(i), p);
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
      if(p.glide <= 0.f)
        v.pitch = v.target;
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

    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Stealing && v.fade <= 0.f)
      {
        // Faded out: the note it waited for starts now
        const int pn = v.pendingNote, pc = v.pendingChannel;
        const float pv = v.pendingVelocity;
        if(pn >= 0)
          startVoice(i, pn, pc, pv, p, true);
        else
          resetVoice(i);
      }
      if(v.state != Voice::Free)
        controlVoice(i, p, n);
    }
    for(auto& g : groups)
    {
      g.active = false;
      for(int l = 0; l < kLanes; l++)
        g.active |= voices[std::size_t(&g - groups.data()) * kLanes + std::size_t(l)].state
                    != Voice::Free;
    }
  }

  static float vibrato(VibratoWave w, float ph) noexcept
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

  //! The ramps of voice i towards where it must be `n` output samples later
  void controlVoice(int i, const Params& p, int n) noexcept
  {
    auto& v = voice(i);
    auto& g = groups[std::size_t(i / kLanes)];
    g.active = true;
    const auto l = std::size_t(i % kLanes);
    const int os = oversampling(p.quality);
    const double fsi = fs * os;
    const int ni = n * os;
    const auto& cs = channels[std::size_t(v.channel)];

    v.age += double(n) / fs;

    // Glide, one-pole in semitones
    if(p.glide > 0.f && p.play != PlayMode::Poly)
      v.pitch += (v.target - v.pitch)
                 * float(1. - std::exp(-double(n) / (fs * double(p.glide) / 3.)));
    else
      v.pitch = v.target;

    float vibAmt = p.vibDepth + 0.5f * modWheel;
    if(p.vibDelay > 0.f)
      vibAmt *= std::clamp(float((v.age - p.vibDelay) / (0.5 * p.vibDelay)), 0.f, 1.f);

    const float pressure = v.hasPressure ? v.pressure : cs.pressure;
    const float base = v.pitch + 12.f * p.octave + 0.01f * p.tune + cs.bend * p.bendRange
                       + vibAmt * lfoValue;
    const float keyFrom60 = float(v.note - 60);

    const bool steal = v.state == Voice::Stealing;
    if(steal)
    {
      // 3 ms to silence
      v.fade -= float(n) / float(0.003 * fs);
      if(v.fade < 0.f)
        v.fade = 0.f;
    }

    bool silent = true;
    const int oscs = p.twoLines() ? 2 : 1;
    for(int o = 0; o < 2; o++)
    {
      const auto oi = std::size_t(o);
      auto& env = v.env[oi];
      const auto& lp = p.lineOf(o);
      const bool on = o < oscs;

      // Envelopes, with key follow on their rates
      const double dcaRate = std::exp2(lp.dcaKeyFollow * keyFrom60 / 24.f) / fs;
      const double dcwRate = std::exp2(lp.dcwKeyFollow * keyFrom60 / 24.f) / fs;
      env[0].advance(lp.dca, dcaRate, n, v.gate());
      env[1].advance(lp.dcw, dcwRate, n, v.gate());
      env[2].advance(lp.dco, 1. / fs, n, v.gate());
      if(on && !env[0].silent())
        silent = false;

      float semis = base + lp.pitchDepth * env[2].value;
      if(o == 1)
        semis += 12.f * p.detuneOctave + p.detuneNote + 0.01f * p.detuneFine;
      const float hz = 440.f * std::exp2((std::clamp(semis, -24.f, 160.f) - 69.f) / 12.f);
      const float inc = std::clamp(float(hz / fsi), 1e-7f, 0.45f);

      float dcwScale = lp.dcwDepth * (1.f - p.velDcw * (1.f - v.velocity));
      dcwScale *= std::clamp(1.f - lp.dcwKeyFollow * std::max(0.f, keyFrom60 + 24.f) / 72.f,
                             0.f, 1.f);
      const float dcw = std::clamp(
          env[1].value * dcwScale + p.pressureDcw * pressure, 0.f, 0.992f);

      float amp = 0.f;
      if(on)
      {
        const float lvl = env[0].value;
        // The chip's DCA: linear in a 78 dB log scale
        amp = lvl > 0.f ? std::exp(-0.11512925f * 78.27f * (1.f - lvl)) : 0.f;
        amp *= std::exp(-0.11512925f * 40.f * p.velLevel * (1.f - v.velocity));
        amp *= lp.level * v.fade;
      }

      auto ramp = [&](Ramp& r, float target) {
        r.step[l] = (target - r.value[l]) / float(ni);
      };
      // The pitch jumps, as on the chip; the others are ramped
      g.inc[oi].value[l] = inc;
      g.inc[oi].step[l] = 0.f;
      ramp(g.dcw[oi], dcw);
      ramp(g.amp[oi], amp);
    }

    if(!steal && silent && !v.gate())
      resetVoice(i);
  }

  static int oversampling(Quality q) noexcept
  {
    return q == Quality::Clean4x ? 4 : q == Quality::Clean2x ? 2 : 1;
  }

  // ---- audio rate ------------------------------------------------------
  /*! The second-order antialiasing is a triangle two samples wide: its
   *  response, sinc^2, is -1.8 dB at a quarter of the rate and -5.4 dB at
   *  0.42. A first-order shelf on the mix gives it back, causal, so without
   *  latency: g (1 - q/z) / (1 - p/z), fitted to +-0.07 dB up to 0.42 of the
   *  rate, or +-0.001 dB up to 0.21 at twice the rate. The same for every
   *  voice, so done once, on the sum.
   */
  void compensate(int os, int ni) noexcept
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

  void renderOscillators(const Params& p, int os, int n) noexcept
  {
    const int ni = n * os;
    std::fill_n(bufL.begin(), ni, 0.f);
    std::fill_n(bufR.begin(), ni, 0.f);

    const bool two = p.twoLines();
    const auto& l0 = p.lineOf(0);
    const auto& l1 = p.lineOf(1);
    const Wave wa[2] = {l0.wave1, l1.wave1};
    const Wave wb[2] = {l0.second(), l1.second()};
    const vec4 formant[2] = {vec4(l0.formant), vec4(l1.formant)};
    const bool ring = two && p.modulation == Modulation::Ring;
    const bool noise = two && p.modulation == Modulation::Noise;


    // The resonance's peak: below 14.4 kHz at 48 kHz in Clean, as its
    // sidebands spread further up; at 2x up to 0.45 of the output rate, the
    // half-band filter removing what spreads past Nyquist
    const vec4 maxReso(os == 4 ? 0.1125f : os == 2 ? 0.225f : kMaxResoCycles);
    const bool live = p.quality == Quality::Live;

    // The noise pitch is drawn anew every 200 us
    const int noisePeriod = std::max(1, int(std::lround(0.0002 * fs * os)));

    /* Spread: line 1 to the left, line 2 (or 1') to the right, equal power;
     * one line alone stays in the centre. Each voice is stereo, so a single
     * note is as wide as a chord, and nothing depends on the pitch: with
     * 1 + 1' both sides are the same line. */
    const float pan0 = two ? 0.5f - 0.5f * p.spread : 0.5f;
    const float pan1 = 0.5f + 0.5f * p.spread;
    const float g0l = kPerOsc * std::cos(1.5707963f * pan0);
    const float g0r = kPerOsc * std::sin(1.5707963f * pan0);
    const float g1l = kPerOsc * std::cos(1.5707963f * pan1);
    const float g1r = kPerOsc * std::sin(1.5707963f * pan1);

    for(auto& g : groups)
    {
      if(!g.active)
        continue;
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

        vec4 out[2];
        for(int o = 0; o < (two ? 2 : 1); o++)
        {
          const auto oi = std::size_t(o);
          // Lanes without a voice have no increment: keep 0 / 0 out
          vec4 dx = kfr::max(vec4(1e-7f), kfr::read<4, true>(g.inc[oi].value.data()));
          if(o == 1 && noise)
            dx = kfr::min(
                vec4(0.45f),
                dx * (vec4(1.f) + vec4(kNoiseUp - 1.f) * kfr::read<4, true>(g.noiseUp.data())));
          out[o] = oscillator(g, oi, wa[o], wb[o], formant[o], dx, maxReso, live);
          advance(g.dcw[oi]);
          advance(g.amp[oi]);
        }
        g.count++;
        if(ring)
          out[1] = out[1] * out[0];

        const float s0 = kfr::hadd(out[0]);
        float sl = s0 * g0l, sr = s0 * g0r;
        if(two)
        {
          const float s1 = kfr::hadd(out[1]);
          sl += s1 * g1l;
          sr += s1 * g1r;
        }
        bufL[std::size_t(s)] += sl;
        bufR[std::size_t(s)] += sr;
      }
    }
    noiseCount = (noiseCount + ni) % noisePeriod;
  }

  static void advance(Ramp& r) noexcept
  {
    kfr::write<true>(
        r.value.data(), kfr::read<4, true>(r.value.data()) + kfr::read<4, true>(r.step.data()));
  }

  //! One sample of oscillator o of a group, times its level
  static vec4 oscillator(
      Group& g, std::size_t o, Wave a, Wave b, vec4 v, vec4 dx, vec4 maxReso,
      bool live) noexcept
  {
    if(live)
      return oscillatorT<false, false>(g, o, a, b, v, dx, maxReso);
    const int window = lineWindow(a, b);
    const bool jumps = a == Wave::DoublePulse || b == Wave::DoublePulse
                       || window == int(Wave::ResoDoubleSaw);
    return jumps ? oscillatorT<true, true>(g, o, a, b, v, dx, maxReso)
                 : oscillatorT<false, true>(g, o, a, b, v, dx, maxReso);
  }

  //! FindJumps: band-limit the jumps; Late: through the delay that needs
  template <bool FindJumps, bool Late>
  static vec4
  oscillatorT(Group& g, std::size_t o, Wave a, Wave b, vec4 v, vec4 dx, vec4 maxReso) noexcept
  {
    alignas(16) std::array<float, kLanes> x0f, parity;
    auto& ph = g.phase[o];
    for(int l = 0; l < kLanes; l++)
    {
      const auto li = std::size_t(l);
      x0f[li] = float(std::int32_t(ph[li] & 0x7fffffffu)) * 4.656612873077393e-10f;
      parity[li] = float(ph[li] >> 31);
    }
    const vec4 x0 = kfr::read<4, true>(x0f.data());

    // The chip's DCW limit: the pivot stays four samples wide, more for the
    // waveforms whose phase moves faster than the saw's
    const vec4 dcw = kfr::read<4, true>(g.dcw[o].value.data());
    const vec4 span = kfr::max(pivotSamples(a, v), pivotSamples(b, v));
    const vec4 pivot = kfr::min(
        vec4(0.5f), kfr::max(vec4(0.5f) * (vec4(1.f) - dcw), kfr::max(span * dx, vec4(kMinPivot))));
    vec4 k = vec4(1.f) + vec4(16.f) * (vec4(1.f) - vec4(2.f) * pivot);
    k = kfr::max(vec4(1.f), kfr::min(k, maxReso / dx));

    // Second-order antialiasing: this sample's triangle, from the carry of
    // the previous interval and the falling side over this one
    Halves h;
    Jumps jumps;
    const int window = lineWindow(a, b);
    if(a == b || (resonant(a) && resonant(b)))
    {
      const Shape s = lineShape(a, window, pivot, k, v);
      h = sampleInterval<FindJumps>(s, s, true, x0, dx, &jumps);
    }
    else
    {
      const Shape sa = lineShape(a, window, pivot, k, v);
      const Shape sb = lineShape(b, window, pivot, k, v);
      const auto odd = kfr::read<4, true>(parity.data()) > vec4(0.5f);
      h = sampleInterval<FindJumps>(
          selectShape(odd, sb, sa), selectShape(odd, sa, sb), false, x0, dx, &jumps);
    }
    auto& carry = g.carry[o];
    const vec4 y = kfr::read<4, true>(carry.data()) + h.falling;
    kfr::write<true>(carry.data(), h.box / dx - h.falling);

    alignas(16) std::array<float, kLanes> dxf;
    kfr::write<true>(dxf.data(), dx);
    for(int l = 0; l < kLanes; l++)
      ph[std::size_t(l)] += std::uint32_t(dxf[std::size_t(l)] * 2147483648.f);

    const vec4 amp = kfr::read<4, true>(g.amp[o].value.data());
    if constexpr(!Late)
      return y * amp;
    auto& late = g.late[o];
    const std::uint32_t n = g.count;
    auto slot = [&](int k) -> std::array<float, kLanes>& {
      return late[(n + std::uint32_t(k)) & 31u];
    };
    kfr::write<true>(slot(0).data(), kfr::read<4, true>(slot(0).data()) + y * amp);

    if constexpr(FindJumps)
    {
      // Each jump's band-limited step, over the samples around it
      const auto& residual = StepResidual::instance();
      alignas(16) std::array<float, kLanes> at, height, gain;
      kfr::write<true>(gain.data(), amp);
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

  double fs{48000.};
  std::array<ChannelState, 16> channels{};
  std::array<float, 4 * kCtrl> bufL{}, bufR{};
  Decimator2x dec[2];
  Decimator4x dec4[2];
  std::uint64_t stamp{0};
  int tickLeft{0};
  int lastQuality{-1};
  int noiseCount{0};
  double lfoPhase{0.};
  float lfoValue{0.f};
  float modWheel{0.f};
  float dcL{0.f}, dcR{0.f}, dcLy{0.f}, dcRy{0.f};
  std::array<float, 2> eqL{}, eqR{}; // the compensation's last input and output
  float dcCoef{0.9987f};
  float limGain{1.f};
  float limRelease{0.0002f};
};
}
