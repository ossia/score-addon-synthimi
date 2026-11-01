#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: a physical-modelling string synthesizer.
//
// Four engines over two kinds of string:
//
//  - Pluck, Bow and Buzz share one digital waveguide (Smith, "Physical Audio
//    Signal Processing", CCRMA, 2010): two delay lines meeting at the
//    excitation point, a rigid nut on one side and the loss / dispersion /
//    bridge filters on the other. It is the bowed-string topology of
//    McIntyre, Schumacher & Woodhouse (JASA 1983) as written in STK's Bowed;
//    a pluck is the same string with the bow lifted, so that nothing ever
//    moves the junction of a string that rings (the arrangement of CVfunk's
//    Twang, MIT). Buzz adds an amplitude-dependent bridge contact, the
//    "curved bridge" of Mutable Instruments' Rings (MIT).
//  - Struck is modal: up to 48 partials of a stiff string per course,
//    f_n = n f0 sqrt((1 + B n^2) / (1 + B)), struck by a Hunt-Crossley
//    hammer (Hunt & Crossley 1975; Chaigne & Askenfelt, JASA 1994).
//
// No code is copied from those projects: the algorithms are reimplemented
// from their published descriptions.
//
// Design rules, each of which some open-source string model gets wrong:
//  - Tuning: the delay is fs/f0 minus the exact phase delay, at f0, of every
//    filter in the loop, computed analytically at control rate. Without it a
//    loop with a lowpass and dispersion is tens of cents flat in the upper
//    register.
//  - Decay is a T60 at the fundamental, so it does not depend on pitch, and
//    Brightness the ratio of the T60 at a high reference frequency to it.
//  - Passive by construction: every filter in a loop has |H| <= 1, the bow
//    reflection is at most 0.98 of the velocity difference, the bridge
//    contact only shortens the delay, and the E-Bow injects a bounded signal
//    into a lossy loop. The clamps and the NaN reset are backstops.
//  - Nothing allocates after prepare(): every delay line lives in one buffer
//    sized for the lowest note at the actual sample rate.
//  - Every port value is clamped here: Process::ControlInlet::setValue does
//    not clamp to the declared range, and a preset or an OSC message can hold
//    anything, NaN included.

#include <Cordito/Dsp/Common.hpp>
#include <Cordito/Dsp/Filters.hpp>
#include <Cordito/Dsp/Body.hpp>
#include <Cordito/Dsp/Exciters.hpp>
#include <Cordito/Dsp/Waveguide.hpp>
#include <Cordito/Dsp/Modal.hpp>
#include <Cordito/Dsp/Sympathetic.hpp>

#include <ossia/detail/algorithms.hpp>
#include <ossia/detail/pod_vector.hpp>
#include <ossia/detail/small_vector.hpp>

#include <vector>

namespace crd
{
// ---------------------------------------------------------------------------
// MIDI expression, per channel: works for plain MIDI (everything on one
// channel) and for MPE (one channel per note) without any mode switch.
// ---------------------------------------------------------------------------

struct ChannelState
{
  double bend = 0.;      // -1 .. 1
  double pressure = 0.;  // 0 .. 1
  bool hasPressure = false;
  double timbre = 0.5;   // CC74
  bool hasTimbre = false;
  bool sustain = false;
};

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

struct Voice
{
  enum class State
  {
    Free,
    Active,
    Stealing
  };
  State state = State::Free;
  int note = -1;
  int channel = 0;
  double velocity = 0.;
  std::uint64_t stamp = 0;
  bool held = false;
  bool sustained = false; // released while the pedal was down
  double polyPressure = 0.;
  bool hasPolyPressure = false;

  Engine engine = Engine::Pluck;
  Exciter exciter = Exciter::Pick;

  double pitch = 60.;  // semitones, after glide
  double target = 60.; // semitones
  double position = 0.13;
  // The excitation's split between the polarisations: cos and sin of its
  // angle, set once per note rather than computed at every sample
  double split[2]{0.765, 0.644};
  double kick = 0.;    // tension transient, 0 .. 1
  double kickDecay = 1.;

  //! The pressure on the voice, smoothed per sample: MIDI pressure comes in
  //! steps of 1/127, often coarser, and the bow's speed, the E-Bow's drive
  //! and the brightness it drives should not step
  double pressure = 0.;

  // Bow and E-Bow: the envelope at the control rate, and as it is ramped
  // from one control tick to the next, per sample (bowLevel, by bowStep)
  double bowEnv = 0., bowLevel = 0., bowStep = 0.;
  //! The force's share of Schelleng's maximum per unit of bow speed, and the
  //! force and position it was computed for
  double bowShare = 0., bowForce = -1., bowPosition = -1.;
  bool bowing = false;

  // Release: 0 while held, ramps to 1 after note-off
  double damp = 0.;

  ExciteGen exc;
  Hammer hammer;
  std::array<WgPol, 2> pol{};
  bool twoPols = true;
  ModalString modal;
  std::array<double, 11> modalKey{}; // coefficients cache key
  //! What the partials' decays were computed for, and the pitch at the
  //! previous control tick
  std::array<double, 8> decayKey{};
  double decayF = 0., modalLastF = 0.;
  double gain = 1.;     // per-engine output gain

  double env = 0.;      // output level follower
  std::int64_t age = 0; // samples since the note started: an int overflows in 12 h
  double stealGain = 1.;

  // A note waiting for this voice to finish fading out
  bool pending = false;
  int pendingNote = 0, pendingChannel = 0;
  double pendingVelocity = 0.;
  std::uint64_t pendingStamp = 0;

  //! The note to start once this voice has faded out; the newest wins
  void queue(int note, int ch, double vel, std::uint64_t stamp) noexcept
  {
    pending = true;
    pendingNote = note;
    pendingChannel = ch;
    pendingVelocity = vel;
    pendingStamp = stamp;
  }

  bool firstUpdate = true;
};

// ---------------------------------------------------------------------------
// The synth
// ---------------------------------------------------------------------------

class Synth
{
public:
  //! Allocates every buffer for the sample rate: the only allocation. The
  //! host calls it again, on the audio thread, when its buffer size grows:
  //! for the same rate it keeps everything as it is.
  void prepare(double rate)
  {
    const double r = (rate > 0. && finite(rate)) ? std::clamp(rate, 8000., 384000.) : 48000.;
    if(prepared && r == fs)
      return;
    fs = r;
    const int needed = int(fs / kMinF0) + 16;
    lineSize = 1;
    while(lineSize < needed)
      lineSize <<= 1;

    const std::size_t lines = std::size_t(kMaxVoices) * 4 + kMaxSymp;
    pool.assign(lines * std::size_t(lineSize), 0.f);
    std::size_t k = 0;
    for(auto& v : voices)
    {
      for(auto& p : v.pol)
      {
        p.neck.attach(pool.data() + (k++) * std::size_t(lineSize), lineSize);
        p.bridge.attach(pool.data() + (k++) * std::size_t(lineSize), lineSize);
      }
      v = resetVoice(v);
    }
    for(auto& s : symp.strings)
      s.line.attach(pool.data() + (k++) * std::size_t(lineSize), lineSize);
    symp.count = 0;
    symp.tuning = SympTuning::Off;
    symp.root = -1;
    symp.clear();

    body.type = Body::None;
    body.size = -1.;
    body.reset();
    outDcL.setCutoff(10., fs);
    outDcR.setCutoff(10., fs);
    limEnv = 0.;
    levelSm = -1.;
    ctrlCount = 0;
    channels = {};
    monoStack.clear();
    monoStack.reserve(128);
    prepared = true;
  }

