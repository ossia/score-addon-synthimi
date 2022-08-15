#pragma once

#include <CDSPResampler.h>
#include <DspFilters/Filter.h>
#include <DspFilters/RBJ.h>
#include <DspFilters/SmoothedFilter.h>
#include <Gamma/Envelope.h>
#include <boost/container/static_vector.hpp>
#include <halp/audio.hpp>
#include <halp/compat/gamma.hpp>
#include <halp/controls.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>
#include <ossia/detail/math.hpp>
#include <rnd/random.hpp>

#include <memory>

namespace Synthimi
{
// Let's define a generic port for the waveform chooser
struct Waveform
{
  halp_meta(columns, 5)
  enum enum_type
  {
    Sine,
    Square,
    Triangle,
    Sawtooth,
    Noise
  } value;
  enum widget
  {
    enumeration,
    list,
    combobox
  };

  struct range
  {
    std::string_view values[5]{"Sine", "Square", "Tri", "Saw", "Noise"};
    std::string_view pixmaps[10]{
        ":/icons/wave_sin_off.png",
        ":/icons/wave_sin_on.png",
        ":/icons/wave_square_off.png",
        ":/icons/wave_square_on.png",
        ":/icons/wave_triangle_off.png",
        ":/icons/wave_triangle_on.png",
        ":/icons/wave_saw_off.png",
        ":/icons/wave_saw_on.png",
        ":/icons/wave_noise1_off.png",
        ":/icons/wave_noise1_on.png"};
    enum_type init = Sine;
  };

  operator enum_type&() noexcept { return value; }
  operator enum_type() const noexcept { return value; }
  auto& operator=(enum_type t) noexcept
  {
    value = t;
    return *this;
  }
};

inline auto m2f(double m)
{
  return (440.0 * std::exp2((m - 69.0) / 12.0));
}

class Synthimi;
struct Voice;

struct Frame
{
  double l, r;
};

static constexpr int oscillators = 4;
struct Subvoice
{
  double pitch = 0., ampl = 0., pan = 0.;
  double phase_incr[oscillators] = {0., 0., 0., 0.};
  double phase[oscillators] = {0., 0., 0., 0.};

  double run(Voice& v, Synthimi& s);
  void set_freq(Synthimi& synth);
};
struct Voice
{
  Voice() noexcept = default;
  Voice(const Voice&) noexcept = delete;
  Voice(Voice&&) noexcept = default;
  Voice& operator=(const Voice&) noexcept = delete;
  Voice& operator=(Voice&&) noexcept = default;
  explicit Voice(double pitch, double ampl) noexcept
  {
    main.pitch = pitch;
    main.ampl = ampl;
    main.pan = 0.5;
    for (int i = 0; i < 16; i++)
    {
      unison[i].pitch = pitch + 1. * (double(rand()) / RAND_MAX) * (i / 16.);
      unison[i].ampl = ampl * ((16. - i) / 16.);
      unison[i].pan = abs((double(rand()) / RAND_MAX)) / 8. + (0.5 - 1. / 8.);
    }
  }

  void set_pitch(Synthimi& synth, double p) noexcept
  {
    if (main.pitch != p)
    {
      for (int i = 0; i < 16; i++)
      {
        unison[i].pitch += (p - main.pitch);
      }
      main.pitch = p;
      set_freq(synth);
    }
  }
  void increment_pitch(Synthimi& synth, double p) noexcept
  {
    main.pitch += p;

    for (int i = 0; i < 16; i++)
    {
      unison[i].pitch += p;
    }
    set_freq(synth);
  }

  void init(double rate)
  {
    amp_adsr.set_sample_rate(rate);
    filt_adsr.set_sample_rate(rate);
  }

  void set_freq(Synthimi& synth);
  void update_envelope(Synthimi& synth);
  Frame run(Synthimi& synth);

  void stop();

  // If we do pitch bend we'll have to know where I guess
  Subvoice main;
  Subvoice unison[16];

  gam::ADSR<double, double, halp::compat::gamma_domain> amp_adsr;
  gam::ADSR<double, double, halp::compat::gamma_domain> filt_adsr;

  // let's rock
  static constexpr int maxOrder = 7;
  static constexpr auto chans = 2;
  Dsp::SmoothedFilterDesign<Dsp::RBJ::Design::LowPass, chans> lowpassFilter{128};
  Dsp::SmoothedFilterDesign<Dsp::RBJ::Design::HighPass, chans> highpassFilter{128};
};

struct Voices
{
  // Can we have +1 voice per note ? hmm
  // yes: as soon as we have a > 0 release time

  boost::container::static_vector<Voice, 1024> active;
};
class Synthimi
{
public:
  halp_meta(name, "Synthimi")
  halp_meta(category, "Audio/Synth")
  halp_meta(author, "Jean-Michaël Celerier")
  halp_meta(
      description,
      "A basic synthesizer. Modeled after the meows of the developer's cat, Shashimi.")
  halp_meta(c_name, "synthimi")
  halp_meta(uuid, "d4008ff6-73b9-4575-80a4-60e3da095db7")

