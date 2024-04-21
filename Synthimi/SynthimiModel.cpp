#include "Synthimi.hpp"

#include <kfr/dsp/oscillators.hpp>
#include <libremidi/message.hpp>
// This is a bit slow to build and I want to go fast tonight... so maybe...

// #include <ossia/network/dataspace/gain.hpp>
// #include <ossia/network/dataspace/time.hpp>
// #include <ossia/network/dataspace/value_with_unit.hpp>

namespace Synthimi
{
constexpr int upsample_factor = 3;
/*
struct nan_detector
{
  double v;
  nan_detector() { ; }
  nan_detector(double v) { *this = v; }
  void check(double v)
  {
    SCORE_ASSERT(!std::isnan(v));
    SCORE_ASSERT(!std::isinf(v));
  }
  operator double()
  {
    check(v);
    return v;
  }
  auto& operator+=(double vv)
  {
    check(v);
    check(vv);
    v += vv;
    return *this;
  }
  friend nan_detector operator+(nan_detector t, double rhs)
  {
    t.check(t.v);
    t.check(rhs);
    t.v = t.v + rhs;
    t.check(t.v);
    return t;
  }
  friend nan_detector operator-(nan_detector t, double rhs)
  {
    t.check(t.v);
    t.check(rhs);
    t.v = t.v - rhs;
    t.check(t.v);
    return t;
  }
  friend nan_detector operator*(nan_detector t, double rhs)
  {
    t.check(t.v);
    t.check(rhs);
    t.v = t.v * rhs;
    t.check(t.v);
    return t;
  }
  friend nan_detector operator*(double rhs, nan_detector t)
  {
    t.check(t.v);
    t.check(rhs);
    t.v = t.v * rhs;
    t.check(t.v);
    return t;
  }
  friend nan_detector operator/(nan_detector t, double rhs)
  {
    SCORE_ASSERT(rhs != 0.);
    t.check(t.v);
    t.check(rhs);
    t.v = t.v / rhs;
    t.check(t.v);
    return t;
  }
  nan_detector& operator=(double vv)
  {
    check(vv);
    v = vv;
    return *this;
  }
};*/

using nan_detector = double;

void Synthimi::prepare(halp::setup info)
{
  double upsample = upsample_factor * info.rate;
  this->settings = info;
  this->mono.init(upsample);
  this->resample_l = std::make_unique<r8b::CDSPResampler>(
      upsample, info.rate, upsample_factor * info.frames, 3.0, 206.91, r8b::fprMinPhase);
  this->resample_r = std::make_unique<r8b::CDSPResampler>(
      upsample, info.rate, upsample_factor * info.frames, 3.0, 206.91, r8b::fprMinPhase);
}

void Synthimi::update_pitches()
{
  if (this->settings.rate <= 0.)
    return;

  for (auto& v : voices.active)
  {
    v.set_freq(*this);
  }
  mono.set_freq(*this);
}

void Synthimi::operator()(halp::tick t)
{
  // Now we have to start thinking about our synth - for good

  // First we'll need to define a voice structure
  // - oscillators
  // - ADSR state

  // And handle them dynamically - with some limits, e.g. 1000 voices

  // I'd like 4xOSC per voice

  // We have to think about our oscillator parameters:
  // - waveform
  // - octave
  // - pitch
  // - unison ?
  // - sub channel
  // - dephase

  // And how we can combine them:
  // sum ? FM ?

  // - legato / portamento ? idk how to do that aha

  // - noise channel would be cool :)

  const int upsample_frames = t.frames * upsample_factor;
  auto* l = (double*)alloca(sizeof(double) * upsample_frames);
  std::fill_n(l, upsample_frames, 0.);
  auto* r = (double*)alloca(sizeof(double) * upsample_frames);
  std::fill_n(r, upsample_frames, 0.);

  process_midi();

  process_voices(t.frames, l, r);

  postprocess(t.frames, l, r);
}

void Synthimi::process_midi()
{
  auto& voices = this->voices.active;
  for (auto m : this->inputs.midi.midi_messages)
  {
    // We get a note -> we add a voice
    // let's use libremidi to see if we have a note

    switch ((libremidi::message_type)(m.bytes[0] & 0xF0))
    {
      case libremidi::message_type::NOTE_ON:
      {
        auto note = m.bytes[1];
        if (auto ampl = m.bytes[2] / 127.; ampl > 0)
        {
          voices.emplace_back(note, ampl);
          voices.back().init(settings.rate * upsample_factor);
          voices.back().set_freq(*this);
          if (voices.size() >= 2)
          {
            porta_samples
                = 0.1 + this->inputs.portamento * upsample_factor * this->settings.rate;
            if (porta_cur_samples > 0)
            {
              porta_cur_samples = 0;
            }
            else
            {
              porta_from = voices[voices.size() - 2].main.pitch;
            }
            porta_to = voices[voices.size() - 1].main.pitch;
          }
          else
          {
            porta_samples = 0;
            porta_cur_samples = 0;
            porta_from = voices.back().main.pitch;
            porta_to = voices.back().main.pitch;
            const bool had_released = mono.amp_adsr.released();
            mono = std::move(voices.back());
            if (!had_released)
            {
              mono.amp_adsr.resetSoft();
              mono.filt_adsr.resetSoft();
            }
            else
            {
              mono.amp_adsr.reset();
              mono.filt_adsr.reset();
            }
          }
        }
        else
        {
          goto note_off; // hehe.jpg
        }
        // this is because some synths sadly send a note_on with vel. 0 to say note off
        break;
      }
      case libremidi::message_type::NOTE_OFF:
      {
      note_off:
        for (auto it = voices.rbegin(); it != voices.rend();)
        {
          if (it->main.pitch == m.bytes[1])
          {
            switch (inputs.poly_mode)
            {
              case decltype(inputs.poly_mode.value)::Mono:
              {
                if (voices.size() == 1)
                {
                  // The currently playing note needs release if we kill it
                  mono.stop();
                  voices.clear();
                  return;
                }
                else
                {
                  it = decltype(it)(voices.erase(std::next(it).base()));

                  if (voices.size() >= 2)
                  {
                    porta_from = voices[voices.size() - 2].main.pitch;
                    porta_to = voices[voices.size() - 1].main.pitch;
                  }
                  else
                  {
                    porta_from = voices.back().main.pitch;
                    porta_to = voices.back().main.pitch;
                  }
                }
                break;
              }
              case decltype(inputs.poly_mode.value)::Poly:
              {
                // We don't remove it directly, we just signal the ADSR that it gotta
                // start the release
                it->stop();
                ++it;
                break;
              }
            }
            break;
          }
          else
          {
            ++it;
          }
        }
        break;
      }
      default:
        break;
    }
  }
}

void Synthimi::process_voices(int frames, double* l, double* r)
{
  for (auto it = this->voices.active.begin(); it != this->voices.active.end();)
  {
    auto& voice = *it;
    voice.update_envelope(*this);
    if (voice.amp_adsr.done())
    {
      it = this->voices.active.erase(it);
    }
    else
    {
      ++it;
    }
  }

  // For each frame, process all the running voices
  const int upsample_frames = frames * upsample_factor;
  this->mono.update_envelope(*this);
  switch (inputs.poly_mode)
  {
    case decltype(inputs.poly_mode.value)::Mono:
    {
      if (this->mono.amp_adsr.done() || (porta_from == -1 && porta_to == -1))
        return;

      for (int i = 0; i < upsample_frames; i++)
      {
        if (porta_cur_samples < porta_samples && porta_to != porta_from)
        {
          const double porta_increment = (porta_to - porta_from) / porta_samples;
          porta_cur_samples++;
          mono.increment_pitch(*this, porta_increment);
        }
        else
        {
          mono.set_pitch(*this, porta_to);
        }

        auto [lx, rx] = mono.run(*this);
        l[i] += lx;
        r[i] += rx;
      }

      break;
    }
    case decltype(inputs.poly_mode.value)::Poly:
    {
      if (this->voices.active.empty())
        return;

      for (int i = 0; i < upsample_frames; i++)
      {
        for (auto& voice : this->voices.active)
        {
          auto [lx, rx] = voice.run(*this);
          l[i] += lx;
          r[i] += rx;
        }
      }
      break;
    }
  }
}

void Synthimi::postprocess(int frames, double* l, double* r)
{
  const int upsample_frames = frames * upsample_factor;

  // Postprocess
  const double drive = 1. + 10. * this->inputs.drive;
  if (this->inputs.drive > 0)
  {
    for (int i = 0; i < upsample_frames; i++)
    {
      l[i] = std::tanh(drive * l[i]);
      r[i] = std::tanh(drive * r[i]);
    }
  }

  // Resample
  double* lptr;
  double* rptr;
  this->resample_l->process(l, upsample_frames, lptr);
  this->resample_r->process(r, upsample_frames, rptr);
  std::copy_n(lptr, frames, outputs.audio.samples[0]);
  std::copy_n(rptr, frames, outputs.audio.samples[1]);
}

static double wave(Waveform::enum_type t, const double ph) noexcept
{
  switch (t)
  {
    default:
    case Waveform::Sine:
      return kfr::sine(ph);
    case Waveform::Square:
      return kfr::square(ph);
    case Waveform::Triangle:
      return kfr::triangle(ph);
    case Waveform::Sawtooth:
      return kfr::sawtooth(ph);
    case Waveform::Noise:
      return 2. * double(std::rand()) / RAND_MAX - 1.;
  }
}

HALP_INLINE_FLATTEN void Voice::set_freq(Synthimi& synth)
{
  main.set_freq(synth);
  for (auto& sub : unison)
  {
    sub.set_freq(synth);
  }
}

void Voice::update_envelope(Synthimi& synth)
{
  amp_adsr.attack(synth.inputs.amp_attack);
  amp_adsr.decay(synth.inputs.amp_decay);
  amp_adsr.sustain(synth.inputs.amp_sustain);
  amp_adsr.release(synth.inputs.amp_release);

  filt_adsr.attack(synth.inputs.filt_attack);
  filt_adsr.decay(synth.inputs.filt_decay);
  filt_adsr.sustain(synth.inputs.filt_sustain);
  filt_adsr.release(synth.inputs.filt_release);
}

HALP_INLINE_FLATTEN double Subvoice::run(Voice& v, Synthimi& s)
{
  auto& p = s.inputs;

  nan_detector x{};

  // 1. Run the oscillators
  nan_detector wf[4] = {0.};
  switch (p.matrix)
  {
    case decltype(p.matrix)::S: // Sum
    {
      wf[0] = wave(p.osc0_waveform, this->phase[0]);
      wf[1] = wave(p.osc1_waveform, this->phase[1]);
      wf[2] = wave(p.osc2_waveform, this->phase[2]);
      wf[3] = wave(p.osc3_waveform, this->phase[3]);

      x += 0.25 * p.osc0_amp * wf[0];
      x += 0.25 * p.osc1_amp * wf[1];
      x += 0.25 * p.osc2_amp * wf[2];
      x += 0.25 * p.osc3_amp * wf[3];

      break;
    }
    case decltype(p.matrix)::C: // Chain
    {
      wf[0] = p.osc0_amp * wave(p.osc0_waveform, this->phase[0]);
      wf[1] = p.osc1_amp * wave(p.osc1_waveform, wf[0] + this->phase[1]);
      wf[2] = p.osc2_amp * wave(p.osc2_waveform, wf[1] + this->phase[2]);
      wf[3] = p.osc3_amp * wave(p.osc3_waveform, wf[2] + this->phase[3]);

      x = wf[3];

      break;
    }
    case decltype(p.matrix)::CSP: // Chain Sum +
    {
      wf[0] = p.osc0_amp * wave(p.osc0_waveform, this->phase[0]);
      wf[1] = p.osc1_amp * wave(p.osc1_waveform, wf[0] + this->phase[1]);
      wf[2] = p.osc2_amp * wave(p.osc2_waveform, this->phase[2]);
      wf[3] = p.osc3_amp * wave(p.osc3_waveform, wf[2] + this->phase[3]);

      x = 0.5 * (wf[1] + wf[3]);

      break;
    }
    case decltype(p.matrix)::CST: // Chain Sum *
    {
      wf[0] = p.osc0_amp * wave(p.osc0_waveform, this->phase[0]);
      wf[1] = p.osc1_amp * wave(p.osc1_waveform, wf[0] * this->phase[1]);
      wf[2] = p.osc2_amp * wave(p.osc2_waveform, this->phase[2]);
      wf[3] = p.osc3_amp * wave(p.osc3_waveform, wf[2] * this->phase[3]);

      x = 0.5 * (wf[1] + wf[3]);

      break;
    }
    case decltype(p.matrix)::CSC: // Chain Sum Chain
    {
      wf[0] = p.osc0_amp * wave(p.osc0_waveform, this->phase[0]);
      wf[1] = p.osc1_amp * wave(p.osc1_waveform, wf[0] + this->phase[1]);
      wf[2] = p.osc2_amp * wave(p.osc2_waveform, this->phase[2]);
      wf[3] = p.osc3_amp * wave(p.osc3_waveform, wf[1] + wf[2] + this->phase[3]);

      x = wf[3];

      break;
    }
  }

  // 2. Increase the phase
  this->phase[0] += this->phase_incr[0];
  this->phase[1] += this->phase_incr[1];
  this->phase[2] += this->phase_incr[2];
  this->phase[3] += this->phase_incr[3];

  if (this->phase[0] > ossia::two_pi)
    this->phase[0] -= ossia::two_pi;
  if (this->phase[1] > ossia::two_pi)
    this->phase[1] -= ossia::two_pi;
  if (this->phase[2] > ossia::two_pi)
    this->phase[2] -= ossia::two_pi;
  if (this->phase[3] > ossia::two_pi)
    this->phase[3] -= ossia::two_pi;
  return x;
}

HALP_INLINE_FLATTEN void Subvoice::set_freq(Synthimi& s)
{
  const auto& p = s.inputs;
  const nan_detector rf = ossia::two_pi / double(upsample_factor * s.settings.rate);
  this->phase_incr[0] = m2f(p.osc0_pitch + p.osc0_oct * 12. + pitch) * rf;
  this->phase_incr[1] = m2f(p.osc1_pitch + p.osc1_oct * 12. + pitch) * rf;
  this->phase_incr[2] = m2f(p.osc2_pitch + p.osc2_oct * 12. + pitch) * rf;
  this->phase_incr[3] = m2f(p.osc3_pitch + p.osc3_oct * 12. + pitch) * rf;
}

Frame Voice::run(Synthimi& synth)
{
  auto& p = synth.inputs;

  const double x0 = main.run(*this, synth) * main.ampl;
  Frame res = {x0, x0};
  for (int i = 0; i < p.unison; i++)
  {
    const double x = unison[i].run(*this, synth);
    res.l += unison[i].ampl * unison[i].pan * x;
    res.r += unison[i].ampl * (1. - unison[i].pan) * x;
  }

  // 3. Apply envelope

  const double env = this->amp_adsr();
  res.l *= env;
  res.r *= env;

  // 4. Apply filter
  // we'll also fetch it from a lib.. DSPFilters it is !
  if (synth.inputs.filt_env)
  {
    double cutoff_mult = this->filt_adsr();
    double* arr[2] = {&res.l, &res.r};

    double cutf = cutoff_mult * p.filt_cutoff;

    Dsp::Params params;
    params[0] = upsample_factor * synth.settings.rate; // sample rate
    params[2] = synth.inputs.filt_res;                 // Q
    switch (p.filt_type)
    {
      case decltype(p.filt_type)::LPF:
      {
        if (cutf < 20.)
          cutf = 20.;
        params[1] = cutf; // cutoff frequency
        this->lowpassFilter.setParams(params);
        this->lowpassFilter.process(1, arr);

        break;
      }
      case decltype(p.filt_type)::HPF:
      {
        if (cutf > 18000.)
          cutf = 18000.;
        params[1] = cutf; // cutoff frequency
        this->highpassFilter.setParams(params);
        this->highpassFilter.process(1, arr);
        break;
      }
    }
  }

  return res;
}

void Voice::stop()
{
  this->amp_adsr.release();
}

}