  double sampleRate() const noexcept { return fs; }

  // ---- MIDI -------------------------------------------------------------

  void midi(const unsigned char* bytes, int size, const Params& p) noexcept
  {
    if(size < 1)
      return;
    const int status = bytes[0] & 0xF0;
    const int ch = bytes[0] & 0x0F;
    const int d1 = size > 1 ? (bytes[1] & 0x7F) : 0;
    const int d2 = size > 2 ? (bytes[2] & 0x7F) : 0;
    auto& cs = channels[std::size_t(ch)];
    switch(status)
    {
      case 0x90:
        if(size < 3)
          return;
        if(d2 > 0)
          noteOn(d1, ch, d2 / 127., p);
        else
          noteOff(d1, ch, p);
        break;
      case 0x80:
        if(size < 3)
          return;
        noteOff(d1, ch, p);
        break;
      case 0xA0:
        if(size < 3)
          return;
        for(auto& v : voices)
          if(v.state == Voice::State::Active && v.note == d1 && v.channel == ch)
          {
            v.polyPressure = d2 / 127.;
            v.hasPolyPressure = true;
          }
        break;
      case 0xB0:
        if(size < 3)
          return;
        if(d1 == 74)
        {
          cs.timbre = d2 / 127.;
          cs.hasTimbre = true;
        }
        else if(d1 == 64)
        {
          cs.sustain = d2 >= 64;
          if(!cs.sustain)
            for(auto& v : voices)
              if(v.channel == ch && v.sustained)
              {
                v.sustained = false;
                release(v, p);
              }
        }
        else if(d1 == 120)
        {
          // All sound off: fade everything out now, notes waiting on a voice
          // that fades out included, and the strings and body that ring on
          for(auto& v : voices)
          {
            v.pending = false;
            if(v.state == Voice::State::Active)
              v.state = Voice::State::Stealing;
          }
          monoStack.clear();
          symp.clear();
          body.reset();
        }
        else if(d1 == 123)
        {
          for(auto& v : voices)
          {
            if(v.pending && v.pendingChannel == ch)
              v.pending = false;
            if(v.channel == ch && v.held)
              release(v, p);
          }
          if(monoChannel == ch)
            monoStack.clear();
        }
        break;
      case 0xD0:
        if(size < 2)
          return;
        cs.pressure = d1 / 127.;
        cs.hasPressure = true;
        break;
      case 0xE0:
        if(size < 3)
          return;
        cs.bend = std::clamp(((d2 << 7) | d1) / 8192. - 1., -1., 1.);
        break;
      default:
        break;
    }
  }

  // ---- Rendering --------------------------------------------------------

  //! Renders `frames` samples, adding nothing: writes l and r.
  void render(const Params& p, double* l, double* r, int frames) noexcept
  {
    if(!prepared)
    {
      std::fill_n(l, frames, 0.);
      std::fill_n(r, frames, 0.);
      return;
    }
    if(levelSm < 0.)
    {
      // The first block starts at the settings, not fading in to them
      levelSm = p.level;
      spreadSm = p.spread;
      bodyMixSm = p.bodyMix;
      sympLevelSm = p.sympLevel;
    }
    for(int i = 0; i < frames; i++)
    {
      if(ctrlCount == 0)
        control(p);
      ctrlCount = (ctrlCount + 1) % kCtrl;

      double sl = 0., sr = 0.;
      for(auto& v : voices)
        if(v.state != Voice::State::Free)
          tickVoice(v, p, sl, sr);

      // Sympathetic strings, driven by the voices
      double yl = 0., yr = 0.;
      sympLevelSm += levelCoef * (p.sympLevel - sympLevelSm);
      if(symp.count > 0)
        symp.tick(0.5 * (sl + sr), sympLevelSm, yl, yr);
      sl += yl;
      sr += yr;

      // Body
      bodyMixSm += levelCoef * (p.bodyMix - bodyMixSm);
      if(body.count > 0 && bodyMixSm > 1e-6)
      {
        double bl = 0., br = 0.;
        body.tick(0.5 * (sl + sr), bl, br);
        const double dry = 1. - 0.65 * bodyMixSm;
        sl = sl * dry + bl * bodyMixSm;
        sr = sr * dry + br * bodyMixSm;
      }

      // Level, after which the limiter: it must never be driven by Level,
      // or turning a synth down would change its timbre.
      levelSm += levelCoef * (p.level - levelSm);
      sl = outDcL.tick(sl) * levelSm;
      sr = outDcR.tick(sr) * levelSm;
      if(!finite(sl) || !finite(sr)) [[unlikely]]
      {
        // A NaN would otherwise stay in the output filters for good
        outDcL = outDcR = DcBlocker{};
        outDcL.setCutoff(10., fs);
        outDcR.setCutoff(10., fs);
        body.reset();
        symp.clear();
        limEnv = 0.;
        sl = sr = 0.;
      }
      preLimiterPeak = std::max({preLimiterPeak, std::abs(sl), std::abs(sr)});

      const double peak = std::max(std::abs(sl), std::abs(sr));
      limEnv = peak > limEnv ? peak : limEnv * limRelease;
      const double g = limEnv > kLimit ? kLimit / limEnv : 1.;
      l[i] = std::clamp(sl * g, -1., 1.);
      r[i] = std::clamp(sr * g, -1., 1.);
    }
  }

  // ---- Introspection, for tests ------------------------------------------

  int activeVoices() const noexcept
  {
    int n = 0;
    for(auto& v : voices)
      n += v.state != Voice::State::Free;
    return n;
  }
  const Voice& voice(int i) const noexcept { return voices[std::size_t(i)]; }
  double preLimiterPeak = 0.;

private:
  static constexpr double kLimit = 0.97;
  //! String velocity amplitude the E-Bow sustains
  // How a voice moves, in seconds
  static constexpr double kKickTime = 0.12;     //!< decay of the tension kick
  static constexpr double kDampTime = 0.01;     //!< note-off to the damper on
  static constexpr double kStealTime = 0.005;   //!< fade of a stolen voice
  static constexpr double kRetireAge = 0.02;    //!< youngest a voice retires
  static constexpr double kEnvTime = 0.03;      //!< release of its level follower
  static constexpr double kDamperTouch = 0.012; //!< the damper's noise
  // Smoothing of the controls, in seconds
  static constexpr double kLevelTime = 0.02;    //!< Level, Spread, the mixes
  static constexpr double kPressureTime = 0.01; //!< MIDI pressure
  static constexpr double kLimiterTime = 0.1;   //!< the output limiter's release

  //! The RMS level of the string an E-Bow holds it at, and its pickup's Q
  static constexpr double kEbowTarget = 0.12;
  static constexpr double kEbowAsym = 0.3;
  static constexpr double kEbowQ = 8.;
  //! Buzz: the bridge allpass's angle per unit of wave, at Buzz 1
  static constexpr double kJawari = 2.;
  //! Velocity-wave heights of a released pick and finger, per unit of force
  static constexpr double kPickAmp = 0.94;
  static constexpr double kFingerAmp = 1.1;

