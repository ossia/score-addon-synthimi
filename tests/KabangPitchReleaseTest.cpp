#include <Kabang/Kabang.hpp>
#include <Kabang/Minibang.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
template <typename Synth>
struct PitchHarness
{
  static constexpr int rate = 48000;
  std::unique_ptr<Synth> synth = std::make_unique<Synth>();
  std::vector<float> sound = std::vector<float>(rate);
  const float* data[1]{sound.data()};
  std::array<double, 64> left{}, right{};
  double* output[2]{left.data(), right.data()};

  PitchHarness()
  {
    for(int i = 0; i < rate; ++i)
      sound[i] = 0.2f * std::sin(2. * 3.141592653589793 * 1000. * i / rate);
    auto& channel = synth->inputs.s1;
    channel.sample.soundfile = {
        .data = data, .frames = rate, .channels = 1, .rate = rate};
    channel.pitch_env_enable.value = true;
    channel.pitch_attack.value = 0.001f;
    channel.pitch_decay.value = 0.01f;
    channel.pitch_sustain.value = 1.f;
    channel.pitch_release.value = 0.02f;
    channel.amp_env_enable.value = false;
    synth->inputs.volume.value = 1.f;
    synth->outputs.audio.samples = output;
  }

  ~PitchHarness()
  {
    synth->for_each_channel([](kbng::DrumChannel& channel) {
      for(auto& voice : channel.sample.voice_pool)
        delete voice.rubberBand;
    });
  }

  void message(unsigned char status, unsigned char velocity)
  {
    synth->inputs.midi.midi_messages.push_back(
        {.bytes = {status, 38, velocity}, .timestamp = 0});
  }

  void render(int frames)
  {
    while(frames > 0)
    {
      const int count = std::min(frames, int(left.size()));
      left.fill(0.);
      right.fill(0.);
      (*synth)(halp::tick{count});
      synth->inputs.midi.midi_messages.clear();
      frames -= count;
    }
  }
};

template <typename Synth>
void check_note_release(unsigned char status)
{
  PitchHarness<Synth> h;
  h.message(0x90, 100);
  h.render(4800);
  auto& voices = h.synth->inputs.s1.sample.voices;
  REQUIRE(voices.size() == 1);
  auto* voice = voices.front();
  REQUIRE(voice->rubberBand->getPitchScale() > 3.9);

  h.message(status, 0);
  h.render(4800);
  REQUIRE(voices.size() == 1);
  CHECK(voice->pitch_adsr.released());
  CHECK(voice->pitch_adsr.done());
  CHECK(std::abs(voice->rubberBand->getPitchScale() - 1.) < 1e-6);
}
}

TEST_CASE("Kabang pitch returns to base after note-off", "[synthimi][kabang][pitch]")
{
  SECTION("explicit note-off") { check_note_release<kbng::Kabang>(0x80); }
  SECTION("zero-velocity note-on") { check_note_release<kbng::Kabang>(0x90); }
}

TEST_CASE("Minibang pitch returns to base after note-off", "[synthimi][minibang][pitch]")
{
  SECTION("explicit note-off") { check_note_release<kbng::Minibang>(0x80); }
  SECTION("zero-velocity note-on") { check_note_release<kbng::Minibang>(0x90); }
}

TEST_CASE("Sample exhaustion releases sustained pitch", "[synthimi][kabang][pitch]")
{
  PitchHarness<kbng::Kabang> h;
  auto& channel = h.synth->inputs.s1;
  channel.sample.soundfile.frames = 2400;
  channel.amp_env_enable.value = true;
  channel.amp_release.value = 0.5f;
  h.message(0x90, 100);
  h.render(64);
  REQUIRE(channel.sample.voices.size() == 1);
  auto* voice = channel.sample.voices.front();

  h.render(12000);
  REQUIRE(channel.sample.voices.size() == 1);
  CHECK(voice->pitch_adsr.released());
  CHECK(voice->pitch_adsr.done());
  CHECK(std::abs(voice->rubberBand->getPitchScale() - 1.) < 1e-6);
}

TEST_CASE("Kabang channels retain independent signed pitch amounts",
          "[synthimi][kabang][pitch]")
{
  PitchHarness<kbng::Kabang> h;
  auto& inputs = h.synth->inputs;
  auto& second = inputs.s2;
  second.sample.soundfile = inputs.s1.sample.soundfile;
  second.midi_key.value = 39;
  second.pitch_env_enable.value = true;
  second.pitch_attack.value = 0.001f;
  second.pitch_decay.value = 0.01f;
  second.pitch_sustain.value = 1.f;
  second.amp_env_enable.value = false;
  inputs.pitch_amount_1.value = 12.f;
  inputs.pitch_amount_2.value = -12.f;
  h.message(0x90, 100);
  inputs.midi.midi_messages.push_back({.bytes = {0x90, 39, 100}, .timestamp = 0});
  h.render(4800);

  REQUIRE(inputs.s1.sample.voices.size() == 1);
  REQUIRE(second.sample.voices.size() == 1);
  CHECK(std::abs(inputs.s1.sample.voices.front()->rubberBand->getPitchScale() - 2.) < 1e-6);
  CHECK(std::abs(second.sample.voices.front()->rubberBand->getPitchScale() - 0.5) < 1e-6);
}

TEST_CASE("Minibang negative pitch amount rises back to base on release",
          "[synthimi][minibang][pitch]")
{
  PitchHarness<kbng::Minibang> h;
  h.synth->inputs.pitch_amount_1.value = -12.f;
  h.message(0x90, 100);
  h.render(4800);
  auto& voices = h.synth->inputs.s1.sample.voices;
  REQUIRE(voices.size() == 1);
  CHECK(std::abs(voices.front()->rubberBand->getPitchScale() - 0.5) < 1e-6);
  h.message(0x80, 0);
  h.render(4800);
  REQUIRE(voices.size() == 1);
  CHECK(std::abs(voices.front()->rubberBand->getPitchScale() - 1.) < 1e-6);
}
