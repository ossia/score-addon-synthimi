// Kaboom's drums played from the UI's play buttons: the message plays the
// drum it names, once, like a note of its key.
#include <Kaboom/Kaboom.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
struct Rig
{
  kbm::Kaboom kaboom;
  static constexpr int frames = 256;
  std::vector<double> l = std::vector<double>(frames), r = std::vector<double>(frames);
  double* channels[2]{l.data(), r.data()};

  Rig()
  {
    kaboom.prepare(
        halp::setup{.input_channels = 0, .output_channels = 2, .frames = frames, .rate = 48000.});
    kaboom.outputs.audio.samples = channels;
  }
  //! Runs `seconds` of audio; the peak over all of it.
  double run(double seconds)
  {
    double peak = 0.;
    for(int b = 0; b < std::max(1, int(seconds * 48000. / frames)); b++)
    {
      std::fill(l.begin(), l.end(), 0.);
      std::fill(r.begin(), r.end(), 0.);
      kaboom(halp::tick{frames});
      for(int i = 0; i < frames; i++)
      {
        REQUIRE(std::isfinite(l[i]));
        peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
      }
    }
    return peak;
  }
};
}

TEST_CASE("Kaboom plays the drum a play button names", "[synthimi][kaboom]")
{
  auto rig = std::make_unique<Rig>();
  CHECK(rig->run(0.1) < 1e-6);

  // Out of range: nothing
  rig->kaboom.process_message({-1});
  rig->kaboom.process_message({8});
  CHECK(rig->kaboom.pending_plays == 0u);
  CHECK(rig->run(0.1) < 1e-6);

  rig->kaboom.process_message({3});
  CHECK(rig->run(0.2) > 1e-3);
  CHECK(rig->kaboom.pending_plays == 0u);
}

// Only the drum named plays: its level at 0 silences exactly it
TEST_CASE("Kaboom plays no other drum than the one named", "[synthimi][kaboom]")
{
  for(int drum = 0; drum < 8; drum++)
  {
    INFO("drum " << drum);
    auto rig = std::make_unique<Rig>();
    int i = 0;
    rig->kaboom.for_each_channel([&](kbm::DrumChannel& c, kbm::ChannelState&) {
      c.level.value = (i++ == drum) ? 0.f : 1.f;
    });
    rig->kaboom.process_message({drum});
    CHECK(rig->run(0.5) < 1e-6);
  }
}
