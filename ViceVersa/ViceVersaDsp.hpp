#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* A vector synthesizer after the Prophet VS.
 *
 * Four wavetable sources, A to D, on the corners of a diamond; where a point
 * is in it sets their mix (Dsp/Vector.hpp). The point is the X/Y controls,
 * plus a five-point mix envelope, four LFOs per voice (Dsp/Modulation.hpp,
 * which also move the pitches, frames, cutoff and level), and per-note
 * expression (pressure to Y, CC 74 to X). Each source reads a band-limited wave bank
 * (Dsp/Wavetable.hpp) at its own frame position and tuning. Then a four-pole
 * ladder low-pass with its envelope, and the amplitude envelope.
 *
 * 16 voices in four groups of four SIMD lanes, one lane per voice. Envelopes,
 * the LFOs, the mix, the filter's coefficients and the wave banks' mip levels
 * are computed every kCtrl samples and ramped; per sample only the table
 * reads, the mix, the filter and the level. A source no voice of a group
 * hears costs nothing in that group.
 *
 * Nothing allocates after prepare().
 */

#include <ViceVersa/Dsp/Banks.hpp>
#include <ViceVersa/Dsp/Ladder.hpp>
#include <ViceVersa/Dsp/Modulation.hpp>
#include <ViceVersa/Dsp/Vector.hpp>
#include <ViceVersa/Dsp/Wavetable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#if (defined(__SSE__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
#include <xmmintrin.h>
#define VICEVERSA_HAS_MXCSR 1
#endif

namespace viceversa
{
inline constexpr int kLanes = 4;
inline constexpr int kGroups = 4;
inline constexpr int kMaxVoices = kLanes * kGroups;
static_assert(kMaxVoices <= 16, "LivePoints holds 16 points");
inline constexpr int kCtrl = 16;

// Stored by index in presets: append only
enum class MixLaw : int
{
  Linear,
  EqualPower
};
enum class PlayMode : int
{
  Poly,
  Mono,
  Legato
};
inline constexpr int kMixLawCount = 2;
inline constexpr int kPlayModeCount = 3;

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

struct SourceParams
{
  BankId bank{BankId::SineSaw};
  float frame{0.f};  // [0, 1] over the bank
  float coarse{0.f}; // semitones
  float fine{0.f};   // cents
  float level{1.f};
};

struct Params
{
  std::array<SourceParams, kSources> src{};

  float x{0.f}, y{0.f};
  MixLaw law{MixLaw::Linear};
  VecEnvSpec vecEnv{VecEnvSpec::centred()};
  float vecEnvDepth{1.f};

  std::array<LfoParams, kLfos> lfos{};

  float frameEnv{0.f};  // the filter envelope's reach over the frames

  float cutoff{8000.f}; // Hz
  float resonance{0.2f};
  float filterEnv{24.f}; // semitones
  float keyTrack{0.5f};
  float velCutoff{12.f}; // semitones at full velocity
  float drive{0.3f};

  float fA{0.005f}, fD{0.4f}, fS{0.3f}, fR{0.4f};
  float aA{0.005f}, aD{0.3f}, aS{0.8f}, aR{0.4f};

  PlayMode play{PlayMode::Poly};
  float glide{0.f};
  float bendRange{2.f};
  int voices{kMaxVoices};
  float octave{0.f};
  float tune{0.f};
  float velLevel{0.5f};
  float pressureY{0.5f};
  float timbreX{0.5f};
  float spread{0.3f};
  float level{0.7f};