  double fs = 48000.;
  int lineSize = 0;
  ossia::float_vector pool;
  std::array<Voice, kMaxVoices> voices{};
  std::array<ChannelState, 16> channels{};
  std::array<Biquad, kNotches> notchProto{};
  int notchProtoCount = 0;
  Body notchBody = Body::None;
  double notchSize = -1., notchK = -1.;
  SympBank symp;
  BodyBank body;
  DcBlocker outDcL, outDcR;
  double limEnv = 0., limRelease = 0.9995;
  double levelSm = -1., levelCoef = 0.001, pressCoef = 0.002;
  double spreadSm = -1., bodyMixSm = -1., sympLevelSm = -1.;
  int ctrlCount = 0;
  std::uint64_t stampCounter = 0;
  Rng rng;
  bool prepared = false;
  //! Held notes in Mono and Legato, last on top: in place, never allocated
  ossia::static_vector<int, 128> monoStack;
  int monoChannel = 0;

  static Voice resetVoice(const Voice& v) noexcept
  {
    Voice n;
    n.pol = v.pol;
    for(auto& p : n.pol)
      p.clear();
    n.modal.clear();
    return n;
  }

  // ---- Notes ------------------------------------------------------------

  void noteOn(int note, int ch, double vel, const Params& p) noexcept
  {
    if(p.sympTuning == SympTuning::Follow)
      symp.noteOn(note);

    if(p.playMode != PlayMode::Poly)
    {
      monoNoteOn(note, ch, vel, p);
      return;
    }

    const int cap = p.voices;
    // 1. The same key: excite the string again, adding to what rings. Only if
    // it is the same string: a voice keeps the engine and exciter it started
    // with, and one left ringing by another preset (Release at its maximum)
    // would otherwise play this note with that preset's engine. That one
    // rings out with its own sound, and the note gets a voice of its own.
    // All voices: one beyond a lowered Voices still rings.
    for(int i = 0; i < kMaxVoices; i++)
    {
      auto& v = voices[std::size_t(i)];
      if(v.state == Voice::State::Active && v.note == note && v.channel == ch)
      {
        if(!sameString(v, p))
        {
          release(v, p);
          v.note = -1;
          continue;
        }
        v.held = true;
        v.sustained = false;
        v.damp = 0.;
        excite(v, vel, p, false);
        return;
      }
    }
    // 2. A free voice
    for(int i = 0; i < cap; i++)
    {
      auto& v = voices[std::size_t(i)];
      if(v.state == Voice::State::Free)
      {
        start(v, note, ch, vel, p);
        return;
      }
    }
    // 3. The quietest released voice, 4. the quietest voice
    Voice* best = nullptr;
    for(int pass = 0; pass < 2 && !best; pass++)
    {
      for(int i = 0; i < cap; i++)
      {
        auto& v = voices[std::size_t(i)];
        if(v.state != Voice::State::Active)
          continue;
        if(pass == 0 && v.held)
          continue;
        if(!best || v.env < best->env)
          best = &v;
      }
    }
    if(!best)
    {
      // Every voice is already fading out: wait on one with no note waiting
      // yet, else replace the note that has waited longest. The newest notes
      // are the ones that sound.
      for(int i = 0; i < cap; i++)
      {
        auto& v = voices[std::size_t(i)];
        if(!v.pending)
        {
          best = &v;
          break;
        }
        if(!best || v.pendingStamp < best->pendingStamp)
          best = &v;
      }
    }
    if(best)
    {
      // Fade the stolen voice out over a few ms rather than cutting it
      best->state = Voice::State::Stealing;
      best->queue(note, ch, vel, ++stampCounter);
    }
  }

  void monoNoteOn(int note, int ch, double vel, const Params& p) noexcept
  {
    auto& v = voices[0];
    const bool wasHeld = !monoStack.empty();
    ossia::remove_erase(monoStack, note);
    if(monoStack.size() < monoStack.capacity())
      monoStack.push_back(note);
    monoChannel = ch;

    if(v.state == Voice::State::Active && !sameString(v, p))
    {
      // Another engine or exciter since: fade it out, start this one fresh
      v.state = Voice::State::Stealing;
      v.queue(note, ch, vel, ++stampCounter);
    }
    else if(v.state == Voice::State::Active)
    {
      v.note = note;
      v.channel = ch;
      v.target = note;
      v.held = true;
      v.sustained = false;
      v.damp = 0.;
      // Legato: slide without exciting the string again; the bow keeps on
      if(p.playMode == PlayMode::Legato && wasHeld)
        return;
      excite(v, vel, p, false);
    }
    else if(v.state == Voice::State::Free)
    {
      start(v, note, ch, vel, p);
    }
    else
    {
      v.queue(note, ch, vel, ++stampCounter);
    }
  }

  //! Whether a sounding voice is the string the current settings would start
  static bool sameString(const Voice& v, const Params& p) noexcept
  {
    return v.engine == p.engine && v.exciter == resolveExciter(p.engine, p.exciter);
  }

  void noteOff(int note, int ch, const Params& p) noexcept
  {
    // Always: a note-off arriving after a switch from Mono must not let a
    // later one glide back to a key that is not held any more
    ossia::remove_erase(monoStack, note);

    bool voice0Done = false;
    if(p.playMode != PlayMode::Poly)
    {
      auto& v = voices[0];
      if(v.state == Voice::State::Active && v.note == note)
      {
        voice0Done = true;
        if(!monoStack.empty())
        {
          // Back to the previous held note
          v.note = monoStack.back();
          v.target = v.note;
          if(p.playMode == PlayMode::Mono)
            excite(v, v.velocity, p, false);
        }
        else if(channels[std::size_t(ch)].sustain)
          v.sustained = true;
        else
          release(v, p);
      }
    }

    // Whatever the mode: notes played before a switch from Poly to Mono or
    // Legato are on other voices, and were never released (a held bow went
    // on bowing forever)
    for(std::size_t i = voice0Done ? 1 : 0; i < voices.size(); i++)
    {
      auto& v = voices[i];
      if(v.state == Voice::State::Stealing && v.pending && v.pendingNote == note
         && v.pendingChannel == ch)
      {
        // Released before it even started
        v.pending = false;
        continue;
      }
      if(v.state != Voice::State::Active || v.note != note || v.channel != ch
         || !v.held)
        continue;
      if(channels[std::size_t(ch)].sustain)
        v.sustained = true;
      else
        release(v, p);
    }
  }

  void release(Voice& v, const Params& p) noexcept
  {
    if(!v.held)
      return;
    v.held = false;
    // The damper's touch
    if(p.damperNoise > 0. && !bowed(v.engine))
    {
      v.exc = {};
      v.exc.kind = ExciteGen::Noise;
      v.exc.total = v.exc.noiseLen = std::max(8, int(kDamperTouch * fs));
      v.exc.noise(
          0.08 * p.damperNoise * (0.3 + 0.7 * v.velocity)
              * (v.engine == Engine::Struck ? 48000. / fs : 1.),
          1240., fs);
    }
  }

  void start(Voice& v, int note, int ch, double vel, const Params& p) noexcept
  {
    v = resetVoice(v);
    v.state = Voice::State::Active;
    v.note = note;
    v.channel = ch;
    v.stamp = ++stampCounter;
    // A note starts at the pressure its channel already has
    v.pressure = rawPressure(v);
    v.held = true;
    v.engine = p.engine;
    v.exciter = resolveExciter(p.engine, p.exciter);
    v.pitch = v.target = note;
    v.firstUpdate = true;
    excite(v, vel, p, true);
    // The string needs its length before its first sample, not at the next
    // control tick
    controlVoice(v, p);
  }

