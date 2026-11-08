#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <CZumarzu/CZumarzuDsp.hpp>
#include <CZumarzu/EnvelopeWidget.hpp>
#include <avnd/common/enum_reflection.hpp>
#include <avnd/introspection/input.hpp>
#include <halp/audio.hpp>
#include <halp/controls.enums.hpp>
#include <halp/controls.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>

#include <array>
#include <string_view>

namespace czumarzu
{
#if AVND_ENUM_REFLECTION_SUPPORTED
static_assert(avnd::enum_names<Wave>().size() == kWaveCount);
static_assert(avnd::enum_names<SecondWave>().size() == kSecondWaveCount);
static_assert(avnd::enum_names<LineMode>().size() == kLineModeCount);
static_assert(avnd::enum_names<Modulation>().size() == kModulationCount);
static_assert(avnd::enum_names<Quality>().size() == kQualityCount);
static_assert(avnd::enum_names<PlayMode>().size() == kPlayModeCount);
static_assert(avnd::enum_names<VibratoWave>().size() == kVibratoWaveCount);
#endif

inline EnvSpec DcaKind::fallback() { return LineParams::defaultDca(); }
inline EnvSpec DcwKind::fallback() { return LineParams::defaultDcw(); }
inline EnvSpec DcoKind::fallback() { return LineParams::defaultDco(); }

inline constexpr halp::look small_knob{
    .size = halp::control_size::compact, .widget = halp::control_widget::knob};
inline constexpr halp::look dropdown{.widget = halp::control_widget::combo};
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
inline constexpr std::array<std::string_view, kWaveCount> kWaveLabels{
    "Saw", "Square", "Pulse", "Double Sine", "Saw-Pulse", "Reso Saw", "Reso Triangle",
    "Reso Trapezoid", "Double Pulse", "Reso Pulse", "Reso Double Saw", "VPS"};
inline constexpr std::array<std::string_view, kSecondWaveCount> kSecondWaveLabels{
    "Off", "Saw", "Square", "Pulse", "Double Sine", "Saw-Pulse", "Reso Saw", "Reso Triangle",
    "Reso Trapezoid", "Double Pulse", "Reso Pulse", "Reso Double Saw", "VPS"};
inline constexpr std::array<std::string_view, kLineModeCount> kLineModeLabels{
    "1", "2", "1 + 1'", "1 + 2'"};
inline constexpr std::array<std::string_view, kModulationCount> kModulationLabels{
    "Off", "Ring", "Noise"};
inline constexpr std::array<std::string_view, kQualityCount> kQualityLabels{
    "Clean", "Clean 2x", "Clean 4x", "Live"};
inline constexpr std::array<std::string_view, kPlayModeCount> kPlayModeLabels{
    "Poly", "Mono", "Legato"};
inline constexpr std::array<std::string_view, kVibratoWaveCount> kVibratoWaveLabels{
    "Triangle", "Saw Up", "Saw Down", "Square", "Sine"};

//! The envelopes' menus: an item per step
enum class SustainStep
{
  Off, S1, S2, S3, S4, S5, S6, S7, S8
};
enum class EndStep
{
  S1, S2, S3, S4, S5, S6, S7, S8
};
inline constexpr std::array<std::string_view, kSteps + 1> kSustainLabels{
    "Off", "1", "2", "3", "4", "5", "6", "7", "8"};
inline constexpr std::array<std::string_view, kSteps> kEndLabels{
    "1", "2", "3", "4", "5", "6", "7", "8"};

//! A combobox showing LABELS, starting at INIT
#define CZUMARZU_COMBO(NAME, ENUM, LABELS, INIT)                                      \
  struct : halp::combobox_t<NAME, ENUM>                                             \
  {                                                                                 \
    struct range                                                                    \
    {                                                                               \
      decltype(LABELS) values = LABELS;                                             \
      ENUM init{INIT};                                                              \
    };                                                                              \
    ENUM value{INIT};                                                               \
  }

//! An envelope's steps in one port, edited on a graph
template <halp::static_string Name>
struct EnvPort : halp::val_port<Name, std::vector<float>>
{
  enum widget
  {
    multi_slider
  };
  struct range
  {
    float min = 0.f, max = 1.f, init = 0.f;
  };
};

class CZumarzu
{
public:
  halp_meta(name, "CZumarzu")
  halp_meta(category, "Synths")
  halp_meta(c_name, "czumarzu")
  halp_meta(author, "Jean-Michaël Celerier")
  halp_meta(
      description,
      "Phase distortion synthesizer after the Casio CZ: two lines of alternating "
      "waveforms with eight-step DCO, DCW and DCA envelopes, ring and noise modulation")
  halp_meta(uuid, "5f0c2b7e-8d41-4a96-b3e5-1c7a9e24d860")