  void sanitize() noexcept
  {
    for(auto& s : src)
    {
      s.bank = saneEnum(s.bank, kBankCount);
      s.frame = sane(s.frame, 0.f, 1.f, 0.f);
      s.coarse = std::round(sane(s.coarse, -24.f, 24.f, 0.f));
      s.fine = sane(s.fine, -100.f, 100.f, 0.f);
      s.level = sane(s.level, 0.f, 1.f, 1.f);
    }
    x = sane(x, -1.f, 1.f, 0.f);
    y = sane(y, -1.f, 1.f, 0.f);
    law = saneEnum(law, kMixLawCount);
    vecEnvDepth = sane(vecEnvDepth, 0.f, 1.f, 1.f);
    for(auto& l : lfos)
    {
      l.target = saneEnum(l.target, kModTargetCount);
      l.shape = saneEnum(l.shape, kLfoShapeCount);
      l.period = sane(l.period, 0.01f, 100.f, 2.f);
      l.depth = sane(l.depth, 0.f, 1.f, 0.5f);
      l.phase = sane(l.phase, 0.f, 1.f, 0.f);
      l.delay = sane(l.delay, 0.f, 10.f, 0.f);
    }
    frameEnv = sane(frameEnv, -1.f, 1.f, 0.f);
    cutoff = sane(cutoff, 20.f, 20000.f, 8000.f);
    resonance = sane(resonance, 0.f, 1.f, 0.2f);
    filterEnv = sane(filterEnv, -72.f, 72.f, 24.f);
    keyTrack = sane(keyTrack, 0.f, 1.f, 0.5f);
    velCutoff = sane(velCutoff, 0.f, 48.f, 12.f);
    drive = sane(drive, 0.f, 1.f, 0.3f);
    fA = sane(fA, 0.f, 20.f, 0.005f);
    fD = sane(fD, 0.f, 20.f, 0.4f);
    fS = sane(fS, 0.f, 1.f, 0.3f);
    fR = sane(fR, 0.f, 20.f, 0.4f);
    aA = sane(aA, 0.f, 20.f, 0.005f);
    aD = sane(aD, 0.f, 20.f, 0.3f);
    aS = sane(aS, 0.f, 1.f, 0.8f);
    aR = sane(aR, 0.f, 20.f, 0.4f);
    play = saneEnum(play, kPlayModeCount);
    glide = sane(glide, 0.f, 10.f, 0.f);
    bendRange = sane(bendRange, 0.f, 48.f, 2.f);
    voices = std::clamp(voices, 1, kMaxVoices);
    octave = std::round(sane(octave, -3.f, 3.f, 0.f));
    tune = sane(tune, -100.f, 100.f, 0.f);
    velLevel = sane(velLevel, 0.f, 1.f, 0.5f);
    pressureY = sane(pressureY, -1.f, 1.f, 0.5f);
    timbreX = sane(timbreX, -1.f, 1.f, 0.5f);
    spread = sane(spread, 0.f, 1.f, 0.3f);
    level = sane(level, 0.f, 2.f, 0.7f);
  }
};

//! Attack linear, decay and release exponential (-40 dB in their time)
struct Adsr
{
  enum Stage : std::uint8_t
  {
    Idle,
    Attack,
    Decay,
    Release
  } stage{Idle};
  float value{0.f};

  void on() noexcept { stage = Attack; }
  void off() noexcept
  {
    if(stage != Idle)
      stage = Release;
  }
  void reset() noexcept
  {
    stage = Idle;
    value = 0.f;
  }
  bool idle() const noexcept { return stage == Idle; }

  void advance(float a, float d, float s, float r, int n, double fs) noexcept
  {
    switch(stage)
    {
      case Idle:
        return;
      case Attack:
        value += float(n / std::max(a * fs, 1.));
        if(value >= 1.f)
        {
          value = 1.f;
          stage = Decay;
        }
        return;
      case Decay:
        value = s + (value - s) * float(std::exp(-4.6 * n / std::max(d * fs, 1.)));
        return;
      case Release:
        value *= float(std::exp(-4.6 * n / std::max(r * fs, 1.)));
        if(value < 1e-4f)
          reset();
        return;
    }
  }
};

struct ScopedFlushDenormals
{
#if VICEVERSA_HAS_MXCSR
  unsigned int saved = _mm_getcsr();
  ScopedFlushDenormals() noexcept { _mm_setcsr(saved | 0x8040); }
  ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#endif
};

class Synth
{
public:
  struct alignas(16) Ramp
  {
    std::array<float, kLanes> value{}, step{};
    vec4 get() const noexcept { return kfr::read<4, true>(value.data()); }
    void advance() noexcept
    {
      kfr::write<true>(value.data(), get() + kfr::read<4, true>(step.data()));
    }
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
    bool held{false};
    bool sustained{false};
    std::uint64_t stamp{0};
    float pitch{60.f};
    float target{60.f};
    double age{0.};
    float pressure{0.f};
    bool hasPressure{false};
    Adsr amp, filter;
    VecEnvState vec;
    int pendingNote{-1};
    int pendingChannel{0};
    float pendingVelocity{0.f};
    float fade{1.f};
    float vx{0.f}, vy{0.f}; // where the point was last, for the editor
    std::array<Lfo, kLfos> lfo{};
    double vibrato{0.}; // the mod wheel's, in cycles