  //! Excites the string, adding to whatever already rings in it
  void excite(Voice& v, double vel, const Params& p, bool fresh) noexcept
  {
    v.velocity = vel;
    v.age = 0;
    const auto& cs = channels[std::size_t(v.channel)];

    // A little variance per note, as hands have: the attack of every note of
    // a string model being identical is a common complaint.
    v.position = std::clamp(
        p.position * (1. + 0.04 * rng.bipolar())
            + (cs.hasTimbre ? (cs.timbre - 0.5) * 0.3 : 0.),
        0.02, 0.5);
    const double hard = std::clamp(p.hardness + 0.04 * rng.bipolar(), 0., 1.);
    if(fresh)
    {
      const double angle = 0.45 + 0.35 * rng.uniform();
      v.split[0] = std::cos(angle);
      v.split[1] = std::sin(angle);
    }

    const double force = p.force * (1. - p.velForce + p.velForce * vel);
    // As voicePitch() bounds it
    const double f0 = std::clamp(midiToHz(v.pitch + p.tune), kMinF0, fs / 10.);
    const double period = fs / f0;
    // A bow builds up its note: it has no hard attack to start sharp
    const bool bowed = v.exciter == Exciter::Bow || v.exciter == Exciter::EBow;
    v.kick = bowed ? 0. : std::min(1., force * 1.6);
    v.kickDecay = std::exp(-double(kCtrl) / (kKickTime * fs));
    // Jaffe and Smith's dynamics: a softer note is a darker one, not only a
    // quieter one
    const double dyn = 0.12 + 0.88 * vel * std::sqrt(vel);
    // A released string sounds about as loud at any pitch, a little louder
    // up high (1 dB per octave), as a fixed displacement would
    const double tilt = std::pow(f0 / 261.63, 1. / 6.);
    // The modes take a momentum per sample: noise for them scales as 1 / fs
    const double perSample = v.engine == Engine::Struck ? 48000. / fs : 1.;

    v.exc = {};
    v.hammer.active = false;
    switch(v.exciter)
    {
      case Exciter::Pick: {
        if(v.engine == Engine::Struck)
        {
          // A force pulse whose area is the momentum given
          // A softer pick stays on the string longer
          const int len = std::clamp(
              int((0.001 - 0.0009 * hard) / (0.4 + 0.6 * dyn) * fs), 2,
              std::max(2, int(0.5 * period)));
          v.exc.kind = ExciteGen::Pulse;
          v.exc.len = v.exc.total = len;
          v.exc.amp = force * 2. / len;
          v.exc.noiseLen = int(period);
          v.exc.noise(
              p.noise * force * 0.5 / std::max(1., period * 0.25),
              2725. * std::pow(6.45, hard) * dyn, fs);
          break;
        }
        // The plectrum: from 700 Hz (soft) to 12.6 kHz (hard), at velocity 1
        const double fast = std::clamp(700. * std::pow(18., hard) * dyn, 3. * f0, 0.4 * fs);
        v.exc.step(force * kPickAmp * tilt, 0.5 * f0, fast, false, fs);
        v.exc.noiseLen = std::max(2, int(period));
        v.exc.noise(p.noise * force * 0.5, 1705. * std::pow(10.3, hard) * dyn, fs);
        v.exc.total = std::max(v.exc.total, v.exc.noiseLen);
        break;
      }
      case Exciter::Finger: {
        // Flesh: from 250 Hz to 3 kHz, and a steeper roll-off than a pick's
        const double fast = std::clamp(250. * std::pow(12., hard) * dyn, 3. * f0, 0.4 * fs);
        v.exc.step(force * kFingerAmp * tilt, 0.5 * f0, fast, true, fs);
        v.exc.noiseLen = std::max(2, int(period));
        v.exc.noise(p.noise * force * 0.15, 805. * std::pow(6.6, hard) * dyn, fs);
        v.exc.total = std::max(v.exc.total, v.exc.noiseLen);
        break;
      }
      case Exciter::Hammer:
      case Exciter::Mallet: {
        const bool mallet = v.exciter == Exciter::Mallet;
        const double h = std::clamp(hard + (mallet ? 0.3 : 0.), 0., 1.);
        if(v.engine == Engine::Struck)
        {
          // Contact time 4 ms (soft felt) to 0.25 ms (hard), at velocity 1,
          // and at most about half a period: a contact longer than the
          // period filters out the fundamental, and the top octave was 40 dB
          // quieter than the middle. Piano hammers are smaller up there.
          const double tc0 = 0.004 * std::pow(0.0625, h);
          const double tcMax = 0.5 / f0;
          const double tc = tc0 / std::sqrt(std::sqrt(1. + std::pow(tc0 / tcMax, 4.)));
          v.hammer.strike(force, tc, (mallet ? 2.8 : 2.2) + 1.2 * h, fs);
          v.exc.noiseLen = std::max(2, int(0.004 * fs));
          v.exc.kind = ExciteGen::Noise;
          v.exc.total = v.exc.noiseLen;
          v.exc.noise(p.noise * force * 0.02 * perSample, 1705., fs);
        }
        else
        {
          const int len = std::clamp(
              int((0.003 - 0.00275 * h) * (1.15 - 0.3 * vel) * fs), 2,
              std::max(2, int(0.5 * period)));
          v.exc.kind = ExciteGen::Pulse;
          v.exc.len = v.exc.total = len;
          v.exc.amp = force;
          v.exc.noiseLen = std::max(2, int(0.003 * fs));
          v.exc.noise(p.noise * force * 0.1, 2725., fs);
        }
        break;
      }
      case Exciter::Burst: {
        const int len = std::max(2, int(period));
        v.exc.kind = ExciteGen::Noise;
        v.exc.total = v.exc.noiseLen = len;
        v.exc.noise(
            v.engine == Engine::Struck
                ? force * 0.6 / std::sqrt(double(len)) * std::sqrt(perSample)
                : force * 1.2,
            390. * std::pow(58., hard) * dyn, fs);
        {
          // Darker, not quieter: the power of lowpassed white noise is
          // a / (2 - a) for a one-pole of coefficient a
          const double a0 = 1. - std::exp(-kTwoPi * std::min(390. * std::pow(58., hard), kTopFreq * fs) / fs);
          const double a1 = v.exc.lpA;
          v.exc.noiseAmp *= std::sqrt((a0 / (2. - a0)) / (a1 / (2. - a1)));
        }
        break;
      }
      case Exciter::Bow:
      case Exciter::EBow: {
        v.bowing = true;
        if(fresh)
          v.bowEnv = v.bowLevel = v.bowStep = 0.;
        // A seed for the E-Bow's feedback to grow from, and the bow's
        // first scratch
        v.exc.kind = ExciteGen::Noise;
        v.exc.total = v.exc.noiseLen = std::max(2, int(0.01 * fs));
        v.exc.noise(
            (v.exciter == Exciter::EBow ? 0.02 : 0.004 + 0.02 * p.noise) * force, 2725., fs);
        break;
      }
    }
  }

  // ---- Control rate -----------------------------------------------------