  // The .scp preset format stores values by port index. Until the first
  // release, ports go where they belong and tools/SynthimiPresets.cpp
  // regenerates the presets; after it, this order is frozen.
  struct ins
  {
    halp::midi_bus<"Input"> midi;

//! An envelope: its steps, its sustain and end steps, its loop
#define CZUMARZU_ENV_PORTS(N, ENV, TITLE, SUSTAIN, END)                                 \
  EnvPort<"L" #N " " TITLE> l##N##_##ENV;                                              \
  CZUMARZU_COMBO("L" #N " " TITLE " Sustain", SustainStep, kSustainLabels,             \
                 SustainStep::SUSTAIN) l##N##_##ENV##_sustain;                         \
  CZUMARZU_COMBO("L" #N " " TITLE " End", EndStep, kEndLabels, EndStep::END)            \
      l##N##_##ENV##_end;                                                              \
  halp::toggle<"L" #N " " TITLE " Loop"> l##N##_##ENV##_loop;

#define CZUMARZU_LINE_PORTS(N)                                                          \
  CZUMARZU_COMBO("L" #N " Wave 1", Wave, kWaveLabels, Wave::Saw) l##N##_wave1;          \
  CZUMARZU_COMBO("L" #N " Wave 2", SecondWave, kSecondWaveLabels, SecondWave::Off)      \
      l##N##_wave2;                                                                    \
  halp::knob_f32<"L" #N " Formant", halp::range{0.5, 8., 1.}> l##N##_formant;          \
  halp::knob_f32<"L" #N " Level", halp::range{0., 1., 1.}> l##N##_level;               \
  halp::knob_f32<"L" #N " DCW Amount", halp::range{0., 1., 1.}> l##N##_dcw_depth;      \
  halp::knob_f32<"L" #N " DCO Range", halp::range{-48., 48., 12.}> l##N##_dco_depth;   \
  halp::knob_f32<"L" #N " DCW Key", halp::range{0., 1., 0.}> l##N##_dcw_kf;            \
  halp::knob_f32<"L" #N " DCA Key", halp::range{0., 1., 0.}> l##N##_dca_kf;            \
  CZUMARZU_ENV_PORTS(N, dca, "DCA", S1, S2)                                            \
  CZUMARZU_ENV_PORTS(N, dcw, "DCW", S2, S3)                                            \
  CZUMARZU_ENV_PORTS(N, dco, "DCO", Off, S1)
    CZUMARZU_LINE_PORTS(1)
    CZUMARZU_LINE_PORTS(2)
#undef CZUMARZU_LINE_PORTS
#undef CZUMARZU_ENV_PORTS

    CZUMARZU_COMBO("Line Select", LineMode, kLineModeLabels, LineMode::Line1And2) mode;
    CZUMARZU_COMBO("Modulation", Modulation, kModulationLabels, Modulation::Off) modulation;
    halp::spinbox_i32<"Detune Oct", halp::range{-3, 3, 0}> detune_oct;
    halp::spinbox_i32<"Detune Note", halp::range{-11, 11, 0}> detune_note;
    halp::knob_f32<"Detune Fine", halp::range{-100., 100., 8.}> detune_fine;

    CZUMARZU_COMBO("Vibrato Wave", VibratoWave, kVibratoWaveLabels, VibratoWave::Triangle)
        vib_wave;
    halp::time_chooser<"Vibrato Period", halp::range{0.01, 10., 0.18}> vib_period;
    halp::time_chooser<"Vibrato Delay", halp::range{0., 4., 0.}> vib_delay;
    halp::knob_f32<"Vibrato Depth", halp::range{0., 12., 0.}> vib_depth;

    halp::spinbox_i32<"Octave", halp::range{-3, 3, 0}> octave;
    halp::knob_f32<"Tune", halp::range{-100., 100., 0.}> tune;
    CZUMARZU_COMBO("Play Mode", PlayMode, kPlayModeLabels, PlayMode::Poly) play_mode;
    halp::time_chooser<"Glide", halp::range{0., 2., 0.}> glide;
    halp::spinbox_i32<"Bend Range", halp::range{0, 48, 2}> bend_range;
    halp::knob_f32<"Vel Level", halp::range{0., 1., 0.5}> vel_level;
    halp::knob_f32<"Vel DCW", halp::range{0., 1., 0.3}> vel_dcw;
    halp::knob_f32<"Pressure DCW", halp::range{-1., 1., 0.3}> pressure_dcw;

    CZUMARZU_COMBO("Quality", Quality, kQualityLabels, Quality::Clean) quality;
    halp::knob_f32<"Spread", halp::range{0., 1., 0.3}> spread;
    halp::knob_f32<"Level", halp::range{0., 2., 0.7}> level;
    halp::spinbox_i32<"Voices", halp::range{1, kMaxVoices, kMaxVoices}> voices;
  } inputs;

  struct
  {
    halp::fixed_audio_bus<"Output", float, 2> audio;
  } outputs;

  Synth synth;

  void prepare(halp::setup s) { synth.prepare(s.rate); }

  //! An envelope from its four ports
  static EnvSpec readEnv(auto& steps, auto& sustain, auto& end, auto& loop,
                         const EnvSpec& fallback) noexcept
  {
    return EnvSpec::from(
        steps.value, int(sustain.value), int(end.value), loop.value, fallback);
  }

#define CZUMARZU_READ_LINE(L, N)                                                          \
  L.wave1 = in.l##N##_wave1.value;                                                        \
  L.wave2 = in.l##N##_wave2.value;                                                        \
  L.formant = in.l##N##_formant.value;                                                    \
  L.level = in.l##N##_level.value;                                                        \
  L.dcwDepth = in.l##N##_dcw_depth.value;                                                 \
  L.pitchDepth = in.l##N##_dco_depth.value;                                               \
  L.dcwKeyFollow = in.l##N##_dcw_kf.value;                                                \
  L.dcaKeyFollow = in.l##N##_dca_kf.value;                                                \
  L.dca = readEnv(in.l##N##_dca, in.l##N##_dca_sustain, in.l##N##_dca_end,                \
                  in.l##N##_dca_loop, LineParams::defaultDca());                          \
  L.dcw = readEnv(in.l##N##_dcw, in.l##N##_dcw_sustain, in.l##N##_dcw_end,                \
                  in.l##N##_dcw_loop, LineParams::defaultDcw());                          \
  L.dco = readEnv(in.l##N##_dco, in.l##N##_dco_sustain, in.l##N##_dco_end,                \
                  in.l##N##_dco_loop, LineParams::defaultDco());

  Params params() const noexcept
  {
    Params p;
    const auto& in = inputs;
    CZUMARZU_READ_LINE(p.line[0], 1)
    CZUMARZU_READ_LINE(p.line[1], 2)
#undef CZUMARZU_READ_LINE
    p.mode = in.mode.value;
    p.modulation = in.modulation.value;
    p.detuneOctave = float(in.detune_oct.value);
    p.detuneNote = float(in.detune_note.value);
    p.detuneFine = in.detune_fine.value;
    p.vibWave = in.vib_wave.value;
    p.vibPeriod = in.vib_period.value;
    p.vibDelay = in.vib_delay.value;
    p.vibDepth = in.vib_depth.value;
    p.octave = float(in.octave.value);
    p.tune = in.tune.value;
    p.play = in.play_mode.value;
    p.glide = in.glide.value;
    p.bendRange = float(in.bend_range.value);
    p.velLevel = in.vel_level.value;
    p.velDcw = in.vel_dcw.value;
    p.pressureDcw = in.pressure_dcw.value;
    p.quality = in.quality.value;
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

  //! Line 2 and its modulation only sound in the two-line modes
  using two_lines = halp::enabled_when<&ins::mode, LineMode::Line1Doubled, LineMode::Line1And2>;
  //! The formant only shapes VPS
  using vps_1 = halp::enabled_when<&ins::l1_wave1, Wave::Vps>;
  using vps_2 = halp::enabled_when<&ins::l2_wave1, Wave::Vps>;

  struct ui
  {
    halp_meta(name, "Main")
    halp_meta(layout, halp::layouts::vbox)
    halp_meta(background, halp::colors::background_mid)

    // What plays: which lines, how they combine
    struct
    {
      halp_meta(name, "Lines")
      halp_meta(layout, halp::layouts::section)
      halp_meta(background, halp::colors::background_dark)
      struct
      {
        halp_meta(layout, halp::layouts::hbox)
        halp::control<&ins::mode, dropdown_as("Lines")> mode;
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          static constexpr auto condition() { return two_lines{}; }
          halp::control<&ins::modulation, dropdown_as("Mod")> modulation;
          halp::item<&ins::detune_oct, halp::label_as("Detune")> oct;
          halp::item<&ins::detune_note, halp::label_as("Note")> note;
          halp::control<&ins::detune_fine, small_knob_as("Fine")> fine;
        } detune;
      } row;
    } lines;

    // One line at a time: the same controls twice would double the height
//! An envelope's graph, and its menus under it
#define CZUMARZU_ENV_COLUMN(N, ENV, KIND)                                               \
  struct                                                                              \
  {                                                                                   \
    halp_meta(layout, halp::layouts::vbox)                                            \
    halp::custom_multi_control<                                                       \
        StepEnvelopeWidget<KIND>, &ins::l##N##_##ENV, &ins::l##N##_##ENV##_sustain,   \
        &ins::l##N##_##ENV##_end, &ins::l##N##_##ENV##_loop>                          \
        graph;                                                                        \
    struct                                                                            \
    {                                                                                 \
      halp_meta(layout, halp::layouts::hbox)                                          \
      halp::control<&ins::l##N##_##ENV##_sustain, dropdown_as("Sustain")> sustain;    \
      halp::control<&ins::l##N##_##ENV##_end, dropdown_as("End")> end;                \
      halp::item<&ins::l##N##_##ENV##_loop, halp::label_as("Loop")> loop;             \
    } menus;                                                                          \
  } ENV;

#define CZUMARZU_LINE_PAGE(N)                                                           \
  struct                                                                              \
  {                                                                                   \
    halp_meta(name, "Line " #N)                                                       \
    halp_meta(layout, halp::layouts::vbox)                                            \
    struct                                                                            \
    {                                                                                 \
      halp_meta(layout, halp::layouts::hbox)                                          \
      halp::control<&ins::l##N##_wave1, dropdown_as("Wave")> wave1;                   \
      halp::control<&ins::l##N##_wave2, dropdown_as("Wave 2")> wave2;                 \
      struct                                                                          \
      {                                                                               \
        halp_meta(layout, halp::layouts::container)                                   \
        static constexpr auto condition() { return vps_##N{}; }                        \
        halp::control<&ins::l##N##_formant, small_knob_as("Formant")> formant;        \
      } formant;                                                                      \
      halp::control<&ins::l##N##_level, small_knob_as("Level")> level;                \
      halp::control<&ins::l##N##_dco_depth, small_knob_as("DCO Range")> dco_depth;    \
      halp::control<&ins::l##N##_dcw_depth, small_knob_as("DCW Amount")> dcw_depth;   \
      halp::control<&ins::l##N##_dcw_kf, small_knob_as("DCW Key")> dcw_kf;            \
      halp::control<&ins::l##N##_dca_kf, small_knob_as("DCA Key")> dca_kf;            \
    } top;                                                                            \
    struct                                                                            \
    {                                                                                 \
      halp_meta(layout, halp::layouts::hbox)                                          \
      CZUMARZU_ENV_COLUMN(N, dco, DcoKind)                                            \
      CZUMARZU_ENV_COLUMN(N, dcw, DcwKind)                                            \
      CZUMARZU_ENV_COLUMN(N, dca, DcaKind)                                            \
    } envelopes;                                                                      \
  } line##N;

    struct
    {
      halp_meta(layout, halp::layouts::tabs)
      halp_meta(background, halp::colors::background_dark)
      CZUMARZU_LINE_PAGE(1)
      CZUMARZU_LINE_PAGE(2)
    } line;
#undef CZUMARZU_LINE_PAGE
#undef CZUMARZU_ENV_COLUMN

    struct
    {
      halp_meta(layout, halp::layouts::hbox)
      struct
      {
        halp_meta(name, "Vibrato")
        halp_meta(layout, halp::layouts::section)
        halp_meta(background, halp::colors::background_dark)
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::control<&ins::vib_wave, dropdown_as("Wave")> wave;
          halp::control<&ins::vib_period, small_knob_as("Period")> period;
          halp::control<&ins::vib_delay, small_knob_as("Delay")> delay;
          halp::control<&ins::vib_depth, small_knob_as("Depth")> depth;
        } row;
      } vibrato;

      struct
      {
        halp_meta(name, "Play")
        halp_meta(layout, halp::layouts::section)
        halp_meta(background, halp::colors::background_dark)
        halp::control<&ins::play_mode, dropdown_as("Mode")> mode;
        struct
        {
          halp_meta(layout, halp::layouts::grid)
          // Each column as wide as its own widest item, not the grid's
          halp_meta(alignment, halp::alignments::center)
          halp_meta(columns, 4)
          halp::control<&ins::glide, small_knob_as("Glide")> glide;
          halp::item<&ins::octave> octave;
          halp::control<&ins::tune, small_knob_as("Tune")> tune;
          halp::item<&ins::bend_range, halp::label_as("Bend")> bend;
          halp::control<&ins::vel_level, small_knob_as("Vel Level")> vel_level;
          halp::control<&ins::vel_dcw, small_knob_as("Vel DCW")> vel_dcw;
          halp::control<&ins::pressure_dcw, small_knob_as("Press DCW")> pressure;
        } grid;
      } play;

      struct
      {
        halp_meta(name, "Output")
        halp_meta(layout, halp::layouts::section)
        halp_meta(background, halp::colors::background_dark)
        halp::control<&ins::quality, dropdown_as("Quality")> quality;
        struct
        {
          halp_meta(layout, halp::layouts::hbox)
          halp::item<&ins::voices> voices;
          halp::control<&ins::spread, small_knob_as("Spread")> spread;
          halp::control<&ins::level, small_knob_as("Level")> level;
        } row;
      } output;
    } bottom;

    void reset()
    {
      line.line1.envelopes.dco.graph.reset();
      line.line1.envelopes.dcw.graph.reset();
      line.line1.envelopes.dca.graph.reset();
      line.line2.envelopes.dco.graph.reset();
      line.line2.envelopes.dcw.graph.reset();
      line.line2.envelopes.dca.graph.reset();
    }
  };
};

//! Presets store values by port index: see the note on `ins`
inline constexpr int kPresetPorts = 61;
static_assert(avnd::parameter_input_introspection<CZumarzu>::size == kPresetPorts);
}
