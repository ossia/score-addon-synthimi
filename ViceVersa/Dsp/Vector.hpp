#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Vector synthesis after the Prophet VS: four sources on the corners of a
 * diamond (A west, B north, C east, D south), mixed by where a point is in
 * it, and a five-point envelope that moves the point.
 *
 * The VS's mixing is documented by its behaviour: each corner alone at 100 %,
 * the centre 25 % of each, the four always summing to 100 %. A bilinear law
 * in the diamond's own axes does exactly that:
 *
 *   u = (X + Y + 1) / 2,  v = (Y - X + 1) / 2   (each clamped to [0, 1])
 *   A = (1 - u) v,  B = u v,  C = u (1 - v),  D = (1 - u)(1 - v)
 *
 * Equal power takes the square roots, for sources that do not add in phase.
 *
 * The mix envelope goes from point 0 to point 4 in four timed moves, can loop
 * over a span of them (round, or back and forth) while the key is held, and on
 * release goes to point 4 in its release time. The times are time ports of
 * their own, so they can follow the tempo, the loop's mode and ends are menus,
 * and the points one std::vector<float> port of kVecEnvSize values, all in
 * [0, 1]: [x0, y0, ..., x4, y4], positions as (v + 1) / 2.
 */

#include <ViceVersa/Dsp/Base.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace viceversa
{
inline constexpr int kVecPoints = 5;
inline constexpr int kVecEnvSize = 2 * kVecPoints;

struct Weights
{
  float a, b, c, d;
};

[[nodiscard]] inline Weights mixLinear(float x, float y) noexcept
{
  const float u = std::clamp(0.5f * (x + y + 1.f), 0.f, 1.f);
  const float v = std::clamp(0.5f * (y - x + 1.f), 0.f, 1.f);
  return {(1.f - u) * v, u * v, u * (1.f - v), (1.f - u) * (1.f - v)};
}

[[nodiscard]] inline Weights mixEqualPower(float x, float y) noexcept
{
  const auto w = mixLinear(x, y);
  return {std::sqrt(w.a), std::sqrt(w.b), std::sqrt(w.c), std::sqrt(w.d)};
}

// Stored by index in presets: append only
enum class VecLoop : int
{
  Off,
  Round,
  BackAndForth
};
inline constexpr int kVecLoopCount = 3;

//! The loop's menus: the point it starts from, the point it goes to
enum class LoopFrom : int
{
  P0,
  P1,
  P2,
  P3
};
enum class LoopTo : int
{
  P1,
  P2,
  P3,
  P4
};
inline constexpr int kLoopFromCount = 4;
inline constexpr int kLoopToCount = 4;
//! Loop To's items start at point 1
[[nodiscard]] constexpr int loopToPoint(LoopTo item) noexcept
{
  return int(item) + 1;
}
[[nodiscard]] constexpr LoopTo loopToItem(int point) noexcept
{
  return LoopTo(point - 1);
}

struct VecEnvSpec
{
  std::array<float, kVecPoints> x{}, y{};
  std::array<double, 4> time{1., 1., 1., 1.}; // seconds, from their ports
  double release{0.3};
  VecLoop loop{VecLoop::Off};
  int from{1}, to{3};

  //! Around the diamond, a second per move, back to the centre on release:
  //! a new instance shows at once what the vector does
  [[nodiscard]] static VecEnvSpec centred() noexcept
  {
    VecEnvSpec s;
    s.x = {-0.6f, 0.f, 0.6f, 0.f, 0.f};
    s.y = {0.f, 0.6f, 0.f, -0.6f, 0.f};
    return s;
  }

  [[nodiscard]] static VecEnvSpec decode(const std::vector<float>& v, const VecEnvSpec& fallback) noexcept
  {
    if(v.size() != std::size_t(kVecEnvSize))
      return fallback;
    auto ok = [](float f) { return f >= 0.f && f <= 1.f; }; // also NaN
    VecEnvSpec s;
    for(int i = 0; i < kVecPoints; i++)
    {
      const float a = v[std::size_t(2 * i)], b = v[std::size_t(2 * i + 1)];
      s.x[std::size_t(i)] = ok(a) ? 2.f * a - 1.f : 0.f;
      s.y[std::size_t(i)] = ok(b) ? 2.f * b - 1.f : 0.f;
    }
    return s;
  }

  //! The loop: the span from..to, at least one move
  void setLoop(VecLoop mode, int loopFrom, int loopTo) noexcept
  {
    loop = saneEnum(mode, kVecLoopCount);
    from = std::clamp(loopFrom, 0, 3);
    to = std::clamp(loopTo, from + 1, 4);
  }
  //! From its menus
  void setLoop(VecLoop mode, LoopFrom loopFrom, LoopTo loopTo) noexcept
  {
    setLoop(mode, int(loopFrom), loopToPoint(loopTo));
  }

  //! Times and points in range, as their ports have them; NaN is the default
  void sanitize() noexcept
  {
    for(auto& t : time)
      t = sane(t, 0.001, 20., 1.);
    release = sane(release, 0.001, 20., 0.3);
    for(int i = 0; i < kVecPoints; i++)
    {
      x[std::size_t(i)] = sane(x[std::size_t(i)], -1.f, 1.f, 0.f);
      y[std::size_t(i)] = sane(y[std::size_t(i)], -1.f, 1.f, 0.f);
    }
    setLoop(loop, from, to);
  }

  //! The points' port
  [[nodiscard]] std::vector<float> points() const
  {
    std::vector<float> v(kVecEnvSize, 0.f);
    for(int i = 0; i < kVecPoints; i++)
    {
      v[std::size_t(2 * i)] = 0.5f * (x[std::size_t(i)] + 1.f);
      v[std::size_t(2 * i + 1)] = 0.5f * (y[std::size_t(i)] + 1.f);
    }
    return v;
  }

  //! The time of the move between neighbouring points a and b
  [[nodiscard]] double moveTime(int a, int b) const noexcept
  {
    const int i = std::clamp(std::max(a, b) - 1, 0, 3);
    return time[std::size_t(i)];
  }
};

//! One voice's travel. Scalar, at control rate.
struct VecEnvState
{
  int a{0}, b{1};
  int dir{1};
  double u{0.};
  bool released{false};
  bool done{false};
  float rx{0.f}, ry{0.f}; // where the release started
  float x{0.f}, y{0.f};

  void start(const VecEnvSpec& s) noexcept
  {
    a = 0;
    b = 1;
    dir = 1;
    u = 0.;
    released = false;
    done = false;
    x = s.x[0];
    y = s.y[0];
  }

  void release() noexcept
  {
    if(released)
      return;
    released = true;
    rx = x;
    ry = y;
    u = 0.;
  }

  void advance(const VecEnvSpec& s, double seconds) noexcept
  {
    if(released)
    {
      u = std::min(1., u + seconds / std::max(s.release, 1e-4));
      x = float(rx + u * (s.x[4] - rx));
      y = float(ry + u * (s.y[4] - ry));
      return;
    }
    if(done)
    {
      x = s.x[4];
      y = s.y[4];
      return;
    }
    double left = seconds;
    for(int guard = 0; guard < 16 && left > 0.; guard++)
    {
      const double T = std::max(1e-4, a == s.to && b == s.from ? s.moveTime(s.from, s.from + 1)
                                                               : s.moveTime(a, b));
      const double need = (1. - u) * T;
      if(need > left)
      {
        u += left / T;
        break;
      }
      left -= need;
      u = 0.;
      next(s);
      if(done)
        break;
    }
    const auto ai = std::size_t(std::clamp(a, 0, 4)), bi = std::size_t(std::clamp(b, 0, 4));
    x = float(s.x[ai] + u * (s.x[bi] - s.x[ai]));
    y = float(s.y[ai] + u * (s.y[bi] - s.y[ai]));
  }

private:
  //! Arrived at b: where next
  void next(const VecEnvSpec& s) noexcept
  {
    const int at = b;
    a = at;
    // The span can change under a voice (automation): one outside it heads
    // back into it
    switch(s.loop)
    {
      case VecLoop::Round:
        if(at >= s.to)
        {
          b = s.from;
          return;
        }
        break;
      case VecLoop::BackAndForth:
        if(at >= s.to)
          dir = -1;
        else if(at <= s.from)
          dir = 1;
        b = std::clamp(at + dir, 0, 4);
        return;
      default:
        break;
    }
    if(at >= 4)
    {
      b = 4;
      u = 1.;
      done = true;
      return;
    }
    b = at + 1;
  }
};
}
