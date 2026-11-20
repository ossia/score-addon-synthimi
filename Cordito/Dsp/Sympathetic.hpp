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
  //! What is heard of the string: it fades out before it is retuned while
  //! it rings, as changing its length under the sound would click
  double gain = 1.;
  double next = -1.; //!< The pitch it takes once faded out
  bool retune = false;
  //! The delay read now, which is d unless the bank is bent, and the one it
  //! reaches in `glide` samples, `step` at a time
  double cur = 2., end = 2., step = 0.;
  int glide = 0;
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

  //! How long a ringing string takes to fade out before it is retuned
  static constexpr double kRetuneFade = 0.03;
  //! The mean square under which a string is retuned at once: nothing is
  //! heard of it (-100 dB)
  static constexpr double kSilent = 1e-10;
  //! The keys held down, the last one pressed first: what the "(follow)"
  //! tunings are built on
  struct Key
  {
    int note = -1;
    int channel = 0;
  };
  static constexpr int kMaxKeys = 8;
  std::array<Key, kMaxKeys> keys{};
  int keyCount = 0;
  bool keysDirty = false;
  double rate = 48000.;
  double fadeStep = 1. / (kRetuneFade * 48000.);
  //! The strings the output is normalized for. The "(follow)" tunings keep
  //! it at the size of the tuning, however many strings the keys held take:
  //! following the number of strings in use would move the level of those
  //! that ring at every note
  int norm = 0;
  //! How far the pitch bend moves every string, in semitones: where it is,
  //! on its way to where the wheel is. A wheel sends steps; smoothed over a
  //! few milliseconds they are a slide, not a staircase.
  double bend = 0.;
  bool bending = false;
  static constexpr double kBendTime = 0.005;

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

  void keyOn(int note, int channel) noexcept
  {
    int n = 0;
    for(int i = 0; i < keyCount; i++)
      if(const Key k = keys[std::size_t(i)]; k.note != note || k.channel != channel)
        keys[std::size_t(n++)] = k;
    // The oldest key gives its place when there are more than the bank
    // could ever give a string to
    n = std::min(n, kMaxKeys - 1);
    for(int i = n; i > 0; i--)
      keys[std::size_t(i)] = keys[std::size_t(i - 1)];
    keys[0] = {note, channel};
    keyCount = n + 1;
    keysDirty = true;
  }

  // Releasing a key retunes nothing: the strings ring on as they are, and
  // the next key pressed is what takes strings from the released ones
  void keyOff(int note, int channel) noexcept
  {
    int n = 0;
    for(int i = 0; i < keyCount; i++)
      if(const Key k = keys[std::size_t(i)]; k.note != note || k.channel != channel)
        keys[std::size_t(n++)] = k;
    keyCount = n;
  }

  void keysOff(int channel) noexcept
  {
    int n = 0;
    for(int i = 0; i < keyCount; i++)
      if(const Key k = keys[std::size_t(i)]; k.channel != channel)
        keys[std::size_t(n++)] = k;
    keyCount = n;
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
      // Built by allocate(), from the tuning they follow
      case SympTuning::OctavesFollow:
      case SympTuning::FifthsFollow:
      case SympTuning::MajorFollow:
      case SympTuning::MinorFollow:
      case SympTuning::DroneFollow:
        return 0;
    }
    return 0;
  }

  bool playable(double note) const noexcept
  {
    const double f = 440. * std::exp2((note - 69.) / 12.);
    return f >= kMinF0 && f <= rate / 10.;
  }

  //! Tunes a string to a note, for the decay and the rate of the bank
  void tune(SympString& s, double note) noexcept
  {
    const double fs = rate, dec = decay;
    const double f = 440. * std::exp2((note - 69.) / 12.);
    const double period = fs / f;
    const double pmax = std::min(0.9, 0.2 * period / (1. + 0.2 * period));
    // Sympathetic strings are damped more than the played ones, and
    // brighter: they shimmer.
    s.loss = designLoss(f, fs, dec, 0.35, std::min(kTopFreq * fs, std::max(4000., 4. * f)), pmax);
    s.loss.y = 0.;
    s.dc.setCutoff(1., fs);
    // A loop of per-period gain g driven at its own pitch builds up to
    // 1 / (1 - g) times its input. With strings tuned to the notes played,
    // that makes the bank an amplifier, driving the output far into the
    // limiter as the level is raised: hence the cap.
    s.norm = std::min(1., kSympMaxGain * (1. - std::exp(-kLn1000 / (dec * f))));
    const double tau = phaseDelay(s.loss.response(kTwoPi * f / fs), kTwoPi * f / fs);
    s.d = std::max(2., period - tau);
    s.note = note;
    s.cur = bend == 0. ? s.d : bent(s);
    s.glide = 0;
    taps(s);
  }

  //! The whole part and the Lagrange weights of the delay a string reads
  static void taps(SympString& s) noexcept
  {
    s.m = int(s.cur);
    const double fr = s.cur - s.m, fm1 = fr - 1., fm2 = fr - 2., fp1 = fr + 1.;
    s.h = {-fr * fm1 * fm2 / 6., fp1 * fm1 * fm2 / 2., -fp1 * fr * fm2 / 2., fp1 * fr * fm1 / 6.};
  }

  //! The delay of a string at the bank's bend. Its loss filter stays the one
  //! of its own note: only its delay at the bent pitch is taken off the loop.
  double bent(const SympString& s) const noexcept
  {
    const double f
        = std::min(440. * std::exp2((s.note + bend - 69.) / 12.), 0.25 * rate);
    const double w = kTwoPi * f / rate;
    const double tau = phaseDelay(s.loss.response(w), w);
    return std::clamp(rate / f - tau, 2., double(s.line.mask - 3));
  }

  //! Called every kCtrl samples with where the wheel is. The strings are not
  //! retuned: their delays slide, sample by sample, to the length of the
  //! bent pitch, read through the same interpolation as when they are still.
  //! What rings in them is bent with them, as a string bent on its neck.
  void bendTo(double semitones) noexcept
  {
    if(semitones == bend && !bending)
      return;
    bend += (1. - std::exp(-kCtrl / (kBendTime * rate))) * (semitones - bend);
    if(std::abs(semitones - bend) < 1e-5)
      bend = semitones;
    bending = bend != semitones;
    for(int i = 0; i < count; i++)
    {
      auto& s = strings[std::size_t(i)];
      // Unbent, the very delay the string was tuned with
      s.end = bend == 0. ? s.d : bent(s);
      s.step = (s.end - s.cur) / kCtrl;
      s.glide = kCtrl;
    }
  }

  //! Mean square of what travels in a string's loop
  static double loudness(const SympString& s) noexcept
  {
    const int n = s.m + 2;
    double e = 0.;
    for(int k = 1; k <= n; k++)
    {
      const double v = s.line.at(k);
      e += v * v;
    }
    return e / n;
  }

  //! Gives strings to the tuning `base` built on each key held.
  //! - Every key takes its nearest intervals first, the last key pressed
  //!   before the others: when the keys ask for more notes than there are
  //!   strings, each still has its unison, then its fifth or third, and the
  //!   far octaves of the oldest keys are what is left out.
  //! - A string already tuned to a wanted note is left alone, so that notes
  //!   common to two chords ring through the change.
  //! - A wanted note that has no string takes the quietest of the strings
  //!   tuned to notes not wanted any more; the others keep ringing where they
  //!   are until a later key needs them, as the strings of an instrument go
  //!   on ringing after the hand has left.
  void allocate(SympTuning base) noexcept
  {
    std::array<double, kMaxSymp> iv{};
    const int n = intervals(base, iv);
    for(int i = 1; i < n; i++)
    {
      const double x = iv[std::size_t(i)];
      int j = i;
      for(; j > 0 && std::abs(iv[std::size_t(j - 1)]) > std::abs(x); j--)
        iv[std::size_t(j)] = iv[std::size_t(j - 1)];
      iv[std::size_t(j)] = x;
    }

    std::array<double, kMaxSymp> want{};
    int w = 0;
    for(int r = 0; r < n; r++)
      for(int k = 0; k < keyCount && w < kMaxSymp; k++)
      {
        const double note = keys[std::size_t(k)].note + iv[std::size_t(r)];
        if(!playable(note))
          continue;
        bool known = false;
        for(int j = 0; j < w; j++)
          known = known || want[std::size_t(j)] == note;
        if(!known)
          want[std::size_t(w++)] = note;
      }

    std::array<bool, kMaxSymp> kept{}, served{};
    // The strings that have a wanted note, or that are on their way to one
    for(int j = 0; j < w; j++)
      for(int i = 0; i < count && !served[std::size_t(j)]; i++)
        if(const auto& s = strings[std::size_t(i)];
           !kept[std::size_t(i)] && (s.retune ? s.next : s.note) == want[std::size_t(j)])
          kept[std::size_t(i)] = served[std::size_t(j)] = true;
    // Then those fading out of a note that is wanted again: they come back
    for(int j = 0; j < w; j++)
      for(int i = 0; i < count && !served[std::size_t(j)]; i++)
        if(auto& s = strings[std::size_t(i)];
           !kept[std::size_t(i)] && s.retune && s.note == want[std::size_t(j)])
        {
          s.retune = false;
          kept[std::size_t(i)] = served[std::size_t(j)] = true;
        }

    for(int j = 0; j < w; j++)
    {
      if(served[std::size_t(j)])
        continue;
      int victim = -1;
      double quietest = 0.;
      for(int i = 0; i < kMaxSymp; i++)
      {
        if(kept[std::size_t(i)])
          continue;
        const auto& s = strings[std::size_t(i)];
        // A string not in use yet, then one already fading out, then the
        // quietest
        const double e = i >= count ? -2. : s.retune ? -1. : loudness(s);
        if(victim < 0 || e < quietest)
        {
          victim = i;
          quietest = e;
        }
      }
      if(victim < 0)
        break;
      auto& s = strings[std::size_t(victim)];
      kept[std::size_t(victim)] = true;
      if(victim >= count || (!s.retune && quietest < kSilent))
      {
        s.line.clear();
        tune(s, want[std::size_t(j)]);
        s.gain = 1.;
        s.retune = false;
        count = std::max(count, victim + 1);
      }
      else
      {
        s.next = want[std::size_t(j)];
        s.retune = true;
      }
    }
  }

  void configureFollowing(SympTuning t, SympTuning base, double dec) noexcept
  {
    const bool entered = t != tuning;
    if(!entered && dec == decay && !keysDirty)
      return;
    // The strings of the tuning there was before ring on, and are taken one
    // by one as the keys need them
    if(dec != decay)
    {
      decay = dec;
      for(int i = 0; i < count; i++)
        tune(strings[std::size_t(i)], strings[std::size_t(i)].note);
    }
    tuning = t;
    std::array<double, kMaxSymp> iv{};
    norm = intervals(base, iv);
    if((entered || keysDirty) && keyCount > 0)
      allocate(base);
    keysDirty = false;
  }

  void configure(SympTuning t, int r, double dec, double fs) noexcept
  {
    inA = 1. - std::exp(-kTwoPi * 2725. / fs);
    rate = fs;
    fadeStep = 1. / (kRetuneFade * fs);
    // Symp. Root has no say in the tunings built on the keys
    if(const SympTuning base = followedTuning(t); base != SympTuning::Off)
      return configureFollowing(t, base, dec);
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
      const bool same = s.note == notes[std::size_t(i)];
      tune(s, notes[std::size_t(i)]);
      s.gain = 1.;
      s.retune = false;
      if(!same)
        s.line.clear();
      count++;
    }
    norm = count;
  }

  //! What is heard of a string that fades out to be retuned, or back in
  double faded(SympString& s, double y) noexcept
  {
    if(s.retune)
    {
      s.gain -= fadeStep;
      if(s.gain <= 0.)
      {
        s.line.clear();
        tune(s, s.next);
        s.gain = 1.;
        s.retune = false;
        return 0.;
      }
    }
    else
      s.gain = std::min(1., s.gain + fadeStep);
    return y * s.gain;
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
      if(s.retune || s.gain < 1.) [[unlikely]]
        y = faded(s, y);
      if(s.glide > 0) [[unlikely]]
      {
        s.cur = --s.glide == 0 ? s.end : s.cur + s.step;
        taps(s);
      }
      // Alternately to either side
      const double side = double(i & 1);
      l += (1. - side) * y;
      r += side * y;
    }
    const double g = level / std::sqrt(double(norm));
    l *= g;
    r *= g;
  }
};
}