  void control(const Params& p) noexcept
  {
    body.configure(p.body, p.bodySize, fs);
    if(p.sympLevel > 0. || sympLevelSm > 1e-4)
      symp.configure(p.sympTuning, p.sympRoot, p.sympDecay, fs);
    else if(symp.count > 0)
    {
      // Off: forget what rang, or turning it on again resumes it
      symp.configure(SympTuning::Off, p.sympRoot, p.sympDecay, fs);
      symp.clear();
    }
    spreadSm += 16. * levelCoef * (p.spread - spreadSm);

    // Filters that only decay settle into subnormals where nothing flushes
    // them (WebAssembly): snap them to 0
    auto snap = [](double& x) {
      if(std::abs(x) < 1e-30)
        x = 0.;
    };
    for(auto& q : body.bp)
    {
      snap(q.s1);
      snap(q.s2);
    }
    for(auto& st : symp.strings)
    {
      snap(st.loss.y);
      snap(st.dc.y1);
    }
    snap(outDcL.y1);
    snap(outDcR.y1);
    levelCoef = 1. - std::exp(-1. / (kLevelTime * fs));
    pressCoef = 1. - std::exp(-1. / (kPressureTime * fs));
    limRelease = std::exp(-1. / (kLimiterTime * fs));

    // Bridge coupling: the first body modes as partial notches in each loop.
    // k <= 0.7 keeps the notches shallow, as CVfunk's Twang measured.
    const double k = 0.7 * p.coupling;
    if(p.body != notchBody || p.bodySize != notchSize || k != notchK)
    {
      notchBody = p.body;
      notchSize = p.bodySize;
      notchK = k;
      notchProtoCount = 0;
      if(k > 0.)
      {
        const auto t = bodyTable(p.body);
        for(int i = 0; i < t.count && notchProtoCount < kNotches; i++)
        {
          const double f = t.modes[std::size_t(i)].f / p.bodySize;
          if(f >= kTopFreq * fs)
            continue;
          notchProto[std::size_t(notchProtoCount++)] = Biquad::partialNotch(
              f, t.modes[std::size_t(i)].q, k * t.modes[std::size_t(i)].gain, fs);
        }
      }
    }

    for(auto& v : voices)
      if(v.state != Voice::State::Free)
        controlVoice(v, p);
  }

  //! Brightness after Material, Mute and pressure, and the matching T60
  //! ratio at the high reference frequency.
  struct Tone
  {
    double t60;
    double ratio;
    double stiffness;
  };

  //! The pressure on a voice: its polyphonic aftertouch, else its channel's.
  //! False when neither was ever received: a keyboard without aftertouch
  //! must not read as zero pressure.
  bool pressureOf(const Voice& v, double& press) const noexcept
  {
    const auto& cs = channels[std::size_t(v.channel)];
    press = v.pressure;
    return v.hasPolyPressure || cs.hasPressure;
  }

  //! What the pressure on a voice is heading to
  double rawPressure(const Voice& v) const noexcept
  {
    const auto& cs = channels[std::size_t(v.channel)];
    return v.hasPolyPressure ? v.polyPressure : cs.hasPressure ? cs.pressure : 0.;
  }

  //! Where pressure goes. Auto: bow speed for a bow, the drive of an E-Bow,
  //! brightness otherwise.
  static Pressure pressureDestination(const Voice& v, const Params& p) noexcept
  {
    if(p.pressure != Pressure::Auto)
      return p.pressure;
    if(bowed(v.engine))
      return v.exciter == Exciter::EBow ? Pressure::Force : Pressure::BowSpeed;
    return Pressure::Brightness;
  }

  Tone tone(const Voice& v, const Params& p) noexcept
  {
    double press = 0.;
    const bool hasPress = pressureOf(v, press);
    const Pressure dest = pressureDestination(v, p);

    // Material: below 0.5 wood and gut, whose upper partials die fast; above,
    // metal, whose upper partials ring and whose stiffness grows. The tilt of
    // RipplerX's material control, applied through the loop's HF loss.
    const double m = p.material - 0.5;
    double bright = p.brightness + 0.6 * m;
    double mute = p.mute;
    if(hasPress && dest == Pressure::Brightness)
      bright += 0.5 * press;
    if(hasPress && dest == Pressure::Mute)
      mute = std::max(mute, press);
    bright -= 0.5 * mute;
    bright = std::clamp(bright, 0., 1.);

    double rate = kLn1000 / p.decay;
    // A palm on the string: T60 down to 80 ms
    rate += mute * mute * kLn1000 / 0.08;
    // After note-off, the damper: the string dies within Release
    if(p.release < Params::kReleaseFree)
    {
      const double target = kLn1000 / std::min(p.release, p.decay);
      rate += v.damp * std::max(0., target - kLn1000 / p.decay);
    }
    Tone t;
    t.t60 = kLn1000 / rate;
    t.ratio = std::pow(10., -2.5 * (1. - bright));
    t.stiffness = std::clamp(p.stiffness + std::max(0., m) * 0.3, 0., 1.);
    return t;
  }

  double voicePitch(const Voice& v, const Params& p) const noexcept
  {
    const auto& cs = channels[std::size_t(v.channel)];
    double st = v.pitch + p.tune + p.fine / 100. + cs.bend * p.bendRange;
    // Tension modulation, as a transient only: a hard pluck starts sharp and
    // settles to the pitch it is tuned to.
    st += 1.5 * p.tension * v.kick;
    return std::clamp(midiToHz(st), kMinF0, fs / 10.);
  }

  void controlVoice(Voice& v, const Params& p) noexcept
  {
    // Glide, only in Mono and Legato
    if(p.playMode != PlayMode::Poly && p.glide > 0.)
    {
      const double a = std::exp(-double(kCtrl) / (fs * std::max(1e-3, p.glide / 3.)));
      v.pitch = v.target + (v.pitch - v.target) * a;
    }
    else
    {
      v.pitch = v.target;
    }
    // Snapped to 0 at the end: an exponential never gets there, and every
    // coefficient cache would miss for the rest of the note
    v.kick = v.kick > 1e-4 ? v.kick * v.kickDecay : 0.;

    // Release ramp: about 10 ms
    const double dTarget = v.held || v.sustained ? 0. : 1.;
    const double dStep = double(kCtrl) / (kDampTime * fs);
    v.damp = v.damp < dTarget ? std::min(dTarget, v.damp + dStep)
                              : std::max(dTarget, v.damp - dStep);

    // Bow envelope: up over Bow Attack while held, down over Bow Lift
    // after: the bow's speed and its grip on the string together. It used to
    // fall with a fixed 30 ms time constant, reaching zero 0.28 s later.
    // The bow is off the string once the ramp has reached zero, not as soon
    // as its target does.
    if(v.bowing)
    {
      if(v.held || v.sustained)
        v.bowEnv = std::min(1., v.bowEnv + double(kCtrl) / (p.bowAttack * fs));
      else if(v.bowEnv > 0.)
        v.bowEnv = std::max(0., v.bowEnv - double(kCtrl) / (p.bowLift * fs));
      else if(v.bowLevel <= 0.)
        v.bowing = false;
      // Ramped per sample: in steps every control tick, a 20 ms Bow Attack
      // pushed the string 60 times, a click train at fs / 16 that bass
      // notes did not mask
      v.bowStep = (v.bowEnv - v.bowLevel) / kCtrl;
    }

    const double f0 = voicePitch(v, p);
    const Tone t = tone(v, p);
    if(v.engine == Engine::Struck)
      controlModal(v, p, f0, t);
    else
      controlWaveguide(v, p, f0, t);
  }

