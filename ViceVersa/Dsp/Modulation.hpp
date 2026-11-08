#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* ViceVersa's modulation: four LFOs per voice, as Cordito's, each moving one
 * target: the vector's point (X, Y), the pitch of all the sources or of one,
 * the frame of a source's bank, the filter's cutoff, or the level.
 *
 * A voice's LFOs start at their Phase on each note, so two of them on X and
 * Y, a quarter of a cycle apart, draw a circle (sine) or the diamond's edge
 * (triangle). Each holds still for its Delay, then fades in over half of it.
 * Random holds a new value each cycle, its own per LFO and per voice.
 *
 * Depth is 0..1 for every target; at 1:
 *   X, Y       the whole diamond, ±1
 *   pitches    ±1 octave, on a squared curve: a vibrato is a small depth
 *   frames     ±1, the whole bank
 *   cutoff     ±4 octaves
 *   level      down to silence at the wave's lowest point
 */

#include <array>
#include <cmath>
#include <cstdint>

namespace viceversa
{
inline constexpr int kSources = 4;
inline constexpr int kLfos = 4;

enum class LfoShape : int
{
  Triangle,
  Sine,
  Square,
  SawUp,
  SawDown,
  Random
};
inline constexpr int kLfoShapeCount = 6;

//! What an LFO moves
enum class ModTarget : int
{
  Off,
  X,
  Y,
  Pitch,
  PitchA,
  PitchB,
  PitchC,
  PitchD,
  FrameA,
  FrameB,
  FrameC,
  FrameD,
  Cutoff,
  Level
};
inline constexpr int kModTargetCount = 14;

inline constexpr float kModPitch = 12.f;  // semitones at depth 1
inline constexpr float kModCutoff = 48.f; // semitones at depth 1

struct LfoParams
{
  ModTarget target{ModTarget::Off};
  LfoShape shape{LfoShape::Sine};
  float period{2.f}; // seconds
  float depth{0.5f};
  float phase{0.f};  // cycles, where each note starts it
  float delay{0.f};  // seconds
};

//! One LFO of one voice. Scalar, at control rate.
struct Lfo
{
  double phase{0.};
  float held{0.f};
  std::uint32_t rng{0x9E3779B9u};

  void start(float startPhase, std::uint32_t seed) noexcept
  {
    phase = startPhase;
    rng = seed | 1u;
    held = random();
  }

  //! Uniform in [-1, 1)
  float random() noexcept
  {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return float(rng >> 8) / 8388608.f - 1.f;
  }

  //! The value at the current phase, in [-1, 1], then the phase moves on by
  //! `advance` cycles
  float tick(LfoShape shape, double advance) noexcept
  {
    const float p = float(phase);
    float v = 0.f;
    switch(shape)
    {
      default:
      case LfoShape::Triangle:
        // From the middle, rising: a quarter of a cycle behind is the
        // diamond's edge with the other axis
        v = p < 0.25f ? 4.f * p : p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f;
        break;
      case LfoShape::Sine:
        v = std::sin(6.28318530717958647692f * p);
        break;
      case LfoShape::Square:
        v = p < 0.5f ? 1.f : -1.f;
        break;
      case LfoShape::SawUp:
        v = 2.f * p - 1.f;
        break;
      case LfoShape::SawDown:
        v = 1.f - 2.f * p;
        break;
      case LfoShape::Random:
        v = held;
        break;
    }
    phase += advance;
    if(phase >= 1.)
    {
      phase -= std::floor(phase);
      held = random();
    }
    return v;
  }
};

//! What a voice's LFOs add, this control period
struct ModAmounts
{
  float x{0.f}, y{0.f};
  float pitch{0.f};                     // semitones, every source
  std::array<float, kSources> pitchOf{}; // semitones, one source
  std::array<float, kSources> frameOf{};
  float cutoff{0.f};                     // semitones
  float level{1.f};                      // a gain

  void add(ModTarget t, float depth, float v) noexcept
  {
    const float pitchDepth = kModPitch * depth * depth;
    switch(t)
    {
      case ModTarget::Off:
        break;
      case ModTarget::X:
        x += depth * v;
        break;
      case ModTarget::Y:
        y += depth * v;
        break;
      case ModTarget::Pitch:
        pitch += pitchDepth * v;
        break;
      case ModTarget::PitchA:
      case ModTarget::PitchB:
      case ModTarget::PitchC:
      case ModTarget::PitchD:
        pitchOf[std::size_t(int(t) - int(ModTarget::PitchA))] += pitchDepth * v;
        break;
      case ModTarget::FrameA:
      case ModTarget::FrameB:
      case ModTarget::FrameC:
      case ModTarget::FrameD:
        frameOf[std::size_t(int(t) - int(ModTarget::FrameA))] += depth * v;
        break;
      case ModTarget::Cutoff:
        cutoff += kModCutoff * depth * v;
        break;
      case ModTarget::Level:
        level *= 1.f - depth * 0.5f * (1.f - v);
        break;
    }
  }
};
}
