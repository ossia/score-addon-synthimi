#pragma once

#include <CDSPResampler.h>
#include <DspFilters/Filter.h>
#include <DspFilters/RBJ.h>
#include <DspFilters/SmoothedFilter.h>
#include <Gamma/Envelope.h>
#include <Process/CodeWriter.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/Process.hpp>
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
/*
struct SynthimiCodeWriter : public Process::CodeWriter
{
  explicit SynthimiCodeWriter(const Process::ProcessModel& p) noexcept
      : CodeWriter{p}
  {
  }

  std::string initializer() const noexcept override
  {
    auto& inls = self.inlets();
    Process::ControlInlet* osc1_amp = static_cast<Process::ControlInlet*>(inls[1]);
    Process::ControlInlet* osc1_wf = static_cast<Process::ControlInlet*>(inls[2]);
    Process::ControlInlet* osc1_pitch = static_cast<Process::ControlInlet*>(inls[3]);
    Process::ControlInlet* osc1_oct = static_cast<Process::ControlInlet*>(inls[4]);
    
    Process::ControlInlet* osc2_amp = static_cast<Process::ControlInlet*>(inls[5]);
    Process::ControlInlet* osc2_wf = static_cast<Process::ControlInlet*>(inls[6]);
    Process::ControlInlet* osc2_pitch = static_cast<Process::ControlInlet*>(inls[7]);
    Process::ControlInlet* osc2_oct = static_cast<Process::ControlInlet*>(inls[8]);
    
    Process::ControlInlet* osc3_amp = static_cast<Process::ControlInlet*>(inls[9]);
    Process::ControlInlet* osc3_wf = static_cast<Process::ControlInlet*>(inls[10]);
    Process::ControlInlet* osc3_pitch = static_cast<Process::ControlInlet*>(inls[11]);
    Process::ControlInlet* osc3_oct = static_cast<Process::ControlInlet*>(inls[12]);
    
    Process::ControlInlet* osc4_amp = static_cast<Process::ControlInlet*>(inls[13]);
    Process::ControlInlet* osc4_wf = static_cast<Process::ControlInlet*>(inls[14]);
    Process::ControlInlet* osc4_pitch = static_cast<Process::ControlInlet*>(inls[15]);
    Process::ControlInlet* osc4_oct = static_cast<Process::ControlInlet*>(inls[16]);
    
    
    Process::ControlInlet* aenv_a = static_cast<Process::ControlInlet*>(inls[17]);
    Process::ControlInlet* aenv_d = static_cast<Process::ControlInlet*>(inls[18]);
    Process::ControlInlet* aenv_s = static_cast<Process::ControlInlet*>(inls[19]);
    Process::ControlInlet* aenv_r = static_cast<Process::ControlInlet*>(inls[20]);
    
    Process::ControlInlet* fenv_type = static_cast<Process::ControlInlet*>(inls[21]);
    
    Process::ControlInlet* fenv_cutoff = static_cast<Process::ControlInlet*>(inls[22]);
    Process::ControlInlet* fenv_reso = static_cast<Process::ControlInlet*>(inls[23]);
    
    Process::ControlInlet* fenv_a = static_cast<Process::ControlInlet*>(inls[24]);
    Process::ControlInlet* fenv_d = static_cast<Process::ControlInlet*>(inls[25]);
    Process::ControlInlet* fenv_s = static_cast<Process::ControlInlet*>(inls[26]);
    Process::ControlInlet* fenv_r = static_cast<Process::ControlInlet*>(inls[27]);
    
    return fmt::format(R"_(.inputs = {{
  .osc1_amp = {},
  .osc2_amp = {},
  .osc3_amp = {},
  .osc4_amp = {},
}})_");
    

  }
  std::string typeName() const noexcept override;
  std::string accessInlet(const Id<Process::Port>& id) const noexcept override;
  std::string accessOutlet(const Id<Process::Port>& id) const noexcept override;
  std::string execute() const noexcept override;

  bool in_vector{}, ret_vector{};
};
*/
class Synthimi
{
public:
  halp_meta(name, "Synthimi")
  halp_meta(category, "Audio/Synth")
  halp_meta(author, "Jean-Michaël Celerier")
  halp_meta(
      description,
      "A basic synthesizer. Modeled after the meows of the developer's cat, Shashimi.")
  halp_meta(manual_url, "https://ossia.io/score-docs/processes/synthimi.html")
  halp_meta(c_name, "synthimi")
  halp_meta(uuid, "d4008ff6-73b9-4575-80a4-60e3da095db7")

  struct ui;
  // using code_writer = SynthimiCodeWriter;

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