  void controlWaveguide(Voice& v, const Params& p, double f0, const Tone& t) noexcept
  {
    v.twoPols = p.detune > 0.01 || p.spread > 0.;
    const double detuneRatio = std::exp2(p.detune / 1200.);
    const auto& cs = channels[std::size_t(v.channel)];
    const bool bowLive = bowed(v.engine);
    double beta = v.position;
    if(bowLive)
    {
      // The bow can move along the string while it plays
      beta = std::clamp(
          p.position + (cs.hasTimbre ? (cs.timbre - 0.5) * 0.3 : 0.), 0.02, 0.5);
      v.position = std::abs(beta - v.position) > 1e-5 ? v.position + 0.2 * (beta - v.position)
                                                      : beta;
      beta = v.position;
    }

    for(int k = 0; k < (v.twoPols ? 2 : 1); k++)
    {
      auto& pol = v.pol[std::size_t(k)];
      const double f = k == 0 ? f0 : f0 * detuneRatio;
      const double period = fs / f;
      const double w = kTwoPi * f / fs;

      // Nothing moved: the delays are where they should be
      const std::array<double, 10> ctrlKey{
          f,    t.t60, t.ratio, t.stiffness, p.couplingDetune,
          notchK, double(int(notchBody)), notchSize, beta, p.hardness};
      // Nor once the pitch has settled where the designs were not made for
      const bool settled = f == pol.lastF && beta == pol.lastBeta;
      pol.lastF = f;
      pol.lastBeta = beta;
      bool same = !v.firstUpdate && std::abs(ctrlKey[0] / pol.ctrlKey[0] - 1.) < 3e-5
                  && !(settled && f != pol.designF);
      for(std::size_t i = 1; i < ctrlKey.size() && same; i++)
        same = ctrlKey[i] == pol.ctrlKey[i];
      if(same)
      {
        pol.dBStep = pol.dNStep = 0.;
        continue;
      }
      pol.ctrlKey = ctrlKey;

      // The filters and the delay they add follow the pitch from afar while
      // it moves (the tension kick, a bend, a glide): within 3.5 cents, a T60
      // off by 0.2 % and a pitch off by less than half a cent, for a moment.
      // Designed at every control tick of the kick, they took three quarters
      // of a plucked voice's time. Once the pitch holds still, they are
      // designed for it exactly. The delay lines always follow it exactly.
      const std::array<double, 8> designKey{
          t.t60, t.ratio, t.stiffness, p.couplingDetune,
          notchK, double(int(notchBody)), notchSize, p.hardness};
      const bool redesign
          = v.firstUpdate || designKey != pol.designKey
            || (settled ? f != pol.designF : std::abs(f / pol.designF - 1.) > 2e-3);
      if(redesign)
      {
        pol.designF = f;
        pol.designKey = designKey;
        // The bow's corner: 15 (soft hair, Hardness 0) to 60 (hard) times
        // the pitch, as a bow's width is a share of the string's length
        pol.roundA = 1.
                     - std::exp(
                         -kTwoPi * std::min(15. * std::pow(4., p.hardness) * f, 0.4 * fs) / fs);
        if(v.exciter == Exciter::EBow)
        {
          const double s1 = pol.ebowRes.s1, s2 = pol.ebowRes.s2;
          pol.ebowRes = Biquad::bandpass(f, kEbowQ, fs);
          pol.ebowRes.s1 = s1;
          pol.ebowRes.s2 = s2;
          // Over four periods, or 30 ms: a level, not a waveform
          pol.ebowCoef = std::exp(-1. / (std::max(0.03, 4. / f) * fs));
        }

        // Loss, net of what the interpolating reads lose. The bridge side of
        // a plucked string is a whole number of samples: the excitation point
        // does not move while it rings. A bow does.
        const double fhi = std::min(kTopFreq * fs, std::max(4000., 4. * f));
        const double w1 = kTwoPi * fhi / fs;
        pol.wholeBridge = !bowLive;
        double droop0 = lagrangeGain(pol.dN - std::floor(pol.dN), w);
        double droop1 = lagrangeGain(pol.dN - std::floor(pol.dN), w1);
        if(!pol.wholeBridge)
        {
          droop0 *= lagrangeGain(pol.dB - std::floor(pol.dB), w);
          droop1 *= lagrangeGain(pol.dB - std::floor(pol.dB), w1);
        }
        const double lp = pol.loss.lp, lp2 = pol.loss.lp2, lpl = pol.loss.lpl;
        pol.loss = designLossShelf(f, fs, t.t60, t.ratio, fhi, droop0, droop1);
        pol.loss.lp = lp;
        pol.loss.lp2 = lp2;
        pol.loss.lpl = lpl;

        // Dispersion: up to four first-order allpasses, fewer when they would
        // take more than half the loop's delay at f0 (a short loop, a high B).
        // A positive coefficient would compress the partials instead: it only
        // comes out of the fit where the stretch is negligible anyway.
        pol.dispCount = 0;
        // A bowed string a tenth as stiff: the stretch of a plucked one
        // keeps a bow from locking to the fundamental (at the default
        // Stiffness, the bow at 48 kHz sounded around the octave)
        const bool bowedHelmholtz = bowLive && v.exciter == Exciter::Bow;
        if(const double B = inharmonicity(t.stiffness, f) * (bowedHelmholtz ? 0.1 : 1.); B > 1e-6)
        {
          for(int m = kDispStages; m >= 1; m--)
          {
            double a = dispersionCoef(B, f, m, fs);
            if(a >= 0.)
              break;
            Allpass1 ap;
            ap.a = a;
            const double per = ap.phaseDelay(w);
            if(m * per > 0.5 * period)
            {
              if(m > 1)
                continue;
              // One stage, at most half the loop at DC
              const double maxDc = 0.5 * period;
              a = std::max(a, (1. - maxDc) / (1. + maxDc));
            }
            pol.dispCount = m;
            for(auto& d : pol.disp)
              d.a = a;
            break;
          }
        }

        // Bridge coupling
        pol.notchCount = notchProtoCount;
        for(int i = 0; i < notchProtoCount; i++)
        {
          auto& q = pol.notch[std::size_t(i)];
          const double s1 = q.s1, s2 = q.s2;
          q = notchProto[std::size_t(i)];
          q.s1 = s1;
          q.s2 = s2;
        }

        // The pitch is heard in the partials, and a bow locks to all of them
        // at once: compensate the loss where the partials are, weighted as a
        // plucked string's amplitudes are, rather than at f0 alone, which put
        // them up to 25 cents off when the loss's corner sits near f0.
        // A bowed string's Helmholtz motion has a sawtooth's 1/n amplitudes,
        // further up: weighted as a pluck's, bows were up to 14 cents sharp at
        // mid Brightness, 8 now.
        double tau = 0., wsum = 0.;
        // An E-Bow drives the fundamental: weighted as a pluck.
        const bool helmholtz = bowLive && v.exciter == Exciter::Bow;
        const int nMax = helmholtz ? 12 : 6;
        for(int n = 1; n <= nMax && n * f < kTopFreq * fs; n++)
        {
          const double wn = helmholtz ? 1. / n : 1. / (n * n);
          tau += wn * pol.lossDelay(n * w);
          // A bow locks the partials to one period: their stretch too is
          // compensated where they are, or it pushed the bow off the
          // fundamental
          if(helmholtz)
            tau += wn * pol.dispersionDelay(n * w);
          wsum += wn;
        }
        tau /= wsum;
        // Plucked and struck, the dispersion at f0: a stiff string is tuned
        // by its fundamental, its partials above it stretched
        if(!helmholtz)
          tau += pol.dispersionDelay(w);
        // The bridge's allpass at rest
        if(v.engine == Engine::Buzz)
          tau += 1.;
        // The coupling notches. At full Coupling Detune they are compensated
        // at f0 alone: near a body mode that moves every harmonic, its
        // detuning. At 0, not at all: each partial is pulled by the body's
        // loading where it is, most near a resonance, as on a real instrument.
        tau += p.couplingDetune * pol.notchDelay(w);
        pol.tau = tau;
      }
      const double total = std::max(8., period - pol.tau);
      // Each line at least 4 samples: the 8-point reads look 3 back
      const double b = std::clamp(beta, 4. / total, 1. - 4. / total);
      double tB = b * total;
      if(pol.wholeBridge)
      {
        // Set at the note's start; a bend or a slide moves the neck side,
        // as a finger does, unless the string becomes too short for it
        tB = v.firstUpdate ? std::round(tB) : pol.dB;
        tB = std::clamp(tB, 4., std::max(4., std::floor(total - 4.)));
      }
      const double tN = std::max(4., total - tB);
      if(v.firstUpdate)
      {
        pol.dB = tB;
        pol.dN = tN;
        pol.dBStep = pol.dNStep = 0.;
      }
      else
      {
        pol.dBStep = (tB - pol.dB) / kCtrl;
        pol.dNStep = (tN - pol.dN) / kCtrl;
      }
    }
    v.firstUpdate = false;
    v.gain = bowed(v.engine) ? 0.7 : 0.9;
  }

