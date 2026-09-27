// FoMo with its envelope times at their minimum of 0: the note still sounds,
// stays finite, and ends on its note-off.
#include <Fomo/Fomo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
struct Rig
{
  Synthimi::Fomo fomo;
  static constexpr int frames = 256;
  std::vector<float> l = std::vector<float>(frames), r = std::vector<float>(frames);
  float* channels[2]{l.data(), r.data()};

  Rig()
  {
    fomo.prepare(
        halp::setup{.input_channels = 0, .output_channels = 2, .frames = frames, .rate = 48000.});
    fomo.outputs.audio.samples = channels;
    fomo.outputs.audio.channels = 2;
  }
  void push(unsigned char status, unsigned char d1, unsigned char d2)
  {
    halp::midi_msg msg;
    msg.bytes = {status, d1, d2};
    fomo.inputs.midi.midi_messages.push_back(msg);
  }
  //! Runs `seconds` of audio; the peak of the last block.
  float run(double seconds)
  {
    float peak = 0.f;
    for(int b = 0; b < std::max(1, int(seconds * 48000. / frames)); b++)
    {
      fomo(frames);
      fomo.inputs.midi.midi_messages.clear();
      peak = 0.f;
      for(int i = 0; i < frames; i++)
      {
        REQUIRE(std::isfinite(l[i]));
        REQUIRE(std::isfinite(r[i]));
        peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
      }
    }
    return peak;
  }
};
}

TEST_CASE("FoMo with envelope times of 0", "[synthimi][fomo]")
{
  auto rig = std::make_unique<Rig>();
  auto& in = rig->fomo.inputs;
  for(auto* t : {&in.attack_0.value, &in.decay_0.value, &in.release_0.value,
                 &in.attack_1.value, &in.decay_1.value, &in.release_1.value,
                 &in.attack_2.value, &in.decay_2.value, &in.release_2.value,
                 &in.attack_3.value, &in.decay_3.value, &in.release_3.value,
                 &in.peg_attack.value, &in.peg_decay.value, &in.lfo_delay.value})
    *t = 0.f;
  in.peg_depth.value = 12.f;

  rig->push(0x90, 60, 100);
  CHECK(rig->run(0.05) > 1e-3);
  rig->push(0x80, 60, 0);
  CHECK(rig->run(0.2) < 1e-4);
}
