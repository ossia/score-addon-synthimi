#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: the bank of sympathetic strings.

#include <Cordito/Dsp/Filters.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Sympathetic strings: lossy Karplus-Strong loops driven by the sum of the
// voices, never fed back into them. One-way coupling is unconditionally
// stable since each loop is passive (as Rings does it).
// ---------------------------------------------------------------------------

struct SympString
{
  DelayLine line;
  double d = 2.;
  OnePole loss;
  DcBlocker dc; // the loop's DC mode would last minutes
  double norm = 1.; //!< 1 - loop gain at the string's pitch
  //! The delay d as a whole part m and the 4-point Lagrange weights of its
  //! fraction, set when the string is tuned: it does not move after
  int m = 2;
  std::array<double, 4> h{0., 1., 0., 0.};
  double note = -1.;
};

struct SympBank
{
  //! Fraction of the played strings' sound that reaches the bank
  static constexpr double kSympDrive = 0.15;
  //! The most a sympathetic string amplifies a note it is in unison with.
  //! Unbounded, that is 1 / (1 - g): 300 for an 8 s decay at middle C.
  //! Capped, Level at its maximum adds a few dB to notes that have strings
  //! to ring with (at most 12 dB, with every string of Drone ringing; the
  //! tests check it), and no longer drives the output into the limiter.
  static constexpr double kSympMaxGain = 30.;

  std::array<SympString, kMaxSymp> strings{};
  int count = 0;
  double inLp = 0., inA = 0.3;
  SympTuning tuning = SympTuning::Off;
  int root = -1;
  double decay = -1.;
  std::array<int, 6> follow{-1, -1, -1, -1, -1, -1};
  int followNext = 0;
  bool followDirty = false;

  void clear() noexcept
  {
    for(auto& s : strings)
    {
      s.line.clear();
      s.loss.y = 0.;
      s.dc.x1 = s.dc.y1 = 0.;
    }
    inLp = 0.;
  }

  void noteOn(int note) noexcept
  {
    for(int n : follow)
      if(n == note)
        return;
    follow[std::size_t(followNext)] = note;
    followNext = (followNext + 1) % int(follow.size());
    followDirty = true;
  }

  static int intervals(SympTuning t, std::array<double, kMaxSymp>& out) noexcept
  {
    auto put = [&](std::initializer_list<double> l) {
      int i = 0;
      for(double x : l)
        if(i < kMaxSymp)
          out[std::size_t(i++)] = x;
      return i;
    };
    switch(t)
    {
      case SympTuning::Off:
        return 0;
      case SympTuning::Octaves:
        return put({0., 12., 24., 36.});
      case SympTuning::Fifths:
        return put({0., 7., 12., 19., 24., 31.});
      case SympTuning::Major:
        return put({0., 4., 7., 12., 16., 19., 24.});
      case SympTuning::Minor:
        return put({0., 3., 7., 12., 15., 19., 24.});
      case SympTuning::Drone:
        // Root, its low octave and the fifths around it, the two roots a
        // few cents apart so that they beat slowly
        return put({-12., -5., 0., 0.04, 12., 7.});
      case SympTuning::Chromatic:
        return put({0., 1., 2., 3., 4., 5., 6., 7., 8., 9., 10., 11.});
      case SympTuning::Follow:
        return 0;
    }
    return 0;
  }

  void configure(SympTuning t, int r, double dec, double fs) noexcept
  {
    inA = 1. - std::exp(-kTwoPi * 2725. / fs);
    const bool follows = t == SympTuning::Follow;
    if(t == tuning && r == root && dec == decay && !(follows && followDirty))
      return;
    tuning = t;
    root = r;
    decay = dec;
    followDirty = false;

    std::array<double, kMaxSymp> notes{};
    int n = 0;
    if(follows)
    {
      for(int x : follow)
        if(x >= 0)
          notes[std::size_t(n++)] = x;
    }
    else
    {
      n = intervals(t, notes);
      for(int i = 0; i < n; i++)
        notes[std::size_t(i)] += r;
    }

    count = 0;
    for(int i = 0; i < n; i++)
    {
      const double f = 440. * std::exp2((notes[std::size_t(i)] - 69.) / 12.);
      if(f < kMinF0 || f > fs / 10.)
        continue;
      auto& s = strings[std::size_t(count)];
      const double period = fs / f;
      const double pmax = std::min(0.9, 0.2 * period / (1. + 0.2 * period));
      // Sympathetic strings are damped more than the played ones, and
      // brighter: they shimmer.
      s.loss = designLoss(f, fs, dec, 0.35, std::min(kTopFreq * fs, std::max(4000., 4. * f)), pmax);
      s.loss.y = 0.;
      s.dc.setCutoff(1., fs);
      // A loop of per-period gain g driven at its own pitch builds up to
      // 1 / (1 - g) times its input. With Follow, which tunes the strings to
      // the notes played, that turned the bank into an amplifier: raising
      // the level drove the output far into the limiter. Capped here.
      s.norm = std::min(1., kSympMaxGain * (1. - std::exp(-kLn1000 / (dec * f))));
      const double tau = phaseDelay(s.loss.response(kTwoPi * f / fs), kTwoPi * f / fs);
      s.d = std::max(2., period - tau);
      s.m = int(s.d);
      {
        const double fr = s.d - s.m, fm1 = fr - 1., fm2 = fr - 2., fp1 = fr + 1.;
        s.h = {-fr * fm1 * fm2 / 6., fp1 * fm1 * fm2 / 2., -fp1 * fr * fm2 / 2., fp1 * fr * fm1 / 6.};
      }
      if(s.note != notes[std::size_t(i)])
        s.line.clear();
      s.note = notes[std::size_t(i)];
      count++;
    }
  }

  void tick(double in, double level, double& l, double& r) noexcept
  {
    l = r = 0.;
    if(count == 0)
      return;
    inLp += inA * (in - inLp);
    // Level applies once, to what the strings give back: applied to their
    // drive as well, it made the sympathetic sound grow as its square
    for(int i = 0; i < count; i++)
    {
      auto& s = strings[std::size_t(i)];
      const auto& ln = s.line;
      const double x = s.h[0] * ln.at(s.m - 1) + s.h[1] * ln.at(s.m) + s.h[2] * ln.at(s.m + 1)
                       + s.h[3] * ln.at(s.m + 2);
      double y = s.dc.tick(s.loss.tick(x));
      y = std::clamp(y, -4., 4.);
      s.line.write(y + inLp * kSympDrive * s.norm);
      // Alternately to either side
      const double side = double(i & 1);
      l += (1. - side) * y;
      r += side * y;
    }
    const double g = level / std::sqrt(double(count));
    l *= g;
    r *= g;
  }
};
}
