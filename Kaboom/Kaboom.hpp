#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Kaboom -- an 8-channel drum synthesiser.
 *
 * Structured after examples/Advanced/Kabang/Kabang.hpp: eight DrumChannel
 * members inside `ins`, each a halp_flag(recursive_group) carrying its own
 * nested ui, surfaced as tabs through halp::recursive_group_item, dispatched
 * by MIDI note number, iterated with for_each_channel.
 *
 * Where Kabang plays samples, this synthesises. See DrumDsp.hpp for the
 * reasoning behind the engines; briefly, all three are the same resonator
 * primitive wearing different hats.
 */
#pragma once

/* DSP primitives for Kaboom, an 8-channel drum synthesiser.
 *
 * The design follows current physical-modelling percussion research rather
 * than analog-drum-machine emulation. Three ideas do most of the work:
 *
 * 1. NONLINEAR EXCITATION.  Avanzini & Rocchesso, "Modeling Collision Sounds:
 *    Non-linear Contact Force" (DAFx-01), applying the Hunt & Crossley (1975)
 *    contact law
 *
 *        f(x, v) = k * x^a  +  lambda * x^a * v      for x > 0
 *
 *    where x is interpenetration and v its rate. The exponent a comes from the
 *    local geometry (3/2 is the Hertzian sphere-on-plane case). What matters
 *    musically is that contact *duration* is not a parameter -- it falls out
 *    of the collision, and it shortens as impact velocity rises. So a harder
 *    hit automatically excites higher modes. That is the physical mechanism
 *    behind velocity-dependent brightness, and it replaces the usual
 *    "velocity -> filter cutoff" patch-up.
 *
 * 2. NONLINEAR PLATE / ENERGY CASCADE.  Ducceschi & Touzé, "Modal approach for
 *    nonlinear vibrations of damped impacted plates: application to sound
 *    synthesis of gongs and cymbals" (J. Sound Vib. 344, 2015), and Bilbao,
 *    Webb, Wang & Ducceschi, "Real-Time Gong Synthesis" (2023). A struck plate
 *    driven hard enters a wave turbulence regime: geometric (von Kármán)
 *    nonlinearity pumps energy from the low modes it was given into a widening
 *    band of high modes. That slow brightening *is* the sound of a crash
 *    cymbal, and a linear modal bank cannot produce it at any parameter
 *    setting -- a linear system only ever redistributes what you excite.
 *
 * 3. STOCHASTIC PARTICLES.  Cook, "Physically Informed Sonic Modeling (PhISM):
 *    Synthesis of Percussive Sounds" (Computer Music J. 21(3), 1997). Shakers,
 *    cabasas and tambourines are not resonators being struck once; they are
 *    many small collisions whose statistics we hear. Modelling the individual
 *    events is both cheaper and more convincing than trying to fake the result
 *    with filtered noise.
 *
 * A note on what is NOT implemented. The full von Kármán modal scheme couples
 * mode triples through a precomputed tensor -- O(N^3) coefficients, and the
 * 2023 real-time work gets its stability from an energy-quadratised
 * (scalar-auxiliary-variable) time integration. What is below is a rank-one
 * reduction of that coupling: the nonlinear force is driven by a single
 * scalar, the surface displacement at the pickup point, which is O(N) and
 * still generates the intermodulation that drives the cascade. Stability is
 * argued differently and more cheaply -- see ModalBank::tick.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace kbm
{

inline constexpr float kPi = 3.14159265358979324f;
inline constexpr float k2Pi = 6.28318530717958648f;

inline constexpr int kMaxModes = 12;

// Coefficients are refreshed every kCtrlDiv samples and held.
inline constexpr int kCtrlDiv = 8;

// Maps the Cascade control onto a *fractional* frequency stretch: every mode's
// frequency is multiplied by (1 + stretch), where stretch is
// cascade * kCascadeScale * w^2 and w, the pickup displacement, is order 1 just
// after a strike. So the top of the control is a couple of semitones of tension
// at full excitation, decaying back to the dialled-in pitch as the sound does --
// which is what a hard-struck plate does. The ceiling keeps a very loud strike
// from turning it into a siren.
inline constexpr float kCascadeScale = 0.0022f;
inline constexpr float kCascadeMaxStretch = 0.35f;

// Input trim for the drive stage. AdaaShaper saturates at exactly 1.0 while the
// modal bank happily peaks at 2, so feeding it the mix directly meant Drive had
// no transparent setting: 0 dB already clipped the attack off every voice. The
// mix is scaled into the shaper's linear region and scaled back out, which makes
// 0 dB unity to within a decibel and lets the control earn its range.
inline constexpr float kDriveHeadroom = 0.25f;

// ============================================================================
// Noise
// ============================================================================
struct Noise
{
  std::uint32_t s{0x9E3779B9u};

  inline float operator()() noexcept
  {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(std::int32_t(s)) * (1.f / 2147483648.f);
  }

  inline float uni() noexcept { return 0.5f * (operator()() + 1.f); }
};

// ============================================================================
// Modal resonator: complex one-pole, z[n] = c*z[n-1] + x[n], c = r*e^(jw).
//
// An impulse gives exactly r^n * cos(w*n): the analytic solution of a struck
// mode. Chosen over a biquad because it stays conditioned at the very high Q
// long tails need, and because the state carries both quadrature components,
// so instantaneous modal energy is just re^2 + im^2 -- which both the tension
// and cascade models want anyway.
// ============================================================================
struct Mode
{
  float re{0.f}, im{0.f};
  float cr{0.f}, ci{0.f};
  // Nominal phase advance per sample, kept so that a frequency perturbation can
  // be expressed as a *fraction* of this mode's own frequency rather than as an
  // absolute number of radians -- see ModalBank::tick.
  float winc{0.f};

  void reset() noexcept { re = im = 0.f; }

  void setCoeffs(float freqHz, float decaySec, double sr) noexcept
  {
    const float f = std::clamp(freqHz, 0.f, float(sr) * 0.49f);
    const float w = k2Pi * f / float(sr);
    const float r = std::exp(-1.f / std::max(1e-4f, decaySec * float(sr)));
    cr = r * std::cos(w);
    ci = r * std::sin(w);
    winc = w;
  }

  inline float tick(float in) noexcept
  {
    const float nr = re * cr - im * ci + in;
    const float ni = re * ci + im * cr;
    re = nr;
    im = ni;
    return re;
  }

  // Perturb this mode's phase by d radians, exactly preserving |z|.
  //   z *= (1 + j*d) / sqrt(1 + d^2)
  // A per-sample frequency perturbation, which is what a displacement-
  // dependent stiffness is. Because the multiplier has unit modulus this can
  // only move energy around the spectrum -- never create any.
  inline void rotate(float d) noexcept
  {
    const float inv = 1.f / std::sqrt(1.f + d * d);
    const float nr = (re - d * im) * inv;
    const float ni = (im + d * re) * inv;
    re = nr;
    im = ni;
  }

  [[nodiscard]] inline float energy() const noexcept { return re * re + im * im; }
};

// ----------------------------------------------------------------------------
// Modal ratio sets.
//   membrane : Bessel zeros, ideal circular membrane -- toms, congas, kicks
//   plate    : free circular plate / gong partials -- cymbals, gongs, metal
//   bar      : free-free bar -- blocks, marimba, cowbell
//   harmonic : integer series -- pitched percussion
// `structure` walks membrane -> harmonic -> bar -> plate.
// ----------------------------------------------------------------------------
inline constexpr std::array<float, kMaxModes> kMembraneRatios{
                                                              1.000f, 1.593f, 2.135f, 2.295f, 2.653f, 2.917f,
                                                              3.155f, 3.500f, 3.598f, 3.652f, 4.060f, 4.230f};
inline constexpr std::array<float, kMaxModes> kHarmonicRatios{
                                                              1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 11.f, 12.f};
inline constexpr std::array<float, kMaxModes> kBarRatios{
                                                         1.000f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f,
                                                         24.82f, 31.87f, 39.81f, 48.63f, 58.33f, 68.91f};
// Free circular plate: nearly quadratic spacing, which is what makes a gong's
// partials pile up in the upper spectrum and gives the cascade somewhere to go
inline constexpr std::array<float, kMaxModes> kPlateRatios{
                                                           1.000f, 2.080f, 3.410f, 3.890f, 5.000f, 6.710f,
                                                           7.800f, 9.010f, 10.65f, 12.43f, 14.02f, 15.80f};

inline float modeRatio(int k, float structure) noexcept
{
  const auto i = std::size_t(std::clamp(k, 0, kMaxModes - 1));
  const float s = std::clamp(structure, 0.f, 1.f) * 3.f;  // 0..3
  const int seg = std::min(2, int(s));
  const float t = s - float(seg);
  const float a[4]{
                   kMembraneRatios[i], kHarmonicRatios[i], kBarRatios[i], kPlateRatios[i]};
  return a[seg] + t * (a[seg + 1] - a[seg]);
}

// ============================================================================
// Modal bank with tension modulation and nonlinear cascade
// ============================================================================
struct ModalBank
{
  std::array<Mode, kMaxModes> modes{};
  std::array<float, kMaxModes> gain{};
  std::array<float, kMaxModes> pick{};  // eigenfunction value at the pickup
  int count{6};

  float baseFreq{100.f};
  float structure{0.f};
  float spread{1.f};
  float decay{0.5f};
  float hfDamp{0.5f};
  float tension{0.f};
  float cascade{0.f};

  float w1{0.f};  // surface displacement, previous sample

  void reset() noexcept
  {
    for(auto& m : modes)
      m.reset();
    w1 = 0.f;
  }

  [[nodiscard]] float energy() const noexcept
  {
    float e = 0.f;
    for(int k = 0; k < count; ++k)
      e += modes[std::size_t(k)].energy();
    return e / float(std::max(1, count));
  }

  // Called once per control period.
  void updateCoeffs(double sr) noexcept
  {
    // Quasistatic tension rises with energy, raising every partial together.
    // Marogna, Avanzini & Bank, "Energy Based Synthesis of Tension Modulation
    // in Membranes" (DAFx-10): the short-time-average tension of a struck
    // membrane is essentially linear in its energy, so the expensive part of
    // the nonlinearity can be had from a quantity we already hold. Energy is
    // highest at the strike, so the glide is downward -- as on a real tom.
    const float stretch = 1.f + tension * std::min(energy(), 4.f);

    for(int k = 0; k < count; ++k)
    {
      const auto i = std::size_t(k);
      const float ratio = modeRatio(k, structure) * std::pow(spread, float(k));
      modes[i].setCoeffs(
          baseFreq * ratio * stretch, decay / (1.f + hfDamp * float(k)), sr);

      // A struck surface drives low modes hardest.
      gain[i] = 1.f / (1.f + 0.6f * float(k));
      // Pickup weights alternate sign, as adjacent eigenfunctions do at a
      // fixed off-centre listening point. The sign pattern matters: it is what
      // stops the cascade term from degenerating into a single fat harmonic.
      pick[i] = ((k & 1) ? -1.f : 1.f) / (1.f + 0.35f * float(k));
    }
  }

  inline float tick(float excitation) noexcept
  {
    // Surface displacement at the pickup, from the previous sample.
    const float w = w1;

    // --- von Kármán-flavoured cascade -----------------------------------
    // A thin plate's geometric nonlinearity makes its stiffness depend on
    // displacement, so every modal frequency is modulated, per sample, by the
    // instantaneous deflection. Applied here as a rank-one reduction: one
    // scalar w perturbs every mode's phase through its own pickup weight.
    // Since w is a mixture of all the modes, that modulation generates sum and
    // difference frequencies among every pair, and the plate ratio set has
    // partials piling up in the upper spectrum ready to receive them. The
    // result is the upward drift of spectral centroid over the life of the
    // sound that a linear bank cannot produce at any setting -- a linear
    // system only redistributes what you excited at t=0, and since high modes
    // damp fastest its centroid can only fall.
    //
    // STABILITY, and why this is a rotation rather than an injected force.
    // The obvious implementation -- compute a cubic restoring force from w and
    // add it to each mode's input -- pumps energy in. Bounding that force does
    // not save you: a bank of high-Q resonators has enormous gain, 1/(1-r) is
    // around 120000 at a 2.5 s decay, so a small bounded input still integrates
    // to an absurd state. Measured, that version's peak output grew linearly
    // with the cascade amount, to 1.4e6, and its centroid fell.
    //
    // A phase rotation of unit modulus cannot do that. It conserves modal
    // energy exactly, by construction, so the nonlinearity can only move
    // energy between modes -- which is what the physical nonlinearity does.
    // The real schemes reach the same guarantee via energy-quadratised time
    // integration (Bilbao et al. 2023); this gets a weaker version of it for
    // four flops per mode and no iteration.
    //
    // WHY THE DRIVE IS w*w AND NOT w. Geometric stiffening is an even
    // nonlinearity: stretching a plate's mid-surface raises its stiffness
    // whichever way it is deflected, so the frequency perturbation goes as the
    // square of displacement and is always positive. This drove the rotation
    // with signed `pick[i] * w` instead, which rotates the odd modes backwards
    // -- and a backwards rotation is a frequency *reduction*. Measured on a
    // struck triangle, that lowered the spectral centroid from 4949 Hz to
    // 3757 Hz (the opposite of what this whole section is for) and left a DC
    // offset of -0.08 against an RMS of 0.25, because once |cascade * pick * w|
    // exceeds the clamp the rotation rectifies. Both faults come from the sign.
    //
    // Squaring also fixes the range. |w| is order 1 just after a strike, so
    // with the old linear drive the clamp saturated for any cascade above
    // about 2 and the remaining 58 units of the knob did nothing at all.
    //
    // WHY THE ROTATION IS SCALED BY THE MODE'S OWN FREQUENCY. rotate() adds a
    // fixed number of radians per sample, which is a fixed offset in *hertz*.
    // Applied uniformly that is not a stiffening at all: it shifts a 60 Hz mode
    // and a 6 kHz mode by the same absolute amount, wrecking the partial ratios,
    // and it moves the fundamental off the pitch that was dialled in. Measured
    // on the anvil voice, nominal 640 Hz: cascade 13 sounded at 1358 Hz and
    // cascade 60 at 1851 Hz, then slid back down as the energy decayed -- which
    // is the "dramatic pitch falloff" this produced. Stiffening is proportional:
    // every omega_k scales by the same factor, so the instrument keeps its
    // tuning and its intervals and merely tightens under load.
    if(cascade > 0.f)
    {
      // w*w still carries every mode pair's sum and difference frequencies, so
      // the intermodulation that feeds the cascade survives the squaring; what
      // does not survive is the ability to push a mode's frequency down.
      const float stretch
          = std::min(cascade * kCascadeScale * w * w, kCascadeMaxStretch);
      for(int k = 0; k < count; ++k)
      {
        const auto i = std::size_t(k);
        modes[i].rotate(stretch * std::abs(pick[i]) * modes[i].winc);
      }
    }

    float out = 0.f;
    float wNext = 0.f;
    for(int k = 0; k < count; ++k)
    {
      const auto i = std::size_t(k);
      const float y = modes[i].tick(excitation * gain[i]);
      out += y * gain[i];
      wNext += y * pick[i];
    }
    w1 = wNext;
    return out;
  }
};

// ============================================================================
// Hunt-Crossley nonlinear contact (Avanzini & Rocchesso, DAFx-01)
//
//     f(x, v) = k*x^a + lambda*x^a*v,    x > 0
//
// A mallet of mass m arrives at velocity v0, compresses, and is thrown back.
// Contact ends when the force would go negative. Contact duration is an
// *output* of this model, not an input: it shortens as v0 rises, so hard hits
// deliver a shorter -- therefore spectrally wider -- impulse and excite the
// high modes that a soft hit leaves alone.
//
// Integrated at kOver x the audio rate while in contact. A collision lasts a
// few milliseconds at most, so the cost is irrelevant, and the stiffnesses
// involved make plain Euler at 48 kHz marginal.
// ============================================================================
struct Impact
{
  static constexpr int kOver = 8;

  float x{0.f};      // interpenetration
  float v{0.f};      // relative velocity, negative = approaching
  bool active{false};

  float stiffness{5e4f};
  float alpha{1.5f};    // 3/2 = Hertzian sphere on plane
  float lambda{40.f};   // dissipation
  float mass{0.02f};    // kg-ish
  float norm{1.f};      // -> unit AREA, for exciting the resonators
  float peakNorm{1.f};  // -> unit PEAK, for the direct transient path

  void reset() noexcept
  {
    active = false;
    x = 0.f;
    v = 0.f;
  }

  void set(float hardness, float geometry, float damping, float m) noexcept
  {
    // hardness in [0,1] -> stiffness 1e3..1e7. Measured contact durations,
    // alpha = 3/2, mass 20 g, at impact velocities 0.5 .. 4 m/s:
    //   hardness 0.00   48.8 .. 32.5 ms   (a hand on a soft head)
    //   hardness 0.50    7.7 ..  5.1 ms   (felt beater)
    //   hardness 0.75    3.1 ..  2.0 ms   (wood stick)
    //   hardness 1.00    1.2 ..  0.8 ms   (metal on metal)
    // The 30-40% shortening across the velocity range at fixed hardness is the
    // whole point: that is where velocity-dependent brightness comes from.
    // Contact duration goes as (m / (k * v^(a-1)))^(1/(a+1)), so `alpha` sits in
    // the exponent and outweighs `hardness` by a wide margin: on a 2.1 kHz plate
    // bank, everything else held, alpha 1.0 gives a bank peak of 2.38 and a
    // centroid of 2389 Hz, alpha 2.0 gives 0.077 and 1910 Hz, alpha 2.6 gives
    // 0.018 and 178 Hz. Past 2 the pulse outlasts the mode period and the bank
    // is simply never excited, which is why the range stops there -- and 2 is
    // also where the physics stops, being the wedge/cone case. 1 is a flat
    // conforming punch and 3/2 the Hertzian sphere on a plane.
    stiffness = std::pow(10.f, 3.f + 4.f * std::clamp(hardness, 0.f, 1.f));
    alpha = std::clamp(geometry, 1.f, 2.f);
    lambda = std::clamp(damping, 0.f, 400.f);
    mass = std::max(1e-4f, m);
  }

  void strike(float velocity) noexcept
  {
    x = 0.f;
    v = -std::max(1e-4f, velocity);  // approaching
    active = true;
    // The delivered impulse is the TIME integral of f, about m*v0*(1+e).
    // Dividing by m*v0 alone leaves a discrete sum of order the sample rate,
    // which then gets integrated by the resonators into an output of order
    // 1e5 -- measured. The 1/sr in tick() converts to a discrete unit-area
    // impulse, so a strike delivers unit momentum whatever the mallet is and
    // only the pulse *shape* stays velocity-dependent.
    norm = 1.f / (mass * std::max(1e-4f, velocity));

    // Unit-area is right for driving a resonator (which integrates) and wrong
    // for anything routed straight to the output. A unit-area pulse gets
    // *shorter and taller* as the mallet hardens, but its height is only
    // 0.012 .. 0.195 over the useful parameter range -- 20 to 45 dB below the
    // resonator body, so a "how much beater click do you want" control fed
    // from it is inaudible however far it is turned. Measured, and it is why
    // the crash had no attack.
    //
    // Hertz/Hunt-Crossley contact time has a closed form,
    //     t_c = C * (m / (k * v^(a-1)))^(1/(a+1)),
    // and both C and the product peak*t_c*sr are nearly independent of every
    // parameter. Fitted against the integrator over the whole usable space:
    //     alpha 1.0   C 3.51    peak*t_c*sr 3.27
    //     alpha 1.5   C 3.26    peak*t_c*sr 3.72
    //     alpha 2.0   C 3.22    peak*t_c*sr 4.22
    //     alpha 3.0   C 3.12    peak*t_c*sr 5.25
    // so the peak-normalising factor is available in closed form at strike
    // time, with no lookahead and no second pass over the contact.
    // NB peakNorm deliberately omits the 1/sr that `norm` carries: the area
    // path integrates over time, the peak path does not.
    const float vpow = std::pow(std::max(1e-4f, velocity), alpha - 1.f);
    const float tc = 3.25f * std::pow(mass / (stiffness * vpow), 1.f / (alpha + 1.f));
    const float shape = 3.2f + 1.0f * (alpha - 1.f);  // the fit above
    peakNorm = norm * std::max(1e-6f, tc) / shape;
  }

  // `direct` receives the same contact force scaled to unit peak: the beater
  // transient as you would hear it radiated, rather than as the resonator
  // integrates it.
  inline float tick(double sr, float& direct) noexcept
  {
    if(!active)
    {
      direct = 0.f;
      return 0.f;
    }

    const float dt = 1.f / (float(sr) * float(kOver));
    float acc = 0.f;

    for(int i = 0; i < kOver; ++i)
    {
      if(x <= 0.f && v >= 0.f)
      {
        active = false;
        break;
      }

      float f = 0.f;
      if(x > 0.f)
      {
        const float xa = std::pow(x, alpha);
        f = stiffness * xa - lambda * xa * v;  // v<0 while compressing
        if(f < 0.f)
          f = 0.f;  // contact cannot pull
      }

      v += (f / mass) * dt;
      x += -v * dt;
      acc += f;
    }

    const float mean = acc * (1.f / float(kOver));
    direct = mean * peakNorm;
    return mean * norm / float(sr);
  }
};

// ============================================================================
// PhISM particle model (Cook, CMJ 21(3), 1997)
//
// A shaker is not a resonator that was struck; it is a few dozen beans whose
// collision statistics we hear. Shake energy decays, collision probability
// follows it, and each collision is an impulse into a shell resonance. Getting
// the statistics right is what makes it read as a shaker rather than as
// gated noise.
// ============================================================================
struct Particles
{
  Noise rng{};
  float shakeEnergy{0.f};
  float decayCoef{0.999f};
  float density{0.5f};  // collisions per sample, scaled by energy
  float sndLevel{0.f};

  void reset() noexcept
  {
    shakeEnergy = 0.f;
    sndLevel = 0.f;
  }

  void set(float decaySec, float count, double sr) noexcept
  {
    decayCoef = std::exp(-1.f / std::max(1.f, decaySec * float(sr)));
    // More beans -> more frequent, individually quieter collisions
    density = std::clamp(count, 1.f, 256.f) / float(sr) * 40.f;
  }

  void shake(float energy) noexcept { shakeEnergy += energy; }

  inline float tick() noexcept
  {
    shakeEnergy *= decayCoef;

    // Each collision adds a burst; between collisions the excitation decays
    // fast, which is what gives the granular texture.
    sndLevel *= 0.85f;
    if(rng.uni() < shakeEnergy * density)
      sndLevel += rng.uni() * std::sqrt(shakeEnergy);

    return rng() * sndLevel;
  }

  [[nodiscard]] bool done() const noexcept
  {
    return shakeEnergy < 1e-5f && sndLevel < 1e-5f;
  }
};

// ============================================================================
// TPT state-variable filter (Zavalishin). Topology-preserving, so cutoff can
// be swept per control period without the instability a direct-form biquad
// shows under fast modulation.
// ============================================================================
struct SVF
{
  float ic1{0.f}, ic2{0.f};
  float a1{0.f}, a2{0.f}, a3{0.f}, k{1.f};

  void reset() noexcept { ic1 = ic2 = 0.f; }

  void set(float cutoffHz, float q, double sr) noexcept
  {
    const float fc = std::clamp(cutoffHz, 10.f, float(sr) * 0.49f);
    const float g = std::tan(kPi * fc / float(sr));
    k = 1.f / std::clamp(q, 0.5f, 20.f);
    a1 = 1.f / (1.f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }

  inline void tick(float v0, float& lp, float& bp, float& hp) noexcept
  {
    const float v3 = v0 - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.f * v1 - ic1;
    ic2 = 2.f * v2 - ic2;
    lp = v2;
    bp = v1;
    hp = v0 - k * v1 - v2;
  }
};

// ============================================================================
// Antialiased soft clipper. Cubic rather than tanh so the antiderivative is a
// polynomial; tanh would need log(cosh) and two transcendentals per sample.
//   f(x) = x - x^3/3      |x| <= 1,   sign(x)*2/3        otherwise
//   F(x) = x^2/2 - x^4/12 |x| <= 1,   |x|*2/3 - 1/4      otherwise
// Both branches of F give 5/12 at |x| = 1.
// ============================================================================
inline float softClip(float x) noexcept
{
  if(x <= -1.f)
    return -2.f / 3.f;
  if(x >= 1.f)
    return 2.f / 3.f;
  return x - x * x * x * (1.f / 3.f);
}

inline float softClipInt(float x) noexcept
{
  const float a = std::abs(x);
  if(a <= 1.f)
    return x * x * 0.5f - x * x * x * x * (1.f / 12.f);
  return a * (2.f / 3.f) - 0.25f;
}

struct AdaaShaper
{
  float prev{0.f};

  void reset() noexcept { prev = 0.f; }

  inline float tick(float x) noexcept
  {
    constexpr float eps = 1e-4f;
    const float d = x - prev;
    const float y = (std::abs(d) < eps)
                        ? softClip(0.5f * (x + prev))
                        : (softClipInt(x) - softClipInt(prev)) / d;
    prev = x;
    return y * 1.5f;
  }
};

// ============================================================================
// Percussion envelope: linear attack, exponential decay. Not an ADSR -- a
// struck object has no sustain, and exponential decay is what both a
// discharging capacitor and a damped mode actually do.
// ============================================================================
struct AttackDecay
{
  float value{0.f};
  float target{0.f};
  float attackInc{1.f};
  float decayCoef{0.999f};
  bool attacking{false};

  void set(float attackSec, float decaySec, double sr) noexcept
  {
    attackInc = 1.f / std::max(1.f, attackSec * float(sr));
    decayCoef = std::exp(-1.f / std::max(1.f, decaySec * float(sr)));
  }

  void trigger(float peak) noexcept
  {
    target = peak;
    attacking = attackInc < 1.f;
    if(!attacking)
      value = peak;
  }

  void choke(double sr) noexcept
  {
    attacking = false;
    decayCoef = std::exp(-1.f / std::max(1.f, 0.003f * float(sr)));
  }

  inline float tick() noexcept
  {
    if(attacking)
    {
      value += target * attackInc;
      if(value >= target)
      {
        value = target;
        attacking = false;
      }
    }
    else
    {
      value *= decayCoef;
    }
    return value;
  }

  [[nodiscard]] bool done() const noexcept { return !attacking && value < 1e-5f; }
};

// ============================================================================
// Two-operator FM, for frankly-electronic drums. No physical pretension.
// ============================================================================
struct FmPair
{
  float cph{0.f}, mph{0.f};
  float cinc{0.f}, minc{0.f};

  void reset() noexcept { cph = mph = 0.f; }

  void setCoeffs(float carrierHz, float ratio, double sr) noexcept
  {
    const float nyq = float(sr) * 0.49f;
    cinc = std::clamp(carrierHz, 0.f, nyq) / float(sr);
    minc = std::clamp(carrierHz * ratio, 0.f, nyq) / float(sr);
  }

  inline float tick(float index) noexcept
  {
    const float m = std::sin(k2Pi * mph);
    const float out = std::sin(k2Pi * (cph + index * m * (1.f / k2Pi)));
    cph += cinc;
    cph -= std::floor(cph);
    mph += minc;
    mph -= std::floor(mph);
    return out;
  }
};

}  // namespace kbm

#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Kaboom -- an 8-channel drum synthesiser.
 *
 * Structured after examples/Advanced/Kabang/Kabang.hpp: eight DrumChannel
 * members inside `ins`, each a halp_flag(recursive_group) carrying its own
 * nested ui, surfaced as tabs through halp::recursive_group_item, dispatched
 * by MIDI note number, iterated with for_each_channel.
 *
 * Where Kabang plays samples, this synthesises. See DrumDsp.hpp for the
 * reasoning behind the engines; briefly, all three are the same resonator
 * primitive wearing different hats.
 */

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/controls.enums.hpp>
#include <halp/layout.hpp>
#include <halp/mappers.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>