    bool gate() const noexcept { return held || sustained; }
  };

  struct alignas(16) Group
  {
    std::array<std::array<float, kLanes>, kSources> phase{};
    std::array<std::array<float, kLanes>, kSources> inc{};
    std::array<std::array<float, kLanes>, kSources> frame{};
    std::array<std::array<int, kLanes>, kSources> mip{};
    std::array<std::array<float, kLanes>, kSources> mipFrac{};
    std::array<Ramp, kSources> weight{};
    //! The Noise bank: a generator and a one-pole colour per source and lane
    std::array<std::array<std::uint32_t, kLanes>, kSources> noise{};
    std::array<std::array<float, kLanes>, kSources> noiseLp{};
    std::array<bool, kSources> heard{};
    Ramp G{}, k{}, amp{};
    Ladder ladder{};      // the left side, or both when there is no spread
    Ladder ladderR{};     // the right side
    bool active{false};
  };

  void prepare(double rate) noexcept
  {
    fs = (rate > 0. && finite(float(rate))) ? rate : 48000.;
    builtinBanks(); // built here, never on the audio thread
    for(auto& v : voices)
      v = Voice{};
    for(auto& g : groups)
      g = Group{};
    for(std::size_t gi = 0; gi < groups.size(); gi++)
      for(std::size_t si = 0; si < kSources; si++)
        for(std::size_t l = 0; l < kLanes; l++)
          groups[gi].noise[si][l] = 0x9E3779B9u * std::uint32_t(1 + l + kLanes * (si + kSources * gi));
    channels = {};
    monoStack.clear();
    tickLeft = 0;
    modWheel = 0.f;
    dcL = dcR = dcLy = dcRy = 0.f;
    dcCoef = float(1. - 2. * 3.14159265358979 * 10. / fs);
    limGain = 1.f;
    limRelease = float(1. - std::exp(-1. / (0.1 * fs)));
  }

