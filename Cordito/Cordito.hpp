#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <Cordito/CorditoDsp.hpp>
#include <avnd/common/enum_reflection.hpp>
#include <halp/audio.hpp>
#include <halp/controls.enums.hpp>
#include <halp/controls.hpp>
#include <halp/layout.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>

#include <array>

namespace crd
{
#if AVND_ENUM_REFLECTION_SUPPORTED
static_assert(avnd::enum_names<Engine>().size() == kEngineCount);
static_assert(avnd::enum_names<Exciter>().size() == kExciterCount);
static_assert(avnd::enum_names<SympTuning>().size() == kSympTuningCount);
static_assert(avnd::enum_names<Body>().size() == kBodyCount);
static_assert(avnd::enum_names<PlayMode>().size() == kPlayModeCount);
static_assert(avnd::enum_names<Pressure>().size() == kPressureCount);
#endif

inline constexpr halp::look small_knob{
    .size = halp::control_size::compact, .widget = halp::control_widget::knob};
consteval halp::look small_knob_as(std::string_view name)
{
  auto l = halp::label_as(name);
  l.size = halp::control_size::compact;
  l.widget = halp::control_widget::knob;
  return l;
}

class Cordito
{
public:
  halp_meta(name, "Cordito")
  halp_meta(category, "Synths")
  halp_meta(c_name, "cordito")
  halp_meta(author, "Jean-Michaël Celerier")
  halp_meta(
      description,
      "Physical-modelling string synthesizer: plucked, struck, bowed and buzzing "
      "strings, with sympathetic strings and a resonant body")
  halp_meta(uuid, "3c8e5f1a-9d27-4b6e-a0c4-7f2d18e95b36")

  // The .scp preset format stores values by port index: this order is
  // frozen. New ports go at the end.
  struct ins
  {
    halp::midi_bus<"Input"> midi;

    // Excite
    halp::combobox_t<"Exciter", Exciter> exciter;
    halp::knob_f32<"Hardness", halp::range{0., 1., 0.6}> hardness;
    halp::knob_f32<"Position", halp::range{0.02, 0.5, 0.13}> position;
    halp::knob_f32<"Force", halp::range{0., 1., 0.5}> force;
    halp::knob_f32<"Noise", halp::range{0., 1., 0.1}> noise;

    // String
    halp::combobox_t<"Engine", Engine> engine;
    halp::knob_f32<"Tune", halp::range{-24., 24., 0.}> tune;
    halp::knob_f32<"Fine", halp::range{-100., 100., 0.}> fine;
    halp::time_chooser<"Decay", halp::range{0.05, 30., 4.}> decay;
    halp::knob_f32<"Brightness", halp::range{0., 1., 0.6}> brightness;
    halp::knob_f32<"Stiffness", halp::range{0., 1., 0.1}> stiffness;
    halp::knob_f32<"Tension", halp::range{0., 1., 0.2}> tension;
    halp::knob_f32<"Detune", halp::range{0., 25., 3.}> detune;
    halp::knob_f32<"Material", halp::range{0., 1., 0.5}> material;

    // Per engine
    halp::spinbox_i32<"Modes", halp::range{8, kMaxModes, 24}> modes;
    halp::knob_f32<"Buzz", halp::range{0., 1., 0.5}> buzz;
    halp::knob_f32<"Bow Speed", halp::range{0., 1., 0.5}> bow_speed;
    halp::time_chooser<"Bow Attack", halp::range{0.005, 2., 0.08}> bow_attack;
    halp::time_chooser<"Bow Lift", halp::range{0.005, 4., 0.1}> bow_lift;

    // Damper
    halp::time_chooser<"Release", halp::range{0.01, 10., 0.3}> release;
    halp::knob_f32<"Mute", halp::range{0., 1., 0.}> mute;
    halp::knob_f32<"Damper Noise", halp::range{0., 1., 0.1}> damper_noise;

    // Sympathetic strings
    halp::knob_f32<"Symp. Level", halp::range{0., 1., 0.}> symp_level;
    halp::combobox_t<"Symp. Tuning", SympTuning> symp_tuning;
    halp::spinbox_i32<"Symp. Root", halp::range{24, 60, 36}> symp_root;
    halp::time_chooser<"Symp. Decay", halp::range{0.5, 30., 8.}> symp_decay;

    // Body
    halp::combobox_t<"Body", Body> body;
    halp::knob_f32<"Body Size", halp::range{0.5, 2., 1.}> body_size;
    halp::knob_f32<"Body Mix", halp::range{0., 1., 0.5}> body_mix;
    halp::knob_f32<"Coupling", halp::range{0., 1., 0.}> coupling;

    // Play
    halp::combobox_t<"Play Mode", PlayMode> play_mode;
    halp::time_chooser<"Glide", halp::range{0., 2., 0.}> glide;
    halp::spinbox_i32<"Bend Range", halp::range{0, 48, 2}> bend_range;
    halp::knob_f32<"Vel->Force", halp::range{0., 1., 0.7}> vel_force;
    halp::combobox_t<"Pressure", Pressure> pressure;