#include <array>
#include <cmath>

namespace kbm
{

// Same log-taper knob Kabang uses: frequency and time controls are unusable
// on a linear taper.
template <halp::static_string lit, auto setup>
struct log_pot : halp::knob_f32<lit, setup>
{
  using mapper = halp::log_mapper<std::ratio<85, 100>>;
};

enum class Engine : int
{
  // Modal bank + energy-based tension modulation (Marogna/Avanzini/Bank
  // DAFx-10). Toms, congas, kicks, anything with a struck-head pitch glide.
  Membrane = 0,
  // Same bank plus the parametric cascade: geometric plate nonlinearity in
  // the manner of Ducceschi & Touzé (JSV 2015) and the real-time gong work of
  // Bilbao, Webb, Wang & Ducceschi (2023). Cymbals, gongs, crashes -- sounds
  // whose spectrum brightens as they ring, which a linear bank cannot do.
  Plate,
  // PhISM stochastic particles (Cook, CMJ 1997). Shakers, cabasa, tambourine,
  // rattles: many small collisions rather than one resonator being struck.
  Particle,
  // Two-operator FM. No physical pretension; there for electronic drums.
  FM
};

enum class NoiseFilter : int
{
  Lowpass = 0,
  Bandpass,
  Highpass
};

// Per-channel runtime state.
//
// This deliberately lives OUTSIDE DrumChannel. A recursive_group is walked by
// boost::pfr and every field is enumerated as a port slot -- input_introspection
// is fields_introspection with no predicate filter, so a plain data member is
// not skipped, it is counted as a port that matches no port concept. Kabang
// keeps this invariant too: every member of its channel group is a port, and
// the voice state lives inside sample_port, which derives from
// halp::soundfile_port.
//
// So the DSP state is held in a parallel array on the processor and threaded
// through the channel's methods by reference.
struct ChannelState
{
  ModalBank bank{};
  FmPair fm{};
  Particles particles{};
  Impact impact{};
  Noise noise{};
  AttackDecay noiseEnv{};
  SVF noiseFilt{};
  // The particle grains need the same filter shape as the noise path but their
  // own state, since both run in the same sample: one SVF cannot carry two
  // signals. Coefficients are set from the same controls.
  SVF particleFilt{};
  AdaaShaper shaper{};
  AttackDecay ampEnv{};
  AttackDecay pitchEnv{};
  AttackDecay indexEnv{};

