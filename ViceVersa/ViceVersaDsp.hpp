#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* A vector synthesizer after the Prophet VS.
 *
 * Four wavetable sources, A to D, on the corners of a diamond; where a point
 * is in it sets their mix (Dsp/Vector.hpp). The point is the X/Y controls,
 * plus a five-point mix envelope, four LFOs per voice (Dsp/Modulation.hpp,
 * which also move the pitches, frames, cutoff and level), and per-note
 * expression (pressure to Y, CC 74 to X). Each source reads a band-limited
 * wave bank (Dsp/Wavetable.hpp) at its own frame position and tuning. Then a
 * four-pole ladder low-pass with its envelope, run at twice the rate, and the
 * amplitude envelope.
 *
 * 16 voices in four groups of four SIMD lanes, one lane per voice. Envelopes,
 * the LFOs, the mix, the filter's coefficients and the wave banks' mip levels
 * are computed every kCtrl samples and ramped; per sample only the table
 * reads, the mix, the filter and the level. A source no voice of a group
 * hears costs only its phase in that group.
 *
 * Nothing allocates after prepare().
 */

#include <ViceVersa/Dsp/Banks.hpp>
#include <ViceVersa/Dsp/Base.hpp>
#include <ViceVersa/Dsp/Ladder.hpp>
#include <ViceVersa/Dsp/Modulation.hpp>
#include <ViceVersa/Dsp/Simd.hpp>
#include <ViceVersa/Dsp/Vector.hpp>
#include <ViceVersa/Dsp/Wavetable.hpp>
#include <Dsp/Denormals.hpp>
#include <Dsp/Halfband.hpp>
#include <Dsp/Mono.hpp>
#include <Dsp/Output.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace viceversa
{
using synthimi::Decimator2x;
using synthimi::softClip;
using synthimi::Upsampler2x;
using ScopedFlushDenormals = synthimi::ScopedFlushDenormals<>;

inline constexpr int kGroups = 4;
inline constexpr int kMaxVoices = kLanes * kGroups;
inline constexpr int kCtrl = 16;
//! The shortest attack, so that a 0 does not click
inline constexpr double kMinAttack = 0.001;
//! The LFOs' outputs are smoothed over this, so that a square's or a random
//! step does not click
inline constexpr double kLfoSlew = 0.001;
//! The ladder's feedback at full resonance: it oscillates from 4
inline constexpr float kMaxFeedback = 4.2f;
//! dB to a natural log
inline constexpr double kLnPerDb = std::numbers::ln10 / 20.;

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

//! The points of the sounding voices, sent to the editor
struct LivePoints
{
  int count{0};
  std::array<float, kMaxVoices> x{}, y{};

  bool operator==(const LivePoints&) const = default;
};

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

  //! In the ports' ranges (ViceVersa.hpp); NaN is the port's default
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
    vecEnv.sanitize();
    vecEnvDepth = sane(vecEnvDepth, 0.f, 1.f, 1.f);
    for(auto& l : lfos)
    {
      l.target = saneEnum(l.target, kModTargetCount);
      l.shape = saneEnum(l.shape, kLfoShapeCount);
      l.period = sane(l.period, 0.02f, 20.f, 2.f);
      l.depth = sane(l.depth, 0.f, 1.f, 0.5f);
      l.phase = sane(l.phase, 0.f, 1.f, 0.f);
      l.delay = sane(l.delay, 0.f, 4.f, 0.f);
    }
    frameEnv = sane(frameEnv, -1.f, 1.f, 0.f);
    cutoff = sane(cutoff, 20.f, 20000.f, 8000.f);
    resonance = sane(resonance, 0.f, 1.f, 0.2f);
    filterEnv = sane(filterEnv, -72.f, 72.f, 24.f);
    keyTrack = sane(keyTrack, 0.f, 1.f, 0.5f);
    velCutoff = sane(velCutoff, 0.f, 48.f, 12.f);
    drive = sane(drive, 0.f, 1.f, 0.3f);
    fA = sane(fA, 0.f, 10.f, 0.005f);
    fD = sane(fD, 0.f, 10.f, 0.4f);
    fS = sane(fS, 0.f, 1.f, 0.3f);
    fR = sane(fR, 0.f, 10.f, 0.4f);
    aA = sane(aA, 0.f, 10.f, 0.005f);
    aD = sane(aD, 0.f, 10.f, 0.3f);
    aS = sane(aS, 0.f, 1.f, 0.8f);
    aR = sane(aR, 0.f, 10.f, 0.4f);
    play = saneEnum(play, kPlayModeCount);
    glide = sane(glide, 0.f, 2.f, 0.f);
    bendRange = std::round(sane(bendRange, 0.f, 48.f, 2.f));
    voices = std::clamp(voices, 1, kMaxVoices);
    octave = std::round(sane(octave, -3.f, 3.f, 0.f));
    tune = sane(tune, -100.f, 100.f, 0.f);
    velLevel = sane(velLevel, 0.f, 1.f, 0.5f);
    pressureY = sane(pressureY, -1.f, 1.f, 0.5f);
    timbreX = sane(timbreX, -1.f, 1.f, 0.5f);
    spread = sane(spread, 0.f, 1.f, 0.3f);
    level = sane(level, 0.f, 4.f, 0.7f);
  }
};

