// Switching Synthimi between Poly and Mono while notes play.
#include <Synthimi/SynthimiModel.hpp>

#include <libremidi/message.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
struct Rig
{
  Synthimi::Synthimi s;
  std::vector<double> l, r;
  double* channels[2]{};
  static constexpr int frames = 256;

  Rig()
  {
    l.resize(frames);
    r.resize(frames);
    s.prepare(halp::setup{.input_channels = 0, .output_channels = 2, .frames = frames, .rate = 48000.});
    channels[0] = l.data();
    channels[1] = r.data();
    s.outputs.audio.samples = channels;
  }
  void on(int note) { push(libremidi::channel_events::note_on(1, note, 100)); }
  void off(int note) { push(libremidi::channel_events::note_off(1, note, 0)); }
  void push(const libremidi::message& m)
  {
    halp::midi_msg msg;
    msg.bytes.assign(m.bytes.begin(), m.bytes.end());
    s.inputs.midi.midi_messages.push_back(msg);
  }
  //! Runs `seconds` of audio; returns the peak of the last block.
  double run(double seconds)
  {
    double peak = 0.;
    const int blocks = std::max(1, int(seconds * 48000. / frames));
    for(int b = 0; b < blocks; b++)
    {
      s(halp::tick{frames});
      s.inputs.midi.midi_messages.clear();
      peak = 0.;
      for(int i = 0; i < frames; i++)
        peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
    }
    return peak;
  }
};
}

TEST_CASE("Synthimi switched from poly to mono plays and stops its notes", "[synthimi]")
{
  // Large (fixed voice storage): not on the stack.
  auto rigp = std::make_unique<Rig>();
  auto& rig = *rigp;
  using Mode = decltype(rig.s.inputs.poly_mode.value);
  rig.s.inputs.poly_mode.value = Mode::Poly;
  rig.on(60);
  rig.on(64);
  rig.run(0.2);
  rig.off(64); // a released poly voice still ringing at the switch

  rig.s.inputs.poly_mode.value = Mode::Mono;
  rig.on(67);
  CHECK(rig.run(0.2) > 1e-3); // the mono note sounds
  rig.off(67);
  rig.off(60); // held across the switch: finds nothing, harmless
  CHECK(rig.run(3.) < 1e-4); // and nothing is left sounding
}

TEST_CASE("Synthimi switched from mono to poly stops the mono voice", "[synthimi]")
{
  // Large (fixed voice storage): not on the stack.
  auto rigp = std::make_unique<Rig>();
  auto& rig = *rigp;
  using Mode = decltype(rig.s.inputs.poly_mode.value);
  rig.s.inputs.poly_mode.value = Mode::Mono;
  rig.on(60);
  rig.on(62); // legato
  rig.run(0.2);
  rig.s.inputs.poly_mode.value = Mode::Poly;
  rig.on(64);
  CHECK(rig.run(0.2) > 1e-3);
  rig.off(64);
  rig.off(60);
  rig.off(62);
  CHECK(rig.run(3.) < 1e-4);

  // And back to mono, a fresh note plays
  rig.s.inputs.poly_mode.value = Mode::Mono;
  rig.on(65);
  CHECK(rig.run(0.2) > 1e-3);
  rig.off(65);
  CHECK(rig.run(3.) < 1e-4);
}

// The first note of a poly passage must not gate the mono voice: in Poly
// nothing renders nor releases it, so switching to Mono while it is held
// would let it sound forever.
TEST_CASE("Synthimi switched to mono with poly notes held leaves nothing stuck", "[synthimi]")
{
  auto rigp = std::make_unique<Rig>();
  auto& rig = *rigp;
  using Mode = decltype(rig.s.inputs.poly_mode.value);
  rig.s.inputs.poly_mode.value = Mode::Poly;
  rig.on(60);
  rig.on(64);
  rig.run(0.2);
  rig.s.inputs.poly_mode.value = Mode::Mono;
  rig.run(0.1);
  rig.off(60);
  rig.off(64);
  CHECK(rig.run(3.) < 1e-4);
}

TEST_CASE("Synthimi in poly leaves every voice to its own note-off", "[synthimi]")
{
  auto rigp = std::make_unique<Rig>();
  auto& rig = *rigp;
  using Mode = decltype(rig.s.inputs.poly_mode.value);
  rig.s.inputs.poly_mode.value = Mode::Poly;
  rig.on(60);
  rig.run(0.1);
  rig.on(64);
  CHECK(rig.run(0.2) > 1e-3);
  rig.off(60);
  rig.off(64);
  CHECK(rig.run(3.) < 1e-4);
}

// A poly passage, all released, then Mono: the mono voice must not have been
// gated by the first poly note, or it sounds with no key held.
TEST_CASE("Synthimi is silent after switching to mono with no key held", "[synthimi]")
{
  auto rigp = std::make_unique<Rig>();
  auto& rig = *rigp;
  using Mode = decltype(rig.s.inputs.poly_mode.value);
  rig.s.inputs.poly_mode.value = Mode::Poly;
  rig.on(60);
  rig.on(64);
  CHECK(rig.run(0.2) > 1e-3);
  rig.off(60);
  rig.off(64);
  CHECK(rig.run(3.) < 1e-4);
  rig.s.inputs.poly_mode.value = Mode::Mono;
  CHECK(rig.run(0.5) < 1e-4);
  rig.on(67);
  CHECK(rig.run(0.2) > 1e-3);
  rig.off(67);
  CHECK(rig.run(3.) < 1e-4);
}