    // Out
    halp::knob_f32<"Spread", halp::range{0., 1., 0.5}> spread;
    halp::knob_f32<"Level", halp::range{0., 2., 1.}> level;
    halp::spinbox_i32<"Voices", halp::range{1, kMaxVoices, kMaxVoices}> voices;

    // Appended: see the frozen order above
    halp::knob_f32<"Coupling Detune", halp::range{0., 1., 1.}> coupling_detune;
  } inputs;

  struct
  {
    halp::fixed_audio_bus<"Output", double, 2> audio;
  } outputs;

  Synth synth;

  void prepare(halp::setup s) { synth.prepare(s.rate); }

  Params params() const noexcept
  {
    Params p;
    const auto& in = inputs;
    p.engine = in.engine.value;
    p.exciter = in.exciter.value;
    p.hardness = in.hardness.value;
    p.position = in.position.value;
    p.force = in.force.value;
    p.noise = in.noise.value;
    p.tune = in.tune.value;
    p.fine = in.fine.value;
    p.decay = in.decay.value;
    p.brightness = in.brightness.value;
    p.stiffness = in.stiffness.value;
    p.tension = in.tension.value;
    p.detune = in.detune.value;
    p.material = in.material.value;
    p.modes = in.modes.value;
    p.buzz = in.buzz.value;
    p.bowSpeed = in.bow_speed.value;
    p.bowAttack = in.bow_attack.value;
    p.bowLift = in.bow_lift.value;
    p.release = in.release.value;
    p.mute = in.mute.value;
    p.damperNoise = in.damper_noise.value;
    p.sympLevel = in.symp_level.value;
    p.sympTuning = in.symp_tuning.value;
    p.sympRoot = in.symp_root.value;
    p.sympDecay = in.symp_decay.value;
    p.body = in.body.value;
    p.bodySize = in.body_size.value;
    p.bodyMix = in.body_mix.value;
    p.coupling = in.coupling.value;
    p.playMode = in.play_mode.value;
    p.glide = in.glide.value;
    p.bendRange = in.bend_range.value;
    p.velForce = in.vel_force.value;
    p.pressure = in.pressure.value;
    p.spread = in.spread.value;
    p.level = in.level.value;
    p.voices = in.voices.value;
    p.couplingDetune = in.coupling_detune.value;
    p.sanitize();
    return p;
  }

  using tick = halp::tick;
  void operator()(halp::tick t)
  {
    const int frames = t.frames;
    double* l = outputs.audio.samples[0];
    double* r = outputs.audio.samples[1];
    if(frames <= 0 || !l || !r)
      return;

    ScopedFlushDenormals ftz;
    const Params p = params();

    // Sample-accurate: render up to each message, then apply it
    int pos = 0;
    for(const auto& msg : inputs.midi)
    {
      const int n = int(msg.bytes.size());
      if(n < 1)
        continue;
      const int at = std::clamp(int(msg.timestamp), pos, frames);
      if(at > pos)
      {
        synth.render(p, l + pos, r + pos, at - pos);
        pos = at;
      }
      unsigned char bytes[3]{};
      for(int i = 0; i < std::min(n, 3); i++)
        bytes[i] = msg.bytes[std::size_t(i)];
      synth.midi(bytes, std::min(n, 3), p);
    }
    if(pos < frames)
      synth.render(p, l + pos, r + pos, frames - pos);
  }

  //! The sympathetic controls do nothing while the bank is off
  using sympathetic_on = halp::enabled_when<
      &ins::symp_tuning, SympTuning::Octaves, SympTuning::Fifths, SympTuning::Major,
      SympTuning::Minor, SympTuning::Drone, SympTuning::Chromatic, SympTuning::Follow>;
  //! Nor the body controls without a body
  using body_on = halp::enabled_when<
      &ins::body, Body::Guitar, Body::Violin, Body::Cello, Body::Harp, Body::Gourd,
      Body::Soundboard, Body::Box, Body::Metal>;

  // As Deuterium: what defines the instrument always in view, the rest in
  // tabs of one or two rows each.
  struct ui
  {
    halp_meta(name, "Main")
    halp_meta(layout, halp::layouts::vbox)
    halp_meta(background, halp::colors::background_mid)

    // Named, as is every struct around engine_pages: a class contained in an
    // unnamed one cannot have static data members.
    struct header_row
    {
      halp_meta(layout, halp::layouts::hbox)
      halp_meta(background, halp::colors::background_dark)

      halp::item<&ins::engine> engine;
      halp::item<&ins::exciter> exciter;

      // The selected engine's own controls, beside it. Named: holds a static
      // model.
      struct engine_pages
      {
        halp_meta(layout, halp::layouts::tabs)
        halp_flag(hide_tabs);
        static constexpr auto model = &ins::engine;

