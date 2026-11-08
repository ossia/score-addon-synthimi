#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The CZ's eight-step envelopes: each step goes to a level at a rate, one
 * step holds while the key is down (sustain) and one ends the envelope (end).
 * Releasing the key jumps to the step after the sustain point.
 *
 * Not on the CZ: an envelope may loop. While the key is down, it goes back to
 * its first step after the sustain step, or after its end when it has none,
 * instead of holding there: repeated hits, a filter that keeps moving.
 * Releasing the key leaves the loop, as it leaves the sustain.
 *
 * As on the chip, a step moves at a constant speed: its time is the time a
 * full-scale move would take, so a smaller move is quicker. The CZ's rates
 * 0..99 are kept as a normalised rate, mapped from 30 s (0) to 0.5 ms (1)
 * per full-scale move; the firmware's own table is not published.
 *
 * Stored in four ports: the steps in a std::vector<float> of kEnvSize values,
 * [rate0, level0, ..., rate7, level7], all in [0, 1], edited on a graph;
 * the sustain step (0 for none, else step + 1), the end step and the loop,
 * as menus and a toggle.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

namespace czumarzu
{
inline constexpr int kSteps = 8;
inline constexpr int kEnvSize = 2 * kSteps;

inline constexpr double kSlowest = 30.;   // seconds for a full-scale move, rate 0
inline constexpr double kFastest = 0.0005; // rate 1
//! ln(kFastest / kSlowest), checked by the tests: std::log is no constant
//! expression before C++26
inline constexpr double kLnRateSpan = -11.002099841204238;

//! The chip's DCA is linear in decibels: level 1 is 0 dB, level 0 this far down
inline constexpr float kDcaRangeDb = 78.27f;
//! Decibels to the exponent of e: ln(10) / 20
inline constexpr float kNepersPerDb = 0.11512925464970229f;

namespace detail
{
//! e^x, as a constant expression: halved until the series converges at once,
//! then squared back, within 1e-14 over the rates' range
[[nodiscard]] constexpr double exp(double x) noexcept
{
  int halvings = 0;
  while(x < -0.5 || x > 0.5)
  {
    x *= 0.5;
    halvings++;
  }
  double term = 1., sum = 1.;
  for(int i = 1; i < 20; i++)
  {
    term *= x / i;
    sum += term;
  }
  while(halvings-- > 0)
    sum *= sum;
  return sum;
}
}

//! Seconds a full-scale move takes at normalised rate r
[[nodiscard]] constexpr double stepTime(double r) noexcept
{
  return kSlowest * detail::exp(kLnRateSpan * std::clamp(r, 0., 1.));
}

struct EnvSpec
{
  std::array<float, kSteps> rate{};
  std::array<float, kSteps> level{};
  int sustain{-1}; // -1: none
  int end{0};
  bool loop{false};
  //! Full-scale moves per second of each step, 1 / stepTime(rate): kept by
  //! sanitize(), so the envelopes never compute a power at control rate
  std::array<double, kSteps> speed{};

  //! The step after which a loop goes back to the first
  [[nodiscard]] constexpr int loopEnd() const noexcept { return sustain >= 0 ? sustain : end; }

  /*! Rates and levels in [0, 1] (0.5 and 0 for what is not, NaN too), the end
   *  a step, the sustain none or a step up to the end; then the speeds.
   */
  constexpr void sanitize() noexcept
  {
    for(int i = 0; i < kSteps; i++)
    {
      auto& r = rate[std::size_t(i)];
      auto& l = level[std::size_t(i)];
      r = (r >= 0.f && r <= 1.f) ? r : 0.5f;
      l = (l >= 0.f && l <= 1.f) ? l : 0.f;
      speed[std::size_t(i)] = 1. / stepTime(r);
    }
    end = std::clamp(end, 0, kSteps - 1);
    if(sustain < -1 || sustain > end)
      sustain = -1;
  }

  /*! From the ports: the steps (`fallback`'s if not kEnvSize values), the
   *  sustain menu (0 none, else the step + 1), the end menu (the step), the
   *  loop. A sustain past the end is none.
   */
  [[nodiscard]] static EnvSpec from(
      const std::vector<float>& points, int sustainItem, int endItem, bool loop,
      const EnvSpec& fallback) noexcept
  {
    EnvSpec e = fallback;
    if(points.size() == std::size_t(kEnvSize))
    {
      for(int i = 0; i < kSteps; i++)
      {
        e.rate[std::size_t(i)] = points[std::size_t(2 * i)];
        e.level[std::size_t(i)] = points[std::size_t(2 * i + 1)];
      }
    }
    e.end = endItem;
    e.sustain = std::clamp(sustainItem, 0, kSteps) - 1;
    e.loop = loop;
    e.sanitize();
    return e;
  }

