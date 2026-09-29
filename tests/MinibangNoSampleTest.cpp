// A note sent to Minibang before a sample is loaded.
//
// DrumChannel::trigger(ts, vel) declines to start a voice when there is no
// sample to play -- no soundfile, or one that decoded to nothing -- and
// returns null. The pitch-ratio overload next to it dereferenced that without
// looking, and Minibang is its only caller: Kabang uses the two-argument form
// and ignores the result, which is why only Minibang crashed.

#include <Kabang/Kabang.hpp>
#include <Kabang/Minibang.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Minibang survives a note with no sample loaded", "[unit][synthimi]")
{
  kbng::DrumChannel channel;
  REQUIRE(channel.sample.soundfile.channels == 0);

  // Both overloads, since the guarded one is what the other relies on.
  CHECK(channel.trigger(0, 100) == nullptr);
  CHECK(channel.trigger(0, 1.5f, 100) == nullptr);

  // And no voice was left half-started behind them.
  CHECK(channel.sample.voices.empty());
}
