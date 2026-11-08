#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <ViceVersa/ViceVersaDsp.hpp>
#include <ViceVersa/VectorWidget.hpp>
#include <avnd/common/enum_reflection.hpp>
#include <avnd/introspection/input.hpp>
#include <halp/audio.hpp>
#include <halp/controls.enums.hpp>
#include <halp/controls.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>
#include <halp/mappers.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <string_view>

namespace viceversa
{
#if AVND_ENUM_REFLECTION_SUPPORTED
static_assert(avnd::enum_names<BankId>().size() == kBankCount);
static_assert(avnd::enum_names<MixLaw>().size() == kMixLawCount);
static_assert(avnd::enum_names<LfoShape>().size() == kLfoShapeCount);
static_assert(avnd::enum_names<ModTarget>().size() == kModTargetCount);
static_assert(avnd::enum_names<PlayMode>().size() == kPlayModeCount);
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
consteval halp::look dropdown_as(std::string_view name)
{
  auto l = halp::label_as(name);
  l.widget = halp::control_widget::combo;
  return l;
}

//! What the menus show; presets store the index, so these can be reworded
inline constexpr std::array<std::string_view, kBankCount> kBankLabels{
    "Sine-Saw", "Shapes", "Square-Saw", "Pulse Width", "Sync",
    "Partials", "Harmonics", "Organ", "Glass", "Comb",
    "Fold", "Digital", "Phase Dist", "FM",
    "Vowels",
    "Brass", "Reeds", "Strings", "Voices", "Keys", "Plucked", "Winds", "Mallets",
    "Noise"};
inline constexpr std::array<std::string_view, kMixLawCount> kMixLawLabels{
    "Linear", "Equal Power"};
inline constexpr std::array<std::string_view, kLfoShapeCount> kLfoShapeLabels{
    "Triangle", "Sine", "Square", "Saw Up", "Saw Down", "Random"};
inline constexpr std::array<std::string_view, kModTargetCount> kModTargetLabels{
    "Off",     "X",       "Y",       "Pitch",   "Pitch A", "Pitch B", "Pitch C",
    "Pitch D", "Frame A", "Frame B", "Frame C", "Frame D", "Cutoff",  "Level"};
inline constexpr std::array<std::string_view, kPlayModeCount> kPlayModeLabels{
    "Poly", "Mono", "Legato"};

//! The vector envelope's loop: its mode, and the points it goes between
enum class LoopFrom
{
  P0, P1, P2, P3
};
enum class LoopTo
{
  P1, P2, P3, P4
};
inline constexpr std::array<std::string_view, 3> kVecLoopLabels{
    "Off", "Round", "Back and forth"};
inline constexpr std::array<std::string_view, 4> kLoopFromLabels{"0", "1", "2", "3"};
inline constexpr std::array<std::string_view, 4> kLoopToLabels{"1", "2", "3", "4"};

#define VICEVERSA_COMBO(NAME, ENUM, LABELS, INIT)                                        \
  struct : halp::combobox_t<NAME, ENUM>                                             \
  {                                                                                 \
    struct range                                                                    \
    {                                                                               \
      decltype(LABELS) values = LABELS;                                             \
      ENUM init{INIT};                                                              \
    };                                                                              \
    ENUM value{INIT};                                                               \
  }

class ViceVersa
{
public:
  halp_meta(name, "ViceVersa")
  halp_meta(category, "Synths")
  halp_meta(c_name, "viceversa")
  halp_meta(author, "Jean-Michaël Celerier")
  halp_meta(
      description,
      "Vector synthesizer after the Prophet VS: four wavetable sources mixed by a point "
      "in a diamond, moved by a five-point looping envelope, through a ladder filter")
  halp_meta(uuid, "a3d6e1f4-2b7c-4c58-9e10-6f8d2b4a7c93")