  //! The steps' port
  [[nodiscard]] std::vector<float> points() const
  {
    std::vector<float> v(kEnvSize, 0.f);
    for(int i = 0; i < kSteps; i++)
    {
      v[std::size_t(2 * i)] = rate[std::size_t(i)];
      v[std::size_t(2 * i + 1)] = level[std::size_t(i)];
    }
    return v;
  }
  //! The sustain menu's item: 0 none, else the step + 1
  [[nodiscard]] constexpr int sustainItem() const noexcept { return sustain + 1; }

  //! Up to `rate` at `level` for each (rate, level) given, sustain and end
  [[nodiscard]] static constexpr EnvSpec make(
      std::initializer_list<std::pair<float, float>> steps, int sustain, int end,
      bool loop = false) noexcept
  {
    EnvSpec e;
    int i = 0;
    for(auto [r, l] : steps)
    {
      e.rate[std::size_t(i)] = r;
      e.level[std::size_t(i)] = l;
      if(++i == kSteps)
        break;
    }
    for(; i < kSteps; i++)
      e.rate[std::size_t(i)] = 0.5f;
    e.sustain = sustain;
    e.end = end;
    e.loop = loop;
    e.sanitize();
    return e;
  }
};

//! The patch's envelopes before any is drawn, and what the ports fall back to
inline constexpr EnvSpec kDefaultDca = EnvSpec::make({{0.95f, 1.f}, {0.62f, 0.f}}, 0, 1);
inline constexpr EnvSpec kDefaultDcw
    = EnvSpec::make({{0.9f, 0.7f}, {0.45f, 0.35f}, {0.6f, 0.f}}, 1, 2);
inline constexpr EnvSpec kDefaultDco = EnvSpec::make({{1.f, 0.f}}, -1, 0);

//! One running envelope. Scalar, at control rate.
struct EnvState
{
  float value{0.f};
  int step{0};
  bool done{true};

  void start(const EnvSpec&) noexcept
  {
    // From where it is: a retriggered note does not jump to zero
    step = 0;
    done = false;
  }

  void reset() noexcept
  {
    value = 0.f;
    step = 0;
    done = true;
  }

  //! Key released: on to the step after the sustain point
  void release(const EnvSpec& s) noexcept
  {
    if(done)
      return;
    if(s.sustain >= 0 && step <= s.sustain)
      step = std::min(s.sustain + 1, s.end);
    else if(s.sustain < 0)
      step = std::max(step, s.end);
  }

  /*! Advances by `n` samples. `perSample` is 1 / the sample rate, times the
   *  key follow. `gate`: the key (or pedal) still holds the note.
   *  After the end step, when released, the level keeps falling to 0 at the
   *  end step's rate: a voice never stays on forever.
   */
  void advance(const EnvSpec& s, double perSample, int n, bool gate) noexcept
  {
    if(done && !(value > 0.f && !gate))
      return;
    double budget = n;
    for(int guard = 0; guard < kSteps + 1 && budget > 0.; guard++)
    {
      const bool past = done || step > s.end;
      const int st = std::clamp(step, 0, kSteps - 1);
      const float target = past ? 0.f : s.level[std::size_t(st)];
      const double speed
          = perSample * s.speed[std::size_t(std::clamp(std::min(st, s.end), 0, kSteps - 1))];
      const double dist = std::abs(double(target) - value);
      const double need = dist / speed;
      if(need > budget)
      {
        value += float((target > value ? 1. : -1.) * speed * budget);
        return;
      }
      value = target;
      budget -= need;

      if(past)
      {
        done = true;
        return;
      }
      if(s.loop && gate && step == s.loopEnd())
      {
        // Steps of no length could loop forever: the guard ends the block
        step = 0;
        continue;
      }
      if(step == s.sustain && gate)
        return;
      if(step >= s.end)
      {
        done = true;
        if(gate || value <= 0.f)
          return;
        continue; // released with a level: fall to 0
      }
      step++;
    }
  }

  //! At the end, nothing left to sound
  [[nodiscard]] bool silent() const noexcept { return done && value <= 1e-4f; }
};
}