//! Attack linear, decay and release exponential (-40 dB in their time)
struct Adsr
{
  //! What a stage does in one control period: the same for every voice
  struct Rates
  {
    float attack, decay, sustain, release; // a step, two factors, a level

    [[nodiscard]] static Rates
    make(float a, float d, float s, float r, int n, double fs) noexcept
    {
      return {
          float(n / (std::max(double(a), kMinAttack) * fs)),
          float(std::exp(-4.6 * n / std::max(d * fs, 1.))), s,
          float(std::exp(-4.6 * n / std::max(r * fs, 1.)))};
    }
  };

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
  [[nodiscard]] bool idle() const noexcept { return stage == Idle; }

  void advance(const Rates& r) noexcept
  {
    switch(stage)
    {
      case Idle:
        return;
      case Attack:
        value += r.attack;
        if(value >= 1.f)
        {
          value = 1.f;
          stage = Decay;
        }
        return;
      case Decay:
        value = r.sustain + (value - r.sustain) * r.decay;
        return;
      case Release:
        value *= r.release;
        if(value < 1e-4f)
          reset();
        return;
    }
  }
};

class Synth
{
public:
  struct alignas(16) Ramp
  {
    std::array<float, kLanes> value{}, step{};
    [[nodiscard]] vec4 get() const noexcept { return kfr::read<kLanes, true>(value.data()); }
    void advance() noexcept
    {
      kfr::write<true>(value.data(), get() + kfr::read<kLanes, true>(step.data()));
    }
    //! Lane l to `target` over n samples; or there at once
    void toward(std::size_t l, float target, float n, bool jump) noexcept
    {
      if(jump)
      {
        value[l] = target;
        step[l] = 0.f;
      }
      else
        step[l] = (target - value[l]) / n;
    }
    void clear(std::size_t l) noexcept { value[l] = step[l] = 0.f; }
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
    float velGain{1.f}; // the velocity's level, from Vel Level at the note-on
    bool held{false};
    bool sustained{false};
    std::uint64_t stamp{0};
    // Double: a long glide's steps at a high rate are under a float's
    // resolution, and it would stop short
    double pitch{60.};
    float target{60.f};
    double age{0.};
    float pressure{0.f};
    bool hasPressure{false};
    Adsr amp, filter;
    VecEnvState vec;
    int pendingNote{-1};
    int pendingChannel{0};
    float pendingVelocity{0.f};
    bool pendingSustained{false}; // released while the pedal was down
    float fade{1.f};
    float vx{0.f}, vy{0.f}; // where the point was last, for the editor
    std::array<Lfo, kLfos> lfo{};
    std::array<float, kLfos> lfoOut{}; // smoothed
    double vibrato{0.}; // the mod wheel's, in cycles
    //! Whether it has had a control period since it started: the first one
    //! puts its filter and frames where they are instead of ramping to them
    bool primed{false};

    [[nodiscard]] bool gate() const noexcept { return held || sustained; }
  };