  float velocity{0.f};
  int ctrlCounter{0};
  bool running{false};

  // Pending strikes, as a COUNTDOWN in samples rather than a frame index
  // inside the current block. A flam's repeats routinely land past the end
  // of the block that triggered them, and a block-relative index cannot
  // express that -- it would either fire early or be silently dropped.
  // Counting down also removes the stale-entry leak the frame-index version
  // had, where an offset >= frames matched no frame and sat in the queue
  // forever, eventually filling it and blocking every later trigger.
  static constexpr int kMaxPending = 16;
  std::array<int, kMaxPending> pendingDelay{};
  std::array<float, kMaxPending> pendingVel{};
  int pendingCount{0};
};

// ============================================================================
struct DrumChannel
{
  halp_flag(recursive_group);

  // -- routing ---------------------------------------------------------
  halp::spinbox_i32<"Input", halp::range{0, 127, 36}> midi_key;
  halp::knob_f32<"Level", halp::range{0., 2., 1.}> level;
  halp::knob_f32<"Pan", halp::range{-1., 1., 0.}> pan;
  // 0 = no choke. Channels sharing a non-zero group cut each other off, which
  // is how a closed hat silences an open one.
  halp::spinbox_i32<"Choke", halp::range{0, 4, 0}> choke;

  // -- tone ------------------------------------------------------------
  halp::combobox_t<"Engine", Engine> engine;
  log_pot<"Pitch", halp::range{20., 4000., 60.}> pitch;
  halp::knob_f32<"P. Env", halp::range{-48., 48., 12.}> pitch_env;
  log_pot<"P. Decay", halp::range{0.001, 1., 0.03}> pitch_decay;
  log_pot<"Decay", halp::range{0.005, 20., 0.35}> decay;