  void controlModal(Voice& v, const Params& p, double f0, const Tone& t) noexcept
  {
    // Recompute only when something moved: a bend of less than 0.05 cent,
    // or nothing at all, costs nothing
    const std::array<double, 11> key{
        f0,       t.t60,  t.ratio,
        t.stiffness, p.detune, double(p.modes),
        notchK,   double(int(notchBody)), notchSize,
        v.position, spreadSm};
    // Nor once the pitch has settled where the decays were not computed for
    const bool settled = f0 == v.modalLastF;
    v.modalLastF = f0;
    bool same = !v.firstUpdate && !(settled && f0 != v.decayF);
    for(std::size_t i = 0; i < key.size() && same; i++)
    {
      const double a = key[i], b = v.modalKey[i];
      same = i == 0 ? std::abs(a / b - 1.) < 3e-5 : a == b;
    }
    if(same)
      return;
    v.modalKey = key;
    // The decays follow the pitch within 3.5 cents while it moves, as the
    // waveguide's loss does, and exactly once it settles: an exp per partial
    // at every control tick of the tension kick. The frequencies always
    // follow it exactly.
    const std::array<double, 8> decayKey{
        t.t60, t.ratio, t.stiffness, p.detune, double(p.modes),
        notchK, double(int(notchBody)), notchSize};
    const bool decays
        = v.firstUpdate || decayKey != v.decayKey
          || (settled ? f0 != v.decayF : std::abs(f0 / v.decayF - 1.) > 2e-3);
    if(decays)
    {
      v.decayKey = decayKey;
      v.decayF = f0;
    }
    v.firstUpdate = false;
    // A hammer of a given contact time excites more partials of a low string
    // than of a high one: without this the bass is 25 dB louder than the
    // treble.
    // Above middle C the contact time shrinks with the period, and less is
    // needed.
    v.gain = 0.25 * std::pow(f0 / 261.63, f0 < 261.63 ? 0.8 : 0.5);

    const double B = inharmonicity(t.stiffness, f0);
    const double s0 = kLn1000 / t.t60;
    const double fhi = std::max(4000., 4. * f0);
    const double shi = kLn1000 / (t.t60 * t.ratio);
    const double c3 = fhi > f0 ? (shi - s0) / (fhi * fhi - f0 * f0) : 0.;
    const auto bt = bodyTable(notchBody);
    const bool couple = notchK > 0.;
    const int courses = p.detune > 0.01 ? 2 : 1;
    const double spread = 0.6 * spreadSm;

    auto& m = v.modal;
    int slot = 0;
    for(int c = 0; c < courses; c++)
    {
      const double fc = c == 0 ? f0 : f0 * std::exp2(p.detune / 1200.);
      // Recurrences instead of three sin or cos per partial, which during
      // the tension kick took half of a struck voice's time: e^{j n w1} by
      // repeated rotation, then the stretch's small extra angle by its
      // series; sin(n pi position) by Chebyshev's recurrence
      const double w1 = kTwoPi * fc / fs;
      const double c1 = std::cos(w1), s1 = std::sin(w1);
      double rc = 1., rs = 0.;
      const double twoCosPick = 2. * std::cos(kPi * v.position);
      double pickPrev = 0., pick = std::sin(kPi * v.position);
      for(int n = 1; n <= p.modes; n++)
      {
        if(n > 1)
        {
          const double next = twoCosPick * pick - pickPrev;
          pickPrev = pick;
          pick = next;
        }
        const double r0 = rc;
        rc = r0 * c1 - rs * s1;
        rs = r0 * s1 + rs * c1;
        const double fn = n * fc * std::sqrt((1. + B * n * n) / (1. + B));
        if(fn >= kTopFreq * fs)
          break;
        const std::size_t k = std::size_t(slot);
        if(decays)
        {
          double sigma = s0 + c3 * std::max(0., fn * fn - f0 * f0);
          if(couple)
          {
            // Partials near a body mode lose energy into it
            for(int i = 0; i < std::min(bt.count, kNotches); i++)
            {
              const double fb = bt.modes[std::size_t(i)].f / notchSize;
              const double bw = fb / bt.modes[std::size_t(i)].q;
              const double x = (fn - fb) / bw;
              sigma += notchK * bt.modes[std::size_t(i)].gain * 20. / (1. + x * x);
            }
          }
          m.rad[k] = std::exp(-sigma / fs);
        }
        const double rad = m.rad[k];
        const double w = kTwoPi * fn / fs;
        double cw, sw;
        if(const double x = w - n * w1; std::abs(x) < 0.25)
        {
          // To 1e-11 at |x| = 0.25
          const double x2 = x * x;
          const double sx = x * (1. - x2 / 6. * (1. - x2 / 20. * (1. - x2 / 42.)));
          const double cx = 1. - x2 / 2. * (1. - x2 / 12. * (1. - x2 / 30. * (1. - x2 / 56.)));
          cw = rc * cx - rs * sx;
          sw = rs * cx + rc * sx;
        }
        else
        {
          cw = std::cos(w);
          sw = std::sin(w);
        }
        // Keep the state when the partial already rings: a bend glides it
        m.cr[k] = rad * cw;
        m.ci[k] = rad * sw;
        // Plaits' attenuation towards Nyquist, at 48 kHz: the same partials
        // at any rate
        const double nyq = std::max(0., 1. - fn / 24000.);
        m.ex[k] = pick * nyq;
        // Odd and even partials to either side, as two pickups would
        const double side = (n % 2 ? 1. : -1.) * (c == 0 ? 1. : -1.);
        m.wl[k] = 0.5 * (1. + spread * side);
        m.wr[k] = 0.5 * (1. - spread * side);
        slot++;
      }
    }
    for(int k = slot; k < m.count; k++)
      m.zr[std::size_t(k)] = m.zi[std::size_t(k)] = 0.;
    m.count = slot;
  }

  // ---- Audio rate -------------------------------------------------------

  void tickVoice(Voice& v, const Params& p, double& outL, double& outR) noexcept
  {
    v.pressure += pressCoef * (rawPressure(v) - v.pressure);
    double l = 0., r = 0.;
    if(v.engine == Engine::Struck)
      tickModal(v, l, r);
    else
      tickWaveguide(v, p, l, r);

    l *= v.gain;
    r *= v.gain;

    // Safety: a voice that blew up is silenced and reset, alone
    if(!finite(l) || !finite(r) || std::abs(l) >= 50. || std::abs(r) >= 50.) [[unlikely]]
    {
      v = resetVoice(v);
      return;
    }

    if(v.state == Voice::State::Stealing)
    {
      v.stealGain -= 1. / (kStealTime * fs);
      if(v.stealGain <= 0.)
      {
        const bool pend = v.pending;
        const int n = v.pendingNote, c = v.pendingChannel;
        const double vel = v.pendingVelocity;
        v = resetVoice(v);
        if(pend)
          start(v, n, c, vel, p);
        return;
      }
      l *= v.stealGain;
      r *= v.stealGain;
    }

    const double a = std::max(std::abs(l), std::abs(r));
    v.env = a > v.env ? a : v.env * envRelease();
    v.age++;

    // Retire a voice nothing excites any more once it is inaudible
    if(v.state == Voice::State::Active && !v.bowing && !v.exc.active()
       && !v.hammer.active && v.env < 1e-5 && v.age > std::int64_t(kRetireAge * fs))
    {
      v = resetVoice(v);
      return;
    }

    outL += l;
    outR += r;
  }