  struct alignas(16) Group
  {
    std::array<std::array<float, kLanes>, kSources> phase{};
    std::array<std::array<float, kLanes>, kSources> inc{};
    std::array<std::array<Mip, kLanes>, kSources> mip{};
    std::array<Ramp, kSources> weight{};
    std::array<Ramp, kSources> frame{}; // positions in [0, frames - 1]
    std::array<Bank::Lanes, kSources> lanes{};
    //! The Noise bank: a generator and a one-pole colour per source and lane,
    //! the pole and the level that keeps the colour's power
    std::array<std::array<std::uint32_t, kLanes>, kSources> noise{};
    std::array<std::array<float, kLanes>, kSources> noiseLp{};
    std::array<Ramp, kSources> noisePole{}, noiseGain{};
    std::array<bool, kSources> heard{};
    Ramp G{}, k{}, inv{}, amp{};
    Upsampler2x<vec4> upL{}, upR{};
    Ladder ladderL{}, ladderR{};
    Decimator2x<vec4> downL{}, downR{};
    //! The bank a source fades out of (Synth::fromBank): its tables for
    //! this period, the frame positions it was left at, in its frames, and
    //! the lanes that played it then: 1, or 0 for a voice started since,
    //! which plays the new bank alone
    std::array<Bank::Lanes, kSources> fromLanes{};
    std::array<std::array<float, kLanes>, kSources> fromFrame{}, fromLive{};
    std::array<bool, kSources> fromHeard{};
    bool active{false};
  };

  //! Also called on the audio thread, when the host's buffers grow: at the
  //! same rate, what sounds goes on
  void prepare(double rate) noexcept
  {
    const double next = (rate > 0. && finite(rate)) ? rate : 48000.;
    if(prepared && next == fs)
      return;
    prepared = true;
    fs = next;
    (void)builtinBanks(); // built once per process, here: never on the audio thread
    for(auto& v : voices)
      v = Voice{};
    for(auto& g : groups)
    {
      g = Group{};
      g.upL.prepare();
      g.upR.prepare();
      g.downL.prepare();
      g.downR.prepare();
    }
    for(std::size_t gi = 0; gi < groups.size(); gi++)
      for(std::size_t si = 0; si < kSources; si++)
        for(std::size_t l = 0; l < kLanes; l++)
          groups[gi].noise[si][l] = 0x9E3779B9u * std::uint32_t(1 + l + kLanes * (si + kSources * gi));
    channels = {};
    monoStack.clear();
    tickLeft = 0;
    modWheel = 0.f;
    output.prepare(fs);
    noiseExp = float(kNoiseRate / fs);
    fromBank = {};
    bankFadeStep = float(1. / (kBankFade * fs));
    incBase = float(std::log2(440. / fs));
    mipBase = float(std::log2(440. * kTopHarmonic / (double(kSafeCycles) * fs)));
  }

  [[nodiscard]] double sampleRate() const noexcept { return fs; }
  [[nodiscard]] int activeVoices() const noexcept
  {
    int n = 0;
    for(auto& v : voices)
      n += v.state != Voice::Free;
    return n;
  }
  //! Where the sounding voices' points are, for the editor
  [[nodiscard]] LivePoints livePoints() const noexcept
  {
    LivePoints out;
    for(auto& v : voices)
    {
      if(v.state != Voice::Active)
        continue;
      out.x[std::size_t(out.count)] = v.vx;
      out.y[std::size_t(out.count)] = v.vy;
      out.count++;
    }
    return out;
  }