  // Modal bank
  halp::spinbox_i32<"Modes", halp::range{1, kMaxModes, 6}> modes;
  halp::knob_f32<"Structure", halp::range{0., 1., 0.}> structure;
  halp::knob_f32<"Spread", halp::range{0.7, 1.4, 1.}> spread;
  halp::knob_f32<"HF Damp", halp::range{0., 4., 0.5}> hf_damp;
  halp::knob_f32<"Tension", halp::range{0., 1., 0.}> tension;
  // Plate engine: strength of the displacement-dependent stiffness. This is
  // the crash build-up; at 0 the bank is linear.
  halp::knob_f32<"Cascade", halp::range{0., 60., 0.}> cascade;

  // Particle engine
  halp::knob_f32<"Grains", halp::range{1., 256., 32.}> grain_count;
  log_pot<"Shake", halp::range{0.005, 4., 0.2}> shake_decay;

  // FM engine
  halp::knob_f32<"FM Ratio", halp::range{0.25, 16., 1.41}> fm_ratio;
  halp::knob_f32<"FM Index", halp::range{0., 24., 6.}> fm_index;
  log_pot<"FM I.Dec", halp::range{0.002, 2., 0.06}> fm_index_decay;

  // -- exciter (Hunt-Crossley contact) ---------------------------------
  // Contact duration is not a control here: it emerges from the collision.
  // Hardness 0 is a hand on a soft head (~40 ms), 0.75 a wooden stick
  // (~2.5 ms), 1.0 metal on metal (~1 ms) -- and each shortens by a third
  // across the velocity range, which is where brightness-with-force comes
  // from without any velocity->cutoff routing.
  halp::knob_f32<"Hardness", halp::range{0., 1., 0.75}> mallet_hard;
  // 1 = flat punch, 3/2 = Hertzian sphere on plane, 2 = wedge or cone. See
  // Impact::set: above 2 the contact outlasts the mode period and the bank
  // stops sounding, and no percussion contact geometry lands there anyway.
  halp::knob_f32<"Geometry", halp::range{1., 2., 1.5}> mallet_alpha;
  halp::knob_f32<"Contact D.", halp::range{0., 400., 40.}> mallet_damp;
  log_pot<"Mass", halp::range{0.0005, 0.5, 0.02}> mallet_mass;
  halp::knob_f32<"Strike V.", halp::range{0.1, 6., 1.5}> strike_vel;

