// license:BSD-3-Clause
// copyright-holders:Devin Acker

/***************************************************************************
    NEC/Casio uPD933 "Phase Distortion" synthesis chip

    Curated from MAME (src/devices/sound/upd933.cpp): see PROVENANCE.
    Test-only reference model.
***************************************************************************/

#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace upd933_ref
{
class upd933
{
public:
  static constexpr unsigned NOTE_SHIFT = 9;
  static constexpr unsigned PITCH_SHIFT = 20;
  static constexpr unsigned PITCH_FINE_SHIFT = 12;
  static constexpr unsigned VOLUME_SHIFT = 12;

  static constexpr unsigned ENV_DCA_SHIFT = 16;
  static constexpr unsigned ENV_DCW_SHIFT = 16;
  static constexpr unsigned ENV_DCO_SHIFT = 11;

  static constexpr double SAMPLE_RATE = 40000.;

  upd933()
  {
    for(int i = 0; i < 0x800; i++)
      m_cosine[i] = std::uint16_t(0xfff * (1 - cos(2.0 * std::numbers::pi * i / 0x7ff)) / 2);

    for(int i = 0; i < 0x80; i++)
    {
      // A4 is note 62, 442 Hz
      const double freq = 442.0 * pow(2, (i - 62) / 12.0);
      m_pitch[i] = std::uint32_t((1 << PITCH_SHIFT) * (freq * 0x800 / 40000));
    }

    for(int i = 0; i < 0x200; i++)
      m_pitch_fine[i]
          = std::uint16_t((1 << PITCH_FINE_SHIFT) * (pow(2, (double)i / (12.0 * 0x200)) - 1));

    // logarithmic volume curve, also scales 12-bit waveform to 13-bit range
    for(int i = 1; i < 0x200; i++)
      m_volume[i] = std::uint16_t(pow(2 << VOLUME_SHIFT, (double)i / 0x1ff));
    m_volume[0] = 0;
  }

  static unsigned BIT(unsigned x, unsigned n) { return (x >> n) & 1u; }
  static unsigned BIT(unsigned x, unsigned n, unsigned w) { return (x >> n) & ((1u << w) - 1u); }

  void write(std::uint8_t reg, std::uint16_t value)
  {
    const int vnum = reg & 7;
    voice_t& voice = m_voice[vnum];
    voice_t& mod_voice = m_voice[(vnum + 6) & 7];

    switch(reg >> 3)
    {
      case 0x0: // 00-07: DCA step
      {
        env_t& dca = m_dca[vnum];
        dca.m_direction = BIT(value, 15);
        dca.m_rate = env_rate(BIT(value, 8, 7));
        dca.m_sustain = BIT(value, 7);
        dca.m_target = BIT(value, 0, 7) << (ENV_DCA_SHIFT + 2);
        dca.m_irq = false;
      }
      break;

      case 0x2: // 10-17: DCO step
      {
        env_t& dco = m_dco[vnum];
        dco.m_direction = BIT(value, 15);
        dco.m_rate = env_rate(BIT(value, 8, 7));
        dco.m_sustain = BIT(value, 7);
        dco.m_target = BIT(value, 0, 6) << (ENV_DCO_SHIFT + 5);
        if(BIT(value, 6))
          dco.m_target <<= 5;
        dco.m_irq = false;
      }
      break;

      case 0x4: // 20-27: DCW step
      {
        env_t& dcw = m_dcw[vnum];
        dcw.m_direction = BIT(value, 15);
        dcw.m_rate = env_rate(BIT(value, 8, 7));
        dcw.m_sustain = BIT(value, 7);
        dcw.m_target = BIT(value, 0, 7) << (ENV_DCW_SHIFT + 3);
        dcw.m_irq = false;
      }
      break;

      case 0xc: // 60-67: pitch (in semitones, as 7.9 fixed point)
        voice.m_pitch = value;
        update_pitch_step(vnum);
        break;

      case 0xd: // 68-6f: waveform
        voice.m_wave[0] = std::uint8_t(BIT(value, 13, 3));
        if(BIT(value, 9))
          voice.m_wave[1] = std::uint8_t(BIT(value, 10, 3));
        else
          voice.m_wave[1] = voice.m_wave[0];
        voice.m_window = std::uint8_t(BIT(value, 6, 3));
        if(!BIT(vnum, 0))
        {
          mod_voice.m_ring_mod = std::uint8_t(BIT(value, 5));
          mod_voice.m_pitch_mod = std::uint8_t(BIT(value, 3, 2));
          mod_voice.m_mute_other = std::uint8_t(BIT(value, 2));
        }
        break;

      case 0x13: // 98-9f: phase counter
        voice.m_position = std::uint32_t(value) << (PITCH_SHIFT - 4);
        break;

      case 0x17: // b8-bb: pitch modulator
        if(vnum < 4)
          m_voice[vnum << 1].m_pm_level = (std::int16_t)value;
        break;

      default:
        break;
    }
  }

  //! One 40 kHz sample, the sum of the eight voices
  std::int32_t sample()
  {
    std::int32_t s = 0;
    static const int voice_map[] = {5, 0, 7, 2, 1, 4, 3, 6};
    for(int j : voice_map)
      s += update(j);
    m_sample_count++;
    return std::clamp(s, -(1 << 15), (1 << 15) - 1);
  }

  //! One voice only, without advancing the others (for single-voice tests)
  std::int16_t sampleVoice(int vnum)
  {
    const auto s = update(vnum);
    m_sample_count++;
    return s;
  }

  struct env_t
  {
    std::uint8_t m_direction = 0, m_sustain = 1, m_irq = 0;
    std::uint32_t m_rate = 0, m_target = 0, m_current = 0;

    void update()
    {
      if(m_current != m_target)
      {
        if(!m_direction)
        {
          if(m_current > m_target || m_target - m_current <= m_rate)
            m_current = m_target;
          else
            m_current += m_rate;
        }
        else
        {
          if(m_current < m_target || m_current - m_target <= m_rate)
            m_current = m_target;
          else
            m_current -= m_rate;
        }
      }

      if(!m_sustain && (m_current == m_target))
        m_irq = m_sustain = true;
    }
  };

  struct voice_t
  {
    std::uint8_t m_wave[2] = {0};
    std::uint8_t m_window = 0, m_ring_mod = 0, m_pitch_mod = 0, m_mute_other = 0;

    std::uint16_t m_pitch = 0;
    std::uint32_t m_position = 0, m_pitch_step = 0;
    std::uint16_t m_dcw_limit = 0;
    std::int16_t m_pm_level = 0;
  };

  std::uint32_t env_rate(std::uint8_t data) const { return (8u | (data & 7u)) << (data >> 3); }

  std::int16_t update(int vnum)
  {
    voice_t& voice = m_voice[vnum];
    std::int16_t sample = 0;

    const std::uint16_t pos = std::uint16_t(BIT(voice.m_position, PITCH_SHIFT, 11));
    const std::uint8_t wave = std::uint8_t(BIT(voice.m_position, PITCH_SHIFT + 11));

    const std::uint16_t dcw
        = std::min(std::uint16_t(m_dcw[vnum].m_current >> ENV_DCW_SHIFT), voice.m_dcw_limit);
    const std::uint16_t pivot = std::uint16_t(0x400 - dcw);
    std::uint16_t phase = 0;
    std::uint16_t window = 0;

    switch(voice.m_wave[wave] & 7)
    {
      case 0: // sawtooth
        if(pos < pivot)
          phase = std::uint16_t(pos * 0x400 / pivot);
        else
          phase = std::uint16_t(0x400 + (pos - pivot) * 0x400 / (0x800 - pivot));
        break;
      case 1: // square
        if((pos & 0x3ff) < pivot)
          phase = std::uint16_t((pos & 0x3ff) * 0x400 / pivot);
        else
          phase = 0x3ff;
        phase |= (pos & 0x400);
        break;
      case 2: // pulse
        if(pos < pivot * 2)
          phase = std::uint16_t(pos * 0x800 / (pivot * 2));
        else
          phase = 0x7ff;
        break;
      case 3: // silent (undocumented)
        break;
      case 4: // double sine
        if(pos < pivot)
          phase = std::uint16_t(pos * 0x800 / pivot);
        else
          phase = std::uint16_t((pos - pivot) * 0x800 / (0x800 - pivot));
        break;
      case 5: // saw pulse
        if(pos < 0x400)
          phase = pos;
        else if(pos < (pivot + 0x400))
          phase = std::uint16_t(0x400 + (pos & 0x3ff) * 0x400 / pivot);
        else
          phase = 0x7ff;
        break;
      case 6: // resonance
        phase = std::uint16_t(pos + ((pos * dcw) >> 6));
        phase &= 0x7ff;
        break;
      case 7: // double pulse (undocumented)
        if((pos & 0x3ff) < pivot)
          phase = std::uint16_t((pos & 0x3ff) * 0x400 / pivot);
        else
          phase = 0x7ff;
        break;
    }

    switch(voice.m_window & 7)
    {
      case 0:
        break;
      case 1:
        window = pos;
        break;
      case 2:
        window = std::uint16_t((pos & 0x3ff) * 2);
        if(pos < 0x400)
          window ^= 0x7fe;
        break;
      case 3:
        if(pos >= 0x400)
          window = std::uint16_t((pos & 0x3ff) * 2);
        break;
      case 4:
        if(pos < 0x400)
          window = std::uint16_t(pos * 2);
        else
          window = 0x7ff;
        break;
      default:
        window = std::uint16_t((0x3ff ^ (pos & 0x3ff)) * 2);
        break;
    }

    sample = std::int16_t(m_cosine[phase]);
    if(window)
      sample = std::int16_t(((std::int32_t)sample * (0x800 - window)) / 0x800);

    const std::uint16_t volume = std::uint16_t(m_dca[vnum].m_current >> ENV_DCA_SHIFT);
    sample = std::int16_t(((std::int32_t)sample * m_volume[volume]) >> VOLUME_SHIFT);
    sample = std::int16_t(sample - m_volume[volume] / 2);

    if(voice.m_ring_mod)
      sample = std::int16_t(((std::int32_t)sample * m_last_sample) / 0x1000);

    if(voice.m_mute_other)
      sample = std::int16_t(sample - m_last_sample);

    const std::uint32_t old_dco = m_dco[vnum].m_current;
    const std::int16_t old_pm = voice.m_pm_level;

    m_dca[vnum].update();
    m_dcw[vnum].update();
    m_dco[vnum].update();

    if(!(m_sample_count & 7))
    {
      switch(voice.m_pitch_mod & 3)
      {
        default:
          voice.m_pm_level = 0;
          break;
        case 2:
          voice.m_pm_level = m_last_sample;
          break;
        case 3:
          voice.m_pm_level = std::int16_t(rand() & (32 << NOTE_SHIFT));
          break;
      }
    }

    if((old_dco ^ m_dco[vnum].m_current) >> ENV_DCO_SHIFT || old_pm != voice.m_pm_level)
      update_pitch_step(vnum);

    voice.m_position += voice.m_pitch_step;

    m_last_sample = sample;
    return sample;
  }

  void update_pitch_step(int vnum)
  {
    voice_t& voice = m_voice[vnum];
    const std::int32_t pitch
        = std::int32_t(voice.m_pitch + (m_dco[vnum].m_current >> ENV_DCO_SHIFT))
          + voice.m_pm_level;
    std::uint32_t step = 0;

    if(pitch > 0 && pitch < (1 << 16))
    {
      const std::uint8_t note = std::uint8_t(pitch >> NOTE_SHIFT);
      const std::uint16_t fine = std::uint16_t(pitch & ((1 << NOTE_SHIFT) - 1));
      step = m_pitch[note];
      if(fine)
        step += (step >> PITCH_FINE_SHIFT) * m_pitch_fine[fine];
    }

    voice.m_pitch_step = step;
    voice.m_dcw_limit
        = std::uint16_t(0x400 - std::min(0x400U, (step >> (PITCH_SHIFT - 2))));
  }

  std::uint32_t rand()
  {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
  }

  // Public for the tests: they set envelopes to their targets directly
  std::array<voice_t, 8> m_voice{};
  std::array<env_t, 8> m_dca{}, m_dco{}, m_dcw{};

private:
  std::uint16_t m_cosine[0x800]{};
  std::uint32_t m_pitch[0x80]{};
  std::uint16_t m_pitch_fine[0x200]{};
  std::uint16_t m_volume[0x200]{};

  std::uint32_t m_sample_count = 0;
  std::int16_t m_last_sample = 0;
  std::uint32_t m_rng = 0x12345678u;
};
}
