#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Cordito: the shared body resonator.

#include <Cordito/Dsp/Filters.hpp>

namespace crd
{
// ---------------------------------------------------------------------------
// Bodies. Mode frequencies, Qs and levels are hand-voiced from published
// measurements of each instrument family (guitar: Christensen & Vistisen,
// JASA 1980; violin: Jansson, "Acoustics for violin and guitar makers";
// cello, harp, gourd resonators, piano soundboard: Fletcher & Rossing, "The
// Physics of Musical Instruments", 1998). They are characters, not fits.
// ---------------------------------------------------------------------------

struct BodyMode
{
  double f, q, gain;
};

struct BodyTable
{
  std::array<BodyMode, kMaxBodyModes> modes{};
  int count = 0;
};

[[nodiscard]] inline BodyTable bodyTable(Body b) noexcept
{
  auto make = [](std::initializer_list<BodyMode> l) {
    BodyTable t;
    for(auto& m : l)
      if(t.count < kMaxBodyModes)
        t.modes[std::size_t(t.count++)] = m;
    return t;
  };
  switch(b)
  {
    case Body::None:
      return {};
    case Body::Guitar:
      return make(
          {{98., 22., 1.0},
           {204., 30., 0.8},
           {258., 30., 0.5},
           {398., 35., 0.45},
           {550., 40., 0.35},
           {780., 40., 0.3},
           {1130., 42., 0.25},
           {1600., 40., 0.2},
           {2300., 35., 0.15},
           {3100., 30., 0.12}});
    case Body::Violin:
      return make(
          {{275., 15., 0.8},
           {415., 20., 0.6},
           {460., 25., 1.0},
           {540., 25., 0.9},
           {700., 30., 0.5},
           {1000., 30., 0.4},
           {2400., 6., 0.6},
           {3000., 8., 0.5},
           {4500., 10., 0.3}});
    case Body::Cello:
      return make(
          {{100., 15., 0.8},
           {175., 20., 0.9},
           {220., 22., 1.0},
           {290., 25., 0.7},
           {420., 30., 0.5},
           {600., 30., 0.4},
           {1200., 20., 0.3},
           {2000., 8., 0.3}});
    case Body::Harp:
      return make(
          {{85., 12., 0.6},
           {160., 15., 0.8},
           {250., 18., 0.7},
           {400., 22., 0.5},
           {650., 25., 0.4},
           {1100., 25., 0.3},
           {1800., 20., 0.2}});
    case Body::Gourd:
      return make(
          {{120., 10., 0.9},
           {330., 18., 0.8},
           {600., 25., 0.6},
           {1100., 30., 0.5},
           {1800., 30., 0.4},
           {2600., 25., 0.4},
           {4000., 20., 0.3}});
    case Body::Soundboard:
      return make(
          {{60., 20., 0.5},
           {110., 25., 0.6},
           {180., 25., 0.6},
           {270., 30., 0.5},
           {400., 30., 0.5},
           {580., 30., 0.4},
           {850., 30., 0.35},
           {1300., 30., 0.3},
           {2000., 25., 0.25},
           {3000., 20., 0.2}});
    case Body::Box:
      return make(
          {{140., 25., 0.9},
           {310., 30., 0.7},
           {520., 35., 0.6},
           {830., 35., 0.5},
           {1350., 35., 0.4},
           {2100., 30., 0.3}});
    case Body::Metal:
      // Inharmonic, high-Q: a plate rather than a wooden box
      return make(
          {{220., 150., 0.6},
           {587., 200., 0.5},
           {1093., 220., 0.45},
           {1732., 250., 0.4},
           {2510., 260., 0.35},
           {3420., 280., 0.3},
           {4470., 300., 0.25}});
  }
  return {};
}

//! The shared stereo body, after the voices: radiation, not coupling
struct BodyBank
{
  std::array<Biquad, kMaxBodyModes> bp{};
  std::array<double, kMaxBodyModes> gl{}, gr{};
  int count = 0;
  Body type = Body::None;
  double size = -1.;
  double norm = 1.;

  void configure(Body b, double sz, double fs) noexcept
  {
    if(b == type && sz == size)
      return;
    type = b;
    size = sz;
    const auto t = bodyTable(b);
    count = 0;
    double sum = 0.;
    for(int i = 0; i < t.count; i++)
    {
      const auto& m = t.modes[std::size_t(i)];
      const double f = m.f / sz;
      if(f >= kTopFreq * fs)
        continue;
      auto& q = bp[std::size_t(count)];
      const double s1 = q.s1, s2 = q.s2;
      q = Biquad::bandpass(f, m.q, fs);
      q.s1 = s1;
      q.s2 = s2;
      // Alternate the modes between the sides, as two pickups would
      const double pan = (i % 2 == 0) ? 0.35 : -0.35;
      gl[std::size_t(count)] = m.gain * (1. + pan);
      gr[std::size_t(count)] = m.gain * (1. - pan);
      sum += m.gain;
      count++;
    }
    norm = sum > 0. ? 2.5 / std::sqrt(sum) : 0.;
  }

  void reset() noexcept
  {
    for(auto& q : bp)
      q.reset();
  }

  void tick(double in, double& l, double& r) noexcept
  {
    double ol = 0., or_ = 0.;
    for(int i = 0; i < count; i++)
    {
      const double y = bp[std::size_t(i)].tick(in);
      ol += y * gl[std::size_t(i)];
      or_ += y * gr[std::size_t(i)];
    }
    l = ol * norm;
    r = or_ * norm;
  }
};
}