  // How much of the raw contact force reaches the output, bypassing the
  // resonator. This is the stick/beater transient. It was hardcoded at 0.5
  // when the exciter became Hunt-Crossley, which left cymbals with no attack:
  // a crash measured a crest factor of 1.7 against 8-15 for the real thing.
  halp::knob_f32<"Attack Tr.", halp::range{0., 2., 0.5}> transient;

  // Flam: a strike repeated a few times in quick succession. A handclap is
  // three or four bursts 10-30 ms apart, not one -- as is a buzz roll, a drag,
  // or a flammed tom. Each repeat is a full independent strike, so the contact
  // model and the resonator state interact exactly as they would from separate
  // MIDI notes.
  halp::spinbox_i32<"Flam N", halp::range{1, 6, 1}> flam_count;
  log_pot<"Flam Time", halp::range{0.002, 0.12, 0.018}> flam_time;
  // <1 tightens successive gaps (accelerating, like a real clap), >1 spreads
  halp::knob_f32<"Flam Skew", halp::range{0.4, 2., 0.85}> flam_skew;
  halp::knob_f32<"Flam Decay", halp::range{0., 1., 0.35}> flam_decay;
  // Humanises the repeat timing; 0 is machine-exact
  halp::knob_f32<"Flam Rand", halp::range{0., 1., 0.25}> flam_rand;

