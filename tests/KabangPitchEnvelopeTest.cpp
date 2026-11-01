#include <Kabang/PitchEnvelope.hpp>

#include <catch2/catch_all.hpp>

using kbng::pitch_scale;

TEST_CASE("Pitch envelope amount chooses signed musical intervals", "[synthimi][pitch]")
{
  CHECK(pitch_scale(1., 1., 12., true, 0.) == Catch::Approx(2.));
  CHECK(pitch_scale(1., 1., -12., true, 0.) == Catch::Approx(0.5));
  CHECK(pitch_scale(1., 0.5, 24., true, 0.) == Catch::Approx(2.));
  CHECK(pitch_scale(1., 0.5, -24., true, 0.) == Catch::Approx(0.5));
  // Transposition is relative to the sample's existing pitch, not additive Hz.
  CHECK(pitch_scale(0.5, 1., 12., true, 0.) == Catch::Approx(1.));
  CHECK(pitch_scale(2., 1., -12., true, 0.) == Catch::Approx(1.));
}

TEST_CASE("Neutral and completed envelopes preserve base tuning", "[synthimi][pitch]")
{
  CHECK(pitch_scale(1.5, 1., 0., true, 0.8) == Catch::Approx(1.5));
  CHECK(pitch_scale(1.5, 0., -24., true, 0.8) == Catch::Approx(1.5));
  CHECK(pitch_scale(1.5, 1., -24., false, 0.8) == Catch::Approx(1.5));
}

TEST_CASE("Velocity changes depth without choosing its direction", "[synthimi][pitch]")
{
  CHECK(pitch_scale(1., 1., 12., true, 1.) == Catch::Approx(4.));
  CHECK(pitch_scale(1., 1., -12., true, 1.) == Catch::Approx(0.25));
  CHECK(pitch_scale(1., 1., 24., true, -0.5) == Catch::Approx(2.));
  CHECK(pitch_scale(1., 1., -24., true, -0.5) == Catch::Approx(0.5));
  CHECK(pitch_scale(1., 1., -24., true, -1.) == Catch::Approx(1.));
  // A small negative tracking amount must not collapse to the pitch limit.
  CHECK(pitch_scale(1., 0.5, 24., true, -0.01) == Catch::Approx(2.).margin(0.03));
}

TEST_CASE("Pitch envelope respects the stretcher limits", "[synthimi][pitch]")
{
  CHECK(pitch_scale(5., 1., 24., true, 1.) == kbng::max_pitch_scale);
  CHECK(pitch_scale(0.25, 1., -24., true, 1.) == kbng::min_pitch_scale);
  CHECK(pitch_scale(1., 1., 24., true, 0.) == Catch::Approx(4.));
  CHECK(pitch_scale(1., 1., -24., true, 0.) == Catch::Approx(0.25));
}