  void midi(const unsigned char* m, int n, const Params& p) noexcept
  {
    if(n < 1)
      return;
    follow(p);
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
        switch(d1)
        {
          case 1:
            modWheel = float(d2) / 127.f;
            break;
          case 74:
            cs.timbre = float(d2) / 127.f;
            break;
          case 64:
            if(d2 >= 64)
              cs.sustain = true;
            else
              sustainOff(ch);
            break;
          case 120: // all sound off
          case 123: // all notes off
            allOff(ch, d1 == 120);
            break;
          case 121: // reset all controllers
            resetControllers(ch);
            break;
          default:
            break;
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
    follow(p);
    {
      /* Spread: each source has its place, A and B to the left, C and D to
       * the right, as mirrored pairs (A against C, B against D, the inner
       * pair a third as far out). Where the vector's point is, the sound is:
       * in the centre, both sides hear the four sources alike. Every voice
       * is stereo, so a single note is as wide as a chord, at any pitch; at
       * no spread, both sides are the same, at the level a centred pan
       * gives. */
      constexpr float place[kSources] = {-1.f, -1.f / 3.f, 1.f, 1.f / 3.f};
      for(int s = 0; s < kSources; s++)
      {
        const float pan = 0.5f + 0.5f * p.spread * place[s];
        panL[std::size_t(s)] = std::cos(0.5f * kPi * pan);
        panR[std::size_t(s)] = std::sin(0.5f * kPi * pan);
      }
    }
    // Drive 0 is all but linear (cubic distortion near -70 dB), 1 is hot
    drive = 0.05f + 3.95f * p.drive * p.drive;
    for(auto& g : groups)
      flushGroup(g);
    output.flushDenormals();

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
          renderGroup(g, n);
      for(std::size_t s = 0; s < kSources; s++)
        if(fromBank[s])
        {
          fromGain[s] -= float(n) * bankFadeStep;
          if(fromGain[s] <= 0.f)
            fromBank[s] = nullptr;
        }

      for(int i = 0; i < n; i++)
      {
        float sl = bufL[std::size_t(i)], sr = bufR[std::size_t(i)];
        output.blockDc(sl, sr);
        float ol = sl * p.level, or_ = sr * p.level;
        if(!finite(ol) || !finite(or_))
        {
          panic();
          ol = or_ = 0.f;
        }
        output.limit(ol, or_);
        l[done + i] = ol;
        r[done + i] = or_;
      }
      done += n;
      tickLeft -= n;
    }
  }

  void panic() noexcept
  {
    for(int i = 0; i < kMaxVoices; i++)
      resetVoice(i);
    output.reset();
    monoStack.clear();
  }

  std::array<Voice, kMaxVoices> voices{};
  std::array<Group, kGroups> groups{};

private:
  static constexpr float kPerVoice = 0.35f;
  //! The rate the noise bank's colours are set for
  static constexpr double kNoiseRate = 48000.;
  //! A source's bank changed: the old one fades out while the new one fades
  //! in, over this
  static constexpr double kBankFade = 0.005;

  struct ChannelState
  {
    float bend{0.f};
    float pressure{0.f};
    float timbre{0.5f};
    bool sustain{false};
  };

  Voice& voice(int i) noexcept { return voices[std::size_t(i)]; }

  //! What changes with the parameters, before a voice uses them
  void follow(const Params& p) noexcept
  {
    // The mono stack only means something in the mode that filled it
    if(p.play != play)
    {
      play = p.play;
      monoStack.clear();
    }
    const auto& banks = builtinBanks();
    for(std::size_t s = 0; s < kSources; s++)
    {
      const Bank* want = &banks[std::size_t(p.src[s].bank)];
      // During a fade, the next change waits for its end
      if(want == bank[s] || fromBank[s])
        continue;
      if(bank[s])
        startBankFade(s);
      bank[s] = want;
    }
  }

  //! Source s from its bank to the next, crossfaded: the old bank is read
  //! where the frame positions were, and the frame ramps go on in the new
  //! bank, whose frames are as many (kBankFrames, every bank)
  void startBankFade(std::size_t s) noexcept
  {
    fromBank[s] = bank[s];
    fromGain[s] = 1.f;
    for(auto& g : groups)
    {
      g.fromFrame[s] = g.frame[s].value;
      g.fromLive[s].fill(1.f);
    }
  }

  static void flushGroup(Group& g) noexcept
  {
    g.ladderL.flushTiny();
    g.ladderR.flushTiny();
    for(auto* c : {&g.upL.chains.x, &g.upL.chains.y, &g.upR.chains.x, &g.upR.chains.y})
      for(auto& s : *c)
        viceversa::flushTiny(s);
    for(auto* c : {&g.downL.chains.x, &g.downL.chains.y, &g.downR.chains.x, &g.downR.chains.y})
      for(auto& s : *c)
        viceversa::flushTiny(s);
  }