  // -- noise path ------------------------------------------------------
  halp::knob_f32<"Noise", halp::range{0., 1., 0.}> noise_level;
  halp::combobox_t<"N. Filter", NoiseFilter> noise_filter;
  log_pot<"N. Cutoff", halp::range{40., 18000., 4000.}> noise_cutoff;
  halp::knob_f32<"N. Reso", halp::range{0.5, 20., 1.}> noise_res;
  log_pot<"N. Decay", halp::range{0.002, 4., 0.08}> noise_decay;

  // -- shaping ---------------------------------------------------------
  halp::knob_f32<"Drive", halp::range{0., 24., 0.}> drive;
  log_pot<"Attack", halp::range{0.0, 0.05, 0.}> amp_attack;
  log_pot<"A. Decay", halp::range{0.005, 8., 0.4}> amp_decay;
  halp::knob_f32<"Vel->Amp", halp::range{0., 1., 0.7}> vel_amp;
  halp::knob_f32<"Vel->Tone", halp::range{0., 1., 0.3}> vel_tone;

  // ==================== runtime state (not ports) =====================

  void prepare(ChannelState& st, double sr) const
  {
    st.bank.reset();
    st.fm.reset();
    st.particles.reset();
    st.impact.reset();
    st.noiseFilt.reset();
    st.particleFilt.reset();
    st.shaper.reset();
    st.running = false;
    st.ctrlCounter = 0;
    st.pendingCount = 0;
    (void)sr;
  }

  void push(ChannelState& st, int delay, float vel) const
  {
    if(st.pendingCount >= ChannelState::kMaxPending)
      return;
    st.pendingDelay[std::size_t(st.pendingCount)] = delay < 0 ? 0 : delay;
    st.pendingVel[std::size_t(st.pendingCount)] = vel;
    ++st.pendingCount;
  }

  // Expand one note into its flam sequence. Gaps follow a geometric
  // progression with ratio `skew`, so skew < 1 accelerates -- which is what a
  // handclap actually does, and what makes it read as several hands rather
  // than a metronome.
  void schedule(ChannelState& st, int offset, float vel, double sr) const
  {
    const int n = std::clamp(flam_count.value, 1, 6);
    push(st, offset, vel);
    if(n <= 1)
      return;

    const float skew = std::clamp(flam_skew.value, 0.4f, 2.f);
    const float rnd = std::clamp(flam_rand.value, 0.f, 1.f);
    const float dec = std::clamp(flam_decay.value, 0.f, 1.f);

    float gap = flam_time.value;
    float t = float(offset);
    float v = vel;
    for(int i = 1; i < n; ++i)
    {
      float g = gap;
      if(rnd > 0.f)
        g *= 1.f + rnd * 0.6f * st.noise();  // +/- 60% at full randomness
      t += std::max(1.f, g * float(sr));
      v *= (1.f - dec);
      gap *= skew;
      push(st, int(t), std::max(0.02f, v));
    }
  }

  void strike(ChannelState& st, float vel, double sr) const
  {
    st.velocity = vel;
    st.running = true;
    st.ctrlCounter = 0;

    const float ampPeak = 1.f - vel_amp.value + vel_amp.value * vel;

    // The mallet: hardness/geometry/damping/mass are the contact law's own
    // parameters, and velocity scales the approach speed. Contact duration
    // follows from them rather than being dialled in.
    st.impact.set(
        mallet_hard.value, mallet_alpha.value, mallet_damp.value,
        mallet_mass.value);
    st.impact.strike(strike_vel.value * (0.25f + 0.75f * vel));

    st.noiseEnv.set(0.f, noise_decay.value, sr);
    st.noiseEnv.trigger(1.f);

    // Without this the ADAA difference quotient at the transient -- the one
    // sample that matters -- is taken against the tail of the previous hit.
    // Flams make that a per-repeat error rather than a rare one.
    st.shaper.reset();

    if(engine.value == Engine::Particle)
    {
      st.particles.set(shake_decay.value, grain_count.value, sr);
      st.particles.shake(vel * vel);
    }

    st.ampEnv.set(amp_attack.value, amp_decay.value, sr);
    st.ampEnv.trigger(ampPeak);

    st.pitchEnv.set(0.f, pitch_decay.value, sr);
    st.pitchEnv.trigger(1.f);

    st.indexEnv.set(0.f, fm_index_decay.value, sr);
    st.indexEnv.trigger(1.f);

    // A fresh strike on a still-ringing resonator should add energy, not
    // reset it -- that is how a real drum behaves and it is what makes fast
    // repeated hits sound connected rather than stuttered. So no bank reset.
    if(engine.value == Engine::FM)
      st.fm.reset();
  }

  void chokeNow(ChannelState& st, double sr) const
  {
    st.ampEnv.choke(sr);
    st.noiseEnv.choke(sr);
    // A choked open hat must not keep firing its queued repeats.
    st.pendingCount = 0;
  }

  // Refresh everything that is allowed to change at control rate.
  void updateCoeffs(ChannelState& st, double sr) const
  {
    const float velTone = 1.f - vel_tone.value + vel_tone.value * st.velocity;

    // Pitch envelope in semitones, decaying to the base pitch.
    const float semis = pitch_env.value * st.pitchEnv.value;
    const float f0 = pitch.value * std::exp2(semis / 12.f);

    switch(engine.value)
    {
      case Engine::Membrane:
      case Engine::Plate:
        st.bank.count = modes.value;
        st.bank.structure = structure.value;
        st.bank.spread = spread.value;
        st.bank.hfDamp = hf_damp.value;
        st.bank.tension = tension.value;
        st.bank.cascade
            = (engine.value == Engine::Plate) ? cascade.value : 0.f;
        st.bank.baseFreq = f0;
        st.bank.decay = decay.value;
        st.bank.updateCoeffs(sr);
        break;

      case Engine::Particle:
        // The shell the beans rattle inside is the filter set below; the grains
        // are routed through particleFilt in run().
        break;

      case Engine::FM:
        st.fm.setCoeffs(f0, fm_ratio.value, sr);
        break;
    }

    const float cutoff = noise_cutoff.value * velTone;
    st.noiseFilt.set(cutoff, noise_res.value, sr);
    st.particleFilt.set(cutoff, noise_res.value, sr);
  }