  // The .scp preset format stores values by port index. Until the first
  // release, ports go where they belong and tools/SynthimiPresets.cpp
  // regenerates the presets; after it, this order is frozen.
  struct ins
  {
    halp::midi_bus<"Input"> midi;

#define VICEVERSA_SOURCE_PORTS(N, BANK)                                                   \
  VICEVERSA_COMBO("" #N " Bank", BankId, kBankLabels, BANK) src##N##_bank;                \
  halp::knob_f32<"" #N " Frame", halp::range{0., 1., 0.}> src##N##_frame;             \
  halp::spinbox_i32<"" #N " Coarse", halp::range{-24, 24, 0}> src##N##_coarse;        \
  halp::knob_f32<"" #N " Fine", halp::range{-100., 100., 0.}> src##N##_fine;          \
  halp::knob_f32<"" #N " Level", halp::range{0., 1., 1.}> src##N##_level;
    VICEVERSA_SOURCE_PORTS(A, BankId::SineSaw)
    VICEVERSA_SOURCE_PORTS(B, BankId::Vowels)
    VICEVERSA_SOURCE_PORTS(C, BankId::Sync)
    VICEVERSA_SOURCE_PORTS(D, BankId::Glass)
#undef VICEVERSA_SOURCE_PORTS

    //! Where the point is in the diamond, as the VS's joystick: one 2D
    //! control, so a single mapping or automation moves it
    halp::xy_pad_f32<"Position", halp::range{-1., 1., 0.}> position;
    VICEVERSA_COMBO("Mix Law", MixLaw, kMixLawLabels, MixLaw::Linear) law;
    struct : halp::val_port<"Vector Env", std::vector<float>>
    {
      enum widget
      {
        multi_slider
      };
      struct range
      {
        float min = 0.f, max = 1.f, init = 0.5f;
      };
    } vec_env;
    VICEVERSA_COMBO("Vector Loop", VecLoop, kVecLoopLabels, VecLoop::Off) vec_loop;
    VICEVERSA_COMBO("Loop From", LoopFrom, kLoopFromLabels, LoopFrom::P1) loop_from;
    VICEVERSA_COMBO("Loop To", LoopTo, kLoopToLabels, LoopTo::P3) loop_to;
    halp::time_chooser<"Move 1", halp::range{0.001, 20., 1.}> move1;
    halp::time_chooser<"Move 2", halp::range{0.001, 20., 1.}> move2;
    halp::time_chooser<"Move 3", halp::range{0.001, 20., 1.}> move3;
    halp::time_chooser<"Move 4", halp::range{0.001, 20., 1.}> move4;
    halp::time_chooser<"Move Release", halp::range{0.001, 20., 0.3}> move_release;
    halp::knob_f32<"Vector Depth", halp::range{0., 1., 1.}> vec_depth;

    // Four LFOs per voice, each moving one target (Dsp/Modulation.hpp). Off
    // by default; the first two, a quarter of a cycle apart, ready to circle
    // the point, the third a vibrato, the fourth a random walk
#define VICEVERSA_LFO_PORTS(N, SHAPE, PERIOD, DEPTH, PHASE)                              \
  VICEVERSA_COMBO("LFO " #N " Target", ModTarget, kModTargetLabels, ModTarget::Off)     \
      lfo##N##_target;                                                                 \
  VICEVERSA_COMBO("LFO " #N " Shape", LfoShape, kLfoShapeLabels, LfoShape::SHAPE)       \
      lfo##N##_shape;                                                                  \
  halp::time_chooser<"LFO " #N " Period", halp::range{0.02, 20., PERIOD}> lfo##N##_period; \
  halp::knob_f32<"LFO " #N " Depth", halp::range{0., 1., DEPTH}> lfo##N##_depth;       \
  halp::knob_f32<"LFO " #N " Phase", halp::range{0., 360., PHASE}> lfo##N##_phase;     \
  halp::time_chooser<"LFO " #N " Delay", halp::range{0., 4., 0.}> lfo##N##_delay;
    VICEVERSA_LFO_PORTS(1, Sine, 2., 0.5, 0.)
    VICEVERSA_LFO_PORTS(2, Sine, 2., 0.5, 90.)
    VICEVERSA_LFO_PORTS(3, Triangle, 0.2, 0.1, 0.)
    VICEVERSA_LFO_PORTS(4, Random, 0.25, 0.3, 0.)
#undef VICEVERSA_LFO_PORTS

    halp::knob_f32<"Frame Env", halp::range{-1., 1., 0.}> frame_env;

    struct : halp::knob_f32<"Cutoff", halp::range{20., 20000., 8000.}>
    {
      using mapper = halp::log_mapper<std::ratio<85, 100>>;
    } cutoff;
    halp::knob_f32<"Resonance", halp::range{0., 1., 0.2}> resonance;
    halp::knob_f32<"Filter Env", halp::range{-72., 72., 24.}> filter_env;
    halp::knob_f32<"Key Track", halp::range{0., 1., 0.5}> key_track;
    halp::knob_f32<"Vel Cutoff", halp::range{0., 48., 12.}> vel_cutoff;
    halp::knob_f32<"Drive", halp::range{0., 1., 0.3}> drive;

    halp::time_chooser<"Filter Attack", halp::range{0., 10., 0.005}> f_attack;
    halp::time_chooser<"Filter Decay", halp::range{0., 10., 0.4}> f_decay;
    halp::knob_f32<"Filter Sustain", halp::range{0., 1., 0.3}> f_sustain;
    halp::time_chooser<"Filter Release", halp::range{0., 10., 0.4}> f_release;
    halp::time_chooser<"Amp Attack", halp::range{0., 10., 0.005}> a_attack;
    halp::time_chooser<"Amp Decay", halp::range{0., 10., 0.3}> a_decay;
    halp::knob_f32<"Amp Sustain", halp::range{0., 1., 0.8}> a_sustain;
    halp::time_chooser<"Amp Release", halp::range{0., 10., 0.4}> a_release;

    VICEVERSA_COMBO("Play Mode", PlayMode, kPlayModeLabels, PlayMode::Poly) play_mode;
    halp::time_chooser<"Glide", halp::range{0., 2., 0.}> glide;
    halp::spinbox_i32<"Bend Range", halp::range{0, 48, 2}> bend_range;
    halp::spinbox_i32<"Octave", halp::range{-3, 3, 0}> octave;
    halp::knob_f32<"Tune", halp::range{-100., 100., 0.}> tune;
    halp::knob_f32<"Vel Level", halp::range{0., 1., 0.5}> vel_level;
    halp::knob_f32<"Pressure Y", halp::range{-1., 1., 0.5}> pressure_y;
    halp::knob_f32<"CC74 X", halp::range{-1., 1., 0.5}> timbre_x;

    halp::knob_f32<"Spread", halp::range{0., 1., 0.3}> spread;
    halp::knob_f32<"Level", halp::range{0., 2., 0.7}> level;
    halp::spinbox_i32<"Voices", halp::range{1, kMaxVoices, kMaxVoices}> voices;
  } inputs;

  struct
  {
    halp::fixed_audio_bus<"Output", float, 2> audio;
  } outputs;

  Synth synth;

  //! The voices' points, to the editor, about 30 times a second
  std::function<void(LivePoints)> send_message;
  LivePoints sent;
  int sinceSent{0};
  int sendEvery{1600};

  void prepare(halp::setup s)
  {
    synth.prepare(s.rate);
    sendEvery = std::max(1, int(s.rate / 30.));
  }

  Params params() const noexcept
  {
    Params p;
    const auto& in = inputs;
    auto src = [](auto& bank, auto& frame, auto& coarse, auto& fine, auto& level) {
      return SourceParams{bank.value, frame.value, float(coarse.value), fine.value, level.value};
    };
    p.src = {
        src(in.srcA_bank, in.srcA_frame, in.srcA_coarse, in.srcA_fine, in.srcA_level),
        src(in.srcB_bank, in.srcB_frame, in.srcB_coarse, in.srcB_fine, in.srcB_level),
        src(in.srcC_bank, in.srcC_frame, in.srcC_coarse, in.srcC_fine, in.srcC_level),
        src(in.srcD_bank, in.srcD_frame, in.srcD_coarse, in.srcD_fine, in.srcD_level)};
    p.x = in.position.value.x;
    p.y = in.position.value.y;
    p.law = in.law.value;
    p.vecEnv = VecEnvSpec::decode(in.vec_env.value, VecEnvSpec::centred());
    p.vecEnv.setLoop(in.vec_loop.value, int(in.loop_from.value), int(in.loop_to.value) + 1);
    p.vecEnv.time = {in.move1.value, in.move2.value, in.move3.value, in.move4.value};
    p.vecEnv.release = in.move_release.value;
    p.vecEnvDepth = in.vec_depth.value;
    auto lfo = [](auto& target, auto& shape, auto& period, auto& depth, auto& phase,
                  auto& delay) {
      return LfoParams{target.value, shape.value,  period.value,
                       depth.value,  phase.value / 360.f, delay.value};
    };
    p.lfos = {
        lfo(in.lfo1_target, in.lfo1_shape, in.lfo1_period, in.lfo1_depth, in.lfo1_phase,
            in.lfo1_delay),
        lfo(in.lfo2_target, in.lfo2_shape, in.lfo2_period, in.lfo2_depth, in.lfo2_phase,
            in.lfo2_delay),
        lfo(in.lfo3_target, in.lfo3_shape, in.lfo3_period, in.lfo3_depth, in.lfo3_phase,
            in.lfo3_delay),
        lfo(in.lfo4_target, in.lfo4_shape, in.lfo4_period, in.lfo4_depth, in.lfo4_phase,
            in.lfo4_delay)};
    p.frameEnv = in.frame_env.value;
    p.cutoff = in.cutoff.value;
    p.resonance = in.resonance.value;
    p.filterEnv = in.filter_env.value;
    p.keyTrack = in.key_track.value;
    p.velCutoff = in.vel_cutoff.value;
    p.drive = in.drive.value;
    p.fA = in.f_attack.value;
    p.fD = in.f_decay.value;
    p.fS = in.f_sustain.value;
    p.fR = in.f_release.value;
    p.aA = in.a_attack.value;
    p.aD = in.a_decay.value;
    p.aS = in.a_sustain.value;
    p.aR = in.a_release.value;
    p.play = in.play_mode.value;
    p.glide = in.glide.value;
    p.bendRange = float(in.bend_range.value);
    p.octave = float(in.octave.value);
    p.tune = in.tune.value;
    p.velLevel = in.vel_level.value;
    p.pressureY = in.pressure_y.value;
    p.timbreX = in.timbre_x.value;
    p.spread = in.spread.value;
    p.level = in.level.value;
    p.voices = in.voices.value;
    p.sanitize();
    return p;
  }

  using tick = halp::tick;
  void operator()(halp::tick t)
  {
    const int frames = t.frames;
    float* l = outputs.audio.samples[0];
    float* r = outputs.audio.samples[1];
    if(frames <= 0 || !l || !r)
      return;
    const Params p = params();
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

    sinceSent += frames;
    if(send_message && sinceSent >= sendEvery)
    {
      sinceSent = 0;
      if(auto now = synth.livePoints(); now != sent)
      {
        sent = now;
        send_message(now);
      }
    }
  }

  //! An LFO's controls, greyed out while it moves nothing
  template <auto Target>
  using lfo_on = halp::enabled_when<
      Target, ModTarget::X, ModTarget::Y, ModTarget::Pitch, ModTarget::PitchA,
      ModTarget::PitchB, ModTarget::PitchC, ModTarget::PitchD, ModTarget::FrameA,
      ModTarget::FrameB, ModTarget::FrameC, ModTarget::FrameD, ModTarget::Cutoff,
      ModTarget::Level>;

  // No UI item may be called x or y: the layout reads those as its position
  struct ui
  {
    halp_meta(name, "Main")
    halp_meta(layout, halp::layouts::vbox)
    halp_meta(background, halp::colors::background_mid)

    struct
    {
      halp_meta(layout, halp::layouts::hbox)
      struct
      {
        halp_meta(name, "Vector")
        halp_meta(layout, halp::layouts::section)
        halp_meta(background, halp::colors::background_dark)
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::custom_multi_control<
              VectorPad, &ins::vec_env, &ins::position, &ins::vec_loop, &ins::loop_from,
              &ins::loop_to>
              pad;
          // A dropdown in a grid widens its whole column: on rows of their own
          struct
          {
            halp_meta(layout, halp::layouts::vbox)
            halp::control<&ins::law, dropdown_as("Mix")> law;
            // Moved in the pad: here, its port, to map or automate it
            halp::item<&ins::position, halp::look{.widget = halp::control_widget::none}>
                position;
            halp::control<&ins::vec_depth, small_knob_as("Env Depth")> depth;
            halp::control<&ins::vec_loop, dropdown_as("Loop")> loop;
            struct
            {
              halp_meta(layout, halp::layouts::hbox)
              static constexpr auto condition()
              {
                return halp::enabled_when<
                    &ins::vec_loop, VecLoop::Round, VecLoop::BackAndForth>{};
              }
              halp::control<&ins::loop_from, dropdown_as("From")> from;
              halp::control<&ins::loop_to, dropdown_as("To")> to;
            } span;
          } knobs;
        } row;
        // The moves between the pad's numbered points
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::control<&ins::move1, small_knob_as("0-1")> m1;
          halp::control<&ins::move2, small_knob_as("1-2")> m2;
          halp::control<&ins::move3, small_knob_as("2-3")> m3;
          halp::control<&ins::move4, small_knob_as("3-4")> m4;
          halp::control<&ins::move_release, small_knob_as("Release")> release;
        } moves;
      } vector;

      struct
      {
        halp_meta(name, "Sources")
        halp_meta(layout, halp::layouts::section)
        halp_meta(background, halp::colors::background_dark)
        struct
        {
          halp_meta(layout, halp::layouts::grid)
          // Each column as wide as its own widest item, not the grid's
          halp_meta(alignment, halp::alignments::center)
          halp_meta(columns, 5)
#define VICEVERSA_SOURCE_ROW(N)                                                           \
  halp::control<&ins::src##N##_bank, dropdown_as(#N)> bank##N;                       \
  halp::control<&ins::src##N##_frame, small_knob_as("Frame")> frame##N;              \
  halp::item<&ins::src##N##_coarse, halp::label_as("Coarse")> coarse##N;             \
  halp::control<&ins::src##N##_fine, small_knob_as("Fine")> fine##N;                 \
  halp::control<&ins::src##N##_level, small_knob_as("Level")> level##N;
          VICEVERSA_SOURCE_ROW(A)
          VICEVERSA_SOURCE_ROW(B)
          VICEVERSA_SOURCE_ROW(C)
          VICEVERSA_SOURCE_ROW(D)
#undef VICEVERSA_SOURCE_ROW
        } grid;
      } sources;
    } top;

    // The voice's filter, amplitude, playing and modulation: tabs, under the
    // vector and the sources, which stay in view (the pad shows the LFOs
    // moving the point)
    struct
    {
      halp_meta(layout, halp::layouts::tabs)
      halp_meta(background, halp::colors::background_dark)
      // The filter and its envelope: a row each
      struct
      {
        halp_meta(name, "Filter")
        halp_meta(layout, halp::layouts::grid)
        // Each column as wide as its own widest item, not the grid's
        halp_meta(alignment, halp::alignments::center)
        halp_meta(columns, 6)
        halp::control<&ins::cutoff, small_knob_as("Cutoff")> cutoff;
        halp::control<&ins::resonance, small_knob_as("Reso")> reso;
        halp::control<&ins::drive, small_knob_as("Drive")> drive;
        halp::control<&ins::key_track, small_knob_as("Key")> key;
        halp::control<&ins::vel_cutoff, small_knob_as("Vel")> vel;
        halp::control<&ins::filter_env, small_knob_as("Env")> env;
        halp::control<&ins::f_attack, small_knob_as("Attack")> a;
        halp::control<&ins::f_decay, small_knob_as("Decay")> d;
        halp::control<&ins::f_sustain, small_knob_as("Sustain")> s;
        halp::control<&ins::f_release, small_knob_as("Release")> r;
        halp::control<&ins::frame_env, small_knob_as("Frame Env")> frame_env;
      } filter;

      // The envelope, then the output
      struct
      {
        halp_meta(name, "Amp")
        halp_meta(layout, halp::layouts::grid)
        halp_meta(alignment, halp::alignments::center)
        halp_meta(columns, 4)
        halp::control<&ins::a_attack, small_knob_as("Attack")> a;
        halp::control<&ins::a_decay, small_knob_as("Decay")> d;
        halp::control<&ins::a_sustain, small_knob_as("Sustain")> s;
        halp::control<&ins::a_release, small_knob_as("Release")> r;
        halp::control<&ins::vel_level, small_knob_as("Vel")> vel;
        halp::control<&ins::spread, small_knob_as("Spread")> spread;
        halp::control<&ins::level, small_knob_as("Level")> level;
        halp::item<&ins::voices> voices;
      } amp;

      // How notes are played, then the expression
      struct
      {
        halp_meta(name, "Play")
        halp_meta(layout, halp::layouts::grid)
        halp_meta(alignment, halp::alignments::center)
        halp_meta(columns, 4)
        halp::control<&ins::play_mode, dropdown_as("Mode")> mode;
        halp::control<&ins::glide, small_knob_as("Glide")> glide;
        halp::item<&ins::octave> octave;
        halp::control<&ins::tune, small_knob_as("Tune")> tune;
        halp::item<&ins::bend_range, halp::label_as("Bend")> bend;
        halp::control<&ins::pressure_y, small_knob_as("Press Y")> pres;
        halp::control<&ins::timbre_x, small_knob_as("CC74 X")> timbre;
      } play;

      // One row per LFO: what it moves, then how; the rest is greyed out
      // while it moves nothing
      struct
      {
        halp_meta(name, "Modulation")
        halp_meta(layout, halp::layouts::grid)
        halp_meta(alignment, halp::alignments::center)
        halp_meta(columns, 6)
#define VICEVERSA_LFO_CELL(N, PORT, LOOK)                                                \
  struct                                                                                \
  {                                                                                     \
    halp_meta(layout, halp::layouts::container)                                         \
    static constexpr auto condition() { return lfo_on<&ins::lfo##N##_target>{}; }       \
    halp::control<&ins::lfo##N##_##PORT, LOOK> PORT;                                    \
  } PORT##N;
#define VICEVERSA_LFO_ROW(N)                                                             \
  halp::control<&ins::lfo##N##_target, dropdown_as("LFO " #N)> target##N;               \
  VICEVERSA_LFO_CELL(N, shape, dropdown_as("Shape"))                                    \
  VICEVERSA_LFO_CELL(N, period, small_knob_as("Period"))                                \
  VICEVERSA_LFO_CELL(N, depth, small_knob_as("Depth"))                                  \
  VICEVERSA_LFO_CELL(N, phase, small_knob_as("Phase"))                                  \
  VICEVERSA_LFO_CELL(N, delay, small_knob_as("Delay"))
        VICEVERSA_LFO_ROW(1)
        VICEVERSA_LFO_ROW(2)
        VICEVERSA_LFO_ROW(3)
        VICEVERSA_LFO_ROW(4)
#undef VICEVERSA_LFO_ROW
#undef VICEVERSA_LFO_CELL
      } modulation;
    } bottom;

    void reset() { top.vector.row.pad.reset(); }

    struct bus
    {
      static void process_message(ui& self, LivePoints msg)
      {
        auto& pad = self.top.vector.row.pad;
        pad.live = msg;
        pad.repaint();
      }
    };
  };
};

inline constexpr int kPresetPorts = 82;
static_assert(avnd::parameter_input_introspection<ViceVersa>::size == kPresetPorts);
}