  double envRelease() const noexcept { return 1. - 1. / (kEnvTime * fs); }

  void tickModal(Voice& v, double& l, double& r) noexcept
  {
    double in = v.exc.tick(rng);
    in += v.hammer.tick();
    v.modal.tick(in, l, r);
    if((v.age & 1023) == 0)
      v.modal.flushDenormals();
  }

  void tickWaveguide(Voice& v, const Params& p, double& l, double& r) noexcept
  {
    const double e = v.exc.tick(rng);
    const bool ebow = v.exciter == Exciter::EBow;

    // Bow: speed, pressure and rosin
    double vBow = 0., fn = 0., v0 = 0., drive = 0., slope = 0., offset = 0.;
    // Bow is STK's table, Friction the friction law
    const bool classic = v.engine == Engine::Bow;
    if(v.bowing)
    {
      v.bowLevel = std::clamp(v.bowLevel + v.bowStep, 0., 1.);
      double press = 0.;
      const bool hasPress = pressureOf(v, press);
      const Pressure dest = pressureDestination(v, p);
      double speed = p.bowSpeed;
      double force = p.force * (1. - p.velForce + p.velForce * v.velocity);
      if(hasPress && dest == Pressure::BowSpeed)
        speed *= 0.2 + 1.6 * press;
      if(hasPress && dest == Pressure::Force)
        force = std::clamp(force * (0.3 + 1.4 * press), 0., 1.);
      if(ebow)
        drive = (0.024 + 0.18 * force) * v.bowLevel;
      else if(classic)
      {
        vBow = (0.03 + 0.22 * speed) * v.bowLevel;
        slope = 5. - 4. * force;
        offset = 0.9 - 0.3 * p.hardness;
      }
      else
      {
        // Force through Schelleng's window: the most a bow can press at this
        // speed and position before the string stops returning to it,
        // vb / ((mus - mud) beta); the knob spans 15 to 80 % of it: from a
        // light, airy surface sound through the Helmholtz motion, in its
        // middle at any speed, position and pitch, to a raucous crunch at
        // the top. Bow Speed sets the loudness, Force the timbre.
        const double vbFull = 0.03 + 0.22 * speed;
        vBow = vbFull * v.bowLevel;
        if(force != v.bowForce || v.position != v.bowPosition)
        {
          // A pow and a division: only when the force or the bow moves
          v.bowForce = force;
          v.bowPosition = v.position;
          v.bowShare = 0.15 * std::pow(0.8 / 0.15, force)
                       / ((BowFriction::kMuS - BowFriction::kMuD) * v.position);
        }
        fn = vbFull * v.bowShare;
        // A bow lifting off takes its pressure away with its speed: at full
        // grip, a slowing bow clamped the string where it touched it, and
        // let go at once when it stopped, a detuned swell after the note
        if(!(v.held || v.sustained))
          fn *= v.bowLevel;
        // Hardness is the rosin: how sharply friction falls as the string
        // slides, from smooth to gritty
        v0 = vbFull * (0.4 - 0.3 * p.hardness);
      }
    }

    const double buzz = v.engine == Engine::Buzz ? p.buzz : 0.;
    const int npol = v.twoPols ? 2 : 1;
    double out[2]{0., 0.};
    for(int k = 0; k < npol; k++)
    {
      auto& pol = v.pol[std::size_t(k)];
      pol.dB += pol.dBStep;
      pol.dN += pol.dNStep;

      const double bridgeOut = pol.bridge.read(pol.dB, pol.bridgeRead);
      const double nutOut = pol.neck.read(pol.dN, pol.neckRead);
      double atBridge = bridgeOut;
      if(v.engine == Engine::Buzz)
      {
        // Buzz: the string wraps onto a curved bridge as it swings, which
        // changes its length with its amplitude: the jawari of a sitar or a
        // tanpura. As a first-order allpass whose angle follows the wave
        // (Pierce and Van Duyne, JASA 1997; Faust's allpassnn, as Tambura
        // uses it): a rotation of the input and the state, lossless whatever
        // the angle does, so the bridge moves energy up the spectrum and
        // takes none. Shortening the delay line's read instead, by up to 17
        // samples from one sample to the next, damped the string by 13 to
        // 25 dB in 0.3 s. At rest, one sample of delay. Driven by a
        // displacement estimate, whose offset after a pluck held it off
        // rest, it detuned the note by up to a semitone.
        const double th = std::clamp(buzz * kJawari * bridgeOut, -1.2, 1.2);
        const double sn = th - th * th * th / 6.;
        const double cs = std::sqrt(1. - sn * sn);
        const double w = cs * atBridge - sn * pol.jaw;
        atBridge = sn * atBridge + cs * pol.jaw;
        pol.jaw = w;
      }
      const double bridgeRefl = -pol.filters(atBridge);
      const double nutRefl = -nutOut;
      const double vString = bridgeRefl + nutRefl;

      double newVel = e * (npol == 2 ? v.split[k] * 1.25 : 1.);
      if(vBow > 0.)
      {
        if(classic)
        {
          // Its grip fades as the bow lifts
          const double dv = vBow - vString;
          const double grip = v.held || v.sustained ? 1. : v.bowLevel;
          newVel += grip * dv * bowTable(dv, slope, offset);
        }
        else
        {
          const double push = pol.friction.tick(vBow, vString, fn, v0);
          pol.bowRound += pol.roundA * (push - pol.bowRound);
          newVel += pol.bowRound;
        }
      }
      if(drive > 0.)
      {
        // What an E-Bow's pickup coil hears is the string near its
        // fundamental, and that is what its driver pushes back: fed back
        // whole, through a saturating gain, the string locked to its 3rd to
        // 5th partials, 20 to 60 dB above the fundamental. A real E-Bow also
        // regulates its drive on the string's level: without that, the
        // feedback grows until the loop's backstop clamp.
        const double y = 30. * pol.ebowRes.tick(vString);
        const double sq = vString * vString;
        pol.ebowEnv = sq + pol.ebowCoef * (pol.ebowEnv - sq);
        const double agc = std::clamp(1. - std::sqrt(pol.ebowEnv) / kEbowTarget, 0., 1.);
        // Its driver is a magnet: it pulls harder than it lets go, which
        // gives the sustained tone even partials as well as odd ones
        const double sat = y / (1. + std::abs(y));
        newVel += drive * agc * (sat + kEbowAsym * sat * sat);
      }

      pol.neck.write(std::clamp(bridgeRefl + newVel, -8., 8.));
      pol.bridge.write(std::clamp(nutRefl + newVel, -8., 8.));
      out[k] = bridgeOut;
    }
    if(npol == 1)
      out[1] = out[0];

    const double s = spreadSm;
    l = out[0] * (1. + s) * 0.5 + out[1] * (1. - s) * 0.5;
    r = out[1] * (1. + s) * 0.5 + out[0] * (1. - s) * 0.5;
  }
};
}