  // Render `frames` samples, accumulating into a stereo pair.
  void run(
      ChannelState& st, int frames, double* outL, double* outR, double sr,
      double master) const
  {
    // Nothing pending and nothing ringing: skip the channel entirely.
    if(!st.running && st.pendingCount == 0)
      return;

    const bool driveOn = drive.value > 0.01f;
    const float driveLin = std::pow(10.f, drive.value / 20.f);
    // AdaaShaper returns 1.5 * softClip(x), and softClip is unity-slope at the
    // origin, so dividing by 1.5 * kDriveHeadroom undoes both the trim and the
    // normalisation: small signals pass through at 0 dB.
    const float drivePre = kDriveHeadroom * driveLin;
    const float kDrivePost = 1.f / (1.5f * kDriveHeadroom);
    const float noiseAmt = noise_level.value;
    const float lvl = level.value * float(master);
    const float p = std::clamp(pan.value, -1.f, 1.f);
    // Constant-power pan
    const float gl = std::cos((p + 1.f) * kPi * 0.25f);
    const float gr = std::sin((p + 1.f) * kPi * 0.25f);
    const NoiseFilter nf = noise_filter.value;
    const Engine eng = engine.value;
    const float idxDepth = fm_index.value;
    const float transientAmt = transient.value;

    for(int f = 0; f < frames; ++f)
    {
      // Sample-accurate strikes. Entries count down; anything reaching zero
      // fires now, and the survivors are decremented once per frame so a
      // pending repeat carries correctly into the next block.
      for(int i = 0; i < st.pendingCount;)
      {
        if(st.pendingDelay[std::size_t(i)] <= 0)
        {
          strike(st, st.pendingVel[std::size_t(i)], sr);
          st.pendingDelay[std::size_t(i)]
              = st.pendingDelay[std::size_t(st.pendingCount - 1)];
          st.pendingVel[std::size_t(i)]
              = st.pendingVel[std::size_t(st.pendingCount - 1)];
          --st.pendingCount;
        }
        else
        {
          --st.pendingDelay[std::size_t(i)];
          ++i;
        }
      }

      if(!st.running)
      {
        continue;
      }

      if(st.ctrlCounter == 0)
      {
        updateCoeffs(st, sr);
        st.ctrlCounter = kCtrlDiv;
      }
      --st.ctrlCounter;

      // Envelopes
      const float amp = st.ampEnv.tick();
      st.pitchEnv.tick();
      const float idx = idxDepth * st.indexEnv.tick();

      // Exciter: the contact force. `pulse` is unit-area (drives the
      // resonators, which integrate); `click` is the same force at unit peak
      // (the radiated beater transient, mixed straight to the output).
      float click = 0.f;
      const float pulse = st.impact.tick(sr, click);
      const float ns = st.noise() * st.noiseEnv.tick();

      // Tone path
      float tone = 0.f;
      switch(eng)
      {
        case Engine::Membrane:
        case Engine::Plate:
          tone = st.bank.tick(pulse);
          break;
        case Engine::Particle:
        {
          // A shaker is beans colliding *inside* something, and the something
          // is what gives the sound its band. Unfiltered grains are just white
          // noise with a granular envelope; through the filter they get a
          // shell.
          float plp, pbp, php;
          st.particleFilt.tick(st.particles.tick(), plp, pbp, php);
          tone = (nf == NoiseFilter::Lowpass)    ? plp
                 : (nf == NoiseFilter::Bandpass) ? pbp
                                                 : php;
          break;
        }
        case Engine::FM:
          // Free-running; the amplitude envelope below does the shaping.
          tone = st.fm.tick(idx);
          break;
      }

      // Noise path
      float lp, bp, hp;
      st.noiseFilt.tick(ns, lp, bp, hp);
      const float noiseOut
          = (nf == NoiseFilter::Lowpass) ? lp
            : (nf == NoiseFilter::Bandpass) ? bp
                                            : hp;

      // Mix, shape, amplify.
      // The shaper is still bypassed at drive 0 -- softClip has f'(0) = 1, so
      // the 1.5x that normalises its *saturated* output to unity would be a
      // flat +3.5 dB applied unconditionally -- but it is no longer a cliff
      // edge: with the headroom trim below, the first audible drive setting is
      // within a decibel of unity instead of already clipping the attack off.
      float mix = tone + noiseAmt * noiseOut + transientAmt * click;
      if(driveOn)
        mix = st.shaper.tick(mix * drivePre) * kDrivePost;
      const float y = mix * amp * lvl;

      outL[f] += double(y * gl);
      outR[f] += double(y * gr);

      // Retire the channel once the amplitude envelope has run out and the
      // resonator has rung down.
      //
      // The bank threshold used to be 1e-9, which a high-Q mode takes an
      // absurdly long time to reach: eight gongs measured 3.8% of a core still
      // burning twenty-five seconds after the single strike that started them,
      // long past audibility, because every running channel re-derives twelve
      // modes' coefficients (a pow, an exp, a sin and a cos each) every eighth
      // sample. The strict threshold was there for a reason -- freezing the
      // bank mid-ring would leave stale energy to reappear under the next
      // strike, since strike() deliberately does not reset it -- so clear the
      // bank on the way out and the threshold can be as loose as inaudibility
      // allows. At this point ampEnv is already below 1e-5, so the residue is
      // 1e-3 of a signal that is itself 100 dB down.
      if(st.pendingCount == 0 && st.ampEnv.done() && st.bank.energy() < 1e-6f
         && st.particles.done() && !st.impact.active)
      {
        st.running = false;
        st.bank.reset();
      }
    }
  }

  // ---------------------------------------------------------------- ui
  struct ui
  {
    halp_meta(layout, halp::layouts::vbox)

    struct
    {
      halp_meta(layout, halp::layouts::hbox)
      halp::item<&DrumChannel::engine> engine;
      halp::item<&DrumChannel::midi_key> midi_key;
      halp::item<&DrumChannel::level> level;
      halp::item<&DrumChannel::pan> pan;
      halp::item<&DrumChannel::choke> choke;
    } routing;

    struct
    {
      halp_meta(layout, halp::layouts::grid)
      halp_meta(columns, 5)

      halp::item<&DrumChannel::pitch> pitch;
      halp::item<&DrumChannel::pitch_env> pitch_env;
      halp::item<&DrumChannel::pitch_decay> pitch_decay;
      halp::item<&DrumChannel::decay> decay;
      halp::label spacer1;

      halp::item<&DrumChannel::modes> modes;
      halp::item<&DrumChannel::structure> structure;
      halp::item<&DrumChannel::spread> spread;
      halp::item<&DrumChannel::hf_damp> hf_damp;
      halp::item<&DrumChannel::tension> tension;

      halp::item<&DrumChannel::cascade> cascade;
      halp::item<&DrumChannel::grain_count> grain_count;
      halp::item<&DrumChannel::shake_decay> shake_decay;
      halp::item<&DrumChannel::fm_ratio> fm_ratio;
      halp::item<&DrumChannel::fm_index> fm_index;