  struct ui;

  struct ins
  {
    halp::midi_bus<"In"> midi;

    // Here we'll define our parameters - very simply, and then we'll try to refactor
    // that afterwards :-)
    halp::knob_f32<"Osc 1 Amp.", halp::range{0., 1., 1.}> osc0_amp;
    struct : Waveform
    {
      halp_meta(name, "Osc 1 Wave")
    } osc0_waveform;
    struct : halp::knob_f32<"Osc 1 Pitch", halp::range{-12, 12, 0.}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc0_pitch;
    struct : halp::knob_i32<"Osc 1 Oct", halp::irange{-5, 5, 0}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc0_oct;

    halp::knob_f32<"Osc 2 Amp.", halp::range{0., 1., 1.}> osc1_amp;
    struct : Waveform
    {
      halp_meta(name, "Osc 2 Wave")
    } osc1_waveform;
    struct : halp::knob_f32<"Osc 2 Pitch", halp::range{-12, 12, 0.}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc1_pitch;
    struct : halp::knob_i32<"Osc 2 Oct", halp::irange{-5, 5, 0}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc1_oct;

    halp::knob_f32<"Osc 3 Amp.", halp::range{0., 1., 1.}> osc2_amp;
    struct : Waveform
    {
      halp_meta(name, "Osc 3 Wave")
    } osc2_waveform;
    struct : halp::knob_f32<"Osc 3 Pitch", halp::range{-12, 12, 0.}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc2_pitch;
    struct : halp::knob_i32<"Osc 3 Oct", halp::irange{-5, 5, 0}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc2_oct;

    halp::knob_f32<"Osc 4 Amp.", halp::range{0., 1., 1.}> osc3_amp;
    struct : Waveform
    {
      halp_meta(name, "Osc 4 Wave")
    } osc3_waveform;
    struct : halp::knob_f32<"Osc 4 Pitch", halp::range{-12, 12, 0.}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc3_pitch;
    struct : halp::knob_i32<"Osc 4 Oct", halp::irange{-5, 5, 0}>
    {
      void update(Synthimi& s) { s.update_pitches(); }
    } osc3_oct;

    halp::knob_f32<"Amp. Attack", halp::range{0., 1., 0.1}> amp_attack;
    halp::knob_f32<"Amp. Decay", halp::range{0., 1., 0.1}> amp_decay;
    halp::knob_f32<"Amp. Sustain", halp::range{0., 1., 0.5}> amp_sustain;
    halp::knob_f32<"Amp. Release", halp::range{0., 1., 0.2}> amp_release;

    struct
    {
      halp__enum_combobox("Type", LPF, LPF, HPF)
    } filt_type;
    halp::knob_f32<"Cutoff", halp::range{20., 20000., 2000.}> filt_cutoff;
    halp::knob_f32<"Reso", halp::range{0., 0., 1.}> filt_res;
    halp::knob_f32<"Flt. Attack", halp::range{0., 1., 0.1}> filt_attack;
    halp::knob_f32<"Flt. Decay", halp::range{0., 1., 0.1}> filt_decay;
    halp::knob_f32<"Flt. Sustain", halp::range{0., 1., 0.5}> filt_sustain;
    halp::knob_f32<"Flt. Release", halp::range{0., 1., 0.2}> filt_release;

    struct
    {
      halp__enum_combobox("Polyphony", Poly, Mono, Poly)
    } poly_mode;
    halp::knob_f32<"Porta", halp::range{0., 1., 0.}> portamento;

    halp::toggle<"Filter", halp::toggle_setup{true}> filt_env;
    halp::knob_f32<"Drive", halp::range{0., 1., 0.}> drive;

    struct
    {
      halp__enum_combobox("Modmatrix", S, S, C, CSP, CST, CSC)
    } matrix;
    halp::knob_i32<"Unison", halp::irange{0, 16, 0}> unison;
  } inputs;

  struct
  {
    halp::fixed_audio_bus<"Out", double, 2> audio;
  } outputs;

  halp::setup settings;
  void prepare(halp::setup info);

  void update_pitches();

  // This should be done in a more generic way
  using tick = halp::tick;
  void operator()(halp::tick t);
  void process_midi();
  void process_voices(int frames, double* l, double* r);
  void postprocess(int frames, double* l, double* r);

  // struct ui;

  // These don't go as input or output, just as plain struct members
  Voices voices;
  Voice mono;
  double porta_samples{};
  double porta_cur_samples{};
  double porta_from = -1;
  double porta_to = -1;

  std::unique_ptr<r8b::CDSPResampler> resample_l;
  std::unique_ptr<r8b::CDSPResampler> resample_r;
};

}