  void resetVoice(int i) noexcept
  {
    voice(i) = Voice{};
    auto& g = groups[std::size_t(i / kLanes)];
    const auto l = std::size_t(i % kLanes);
    for(std::size_t s = 0; s < kSources; s++)
    {
      g.phase[s][l] = 0.f;
      g.inc[s][l] = 0.f;
      g.mip[s][l] = {};
      g.noiseLp[s][l] = 0.f;
      g.fromLive[s][l] = 0.f;
      for(Ramp* r : {&g.weight[s], &g.frame[s], &g.noisePole[s], &g.noiseGain[s]})
        r->clear(l);
    }
    // The lane's filter, its coefficients and its state: zeroed, the other
    // lanes left. The next voice's first control period sets the
    // coefficients where it starts, instead of ramping from these.
    for(Ramp* r : {&g.G, &g.k, &g.inv, &g.amp})
      r->clear(l);
    g.ladderL.clearLane(l);
    g.ladderR.clearLane(l);
    for(auto* c : {&g.upL.chains.x, &g.upL.chains.y, &g.upR.chains.x, &g.upR.chains.y})
      for(auto& s : *c)
        clearLane(s, l);
    for(auto* c : {&g.downL.chains.x, &g.downL.chains.y, &g.downR.chains.x, &g.downR.chains.y})
      for(auto& s : *c)
        clearLane(s, l);
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
    v.velGain = float(std::exp(-kLnPerDb * 40. * p.velLevel * (1. - vel)));
    v.held = true;
    v.sustained = false;
    v.stamp = ++stamp;
    v.target = float(note);
    if(fresh || p.play == PlayMode::Poly || p.glide <= 0.f)
      v.pitch = note;
    v.age = 0.;
    v.pendingNote = -1;
    v.pendingSustained = false;
    v.fade = 1.f;
    v.amp.on();
    v.filter.on();
    v.vec.start(p.vecEnv);
    for(int k = 0; k < kLfos; k++)
      v.lfo[std::size_t(k)].start(
          p.lfos[std::size_t(k)].phase,
          0x9E3779B9u * std::uint32_t(1 + k + kLfos * i) + std::uint32_t(stamp));
    v.vibrato = 0.;
    // Mid-period: what is left of it, from the new note. The glide of a
    // voice that goes on has already moved this period.
    if(tickLeft > 0)
      controlVoice(i, p, tick(p, tickLeft), fresh);
  }

  void releaseVoice(Voice& v) noexcept
  {
    if(v.state != Voice::Active)
      return;
    v.amp.off();
    v.filter.off();
    v.vec.release();
  }

  void sustainOff(int ch) noexcept
  {
    channels[std::size_t(ch)].sustain = false;
    for(auto& v : voices)
    {
      if(v.state == Voice::Stealing && v.pendingChannel == ch && v.pendingSustained)
        v.pendingNote = -1;
      if(v.channel == ch && v.sustained)
      {
        v.sustained = false;
        if(!v.held)
          releaseVoice(v);
      }
    }
  }

