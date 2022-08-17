#pragma once

#include <Synthimi/SynthimiModel.hpp>
#include <halp/layout.hpp>

namespace Synthimi
{
struct Synthimi::ui
{
  using enum halp::colors;
  using enum halp::layouts;

  halp_meta(name, "Main")
  halp_meta(layout, split)
  halp_meta(background, "synthimi/bg.svg")

  struct
  {
    halp_meta(name, "Oscillators")
    halp_meta(layout, halp::layouts::group)
    // halp_meta(background, halp::colors::light)
    struct
    {
      halp_meta(layout, halp::layouts::tabs)

      struct
      {
        halp_meta(layout, halp::layouts::vbox)
        halp_meta(name, "Osc. 1")

        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::item<&ins::osc0_amp> a;
          halp::item<&ins::osc0_pitch> b;
          halp::item<&ins::osc0_oct> c;
        } hb;
        halp::item<&ins::osc0_waveform> d;
      } osc0;

      struct
      {
        halp_meta(layout, halp::layouts::vbox)
        halp_meta(name, "Osc. 2")
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::item<&ins::osc1_amp> a;
          halp::item<&ins::osc1_pitch> b;
          halp::item<&ins::osc1_oct> c;
        } hb;
        halp::item<&ins::osc1_waveform> d;
      } osc1;

      struct
      {
        halp_meta(layout, halp::layouts::vbox)
        halp_meta(name, "Osc. 3")
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::item<&ins::osc2_amp> a;
          halp::item<&ins::osc2_pitch> b;
          halp::item<&ins::osc2_oct> c;
        } hb;
        halp::item<&ins::osc2_waveform> d;
      } osc2;
      struct
      {
        halp_meta(layout, halp::layouts::vbox)
        halp_meta(name, "Osc. 4")
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::item<&ins::osc3_amp> a;
          halp::item<&ins::osc3_pitch> b;
          halp::item<&ins::osc3_oct> c;
        } hb;
        halp::item<&ins::osc3_waveform> d;
      } osc3;
    } a_hbox;
  } b_group;

  struct
  {
    halp_meta(layout, halp::layouts::grid)
    halp_meta(columns, 4)
    halp::item<&ins::amp_attack> a;
    halp::item<&ins::amp_decay> b;
    halp::item<&ins::amp_sustain> c;
    halp::item<&ins::amp_release> d;

    halp::item<&ins::filt_attack> fa;
    halp::item<&ins::filt_decay> fb;
    halp::item<&ins::filt_sustain> fc;
    halp::item<&ins::filt_release> fd;

    halp::item<&ins::filt_env> cy;
    halp::item<&ins::filt_type> cx;
    halp::item<&ins::filt_cutoff> ca;
    halp::item<&ins::filt_res> cb;
  } env;

  struct
  {
    halp_meta(layout, halp::layouts::hbox)
    struct
    {
      halp_meta(layout, halp::layouts::vbox)
      halp::item<&ins::drive> c;
      halp::item<&ins::portamento> a;
    } v1;
    struct
    {
      halp_meta(layout, halp::layouts::vbox)
      halp::item<&ins::matrix> e;
      halp::item<&ins::poly_mode> b;
      halp::item<&ins::unison> d;
    } v2;
  } common;
};
};