        struct
        {
          halp_meta(name, "Waveguide")
          halp_meta(layout, halp::layouts::hbox)
          static constexpr auto when() { return std::array{Engine::Pluck, Engine::Buzz}; }
          struct
          {
            halp_meta(layout, halp::layouts::container)
            static constexpr auto condition()
            {
              return halp::enabled_when<&ins::engine, Engine::Buzz>{};
            }
            halp::item<&ins::buzz, small_knob> buzz;
          } buzz;
        } waveguide;

        struct
        {
          halp_meta(name, "Struck")
          halp_meta(layout, halp::layouts::hbox)
          static constexpr auto when() { return std::array{Engine::Struck}; }
          halp::item<&ins::modes> modes;
        } struck;

        struct
        {
          halp_meta(name, "Bow")
          halp_meta(layout, halp::layouts::hbox)
          static constexpr auto when() { return std::array{Engine::Bow}; }
          halp::item<&ins::bow_speed, small_knob> bow_speed;
          halp::item<&ins::bow_attack, small_knob> bow_attack;
          halp::item<&ins::bow_lift, small_knob> bow_lift;
        } bow;
      } pages;

      // Level at the right edge, whichever engine page is shown
      struct
      {
        halp_meta(layout, halp::layouts::spacing)
        static constexpr bool stretch() { return true; }
      } fill;
      halp::item<&ins::level, small_knob> level;
    } header;

    struct
    {
      halp_meta(layout, halp::layouts::tabs)
      halp_meta(background, halp::colors::background_dark)

      // How the string is set going, then the string itself: one tab of
      // three rows, rather than one of a row and one of two
      struct
      {
        halp_meta(name, "String")
        halp_meta(layout, halp::layouts::grid)
        halp_meta(columns, 5)
        halp::item<&ins::hardness, small_knob> hardness;
        halp::item<&ins::position, small_knob> position;
        halp::item<&ins::force, small_knob> force;
        halp::item<&ins::vel_force, small_knob> vel_force;
        halp::item<&ins::noise, small_knob> noise;

        halp::item<&ins::tune, small_knob> tune;
        halp::item<&ins::fine, small_knob> fine;
        halp::item<&ins::decay, small_knob> decay;
        halp::item<&ins::brightness, small_knob> brightness;
        halp::item<&ins::material, small_knob> material;

        halp::item<&ins::stiffness, small_knob> stiffness;
        halp::item<&ins::tension, small_knob> tension;
        halp::item<&ins::detune, small_knob> detune;
      } string;

      // Each conditional control is a cell of its own, so the grid stays
      // aligned
      struct
      {
        halp_meta(name, "Resonance")
        halp_meta(layout, halp::layouts::grid)
        halp_meta(columns, 5)
        halp::item<&ins::symp_tuning, halp::label_as("Sympathetic")> tuning;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return sympathetic_on{}; }
          halp::item<&ins::symp_root, halp::label_as("Root")> root;
        } root;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return sympathetic_on{}; }
          halp::item<&ins::symp_level, small_knob_as("Level")> level;
        } level;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return sympathetic_on{}; }
          halp::item<&ins::symp_decay, small_knob_as("Decay")> decay;
        } decay;
        halp::spacing gap{};

        halp::item<&ins::body> body;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return body_on{}; }
          halp::item<&ins::body_size, small_knob_as("Size")> size;
        } size;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return body_on{}; }
          halp::item<&ins::body_mix, small_knob_as("Mix")> mix;
        } mix;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return body_on{}; }
          halp::item<&ins::coupling, small_knob> coupling;
        } coupling;
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition() { return body_on{}; }
          halp::item<&ins::coupling_detune, small_knob_as("Detune")> coupling_detune;
        } coupling_detune;
      } resonance;

      struct
      {
        halp_meta(name, "Play")
        halp_meta(layout, halp::layouts::grid)
        halp_meta(columns, 5)
        halp::item<&ins::play_mode, halp::label_as("Mode")> mode;
        halp::item<&ins::pressure> pressure;
        // Glide slides between the notes of one voice: Mono and Legato
        struct
        {
          halp_meta(layout, halp::layouts::container)
          static constexpr auto condition()
          {
            return halp::enabled_when<&ins::play_mode, PlayMode::Mono, PlayMode::Legato>{};
          }
          halp::item<&ins::glide, small_knob> glide;
        } glide;
        halp::item<&ins::bend_range, halp::label_as("Bend")> bend;
        halp::item<&ins::voices> voices;

        halp::item<&ins::release, small_knob> release;
        halp::item<&ins::mute, small_knob> mute;
        // Not "Noise": the exciter has one
        halp::item<&ins::damper_noise, small_knob_as("Thump")> damper_noise;
        halp::item<&ins::spread, small_knob> spread;
      } play;
    } tabs;
  };
};
}