      halp::item<&DrumChannel::fm_index_decay> fm_index_decay;
      halp::item<&DrumChannel::mallet_hard> mallet_hard;
      halp::item<&DrumChannel::mallet_alpha> mallet_alpha;
      halp::item<&DrumChannel::mallet_damp> mallet_damp;
      halp::item<&DrumChannel::mallet_mass> mallet_mass;

      halp::item<&DrumChannel::strike_vel> strike_vel;
      halp::item<&DrumChannel::transient> transient;
      halp::item<&DrumChannel::flam_count> flam_count;
      halp::item<&DrumChannel::flam_time> flam_time;
      halp::item<&DrumChannel::flam_skew> flam_skew;

      halp::item<&DrumChannel::flam_decay> flam_decay;
      halp::item<&DrumChannel::flam_rand> flam_rand;
      halp::label spacer2;
      halp::label spacer3;
      halp::label spacer4;

      halp::item<&DrumChannel::noise_level> noise_level;
      halp::item<&DrumChannel::noise_filter> noise_filter;
      halp::item<&DrumChannel::noise_cutoff> noise_cutoff;
      halp::item<&DrumChannel::noise_res> noise_res;
      halp::item<&DrumChannel::noise_decay> noise_decay;

      halp::item<&DrumChannel::drive> drive;
      halp::item<&DrumChannel::amp_attack> amp_attack;
      halp::item<&DrumChannel::amp_decay> amp_decay;
      halp::item<&DrumChannel::vel_amp> vel_amp;
      halp::item<&DrumChannel::vel_tone> vel_tone;
    } controls;
  };
};

// ============================================================================
class Kaboom
{
public:
  halp_meta(name, "Kaboom")
  halp_meta(category, "Audio/Synth")
  halp_meta(c_name, "kaboom")
  halp_meta(author, "Jean-Michaël Celerier")
  halp_meta(description, "8-channel modal / analog / FM drum synthesiser")
  halp_meta(uuid, "7b1f4a2c-5d63-4e18-9a07-2f8c6b3d41e5")

  struct ins
  {
    halp::midi_bus<"Input"> midi;
    halp::knob_f32<"Volume"> volume;

    DrumChannel s1;
    DrumChannel s2;
    DrumChannel s3;
    DrumChannel s4;
    DrumChannel s5;
    DrumChannel s6;
    DrumChannel s7;
    DrumChannel s8;
  } inputs;

  struct
  {
    halp::fixed_audio_bus<"Output", double, 2> audio;
  } outputs;

  static constexpr int kChannels = 8;

  // Parallel to the eight DrumChannel port groups above. Kept off the group so
  // that every member of DrumChannel remains a port -- see the comment on
  // ChannelState. Keeping the DSP state contiguous here also stops it from
  // being interleaved with the parameter storage the UI writes to.
  std::array<ChannelState, kChannels> states{};

  // f(channel, state) for each of the eight.
  void for_each_channel(auto&& f)
  {
    f(inputs.s1, states[0]);
    f(inputs.s2, states[1]);
    f(inputs.s3, states[2]);
    f(inputs.s4, states[3]);
    f(inputs.s5, states[4]);
    f(inputs.s6, states[5]);
    f(inputs.s7, states[6]);
    f(inputs.s8, states[7]);
  }

  double sr{44100.};

  void prepare(halp::setup s)
  {
    sr = (s.rate > 0.) ? s.rate : 44100.;
    for_each_channel(
        [this](const DrumChannel& c, ChannelState& st) { c.prepare(st, sr); });
  }

  using tick = halp::tick;

  void operator()(halp::tick t)
  {
    const int frames = t.frames;

    for(auto& msg : inputs.midi)
    {
      if(msg.bytes.size() < 3)
        continue;

      const unsigned char status = msg.bytes[0] & 0xF0;
      const unsigned char key = msg.bytes[1] & 0x7F;
      const unsigned char vel = msg.bytes[2] & 0x7F;

      // Note-off is ignored: a struck drum rings for as long as it rings.
      // Gate-style cutoff is what choke groups are for.
      if(status != 0x90 || vel == 0)
        continue;

      // std::clamp with lo > hi is UB, and hosts do call with zero frames
      // (freewheeling, transport edges).
      const int offset
          = (frames > 0) ? std::clamp(int(msg.timestamp), 0, frames - 1) : 0;

      int group = 0;
      for_each_channel([&](const DrumChannel& c, ChannelState& st) {
        if(key == c.midi_key)
        {
          c.schedule(st, offset, float(vel) / 127.f, sr);
          if(c.choke.value != 0)
            group = c.choke.value;
        }
      });

      if(group != 0)
      {
        for_each_channel([&](const DrumChannel& c, ChannelState& st) {
          if(c.choke.value == group && key != c.midi_key)
            c.chokeNow(st, sr);
        });
      }
    }

    double* outL = outputs.audio.samples[0];
    double* outR = outputs.audio.samples[1];
    for(int f = 0; f < frames; ++f)
    {
      outL[f] = 0.;
      outR[f] = 0.;
    }

    for_each_channel([&](const DrumChannel& c, ChannelState& st) {
      c.run(st, frames, outL, outR, sr, inputs.volume);
    });
  }

  // ---------------------------------------------------------------- ui
  struct ui
  {
    halp_meta(name, "Main")
    halp_meta(layout, halp::layouts::hbox)
    halp_meta(background, halp::colors::background_mid)

    struct
    {
      halp_meta(name, "Tabs")
      halp_meta(layout, halp::layouts::tabs)
      halp_meta(background, halp::colors::background_darker)

      struct : halp::recursive_group_item<&ins::s1, DrumChannel::ui>
      {
        halp_meta(name, "Drum 1")
      } s1;
      struct : halp::recursive_group_item<&ins::s2, DrumChannel::ui>
      {
        halp_meta(name, "Drum 2")
      } s2;
      struct : halp::recursive_group_item<&ins::s3, DrumChannel::ui>
      {
        halp_meta(name, "Drum 3")
      } s3;
      struct : halp::recursive_group_item<&ins::s4, DrumChannel::ui>
      {
        halp_meta(name, "Drum 4")
      } s4;
      struct : halp::recursive_group_item<&ins::s5, DrumChannel::ui>
      {
        halp_meta(name, "Drum 5")
      } s5;
      struct : halp::recursive_group_item<&ins::s6, DrumChannel::ui>
      {
        halp_meta(name, "Drum 6")
      } s6;
      struct : halp::recursive_group_item<&ins::s7, DrumChannel::ui>
      {
        halp_meta(name, "Drum 7")
      } s7;
      struct : halp::recursive_group_item<&ins::s8, DrumChannel::ui>
      {
        halp_meta(name, "Drum 8")
      } s8;
    } drum_tabs;

    struct
    {
      halp_meta(name, "Global")
      halp_meta(layout, halp::layouts::vbox)
      halp_meta(background, halp::colors::background_darker)

      halp::item<&ins::volume> globalvol;
    } global;
  };
};

}  // namespace kbm