  double sampleRate() const noexcept { return fs; }
  int activeVoices() const noexcept
  {
    int n = 0;
    for(auto& v : voices)
      n += v.state != Voice::Free;
    return n;
  }
  //! Where the sounding voices' points are, for the editor
  LivePoints livePoints() const noexcept
  {
    LivePoints out;
    for(auto& v : voices)
    {
      if(v.state != Voice::Active || out.count >= int(out.x.size()))
        continue;
      out.x[std::size_t(out.count)] = v.vx;
      out.y[std::size_t(out.count)] = v.vy;
      out.count++;
    }
    return out;
  }
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
        else if(d1 == 74)
          cs.timbre = float(d2) / 127.f;
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
                  releaseVoice(v);
              }
        }
        else if(d1 == 120 || d1 == 123)
        {
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
              releaseVoice(v);
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
    const auto& banks = builtinBanks();
    for(int s = 0; s < kSources; s++)
      bank[std::size_t(s)] = &banks[std::size_t(p.src[std::size_t(s)].bank)];

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
      std::fill_n(bufL.begin(), n, 0.f);
      std::fill_n(bufR.begin(), n, 0.f);
      for(auto& g : groups)
        if(g.active)
          renderGroup(g, p, n);

      for(int i = 0; i < n; i++)
      {
        const float sl = bufL[std::size_t(i)], sr = bufR[std::size_t(i)];
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

  void panic() noexcept
  {
    for(int i = 0; i < kMaxVoices; i++)
      resetVoice(i);
    dcL = dcR = dcLy = dcRy = 0.f;
    monoStack.clear();
  }

  std::array<Voice, kMaxVoices> voices{};
  std::array<Group, kGroups> groups{};

private:
  static constexpr float kLimit = 0.97f;
  static constexpr float kPerVoice = 0.35f;

  struct ChannelState
  {
    float bend{0.f};
    float pressure{0.f};
    float timbre{0.5f};
    bool sustain{false};
  };

  Voice& voice(int i) noexcept { return voices[std::size_t(i)]; }

  void resetVoice(int i) noexcept
  {
    voice(i) = Voice{};
    auto& g = groups[std::size_t(i / kLanes)];
    const auto l = std::size_t(i % kLanes);
    for(int s = 0; s < kSources; s++)
    {
      const auto si = std::size_t(s);
      g.phase[si][l] = 0.f;
      g.inc[si][l] = 0.f;
      g.weight[si].value[l] = g.weight[si].step[l] = 0.f;
    }
    g.amp.value[l] = g.amp.step[l] = 0.f;
    // The lane's filter state: zero it, leave the others
    alignas(16) std::array<float, kLanes> t{};
    for(vec4* s : {&g.ladder.s1, &g.ladder.s2, &g.ladder.s3, &g.ladder.s4, &g.ladderR.s1,
                   &g.ladderR.s2, &g.ladderR.s3, &g.ladderR.s4})
    {
      kfr::write<true>(t.data(), *s);
      t[l] = 0.f;
      *s = kfr::read<4, true>(t.data());
    }
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
      v.pitch = float(note);
    v.age = 0.;
    v.pendingNote = -1;
    v.fade = 1.f;
    v.amp.on();
    v.filter.on();
    v.vec.start(p.vecEnv);
    for(int k = 0; k < kLfos; k++)
      v.lfo[std::size_t(k)].start(
          p.lfos[std::size_t(k)].phase,
          0x9E3779B9u * std::uint32_t(1 + k + kLfos * i) + std::uint32_t(stamp));
    v.vibrato = 0.;
    if(tickLeft > 0)
      controlVoice(i, p, tickLeft);
  }

  void releaseVoice(Voice& v) noexcept
  {
    if(v.state != Voice::Active)
      return;
    v.amp.off();
    v.filter.off();
    v.vec.release();
  }

  void noteOn(int note, int ch, float vel, const Params& p) noexcept
  {
    if(p.play != PlayMode::Poly)
    {
      monoNoteOn(note, ch, vel, p);
      return;
    }
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Active && v.note == note && v.channel == ch)
      {
        startVoice(i, note, ch, vel, p, false);
        return;
      }
    }
    int freeIdx = -1, releasedIdx = -1, oldestIdx = -1, sounding = 0;
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
    if(freeIdx >= 0 && sounding < p.voices)
    {
      startVoice(freeIdx, note, ch, vel, p, true);
      return;
    }
    int steal = releasedIdx >= 0 ? releasedIdx : oldestIdx;
    if(steal < 0)
    {
      for(int i = 0; i < kMaxVoices; i++)
        if(voice(i).state == Voice::Stealing
           && (steal < 0 || voice(i).stamp > voice(steal).stamp))
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
      monoNoteOff(note, ch);
      return;
    }
    for(auto& v : voices)
    {
      if(v.state == Voice::Stealing && v.pendingNote == note && v.pendingChannel == ch)
        v.pendingNote = -1;
      if(v.state == Voice::Active && v.note == note && v.channel == ch && v.held)
      {
        v.held = false;
        if(channels[std::size_t(ch)].sustain)
          v.sustained = true;
        else
          releaseVoice(v);
      }
    }
  }

  struct MonoStack
  {
    std::array<std::pair<int, int>, 16> notes{};
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
    for(int i = 1; i < kMaxVoices; i++)
      if(voice(i).state == Voice::Active && voice(i).held)
      {
        voice(i).held = false;
        releaseVoice(voice(i));
      }
    auto& v = voice(0);
    if(v.state == Voice::Free)
    {
      startVoice(0, note, ch, vel, p, true);
      return;
    }
    if(p.play == PlayMode::Legato && wasHeld && v.state == Voice::Active)
    {
      v.note = note;
      v.channel = ch;
      v.target = float(note);
      if(p.glide <= 0.f)
        v.pitch = v.target;
      v.held = true;
      return;
    }
    startVoice(0, note, ch, vel, p, false);
    if(p.glide <= 0.f)
      v.pitch = v.target;
  }

  void monoNoteOff(int note, int ch) noexcept
  {
    monoStack.remove(note, ch);
    for(int i = 1; i < kMaxVoices; i++)
      if(voice(i).state == Voice::Active && voice(i).note == note && voice(i).held)
      {
        voice(i).held = false;
        releaseVoice(voice(i));
      }
    auto& v = voice(0);
    if(v.state != Voice::Active || v.note != note || v.channel != ch)
      return;
    if(monoStack.size > 0)
    {
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
      releaseVoice(v);
  }

  // ---- control rate ----------------------------------------------------
  void control(const Params& p, int n) noexcept
  {
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Stealing && v.fade <= 0.f)
      {
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
    for(int gi = 0; gi < kGroups; gi++)
    {
      auto& g = groups[std::size_t(gi)];
      g.active = false;
      for(int l = 0; l < kLanes; l++)
        g.active |= voices[std::size_t(gi * kLanes + l)].state != Voice::Free;
    }
  }

  void controlVoice(int i, const Params& p, int n) noexcept
  {
    auto& v = voice(i);
    auto& g = groups[std::size_t(i / kLanes)];
    g.active = true;
    const auto l = std::size_t(i % kLanes);
    const auto& cs = channels[std::size_t(v.channel)];
    const double dt = double(n) / fs;
    v.age += dt;

    if(p.glide > 0.f && p.play != PlayMode::Poly)
      v.pitch += (v.target - v.pitch) * float(1. - std::exp(-dt / (double(p.glide) / 3.)));
    else
      v.pitch = v.target;

    v.amp.advance(p.aA, p.aD, p.aS, p.aR, n, fs);
    v.filter.advance(p.fA, p.fD, p.fS, p.fR, n, fs);
    v.vec.advance(p.vecEnv, dt);

    const bool steal = v.state == Voice::Stealing;
    if(steal)
      v.fade = std::max(0.f, v.fade - float(n) / float(0.003 * fs));

    // The LFOs: each still for its Delay, then rising over half of it
    ModAmounts mod;
    for(int k = 0; k < kLfos; k++)
    {
      const auto& lp = p.lfos[std::size_t(k)];
      if(lp.target == ModTarget::Off || lp.depth <= 0.f)
        continue;
      if(v.age < lp.delay)
        continue;
      const float fade
          = lp.delay > 0.f ? std::min(1.f, float((v.age - lp.delay) / (0.5 * lp.delay))) : 1.f;
      mod.add(lp.target, lp.depth * fade,
              v.lfo[std::size_t(k)].tick(lp.shape, dt / double(lp.period)));
    }
    // The mod wheel: a vibrato of its own, up to half a semitone at 5 Hz
    float wheel = 0.f;
    if(modWheel > 0.f)
    {
      v.vibrato += 5. * dt;
      v.vibrato -= std::floor(v.vibrato);
      wheel = 0.5f * modWheel * std::sin(6.28318530717958647692f * float(v.vibrato));
    }
    const float pressure = v.hasPressure ? v.pressure : cs.pressure;

    // Where the point is
    const float vx = std::clamp(
        p.x + p.vecEnvDepth * v.vec.x + mod.x + p.timbreX * 2.f * (cs.timbre - 0.5f),
        -1.f, 1.f);
    const float vy = std::clamp(
        p.y + p.vecEnvDepth * v.vec.y + mod.y + p.pressureY * pressure, -1.f, 1.f);
    // Shown where it mixes: outside the diamond, on its edge
    {
      const float u = std::clamp(0.5f * (vx + vy + 1.f), 0.f, 1.f);
      const float w = std::clamp(0.5f * (vy - vx + 1.f), 0.f, 1.f);
      v.vx = u - w;
      v.vy = u + w - 1.f;
    }
    const Weights w = p.law == MixLaw::Linear ? mixLinear(vx, vy) : mixEqualPower(vx, vy);
    const float wt[kSources] = {w.a, w.b, w.c, w.d};

    const float semis = v.pitch + 12.f * p.octave + 0.01f * p.tune + cs.bend * p.bendRange
                        + mod.pitch + wheel;
    const float fenv = v.filter.value;
    for(int s = 0; s < kSources; s++)
    {
      const auto si = std::size_t(s);
      const auto& sp = p.src[si];
      const float hz = 440.f
                       * std::exp2((std::clamp(semis + sp.coarse + 0.01f * sp.fine + mod.pitchOf[si],
                                               -24.f, 160.f)
                                    - 69.f)
                                   / 12.f);
      const float dx = std::clamp(float(hz / fs), 0.f, 0.49f);
      g.inc[si][l] = dx;
      Bank::levelFor(dx, g.mip[si][l], g.mipFrac[si][l]);
      const int frames = std::max(1, bank[si] ? bank[si]->frames() : 1);
      g.frame[si][l]
          = std::clamp(sp.frame + p.frameEnv * fenv + mod.frameOf[si], 0.f, 1.f)
            * float(frames - 1);
      g.weight[si].step[l] = (wt[s] * sp.level - g.weight[si].value[l]) / float(n);
    }

    // The filter: cutoff in semitones from its envelope, the key, the velocity
    const float cutSemis = 12.f * std::log2(p.cutoff / 440.f) + p.filterEnv * fenv
                           + p.keyTrack * float(v.note - 60) + p.velCutoff * (v.velocity - 1.f)
                           + mod.cutoff;
    const double fc = std::clamp(440. * std::exp2(cutSemis / 12.), 10., 0.45 * fs);
    const double gTan = std::tan(3.14159265358979 * fc / fs);
    const float G = float(gTan / (1. + gTan));
    const float k = 3.9f * p.resonance;
    g.G.step[l] = (G - g.G.value[l]) / float(n);
    g.k.step[l] = (k - g.k.value[l]) / float(n);
    if(g.G.value[l] <= 0.f)
    {
      // A new voice starts where its filter is
      g.G.value[l] = G;
      g.G.step[l] = 0.f;
    }

    const float gain
        = kPerVoice * v.amp.value * v.fade * mod.level
          * float(std::exp(-0.11512925 * 40. * p.velLevel * (1. - v.velocity)));
    g.amp.step[l] = (gain - g.amp.value[l]) / float(n);

    if(!steal && v.amp.idle() && !v.gate())
      resetVoice(i);
  }

  // ---- audio rate ------------------------------------------------------
  void renderGroup(Group& g, const Params& p, int n) noexcept
  {
    // A source is read only if a voice of the group can hear it this tick
    for(int s = 0; s < kSources; s++)
    {
      const auto si = std::size_t(s);
      bool heard = false;
      for(int l = 0; l < kLanes; l++)
        heard |= g.weight[si].value[std::size_t(l)] > 1e-5f
                 || g.weight[si].value[std::size_t(l)] + float(n) * g.weight[si].step[std::size_t(l)]
                        > 1e-5f;
      g.heard[si] = heard && bank[si] && !bank[si]->empty();
    }
    std::array<bool, kLanes> live{};
    for(int l = 0; l < kLanes; l++)
      live[std::size_t(l)] = g.amp.value[std::size_t(l)] > 0.f || g.amp.step[std::size_t(l)] != 0.f;

    // Drive 0 is all but linear (cubic distortion near -70 dB), 1 is hot
    const vec4 drive(0.05f + 3.95f * p.drive * p.drive);
    /* Spread: each source has its place, A and B to the left, C and D to
     * the right, as mirrored pairs (A against C, B against D, the inner pair
     * a third as far out). Where the vector's point is, the sound is: in the
     * centre, both sides hear the four sources alike. Every voice is stereo,
     * so a single note is as wide as a chord, at any pitch. */
    const bool stereo = p.spread > 0.f;
    float gl[kSources], gr[kSources];
    {
      const float place[kSources] = {-1.f, -1.f / 3.f, 1.f, 1.f / 3.f};
      for(int s = 0; s < kSources; s++)
      {
        const float pan = 0.5f + 0.5f * p.spread * place[s];
        gl[s] = std::cos(1.5707963f * pan);
        gr[s] = std::sin(1.5707963f * pan);
      }
    }

    for(int i = 0; i < n; i++)
    {
      vec4 mix(0.f), mixR(0.f);
      for(int s = 0; s < kSources; s++)
      {
        const auto si = std::size_t(s);
        if(!g.heard[si])
          continue;
        alignas(16) std::array<float, kLanes> out{};
        const Bank& b = *bank[si];
        auto& ph = g.phase[si];
        if(b.noise())
        {
          // White noise, a one-pole darkening it as the frame position rises,
          // its level held
          const float span = float(std::max(1, b.frames() - 1));
          for(int l = 0; l < kLanes; l++)
          {
            const auto li = std::size_t(l);
            if(!live[li])
              continue;
            auto& x = g.noise[si][li];
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const float white = float(std::int32_t(x)) * (1.f / 2147483648.f);
            const float a = 0.995f * g.frame[si][li] / span;
            auto& lp = g.noiseLp[si][li];
            lp = (1.f - a) * white + a * lp;
            out[li] = 0.866f * lp * std::sqrt((1.f + a) / (1.f - a));
          }
        }
        else
          for(int l = 0; l < kLanes; l++)
          {
            const auto li = std::size_t(l);
            if(!live[li])
              continue;
            out[li] = b.read(g.frame[si][li], g.mip[si][li], g.mipFrac[si][li], ph[li]);
            float x = ph[li] + g.inc[si][li];
            ph[li] = x >= 1.f ? x - 1.f : x;
          }
        const vec4 o = kfr::read<4, true>(out.data()) * g.weight[si].get();
        if(stereo)
        {
          mix += o * vec4(gl[s]);
          mixR += o * vec4(gr[s]);
        }
        else
          mix += o;
      }
      for(int s = 0; s < kSources; s++)
        g.weight[std::size_t(s)].advance();

      const vec4 amp = g.amp.get();
      if(stereo)
      {
        bufL[std::size_t(i)] += kfr::hadd(g.ladder.process(mix, g.G.get(), g.k.get(), drive) * amp);
        bufR[std::size_t(i)]
            += kfr::hadd(g.ladderR.process(mixR, g.G.get(), g.k.get(), drive) * amp);
      }
      else
      {
        // Centred: the same on both sides, at the level a centred pan gives
        const float y = 0.70710678f
                        * kfr::hadd(g.ladder.process(mix, g.G.get(), g.k.get(), drive) * amp);
        bufL[std::size_t(i)] += y;
        bufR[std::size_t(i)] += y;
      }
      g.G.advance();
      g.k.advance();
      g.amp.advance();
    }
  }

  double fs{48000.};
  std::array<const Bank*, kSources> bank{};
  std::array<ChannelState, 16> channels{};
  std::array<float, kCtrl> bufL{}, bufR{};
  std::uint64_t stamp{0};
  int tickLeft{0};
  float modWheel{0.f};
  float dcL{0.f}, dcR{0.f}, dcLy{0.f}, dcRy{0.f};
  float dcCoef{0.9987f};
  float limGain{1.f};
  float limRelease{0.0002f};
};
}