  //! All the channel's notes released, or silenced at once
  void allOff(int ch, bool silence) noexcept
  {
    channels[std::size_t(ch)].sustain = false;
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Stealing && v.pendingChannel == ch)
        v.pendingNote = -1;
      if(v.state == Voice::Free || v.channel != ch)
        continue;
      if(silence)
        resetVoice(i);
      else
      {
        v.held = v.sustained = false;
        releaseVoice(v);
      }
    }
    monoStack.removeChannel(ch);
  }

  void resetControllers(int ch) noexcept
  {
    auto& cs = channels[std::size_t(ch)];
    cs.bend = 0.f;
    cs.pressure = 0.f;
    cs.timbre = ChannelState{}.timbre;
    modWheel = 0.f;
    for(auto& v : voices)
      if(v.channel == ch)
      {
        v.pressure = 0.f;
        v.hasPressure = false;
      }
    sustainOff(ch);
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
    v.pendingSustained = false;
    v.stamp = ++stamp;
  }

  void noteOff(int note, int ch, const Params& p) noexcept
  {
    // In any mode: a note held in Mono, released in Poly, is no longer held
    monoStack.remove(note, ch);
    if(p.play != PlayMode::Poly)
    {
      monoNoteOff(note, ch);
      return;
    }
    const bool pedal = channels[std::size_t(ch)].sustain;
    for(auto& v : voices)
    {
      if(v.state == Voice::Stealing && v.pendingNote == note && v.pendingChannel == ch)
      {
        // Still waiting for its voice: it will start sustained, or not at all
        if(pedal)
          v.pendingSustained = true;
        else
          v.pendingNote = -1;
      }
      if(v.state == Voice::Active && v.note == note && v.channel == ch && v.held)
      {
        v.held = false;
        if(pedal)
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
    void removeChannel(int c) noexcept
    {
      size = int(std::remove_if(notes.begin(), notes.begin() + size,
                                [c](const auto& e) { return e.second == c; })
                 - notes.begin());
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
  }

  void monoNoteOff(int note, int ch) noexcept
  {
    synthimi::monoNoteOff(
        voices, monoStack, channels[std::size_t(ch)].sustain, note, ch,
        [this](Voice& v) { releaseVoice(v); });
  }

  // ---- control rate ----------------------------------------------------
  //! What a control period of n samples is for every voice alike
  struct Tick
  {
    int n;
    double dt;
    Adsr::Rates amp, filter;
    double glide; // the glide's step, as a fraction of what is left; 0: none
    float slew;   // the LFOs' smoothing, likewise
    float fade;   // a stolen voice's fade's step
    float cutoff; // semitones from A4
    float k;
  };

  [[nodiscard]] Tick tick(const Params& p, int n) const noexcept
  {
    const double dt = double(n) / fs;
    return {
        n,
        dt,
        Adsr::Rates::make(p.aA, p.aD, p.aS, p.aR, n, fs),
        Adsr::Rates::make(p.fA, p.fD, p.fS, p.fR, n, fs),
        p.glide > 0.f && p.play != PlayMode::Poly
            ? 1. - std::exp(-dt / (double(p.glide) / 3.))
            : 0.,
        float(1. - std::exp(-dt / kLfoSlew)),
        float(n / (0.003 * fs)),
        12.f * std::log2(p.cutoff / 440.f),
        kMaxFeedback * p.resonance};
  }

  void control(const Params& p, int n) noexcept
  {
    const Tick t = tick(p, n);
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voice(i);
      if(v.state == Voice::Stealing && v.fade <= 0.f)
      {
        const int pn = v.pendingNote, pc = v.pendingChannel;
        const float pv = v.pendingVelocity;
        const bool sustained = v.pendingSustained;
        if(pn >= 0)
        {
          startVoice(i, pn, pc, pv, p, true);
          if(sustained)
          {
            v.held = false;
            v.sustained = true;
          }
        }
        else
          resetVoice(i);
      }
      if(v.state != Voice::Free)
        controlVoice(i, p, t, true);
    }
    for(int gi = 0; gi < kGroups; gi++)
    {
      auto& g = groups[std::size_t(gi)];
      g.active = false;
      for(int l = 0; l < kLanes; l++)
        g.active |= voices[std::size_t(gi * kLanes + l)].state != Voice::Free;
    }
  }

  void controlVoice(int i, const Params& p, const Tick& t, bool advanceGlide) noexcept
  {
    auto& v = voice(i);
    auto& g = groups[std::size_t(i / kLanes)];
    g.active = true;
    const auto l = std::size_t(i % kLanes);
    const auto& cs = channels[std::size_t(v.channel)];
    const float n = float(t.n);
    v.age += t.dt;

    if(advanceGlide)
    {
      if(t.glide > 0.)
      {
        v.pitch += (v.target - v.pitch) * t.glide;
        if(std::abs(v.target - v.pitch) < 1e-4)
          v.pitch = v.target;
      }
      else
        v.pitch = v.target;
    }

    v.amp.advance(t.amp);
    v.filter.advance(t.filter);
    v.vec.advance(p.vecEnv, t.dt);

    const bool steal = v.state == Voice::Stealing;
    if(steal)
      v.fade = std::max(0.f, v.fade - t.fade);

    // The LFOs: each still for its Delay, then rising over half of it
    ModAmounts mod;
    for(int k = 0; k < kLfos; k++)
    {
      const auto ki = std::size_t(k);
      const auto& lp = p.lfos[ki];
      if(lp.target == ModTarget::Off || lp.depth <= 0.f)
        continue;
      if(v.age < lp.delay)
        continue;
      const float fade
          = lp.delay > 0.f ? std::min(1.f, float((v.age - lp.delay) / (0.5 * lp.delay))) : 1.f;
      const float raw = v.lfo[ki].tick(lp.shape, t.dt / double(lp.period));
      float& out = v.lfoOut[ki];
      out = v.primed ? out + (raw - out) * t.slew : raw;
      mod.add(lp.target, lp.depth * fade, out);
    }
    // The mod wheel: a vibrato of its own, up to half a semitone at 5 Hz
    float wheel = 0.f;
    if(modWheel > 0.f)
    {
      v.vibrato += 5. * t.dt;
      v.vibrato -= std::floor(v.vibrato);
      wheel = 0.5f * modWheel * std::sin(kTwoPi * float(v.vibrato));
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

    const float pitch = float(v.pitch);
    const float semis = pitch + 12.f * p.octave + 0.01f * p.tune + cs.bend * p.bendRange
                        + mod.pitch + wheel;
    const float fenv = v.filter.value;
    const bool jump = !v.primed;
    for(int s = 0; s < kSources; s++)
    {
      const auto si = std::size_t(s);
      const auto& sp = p.src[si];
      // Octaves from A4: the increment and the mip level from it
      const float oct
          = (std::clamp(semis + sp.coarse + 0.01f * sp.fine + mod.pitchOf[si], -24.f, 160.f)
             - 69.f)
            / 12.f;
      g.inc[si][l] = std::min(std::exp2(oct + incBase), 0.49f);
      g.mip[si][l] = Bank::levelFor(oct + mipBase);
      const Bank* b = bank[si];
      const float pos
          = std::clamp(sp.frame + p.frameEnv * fenv + mod.frameOf[si], 0.f, 1.f);
      g.frame[si].toward(l, pos * float(std::max(0, b ? b->frames() - 1 : 0)), n, jump);
      g.weight[si].toward(l, wt[s] * sp.level, n, false);
      if((b && b->noise()) || (fromBank[si] && fromBank[si]->noise()))
      {
        // White noise, a one-pole darkening it as the frame position rises,
        // its power kept. The pole is 0.995 * position at kNoiseRate; at
        // another rate, the same time constant: the corner stays in Hz.
        const float a = std::pow(0.995f * pos, noiseExp);
        g.noisePole[si].toward(l, a, n, jump);
        g.noiseGain[si].toward(l, 0.866f * std::sqrt((1.f + a) / (1.f - a)), n, jump);
      }
    }

    // The filter: cutoff in semitones from its envelope, the key, the
    // velocity; its coefficients for twice the rate, where it runs
    const float cutSemis = t.cutoff + p.filterEnv * fenv + p.keyTrack * (pitch - 60.f)
                           + p.velCutoff * (v.velocity - 1.f) + mod.cutoff;
    const float fc
        = std::clamp(440.f * std::exp2(cutSemis / 12.f), 10.f, float(0.45 * fs));
    const float gTan = tanCutoff(kPi * fc / float(2. * fs));
    const float G = gTan / (1.f + gTan);
    const float G2 = G * G;
    g.G.toward(l, G, n, jump);
    g.k.toward(l, t.k, n, jump);
    g.inv.toward(l, 1.f / (1.f + t.k * G2 * G2), n, jump);

    const float gain = kPerVoice * v.amp.value * v.fade * mod.level * v.velGain;
    g.amp.toward(l, gain, n, false);
    v.primed = true;

    if(!steal && v.amp.idle() && !v.gate())
      resetVoice(i);
  }

  // ---- audio rate ------------------------------------------------------
  //! Source si's next sample in each lane, from bank b
  KFR_ALWAYS_INLINE vec4 source(
      Group& g, std::size_t si, const Bank& b, const Bank::Lanes& lanes, vec4 frame,
      vec4 phase) noexcept
  {
    if(!b.noise())
      return Bank::read(lanes, frame, phase);
    alignas(16) std::array<float, kLanes> out;
    const auto& a = g.noisePole[si].value;
    for(std::size_t l = 0; l < kLanes; l++)
    {
      auto& x = g.noise[si][l];
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      const float white = float(std::int32_t(x)) * (1.f / 2147483648.f);
      auto& lp = g.noiseLp[si][l];
      lp = (1.f - a[l]) * white + a[l] * lp;
      out[l] = lp;
    }
    const vec4 o = kfr::read<kLanes, true>(out.data()) * g.noiseGain[si].get();
    g.noisePole[si].advance();
    g.noiseGain[si].advance();
    return o;
  }

  void renderGroup(Group& g, int n) noexcept
  {
    // A source's table is read only if a voice of the group can hear it
    // this period; its phases move on in any case, so that sources at the
    // same pitch keep their relation whatever the vector did
    for(int s = 0; s < kSources; s++)
    {
      const auto si = std::size_t(s);
      bool heard = false;
      for(std::size_t l = 0; l < kLanes; l++)
        heard |= g.weight[si].value[l] > 1e-5f
                 || g.weight[si].value[l] + float(n) * g.weight[si].step[l] > 1e-5f;
      g.heard[si] = heard && bank[si] && !bank[si]->empty();
      if(g.heard[si] && !bank[si]->noise())
        bank[si]->lanes(g.lanes[si], g.mip[si]);
      const Bank* from = fromBank[si];
      g.fromHeard[si] = heard && from && !from->empty();
      if(g.fromHeard[si] && !from->noise())
        from->lanes(g.fromLanes[si], g.mip[si]);
    }

    vec4 phase[kSources], inc[kSources];
    for(std::size_t s = 0; s < kSources; s++)
    {
      phase[s] = kfr::read<kLanes, true>(g.phase[s].data());
      inc[s] = kfr::read<kLanes, true>(g.inc[s].data());
    }
    const vec4 one(1.f), driveIn(drive), driveOut(1.f / drive);

    for(int i = 0; i < n; i++)
    {
      vec4 mixL(0.f), mixR(0.f);
      for(int s = 0; s < kSources; s++)
      {
        const auto si = std::size_t(s);
        if(g.heard[si] || g.fromHeard[si])
        {
          vec4 o = g.heard[si]
                       ? source(g, si, *bank[si], g.lanes[si], g.frame[si].get(), phase[si])
                       : vec4(0.f);
          if(g.fromHeard[si])
          {
            const vec4 old = source(
                g, si, *fromBank[si], g.fromLanes[si],
                kfr::read<kLanes>(g.fromFrame[si].data()), phase[si]);
            const float f = std::max(0.f, fromGain[si] - float(i) * bankFadeStep);
            o += (old - o) * (vec4(f) * kfr::read<kLanes>(g.fromLive[si].data()));
          }
          o *= g.weight[si].get();
          mixL += o * vec4(panL[si]);
          mixR += o * vec4(panR[si]);
        }
        const vec4 x = phase[si] + inc[si];
        phase[si] = kfr::select(x >= one, x - one, x);
        g.weight[si].advance();
        g.frame[si].advance();
      }

      // The filter at twice the rate: both sides, both halves of the sample,
      // with the same coefficients
      const vec4 k = g.k.get();
      const auto c = Ladder::Coefs::make(g.G.get(), k, g.inv.get());
      vec4 l0, l1, r0, r1;
      g.upL.process(mixL * driveIn, l0, l1);
      g.upR.process(mixR * driveIn, r0, r1);
      l0 = g.ladderL.process(l0, c);
      l1 = g.ladderL.process(l1, c);
      r0 = g.ladderR.process(r0, c);
      r1 = g.ladderR.process(r1, c);
      // Back to the input's level; resonance thins the passband, give some back
      const vec4 out = (one + vec4(0.5f) * k) * driveOut * g.amp.get();
      bufL[std::size_t(i)] += kfr::hadd(g.downL.process(l0, l1) * out);
      bufR[std::size_t(i)] += kfr::hadd(g.downR.process(r0, r1) * out);
      g.G.advance();
      g.k.advance();
      g.inv.advance();
      g.amp.advance();
    }

    for(std::size_t s = 0; s < kSources; s++)
      kfr::write<true>(g.phase[s].data(), phase[s]);
  }

  double fs{48000.};
  bool prepared{false};
  PlayMode play{PlayMode::Poly};
  std::array<const Bank*, kSources> bank{};
  //! The bank each source fades out of, or null, and its gain, 1 to 0
  std::array<const Bank*, kSources> fromBank{};
  std::array<float, kSources> fromGain{};
  float bankFadeStep{1.f / 240.f};
  std::array<ChannelState, 16> channels{};
  std::array<float, kCtrl> bufL{}, bufR{};
  std::array<float, kSources> panL{}, panR{};
  std::uint64_t stamp{0};
  int tickLeft{0};
  float modWheel{0.f};
  float drive{1.f};
  float noiseExp{1.f}; // kNoiseRate / fs: a pole at kNoiseRate to one at fs
  float incBase{0.f}, mipBase{0.f}; // log2 of A4's increment, and its octaves over level 0
  synthimi::OutputStage output;
};
}
