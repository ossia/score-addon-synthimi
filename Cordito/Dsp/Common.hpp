#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: constants, the enumerations of the ports, the parameters as the DSP
// sees them, and small helpers.

#include <Dsp/Denormals.hpp>
#include <Dsp/Numeric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace crd
{
using synthimi::finite;
using synthimi::sane;
using synthimi::saneEnum;
//! Flush-to-zero for the scope of a render, restored after, on x86 only: on
//! AArch64 the FPCR stays as the host set it, and Synth::control snaps the
//! filters that only decay to 0, as wherever nothing flushes them
using ScopedFlushDenormals = synthimi::ScopedFlushDenormals<false>;

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2. * kPi;
//! ln(1000): a T60 is the time for the amplitude to fall by 1000
inline constexpr double kLn1000 = 6.907755278982137;

inline constexpr int kMaxVoices = 16;
//! Samples between two coefficient updates
inline constexpr int kCtrl = 16;
inline constexpr int kMaxModes = 48;
inline constexpr int kMaxSymp = 12;
inline constexpr int kMaxBodyModes = 10;
inline constexpr int kDispStages = 4;
inline constexpr int kNotches = 4;
//! Lowest fundamental a string can be bent to; sizes the delay lines
inline constexpr double kMinF0 = 20.;
//! The highest frequency, as a share of the rate, that a partial, a body
//! mode or a filter corner is put at
inline constexpr double kTopFreq = 0.45;

// The .scp preset format stores a combobox by its enumerator INDEX: every
// enum below is append-only.
enum class Engine : int
{
  Pluck = 0,
  Struck,
  Bow,
  Buzz,
  Friction
};

enum class Exciter : int
{
  Pick = 0,
  Finger,
  Hammer,
  Mallet,
  Burst,
  Bow,
  EBow
};

enum class SympTuning : int
{
  Off = 0,
  Octaves,
  Fifths,
  Major,
  Minor,
  Drone,
  Chromatic,
  Follow
};

enum class Body : int
{
  None = 0,
  Guitar,
  Violin,
  Cello,
  Harp,
  Gourd,
  Soundboard,
  Box,
  Metal
};

enum class PlayMode : int
{
  Poly = 0,
  Mono,
  Legato
};

//! Where channel pressure, polyphonic aftertouch and MPE pressure go
enum class Pressure : int
{
  Auto = 0,
  Force,
  Brightness,
  BowSpeed,
  Mute,
  Off
};

//! What an LFO moves
enum class LfoTarget : int
{
  Off = 0,
  Pitch,
  Level,
  Pan,
  Brightness,
  Position,
  BowSpeed,
  Force,
  Buzz
};

enum class LfoShape : int
{
  Sine = 0,
  Triangle,
  SawUp,
  SawDown,
  Square,
  Random
};

// The number of enumerators of each enum above. Cordito.hpp checks them
// against the reflected enums at compile time: appending an enumerator
// without updating its count would silently map it to the default.
inline constexpr int kEngineCount = 5;
inline constexpr int kExciterCount = 7;
inline constexpr int kSympTuningCount = 8;
inline constexpr int kBodyCount = 9;
inline constexpr int kPlayModeCount = 3;
inline constexpr int kPressureCount = 6;
inline constexpr int kLfoTargetCount = 9;
inline constexpr int kLfoShapeCount = 6;
//! LFOs per voice
inline constexpr int kLfos = 4;

//! One LFO: what it moves, how, how fast and how much
struct LfoParams
{
  LfoTarget target = LfoTarget::Off;
  LfoShape shape = LfoShape::Sine;
  double period = 0.2; // seconds
  double depth = 0.5;  // 0 .. 1: what 1 is depends on the target
  double delay = 0.;   // seconds to fade in from the note's start
};

//! An exciter the engine does not have falls back to its default, so that a
//! preset can never be silent because of a combination.
//! The engines that bow the waveguide: STK's table, or the friction law
[[nodiscard]] inline constexpr bool bowed(Engine e) noexcept
{
  return e == Engine::Bow || e == Engine::Friction;
}

[[nodiscard]] inline Exciter resolveExciter(Engine e, Exciter x) noexcept
{
  switch(e)
  {
    case Engine::Pluck:
      if(x == Exciter::Pick || x == Exciter::Finger || x == Exciter::Hammer
         || x == Exciter::Burst)
        return x;
      return Exciter::Pick;
    case Engine::Struck:
      if(x == Exciter::Pick || x == Exciter::Hammer || x == Exciter::Mallet
         || x == Exciter::Burst)
        return x;
      return Exciter::Hammer;
    case Engine::Bow:
    case Engine::Friction:
      return x == Exciter::EBow ? Exciter::EBow : Exciter::Bow;
    case Engine::Buzz:
      if(x == Exciter::Pick || x == Exciter::Finger || x == Exciter::Burst)
        return x;
      return Exciter::Pick;
  }
  return Exciter::Pick;
}

//! Every control of the synth, in physical units, clamped by sanitize()
struct Params
{
  Engine engine = Engine::Pluck;
  Exciter exciter = Exciter::Pick;
  double hardness = 0.6;
  double position = 0.13;
  double force = 0.5;
  double noise = 0.1;

  double tune = 0.;  // semitones
  double fine = 0.;  // cents
  double decay = 4.; // T60 at the fundamental, seconds
  double brightness = 0.6;
  double stiffness = 0.1;
  double tension = 0.2;
  double detune = 3.; // cents
  double material = 0.5;

  int modes = 24;
  double buzz = 0.5;
  double bowSpeed = 0.5;
  double bowAttack = 0.08; // seconds
  double bowLift = 0.1;    // seconds

  double release = 0.3; // seconds
  double mute = 0.;
  double damperNoise = 0.1;

  //! Above 1, louder than a real instrument's: for a halo heard without its
  //! string
  double sympLevel = 0.;
  SympTuning sympTuning = SympTuning::Off;
  int sympRoot = 36; // MIDI note
  double sympDecay = 8.;

  Body body = Body::None;
  double bodySize = 1.;
  double bodyMix = 0.5;
  double coupling = 0.;
  //! How much of the coupling notches' delay at f0 is compensated: at 1,
  //! near a body mode every harmonic moves, a detuning of its own; at 0,
  //! only the partials near the body's resonances are pulled.
  double couplingDetune = 1.;

  PlayMode playMode = PlayMode::Poly;
  double glide = 0.;     // seconds
  double bendRange = 2.; // semitones
  double velForce = 0.7;
  Pressure pressure = Pressure::Auto;

  double spread = 0.5;
  double level = 1.;
  //! The played strings in the mix. The sympathetic strings are fed before
  //! it: at 0 they still ring.
  double stringLevel = 1.;
  //! The excitation heard directly: a pick's click, a hammer's thump, the
  //! rosin of a bow
  double exciterLevel = 0.;
  int voices = kMaxVoices;
  std::array<LfoParams, kLfos> lfos{};

  //! The release time at which notes ring free instead of being damped
  static constexpr double kReleaseFree = 10.;

  void sanitize() noexcept
  {
    engine = saneEnum(engine, kEngineCount);
    exciter = saneEnum(exciter, kExciterCount);
    hardness = sane(hardness, 0., 1., 0.6);
    position = sane(position, 0.02, 0.5, 0.13);
    force = sane(force, 0., 1., 0.5);
    noise = sane(noise, 0., 1., 0.1);
    tune = sane(tune, -24., 24., 0.);
    fine = sane(fine, -100., 100., 0.);
    decay = sane(decay, 0.05, 30., 4.);
    brightness = sane(brightness, 0., 1., 0.6);
    stiffness = sane(stiffness, 0., 1., 0.1);
    tension = sane(tension, 0., 1., 0.2);
    detune = sane(detune, 0., 25., 3.);
    material = sane(material, 0., 1., 0.5);
    modes = std::clamp(modes, 8, kMaxModes);
    buzz = sane(buzz, 0., 1., 0.5);
    bowSpeed = sane(bowSpeed, 0., 1., 0.5);
    bowAttack = sane(bowAttack, 0.005, 2., 0.08);
    bowLift = sane(bowLift, 0.005, 4., 0.1);
    release = sane(release, 0.01, kReleaseFree, 0.3);
    mute = sane(mute, 0., 1., 0.);
    damperNoise = sane(damperNoise, 0., 1., 0.1);
    sympLevel = sane(sympLevel, 0., 4., 0.);
    sympTuning = saneEnum(sympTuning, kSympTuningCount);
    sympRoot = std::clamp(sympRoot, 24, 60);
    sympDecay = sane(sympDecay, 0.5, 30., 8.);
    body = saneEnum(body, kBodyCount);
    bodySize = sane(bodySize, 0.5, 2., 1.);
    bodyMix = sane(bodyMix, 0., 1., 0.5);
    coupling = sane(coupling, 0., 1., 0.);
    couplingDetune = sane(couplingDetune, 0., 1., 1.);
    playMode = saneEnum(playMode, kPlayModeCount);
    glide = sane(glide, 0., 2., 0.);
    bendRange = sane(bendRange, 0., 48., 2.);
    velForce = sane(velForce, 0., 1., 0.7);
    pressure = saneEnum(pressure, kPressureCount);
    spread = sane(spread, 0., 1., 0.5);
    level = sane(level, 0., 2., 1.);
    stringLevel = sane(stringLevel, 0., 1., 1.);
    exciterLevel = sane(exciterLevel, 0., 1., 0.);
    voices = std::clamp(voices, 1, kMaxVoices);
    for(auto& l : lfos)
    {
      l.target = saneEnum(l.target, kLfoTargetCount);
      l.shape = saneEnum(l.shape, kLfoShapeCount);
      l.period = sane(l.period, 0.025, 20., 0.2);
      l.depth = sane(l.depth, 0., 1., 0.5);
      l.delay = sane(l.delay, 0., 4., 0.);
    }
  }
};

//! Small, fast, per-instance: never a global RNG on the audio thread
struct Rng
{
  std::uint32_t s = 0x9E3779B9u;
  std::uint32_t next() noexcept
  {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  //! [0, 1)
  float uniform() noexcept { return float(next() >> 8) * (1.f / 16777216.f); }
  //! [-1, 1)
  float bipolar() noexcept { return 2.f * uniform() - 1.f; }
};

//! Helpers shared by the processor and the tests
[[nodiscard]] inline double midiToHz(double note) noexcept
{
  return 440. * std::exp2((note - 69.) / 12.);
}
}
