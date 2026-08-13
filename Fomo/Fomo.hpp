#pragma once

#pragma once

/* Custom avnd::painter UI for the 4-operator matrix FM synth.
 *
 * Follows examples/Advanced/UI/Multislider.hpp: a single std::vector<float>
 * port bound through halp::custom_control, with halp::transaction driving
 * press / drag / commit so the host records one undo-able edit per gesture.
 *
 * Data layout, row-major, 4 rows x 5 columns:
 *
 *     matrix[row * 5 + col]
 *
 *       row = destination operator (the one being phase-modulated)
 *       col = 0..3 : modulation source operator; value is the FM index
 *       col = 4    : output level of that operator (carrier gain)
 *
 * The diagonal (row == col) is self-feedback.
 *
 * CELLS ARE STORED NORMALISED IN [0, 1] and mapped exponentially on the way
 * out -- quadratic for the FM index, decibel-linear for output level. Linear
 * index and level controls feel dead through the lower two thirds of their
 * travel; the DX7 maps its 0..99 output level through a similar curve for
 * exactly this reason. The cell text shows the *mapped* value, so the drag
 * feels right and the readout is still meaningful.
 *
 * NOTE ON COLOURS: every backend declares its own rgba aggregate
 * (wasm::rgba_color in the WASM binding, the host's own type elsewhere), and
 * avnd::painter only requires that `{r, g, b, a}` be constructible. Colours
 * must therefore be braced-init-lists at the call site -- a named struct will
 * not convert. fill_rgba/stroke_rgba keep that property while still allowing
 * colours to be stored and selected conditionally.
 */

#include <avnd/concepts/painter.hpp>
#include <halp/custom_widgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Synthimi
{

inline constexpr int kMatrixOps = 4;
inline constexpr int kMatrixCols = kMatrixOps + 1;  // + output level column
inline constexpr int kMatrixSize = kMatrixOps * kMatrixCols;

inline constexpr float kMaxIndex = 16.f;     // FM index ceiling, in radians
inline constexpr float kLevelRangeDb = 48.f; // output level taper

// Normalised cell -> FM index (radians). Quadratic: fine control down low,
// where the musically interesting part of the index range lives.
inline float indexFromNorm(float v) noexcept
{
  const float x = std::clamp(v, 0.f, 1.f);
  return kMaxIndex * x * x;
}

// Normalised cell -> linear gain, decibel-linear with a hard zero at 0.
inline float gainFromNorm(float v) noexcept
{
  const float x = std::clamp(v, 0.f, 1.f);
  if(x <= 0.f)
    return 0.f;
  return std::pow(10.f, (x - 1.f) * kLevelRangeDb / 20.f);
}

using Rgba = std::array<unsigned char, 4>;

inline constexpr Rgba kBg{18, 19, 23, 255};
inline constexpr Rgba kWell{30, 32, 38, 255};
inline constexpr Rgba kEdge{58, 62, 72, 255};
inline constexpr Rgba kLabel{150, 156, 170, 255};
inline constexpr Rgba kText{205, 210, 220, 255};
inline constexpr Rgba kTextDark{16, 18, 22, 255};
inline constexpr Rgba kMod{90, 165, 225, 220};
inline constexpr Rgba kFeedback{215, 95, 120, 220};
inline constexpr Rgba kOut{225, 170, 70, 220};
inline constexpr Rgba kHilite{255, 255, 255, 230};

template <typename Ctx>
inline void fill_rgba(Ctx& ctx, const Rgba& c)
{
  ctx.set_fill_color({c[0], c[1], c[2], c[3]});
}

template <typename Ctx>
inline void stroke_rgba(Ctx& ctx, const Rgba& c)
{
  ctx.set_stroke_color({c[0], c[1], c[2], c[3]});
}

inline Rgba withAlpha(Rgba c, unsigned char a)
{
  c[3] = a;
  return c;
}

struct ModMatrixWidget
{
  // ---- geometry -------------------------------------------------------
  static constexpr double kCellW = 54.;
  static constexpr double kCellH = 34.;
  static constexpr double kGap = 4.;
  static constexpr double kGutter = 46.;  // row labels on the left
  static constexpr double kHeader = 22.;  // column labels on top
  static constexpr double kGraphTop = 186.;

  static constexpr double width()
  {
    return kGutter + kMatrixCols * kCellW + (kMatrixCols - 1) * kGap + 8.;
  }
  static constexpr double height() { return 316.; }

  static constexpr double cellX(int col) { return kGutter + col * (kCellW + kGap); }
  static constexpr double cellY(int row) { return kHeader + row * (kCellH + kGap); }

  // ---- state ----------------------------------------------------------
  halp::transaction<std::vector<float>> transaction;
  std::vector<float> value = std::vector<float>(kMatrixSize, 0.f);

  int dragRow{-1};
  int dragCol{-1};
  double dragStartY{0.};
  float dragStartVal{0.f};

  // ---- control binding ------------------------------------------------
  void set_value(const auto& /*control*/, std::vector<float> v)
  {
    value = std::move(v);
    ensureSize();
  }

  static auto value_to_control(auto& /*control*/, std::vector<float> v) { return v; }

  void ensureSize()
  {
    if(value.size() != std::size_t(kMatrixSize))
      value.resize(std::size_t(kMatrixSize), 0.f);
  }

  float at(int row, int col) const
  {
    const auto i = std::size_t(row * kMatrixCols + col);
    return (i < value.size()) ? value[i] : 0.f;
  }

  // ---- painting -------------------------------------------------------
  void paint(avnd::painter auto ctx)
  {
    ctx.set_font("Inconsolata");

    ctx.begin_path();
    fill_rgba(ctx, kBg);
    ctx.draw_rounded_rect(0., 0., width(), height(), 6.);
    ctx.fill();

    paintHeaders(ctx);
    paintCells(ctx);
    paintRouting(ctx);
  }

  void paintHeaders(avnd::painter auto& ctx)
  {
    ctx.set_font_size(10.);

    for(int c = 0; c < kMatrixCols; ++c)
    {
      const bool out = (c == kMatrixOps);
      ctx.begin_path();
      fill_rgba(ctx, out ? kOut : kLabel);
      ctx.draw_text(
          cellX(c) + 14., kHeader - 8.,
          out ? std::string("OUT") : ("OP" + std::to_string(c + 1)));
      ctx.fill();
    }

    for(int r = 0; r < kMatrixOps; ++r)
    {
      ctx.begin_path();
      fill_rgba(ctx, kLabel);
      ctx.draw_text(
          6., cellY(r) + kCellH * 0.5 + 4., "-> OP" + std::to_string(r + 1));
      ctx.fill();
    }
  }

  void paintCells(avnd::painter auto& ctx)
  {
    for(int r = 0; r < kMatrixOps; ++r)
    {
      for(int c = 0; c < kMatrixCols; ++c)
      {
        const double x = cellX(c);
        const double y = cellY(r);
        const float v = std::clamp(at(r, c), 0.f, 1.f);

        const bool isOut = (c == kMatrixOps);
        const bool isFeedback = (!isOut && r == c);
        const bool isDrag = (r == dragRow && c == dragCol);

        const Rgba accent = isOut ? kOut : (isFeedback ? kFeedback : kMod);

        // Well
        ctx.begin_path();
        fill_rgba(ctx, kWell);
        ctx.draw_rounded_rect(x, y, kCellW, kCellH, 3.);
        ctx.fill();

        // Level fill, growing from the bottom
        if(v > 0.001f)
        {
          const double h = (kCellH - 4.) * double(v);
          ctx.begin_path();
          fill_rgba(ctx, accent);
          ctx.draw_rounded_rect(x + 2., y + kCellH - 2. - h, kCellW - 4., h, 2.);
          ctx.fill();
        }

        // Border: tinted on the diagonal and output column, bright on drag
        ctx.begin_path();
        ctx.set_stroke_width(isDrag ? 2. : 1.);
        stroke_rgba(
            ctx, isDrag ? kHilite
                   : ((isFeedback || isOut) ? withAlpha(accent, 140) : kEdge));
        ctx.draw_rounded_rect(x, y, kCellW, kCellH, 3.);
        ctx.stroke();

        // Readout shows the *mapped* value, not the raw normalised cell
        ctx.begin_path();
        ctx.set_font_size(10.);
        fill_rgba(ctx, v > 0.55f ? kTextDark : kText);
        ctx.draw_text(
            x + 5., y + kCellH * 0.5 + 4., isOut ? levelText(v) : indexText(v));
        ctx.fill();
      }
    }
  }

  // Compact algorithm view: operator nodes, modulation arcs, output bus.
  void paintRouting(avnd::painter auto& ctx)
  {
    constexpr double nodeW = 46.;
    constexpr double nodeH = 24.;
    const double nodeY = kGraphTop + 56.;
    const double busY = kGraphTop + 108.;

    ctx.begin_path();
    ctx.set_stroke_width(1.);
    stroke_rgba(ctx, kEdge);
    ctx.draw_line(8., kGraphTop - 8., width() - 8., kGraphTop - 8.);
    ctx.stroke();

    auto nodeX = [](int op) { return cellX(op) + (kCellW - nodeW) * 0.5; };

    // Modulation arcs first, so the nodes paint over their endpoints
    for(int dst = 0; dst < kMatrixOps; ++dst)
    {
      for(int src = 0; src < kMatrixOps; ++src)
      {
        const float v = at(dst, src);
        if(v <= 0.005f)
          continue;

        const double w = 1. + 3. * std::clamp(double(v), 0., 1.);
        const double sx = nodeX(src) + nodeW * 0.5;
        const double dx = nodeX(dst) + nodeW * 0.5;

        if(src == dst)
        {
          // Self-feedback: a loop above the node
          ctx.begin_path();
          ctx.set_stroke_width(w);
          stroke_rgba(ctx, kFeedback);
          ctx.move_to(sx - 10., nodeY);
          ctx.cubic_to(sx - 26., nodeY - 34., sx + 26., nodeY - 34., sx + 10., nodeY);
          ctx.stroke();
          arrowHead(ctx, sx + 10., nodeY, kFeedback);
        }
        else
        {
          const double lift = 18. + 12. * std::abs(dst - src);
          ctx.begin_path();
          ctx.set_stroke_width(w);
          stroke_rgba(ctx, kMod);
          ctx.move_to(sx, nodeY);
          ctx.cubic_to(sx, nodeY - lift, dx, nodeY - lift, dx, nodeY);
          ctx.stroke();
          arrowHead(ctx, dx, nodeY, kMod);
        }
      }
    }

    // Operator nodes
    for(int op = 0; op < kMatrixOps; ++op)
    {
      const double x = nodeX(op);
      const double lvl = std::clamp(double(at(op, kMatrixOps)), 0., 1.);
      const bool carrier = lvl > 0.005;

      ctx.begin_path();
      fill_rgba(ctx, carrier ? Rgba{52, 46, 30, 255} : Rgba{34, 37, 44, 255});
      ctx.draw_rounded_rect(x, nodeY, nodeW, nodeH, 4.);
      ctx.fill();

      ctx.begin_path();
      ctx.set_stroke_width(1.);
      stroke_rgba(ctx, carrier ? kOut : Rgba{70, 76, 90, 255});
      ctx.draw_rounded_rect(x, nodeY, nodeW, nodeH, 4.);
      ctx.stroke();

      ctx.begin_path();
      ctx.set_font_size(10.);
      fill_rgba(ctx, kText);
      ctx.draw_text(x + 8., nodeY + nodeH * 0.5 + 4., "OP" + std::to_string(op + 1));
      ctx.fill();

      if(carrier)
      {
        ctx.begin_path();
        ctx.set_stroke_width(1. + 3. * lvl);
        stroke_rgba(ctx, kOut);
        ctx.draw_line(x + nodeW * 0.5, nodeY + nodeH, x + nodeW * 0.5, busY);
        ctx.stroke();
      }
    }

    // Output bus
    ctx.begin_path();
    ctx.set_stroke_width(2.);
    stroke_rgba(ctx, withAlpha(kOut, 160));
    ctx.draw_line(kGutter, busY, width() - 12., busY);
    ctx.stroke();

    ctx.begin_path();
    ctx.set_font_size(10.);
    fill_rgba(ctx, kOut);
    ctx.draw_text(8., busY + 4., "OUT");
    ctx.fill();
  }

  // ---- interaction ----------------------------------------------------
  bool mouse_press(double x, double y)
  {
    int r{}, c{};
    if(!cellAt(x, y, r, c))
      return false;

    ensureSize();
    dragRow = r;
    dragCol = c;
    dragStartY = y;
    dragStartVal = at(r, c);
    transaction.start();
    return true;
  }

  bool mouse_move(double /*x*/, double y)
  {
    if(dragRow < 0)
      return false;

    // Relative vertical drag: 140 px traverses the full range, which gives
    // usable resolution on a 34 px cell. Absolute positioning inside the cell
    // would make fine adjustments impossible.
    constexpr double kSpan = 140.;
    const double v = double(dragStartVal) - (y - dragStartY) / kSpan;

    value[std::size_t(dragRow * kMatrixCols + dragCol)]
        = float(std::clamp(v, 0., 1.));
    transaction.update(value);
    return true;
  }

  bool mouse_release(double /*x*/, double /*y*/)
  {
    if(dragRow >= 0)
      transaction.commit();
    dragRow = -1;
    dragCol = -1;
    return false;
  }

  bool cellAt(double x, double y, int& row, int& col) const
  {
    for(int r = 0; r < kMatrixOps; ++r)
    {
      for(int c = 0; c < kMatrixCols; ++c)
      {
        const double cx = cellX(c);
        const double cy = cellY(r);
        if(x >= cx && x < cx + kCellW && y >= cy && y < cy + kCellH)
        {
          row = r;
          col = c;
          return true;
        }
      }
    }
    return false;
  }

  void reset() { dragRow = dragCol = -1; }

private:
  static std::string indexText(float norm)
  {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", double(indexFromNorm(norm)));
    return buf;
  }

  static std::string levelText(float norm)
  {
    if(norm <= 0.f)
      return "-inf";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0fdB", double((norm - 1.f) * kLevelRangeDb));
    return buf;
  }

  // Small downward-pointing filled triangle at the arc endpoint.
  static void arrowHead(avnd::painter auto& ctx, double x, double y, const Rgba& col)
  {
    constexpr double s = 5.;
    ctx.begin_path();
    fill_rgba(ctx, col);
    ctx.move_to(x, y + s);
    ctx.line_to(x - s * 0.7, y - s * 0.4);
    ctx.line_to(x + s * 0.7, y - s * 0.4);
    ctx.close_path();
    ctx.fill();
  }
};

}

#pragma once

#include <Gamma/Envelope.h>
#include <halp/audio.hpp>
#include <halp/compat/gamma.hpp>
#include <halp/controls.hpp>
#include <halp/controls.enums.hpp>
#include <halp/layout.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>
#include <halp/value_types.hpp>

#include <kfr/base.hpp>
#include <kfr/math.hpp>


#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Synthimi
{

inline constexpr int NumOps = 4;
inline constexpr int Lanes = 4;      // SIMD width == voices per group
inline constexpr int NumGroups = 4;  // 16 voices total
static_assert(Lanes == 4, "the vec4 construction below is unrolled for 4 lanes");
static_assert(NumOps == kMatrixOps, "the matrix UI is laid out for NumOps operators");

using vec4 = kfr::vec<float, Lanes>;
using env_t = gam::ADSR<float, float, halp::compat::gamma_domain>;

inline constexpr float kPi = 3.14159265358979324f;
inline constexpr float k2Pi = 6.28318530717958648f;

// Modulation is accumulated in normalised phase units (cycles); the index is
// conventionally quoted in radians, hence this factor.
inline constexpr float kIndexToCycles = 1.f / k2Pi;

// kfr::fastsin is the cheap polynomial approximation (note the spelling --
// there is no kfr::sin_fast).
inline vec4 wsin(const vec4& x) noexcept { return kfr::sin(x); }
inline vec4 wcos(const vec4& x) noexcept { return kfr::cos(x); }

enum class WaveformFM : int
{
  Sine = 0,
  HalfSine,  // TX81Z W2
  Saw,
  Square
};

// Order matters: halp::enum_t initialises to the first enumerator, and
// measurement says the wavetable is 16-27 dB cleaner than ADAA on a saw
// operator, so it is the default.
enum class AntiAlias : int
{
  Wavetable = 0,  // bandlimited mip-map, driven by instantaneous |delta|
  ADAA            // analytic, stateless, cheap
};

enum class PhaseMode : int
{
  Reset = 0,  // deterministic attack timbre, DX7-like
  Free        // free-running across retriggers
};

enum class LfoShape : int
{
  Triangle = 0,
  Sine,
  SawDown,
  SawUp,
  Square,
  SampleHold
};

// ============================================================================
// Part 1 -- Antiderivative antialiasing
//
// All waveforms are expressed over normalised phase p in [0, 1).
//
//   f(p)  - direct evaluation
//   F(p)  - antiderivative of (f - mean), i.e. of the DC-free waveform
//   mean  - average value of f over one period
//
// The invariant that makes first-order ADAA work is F(0) == F(1). Removing the
// mean before integrating is what guarantees it: the antiderivative of a
// waveform with non-zero average grows without bound, and the difference
// quotient then explodes at every phase wrap. The mean is added back after the
// division, so the emitted waveform keeps its original DC.
//
// ADAA output is the average of f over the phase interval swept during one
// sample: a half-sample group delay, identical for every operator, so relative
// operator phase is preserved.
// ============================================================================
template <WaveformFM W>
struct WaveTraits;

template <>
struct WaveTraits<WaveformFM::Sine>
{
  static constexpr bool adaa = false;
  static constexpr float mean = 0.f;
  static vec4 f(const vec4& p) noexcept { return wsin(vec4(k2Pi) * p); }
  static vec4 F(const vec4&) noexcept { return vec4(0.f); }
};

template <>
struct WaveTraits<WaveformFM::HalfSine>
{
  // f(p) = sin(2*pi*p) for p < 0.5, else 0.  Mean = 1/pi.
  // Value-continuous with a slope discontinuity, so ADAA-1 helps less here
  // than on saw/square, but it is nearly free once we are on this path.
  static constexpr bool adaa = true;
  static constexpr float mean = 1.f / kPi;

  static vec4 f(const vec4& p) noexcept
  {
    return kfr::select(p < vec4(0.5f), wsin(vec4(k2Pi) * p), vec4(0.f));
  }

  static vec4 F(const vec4& p) noexcept
  {
    // p <  0.5 : (1 - cos(2*pi*p)) / (2*pi) - p/pi
    // p >= 0.5 : (1 - p) / pi
    const vec4 lo = (vec4(1.f) - wcos(vec4(k2Pi) * p)) * vec4(1.f / k2Pi)
                    - p * vec4(1.f / kPi);
    const vec4 hi = (vec4(1.f) - p) * vec4(1.f / kPi);
    return kfr::select(p < vec4(0.5f), lo, hi);
  }
};

template <>
struct WaveTraits<WaveformFM::Saw>
{
  // f(p) = 2p - 1,  mean 0,  F(p) = p^2 - p,  F(0) == F(1) == 0.
  static constexpr bool adaa = true;
  static constexpr float mean = 0.f;
  static vec4 f(const vec4& p) noexcept { return p * vec4(2.f) - vec4(1.f); }
  static vec4 F(const vec4& p) noexcept { return p * (p - vec4(1.f)); }
};

template <>
struct WaveTraits<WaveformFM::Square>
{
  // f(p) = +1 for p < 0.5, -1 otherwise.  mean 0.
  // F(p) = 0.5 - |p - 0.5|, branchless, F(0) == F(1) == 0.
  static constexpr bool adaa = true;
  static constexpr float mean = 0.f;

  static vec4 f(const vec4& p) noexcept
  {
    return kfr::select(p < vec4(0.5f), vec4(1.f), vec4(-1.f));
  }

  static vec4 F(const vec4& p) noexcept
  {
    return vec4(0.5f) - kfr::abs(p - vec4(0.5f));
  }
};

// First-order ADAA.
//   p1    : wrapped modulated phase at t
//   p0    : wrapped modulated phase at t-1
//   delta : *unwrapped* increment, theta[t] - theta[t-1]. Do not derive this
//           from p1 - p0: wrong across a wrap, and wrong everywhere under PM.
//
// The denominator is clamped in magnitude with the sign preserved, so it can
// never be zero regardless of how kfr::select behaves. Worst case on a bad
// select is a wrong-sounding sample, never a NaN that sticks in the feedback
// state.
template <WaveformFM W>
inline vec4 evalAdaa(const vec4& p1, const vec4& p0, const vec4& delta) noexcept
{
  using T = WaveTraits<W>;

  if constexpr(!T::adaa)
  {
    return T::f(p1);
  }
  else
  {
    constexpr float eps = 1e-4f;
    const vec4 mag = kfr::max(kfr::abs(delta), vec4(eps));
    const vec4 sgn = kfr::select(delta < vec4(0.f), vec4(-1.f), vec4(1.f));
    const vec4 den = mag * sgn;  // |den| >= eps by construction
    const vec4 aa = (T::F(p1) - T::F(p0)) / den + vec4(T::mean);
    return kfr::select(kfr::abs(delta) < vec4(eps), T::f(p1), aa);
  }
}

inline vec4 renderAdaa(
    WaveformFM w, const vec4& p1, const vec4& p0, const vec4& delta) noexcept
{
  switch(w)
  {
    case WaveformFM::HalfSine:
      return evalAdaa<WaveformFM::HalfSine>(p1, p0, delta);
    case WaveformFM::Saw:
      return evalAdaa<WaveformFM::Saw>(p1, p0, delta);
    case WaveformFM::Square:
      return evalAdaa<WaveformFM::Square>(p1, p0, delta);
    case WaveformFM::Sine:
    default:
      return evalAdaa<WaveformFM::Sine>(p1, p0, delta);
  }
}

// ============================================================================
// Part 2 -- Bandlimited mip-mapped wavetables
//
// Level L holds a table bandlimited to H(L) harmonics, with
//
//     H(L) = kMaxHarmonics * 2^(-L / kLevelsPerOctave)
//
// A table with H harmonics is alias-free while H * |delta| <= 0.5, so the
// lowest safe level for a given increment is
//
//     Lsafe = ceil( kLevelsPerOctave * log2(2 * kMaxHarmonics * |delta|) )
//
// The point of doing this in an FM operator rather than a plain oscillator is
// that `delta` is the *modulated* phase increment. Under heavy modulation the
// operator sweeps far more phase per sample than its nominal pitch implies,
// and the mip level follows that automatically -- which is the one thing no
// waveform-domain trick (ADAA included) can do.
// ============================================================================
inline constexpr int kMaxHarmonics = 1024;
inline constexpr int kLevelsPerOctave = 2;
inline constexpr int kNumLevels = 11 * kLevelsPerOctave;
inline constexpr int kMinTableSize = 64;
inline constexpr int kMaxTableSize = 4096;

// Both blended levels are at or above Lsafe, so the result never aliases, at
// the cost of roughly half an octave of top end. Set to 0 for a brighter,
// slightly aliasing oscillator.
inline constexpr int kMipBias = 1;

// Table length per level, as a multiple of the harmonic count. Measured on a
// 440 Hz saw: 4x + linear = -47.7 dB alias/signal, 4x + cubic = -54.5 dB,
// 16x + linear = -71.5 dB. 8x + cubic is the knee of that curve.
inline constexpr int kTableOversample = 8;

struct BandlimitedTable
{
  std::vector<float> data;  // levels concatenated, each (size + 1) long
  std::array<int, kNumLevels> offset{};
  std::array<int, kNumLevels> size{};
  std::array<float, kNumLevels> sizeF{};
};

namespace detail
{
inline int nextPow2(int v) noexcept
{
  int r = 1;
  while(r < v)
    r <<= 1;
  return r;
}

inline int harmonicsAt(int level) noexcept
{
  const double h = double(kMaxHarmonics)
                   * std::pow(2.0, -double(level) / double(kLevelsPerOctave));
  const int r = int(h + 0.5);
  return r < 1 ? 1 : r;
}

struct Harmonics
{
  float dc{0.f};
  std::vector<float> sinAmp;  // indexed 1..H
  std::vector<float> cosAmp;
};

inline Harmonics harmonicsFor(WaveformFM w, int H)
{
  Harmonics h;
  h.sinAmp.assign(std::size_t(H) + 1, 0.f);
  h.cosAmp.assign(std::size_t(H) + 1, 0.f);

  switch(w)
  {
    case WaveformFM::Saw:  // 2p - 1
      for(int k = 1; k <= H; ++k)
        h.sinAmp[std::size_t(k)] = -2.f / (kPi * float(k));
      break;

    case WaveformFM::Square:  // +1 for p < 0.5
      for(int k = 1; k <= H; k += 2)
        h.sinAmp[std::size_t(k)] = 4.f / (kPi * float(k));
      break;

    case WaveformFM::HalfSine:  // half-wave rectified sine, DC included
      h.dc = 1.f / kPi;
      if(H >= 1)
        h.sinAmp[1] = 0.5f;
      for(int n = 2; n <= H; n += 2)
        h.cosAmp[std::size_t(n)] = -2.f / (kPi * float(n * n - 1));
      break;

    case WaveformFM::Sine:
    default:
      if(H >= 1)
        h.sinAmp[1] = 1.f;
      break;
  }
  return h;
}

// Inverse DFT by index arithmetic against one shared sine table -- no trig in
// the inner loop, so building the whole set costs single-digit milliseconds.
inline void synthesise(
    float* out, int N, const Harmonics& h, int H, const std::vector<float>& st)
{
  const int q = N / 4;  // cos(x) == sin(x + pi/2)
  for(int n = 0; n < N; ++n)
  {
    double acc = h.dc;
    for(int k = 1; k <= H; ++k)
    {
      const float sa = h.sinAmp[std::size_t(k)];
      const float ca = h.cosAmp[std::size_t(k)];
      if(sa == 0.f && ca == 0.f)
        continue;
      const int idx = int(((long long)(k) * n) % N);
      if(sa != 0.f)
        acc += double(sa) * st[std::size_t(idx)];
      if(ca != 0.f)
        acc += double(ca) * st[std::size_t((idx + q) % N)];
    }
    out[n + 1] = float(acc);  // slot 0 is the wrap-around guard
  }
  out[0] = out[N];      // t[-1] == t[N-1]
  out[N + 1] = out[1];  // t[N]  == t[0]
  out[N + 2] = out[2];  // t[N+1]== t[1]
}

inline BandlimitedTable buildTable(WaveformFM w)
{
  BandlimitedTable t;

  int total = 0;
  for(int L = 0; L < kNumLevels; ++L)
  {
    int N = nextPow2(kTableOversample * harmonicsAt(L));
    if(N < kMinTableSize)
      N = kMinTableSize;
    if(N > kMaxTableSize)
      N = kMaxTableSize;
    t.size[std::size_t(L)] = N;
    t.sizeF[std::size_t(L)] = float(N);
    t.offset[std::size_t(L)] = total;
    total += N + 3;
  }
  t.data.assign(std::size_t(total), 0.f);

  std::vector<float> st;
  int stN = -1;

  for(int L = 0; L < kNumLevels; ++L)
  {
    const int N = t.size[std::size_t(L)];
    int H = harmonicsAt(L);
    if(H > N / 2)
      H = N / 2;  // cannot represent more than N/2 harmonics

    if(N != stN)
    {
      st.resize(std::size_t(N));
      for(int i = 0; i < N; ++i)
        st[std::size_t(i)] = std::sin(k2Pi * float(i) / float(N));
      stN = N;
    }

    synthesise(
        t.data.data() + t.offset[std::size_t(L)], N, harmonicsFor(w, H), H, st);
  }
  return t;
}

struct TableSet
{
  std::array<BandlimitedTable, 4> byWaveformFM;

  TableSet()
  {
    byWaveformFM[0] = buildTable(WaveformFM::Sine);
    byWaveformFM[1] = buildTable(WaveformFM::HalfSine);
    byWaveformFM[2] = buildTable(WaveformFM::Saw);
    byWaveformFM[3] = buildTable(WaveformFM::Square);
  }
};
}  // namespace detail

// Built once, shared by every instance. Thread-safe initialisation.
inline const detail::TableSet& tableSet()
{
  static const detail::TableSet s;
  return s;
}

// Catmull-Rom. Linear interpolation of a 4x-oversampled table was the
// dominant error term at musical pitches; cubic buys ~7 dB for four extra
// multiplies and no extra memory.
inline float tableTap(const BandlimitedTable& t, int level, float p) noexcept
{
  const int n = t.size[std::size_t(level)];
  const float x = p * t.sizeF[std::size_t(level)];
  int i = int(x);
  if(i < 0)
    i = 0;
  else if(i >= n)
    i = n - 1;  // guards p rounding up to exactly 1.0f
  const float f = x - float(i);

  const float* d = t.data.data() + t.offset[std::size_t(level)] + i;
  const float y0 = d[0], y1 = d[1], y2 = d[2], y3 = d[3];
  const float c1 = 0.5f * (y2 - y0);
  const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
  const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * f + c2) * f + c1) * f + y1;
}

// Per-lane mip selection: each voice in the register may sit at a different
// level, so the taps are scalar. Structured to be replaceable by a gather:
// all levels live in one contiguous buffer, indexed by offset[level] + i.
inline vec4 renderTable(WaveformFM w, const vec4& p1, const vec4& delta) noexcept
{
  if(w == WaveformFM::Sine)
    return wsin(vec4(k2Pi) * p1);  // one harmonic; a table would only be worse

  const BandlimitedTable& t = tableSet().byWaveformFM[std::size_t(int(w))];

  const vec4 mag = kfr::max(kfr::abs(delta), vec4(1e-7f));
  const vec4 lf = vec4(float(kLevelsPerOctave))
                  * kfr::log2(mag * vec4(float(2 * kMaxHarmonics)));

  float o[Lanes];
  for(int l = 0; l < Lanes; ++l)
  {
    float v = lf[l];
    if(!(v > 0.f))
      v = 0.f;  // also catches NaN
    const int n0 = int(v);
    const float wgt = v - float(n0);

    int a = n0 + kMipBias;
    int b = a + 1;
    if(a > kNumLevels - 1)
      a = kNumLevels - 1;
    if(b > kNumLevels - 1)
      b = kNumLevels - 1;

    const float p = p1[l];
    const float sa = tableTap(t, a, p);
    const float sb = tableTap(t, b, p);
    o[l] = sa + wgt * (sb - sa);
  }
  return vec4(o[0], o[1], o[2], o[3]);
}

inline vec4 renderOperator(
    AntiAlias mode, WaveformFM w, const vec4& p1, const vec4& p0,
    const vec4& delta) noexcept
{
  return (mode == AntiAlias::Wavetable) ? renderTable(w, p1, delta)
                                        : renderAdaa(w, p1, p0, delta);
}

// ============================================================================
// Part 3 -- Per-operator parameters
// ============================================================================
struct EnvShape
{
  float attack{0.01f};
  float decay{0.30f};
  float sustain{0.70f};
  float release{0.40f};
};

struct OpParams
{
  WaveformFM wave{WaveformFM::Sine};

  // Tuning. fixedHz > 0 detaches the operator from the keyboard entirely,
  // which is what formants, bell partials and percussive attacks need.
  float ratio{1.f};
  float fixedHz{0.f};
  float detuneCents{0.f};

  EnvShape env{};

  // Velocity depth. In FM the expressive payoff is routing velocity into
  // *modulator* level, not carrier level: harder playing gets brighter, not
  // just louder. 0 = velocity ignored, 1 = full range.
  float velDepth{0.f};

  // Keyboard level scaling: dB per octave relative to the breakpoint note.
  // Negative depth makes the operator quieter as you play up. On a modulator
  // this doubles as index limiting -- the musical expression of the same
  // antialiasing measure.
  float klsBreakpoint{60.f};
  float klsDepth{0.f};

  // Key rate scaling: envelopes run faster on high notes. 1.0 halves every
  // segment time two octaves above middle C.
  float keyRateScale{0.f};

  // LFO -> level. Into a modulator this is timbral movement, not tremolo.
  float amDepth{0.f};

  PhaseMode phaseMode{PhaseMode::Reset};
};

using Matrix = std::array<std::array<float, NumOps>, NumOps>;
using OpGains = std::array<float, NumOps>;
using OpParamSet = std::array<OpParams, NumOps>;

// Evaluation order for the operator graph.
//
// The reference DX7 engine (msfa fm_core.cc) writes a modulator's output to a
// bus and the carrier reads that bus in the *same* sample -- feedforward
// modulation is not delayed. Only the self-feedback path goes through
// fb_buf, where fm_op_kernel.cc computes (y[n-1] + y[n-2]) >> (shift+1),
// i.e. the two-sample average.
//
// Delaying every edge, as a naive "snapshot everything" implementation does,
// puts a 1.5-sample delay and a |cos(w/2)| lowpass on every modulator. Against
// an 8x-oversampled reference that measures -13 dB error at A3 and +2.9 dB at
// A6 -- at which point the error is louder than the signal.
//
// So: topologically sort the graph, evaluate in that order using current-sample
// modulator values, and use the delayed two-sample average only on back edges
// (which includes every self-feedback connection).
struct Routing
{
  std::array<int, NumOps> order{};
  std::array<std::array<bool, NumOps>, NumOps> delayed{};  // [dst][src]
};

namespace detail
{
inline void routingDfs(
    int u, const Matrix& m, std::array<int, NumOps>& colour,
    std::array<int, NumOps>& post, int& n, Routing& r)
{
  colour[std::size_t(u)] = 1;  // grey: on the stack
  for(int v = 0; v < NumOps; ++v)
  {
    // edge u -> v exists when operator u modulates operator v
    if(m[std::size_t(v)][std::size_t(u)] == 0.f)
      continue;

    if(colour[std::size_t(v)] == 1)
      r.delayed[std::size_t(v)][std::size_t(u)] = true;  // back edge (u == v included)
    else if(colour[std::size_t(v)] == 0)
      routingDfs(v, m, colour, post, n, r);
  }
  colour[std::size_t(u)] = 2;
  post[std::size_t(n++)] = u;
}
}  // namespace detail

inline Routing computeRouting(const Matrix& m)
{
  Routing r{};
  std::array<int, NumOps> colour{};
  std::array<int, NumOps> post{};
  int n = 0;
  for(int u = 0; u < NumOps; ++u)
    if(colour[std::size_t(u)] == 0)
      detail::routingDfs(u, m, colour, post, n, r);

  // reverse postorder == topological order
  for(int i = 0; i < NumOps; ++i)
    r.order[std::size_t(i)] = post[std::size_t(NumOps - 1 - i)];
  return r;
}

struct BlockParams
{
  const Matrix* matrix{};
  const OpGains* carriers{};
  const OpParamSet* ops{};
  const Routing* routing{};
  const float* pitch{};  // per-sample LFO pitch factor (global, multiplicative)
  const float* lfo{};    // per-sample, -1..1
  float pegAttack{0.02f};
  float pegDecay{0.15f};
  float pegDepth{0.f};
  double rate{44100.};
  AntiAlias mode{AntiAlias::Wavetable};
};

// ============================================================================
// Part 4 -- Global modulators
// ============================================================================
struct Lfo
{
  double phase{0.};
  float delayRamp{1.f};
  std::uint32_t rng{0x9E3779B9u};
  float held{0.f};

  void retrigger() noexcept { delayRamp = 0.f; }

  float tick(float rateHz, LfoShape shape, float delaySec, double sr) noexcept
  {
    if(delaySec > 1e-4f)
      delayRamp = std::min(1.f, delayRamp + float(1.0 / (double(delaySec) * sr)));
    else
      delayRamp = 1.f;

    phase += double(rateHz) / sr;
    bool wrapped = false;
    if(phase >= 1.0)
    {
      phase -= std::floor(phase);
      wrapped = true;
    }

    const float p = float(phase);
    float v = 0.f;
    switch(shape)
    {
      case LfoShape::Triangle:
        v = 4.f * std::abs(p - 0.5f) - 1.f;
        break;
      case LfoShape::Sine:
        v = std::sin(k2Pi * p);
        break;
      case LfoShape::SawDown:
        v = 1.f - 2.f * p;
        break;
      case LfoShape::SawUp:
        v = 2.f * p - 1.f;
        break;
      case LfoShape::Square:
        v = (p < 0.5f) ? 1.f : -1.f;
        break;
      case LfoShape::SampleHold:
        if(wrapped)
        {
          rng = rng * 1664525u + 1013904223u;
          held = float(rng >> 8) * (2.f / 16777216.f) - 1.f;
        }
        v = held;
        break;
    }
    return v * delayRamp;
  }
};

// Global pitch envelope: attack up to depth, then decay back to zero.
// Gives attack blips, drops, and the classic brass "sproing".
struct PitchEnv
{
  float value{0.f};
  int stage{2};  // 0 = attack, 1 = decay, 2 = idle

  void retrigger() noexcept
  {
    stage = 0;
    value = 0.f;
  }

  float tick(float attack, float decay, double sr) noexcept
  {
    if(stage == 0)
    {
      value += float(1.0 / (double(std::max(1e-4f, attack)) * sr));
      if(value >= 1.f)
      {
        value = 1.f;
        stage = 1;
      }
    }
    else if(stage == 1)
    {
      value -= float(1.0 / (double(std::max(1e-4f, decay)) * sr));
      if(value <= 0.f)
      {
        value = 0.f;
        stage = 2;
      }
    }
    return value;
  }
};

// ============================================================================
// Part 5 -- One group of `Lanes` voices, evaluated in a single SIMD register
// ============================================================================
struct VoiceGroup
{
  // kfr::vec has a user-provided no-op default constructor over a raw SIMD
  // union, so std::array<vec4, N>{} does NOT zero the lanes -- it leaves them
  // indeterminate. Any NaN/Inf bit pattern in a never-played lane then rides
  // the horizontal sum into the output and sticks there. Every vec4 member
  // must be zeroed explicitly.
  static std::array<vec4, NumOps> zeroedLanes() noexcept
  {
    std::array<vec4, NumOps> a;
    a.fill(vec4(0.f));
    return a;
  }

  // SIMD state: one lane per voice
  std::array<vec4, NumOps> phase = zeroedLanes();     // unmodulated carrier phase, [0, 1)
  std::array<vec4, NumOps> inc = zeroedLanes();       // base increment, before pitch mod
  std::array<vec4, NumOps> env = zeroedLanes();
  std::array<vec4, NumOps> z1 = zeroedLanes();        // operator output at t-1
  std::array<vec4, NumOps> z2 = zeroedLanes();        // operator output at t-2
  std::array<vec4, NumOps> prevMod = zeroedLanes();   // modulation offset at t-1
  std::array<vec4, NumOps> prevWrap = zeroedLanes();  // wrapped modulated phase at t-1
  std::array<vec4, NumOps> opGain = zeroedLanes();    // velocity depth * key level scaling

  // Scalar per-lane state
  std::array<std::array<env_t, NumOps>, Lanes> envelopes{};
  // One pitch envelope per voice. A single global one cannot be right: gating
  // its retrigger skips new notes, and not gating it restarts the sweep under
  // notes that are already sounding.
  std::array<PitchEnv, Lanes> pitchEnv{};
  std::array<int, Lanes> note{};
  std::array<bool, Lanes> gate{};    // key currently held
  std::array<bool, Lanes> active{};  // still producing sound
  std::array<std::uint64_t, Lanes> age{};

  VoiceGroup() { note.fill(-1); }

  void prepare(double rate) noexcept
  {
    for(auto& lane : envelopes)
      for(auto& e : lane)
      {
        // NOTE: verify against your own halp::compat::gamma_domain. Gamma
        // converts segment lengths to samples lazily, via Td::spu(), at the
        // moment a segment starts -- so the domain rate must be correct
        // before the first note-on, not merely before the first sample.
        e.set_sample_rate(rate);
        e.amp(1.f);  // never touch amp() again: it is maxLevel(), and
                     // maxLevel(0) poisons the level array with NaN.
      }
  }

  // Carson index limiting.
  //
  // Sine operators alias badly with no waveform discontinuity anywhere: the
  // Bessel sidebands simply run past Nyquist. Measured, sine-only, 48 kHz:
  //   fc=440  ratio=2 I=4   top sideband  4.8 kHz  ->  -104 dB alias/signal
  //   fc=440  ratio=7 I=8   top sideband 28.2 kHz  ->    -8 dB
  //   fc=1760 ratio=4 I=8   top sideband 65.1 kHz  ->    +3 dB
  // Alias energy exceeding signal energy. No waveform-domain trick touches
  // this -- only bandwidth does.
  //
  // Carson: the highest significant sideband sits at f_c + (I+1)*f_m, so
  //   I_max = (Nyquist - f_c) / f_m - 1
  // Working in cycles/sample makes Nyquist exactly 0.5. Scaling a modulator's
  // output level by note is what the DX7 does with keyboard level scaling;
  // this is the same measure derived rather than dialled in.
  //
  // Carson's rule captures ~98% of the energy; the tail still folds, so a
  // guard factor tightens the bound. Measured worst case after limiting:
  //   guard 1.00 -> -16 dB    guard 0.80 -> -30 dB
  //   guard 0.60 -> -52 dB    guard 0.50 -> -67 dB
  // 0.6 is the knee: two more octaves of alias suppression for a modest loss
  // of brightness on high notes, which is what real FM instruments do anyway.
  //
  // First-order only: it does not compound through cascades (op3->op2->op1).
  std::array<float, NumOps> carsonLimit(
      int lane, const Matrix& matrix, float amount) const noexcept
  {
    std::array<float, NumOps> scale{};
    scale.fill(1.f);
    if(amount <= 0.f)
      return scale;

    for(int j = 0; j < NumOps; ++j)  // j = modulation source
    {
      const float incJ = inc[std::size_t(j)][lane];
      if(incJ <= 0.f)
        continue;

      float worst = 1.f;
      for(int i = 0; i < NumOps; ++i)  // i = destination
      {
        const float idxCyc = matrix[std::size_t(i)][std::size_t(j)];
        if(idxCyc <= 0.f)
          continue;

        const float incI = inc[std::size_t(i)][lane];
        constexpr float kCarsonGuard = 0.6f;
        const float headroom = (0.5f - incI) * kCarsonGuard;
        // matrix stores index/2pi, so convert the Carson bound the same way
        const float maxCyc
            = std::max(0.f, (headroom / incJ - 1.f)) * (1.f / k2Pi);
        worst = std::min(worst, std::clamp(maxCyc / idxCyc, 0.f, 1.f));
      }
      scale[std::size_t(j)] = 1.f + amount * (worst - 1.f);
    }
    return scale;
  }

  void noteOn(
      int lane, int midiNote, float vel, double rate, const OpParamSet& ops,
      const Matrix& matrix, float indexLimit, std::uint64_t stamp) noexcept
  {
    note[std::size_t(lane)] = midiNote;
    gate[std::size_t(lane)] = true;
    active[std::size_t(lane)] = true;
    age[std::size_t(lane)] = stamp;
    pitchEnv[std::size_t(lane)].retrigger();

    const float key = float(midiNote);
    const float noteFreq = 440.f * std::exp2((key - 69.f) / 12.f);
    const float invSr = 1.f / float(rate > 0. ? rate : 44100.);

    // Key rate scaling: one shared factor per note, applied to every segment.
    for(int op = 0; op < NumOps; ++op)
    {
      const OpParams& P = ops[std::size_t(op)];

      // --- tuning
      const float base = (P.fixedHz > 0.f)
                             ? P.fixedHz
                             : noteFreq * P.ratio;
      const float detuned = base * std::exp2(P.detuneCents / 1200.f);
      float d = detuned * invSr;
      if(!(d > 0.f))
        d = 0.f;
      else if(d > 0.49f)
        d = 0.49f;  // keep the fundamental itself under Nyquist
      inc[std::size_t(op)][lane] = d;

      // --- level: velocity depth * keyboard level scaling
      const float velGain = 1.f - P.velDepth + P.velDepth * vel;
      const float klsDb = P.klsDepth * (key - P.klsBreakpoint) / 12.f;
      const float klsGain = std::pow(10.f, klsDb / 20.f);
      opGain[std::size_t(op)][lane]
          = velGain * std::clamp(klsGain, 0.f, 4.f);

      // --- phase
      if(P.phaseMode == PhaseMode::Reset)
        phase[std::size_t(op)][lane] = 0.f;
      z1[std::size_t(op)][lane] = 0.f;
      z2[std::size_t(op)][lane] = 0.f;
      prevMod[std::size_t(op)][lane] = 0.f;
      prevWrap[std::size_t(op)][lane] = phase[std::size_t(op)][lane];

      // --- envelope, with key rate scaling
      const float rateScale
          = std::exp2(-P.keyRateScale * (key - 60.f) / 24.f);
      auto& e = envelopes[std::size_t(lane)][std::size_t(op)];
      e.attack(std::max(1e-4f, P.env.attack * rateScale))
          .decay(std::max(1e-4f, P.env.decay * rateScale))
          .release(std::max(1e-4f, P.env.release * rateScale));
      e.sustain(P.env.sustain);
      e.reset();  // also restores the sustain point after a previous release
    }

    // Needs every inc[] filled first, hence a second pass.
    const auto limit = carsonLimit(lane, matrix, indexLimit);
    for(int op = 0; op < NumOps; ++op)
      opGain[std::size_t(op)][lane] *= limit[std::size_t(op)];
  }

  void noteOff(int lane) noexcept
  {
    gate[std::size_t(lane)] = false;
    for(int op = 0; op < NumOps; ++op)
      envelopes[std::size_t(lane)][std::size_t(op)].release();
  }

  [[nodiscard]] bool sounding() const noexcept
  {
    for(bool a : active)
      if(a)
        return true;
    return false;
  }

  void render(float* out, int frames, const BlockParams& bp) noexcept
  {
    if(!sounding())
      return;

    const Matrix& matrix = *bp.matrix;
    const OpGains& carriers = *bp.carriers;
    const OpParamSet& ops = *bp.ops;
    const auto& order = bp.routing->order;
    const auto& delayed = bp.routing->delayed;

    // Hoist the per-operator block-rate parameters out of the sample loop.
    std::array<WaveformFM, NumOps> waves{};
    std::array<bool, NumOps> fixedOp{};
    std::array<float, NumOps> amDepth{};
    for(int i = 0; i < NumOps; ++i)
    {
      waves[std::size_t(i)] = ops[std::size_t(i)].wave;
      fixedOp[std::size_t(i)] = ops[std::size_t(i)].fixedHz > 0.f;
      amDepth[std::size_t(i)] = ops[std::size_t(i)].amDepth;
    }

    for(int f = 0; f < frames; ++f)
    {
      // Global LFO pitch factor, shared by every lane...
      vec4 pitchFactor(bp.pitch[f]);

      // ...times each voice's own pitch envelope.
      {
        float pe[Lanes];
        bool anyPeg = false;
        for(int l = 0; l < Lanes; ++l)
        {
          const float v
              = active[std::size_t(l)]
                    ? pitchEnv[std::size_t(l)].tick(
                          bp.pegAttack, bp.pegDecay, bp.rate)
                    : 0.f;
          pe[l] = v;
          anyPeg |= (v > 0.f);
        }
        if(anyPeg && bp.pegDepth != 0.f)
        {
          const float k = bp.pegDepth / 12.f;
          pitchFactor = pitchFactor
                        * vec4(
                            std::exp2(pe[0] * k), std::exp2(pe[1] * k),
                            std::exp2(pe[2] * k), std::exp2(pe[3] * k));
        }
      }

      const float lfoValue = bp.lfo[f];

      // --- 1. Control-rate envelopes, packed into one register per operator,
      // scaled by the per-operator level (velocity, key scaling) and by the
      // LFO amplitude modulation depth.
      for(int op = 0; op < NumOps; ++op)
      {
        float e[Lanes];
        for(int l = 0; l < Lanes; ++l)
          e[l] = active[std::size_t(l)]
                     ? envelopes[std::size_t(l)][std::size_t(op)]()
                     : 0.f;

        // Unipolar AM: depth 0 leaves the level alone, depth 1 dips to zero
        // at the LFO trough.
        const float am
            = 1.f - amDepth[std::size_t(op)] * (0.5f - 0.5f * lfoValue);

        env[std::size_t(op)] = vec4(e[0], e[1], e[2], e[3])
                               * opGain[std::size_t(op)] * vec4(am);
      }

      // --- 2. Delayed sources, used only on back edges. The two-sample
      // average is the msfa/DX7 feedback damping; applying it to feedforward
      // paths would be a 1.5-sample delay and a lowpass on every modulator.
      std::array<vec4, NumOps> fb;
      for(int op = 0; op < NumOps; ++op)
        fb[std::size_t(op)]
            = (z1[std::size_t(op)] + z2[std::size_t(op)]) * 0.5f;

      // --- 3. Evaluate the operator graph in topological order, so a
      // feedforward modulator's *current* sample reaches its destination.
      // Zeroed for real (vec4{} would not be): every op is written exactly
      // once below, but a zero fallback is the sane value if the routing
      // ever lets a read through first.
      std::array<vec4, NumOps> cur = zeroedLanes();
      vec4 acc(0.f);

      for(int step = 0; step < NumOps; ++step)
      {
        const int i = order[std::size_t(step)];
        const auto si = std::size_t(i);

        vec4 mod(0.f);
        for(int j = 0; j < NumOps; ++j)
        {
          const auto sj = std::size_t(j);
          if(matrix[si][sj] == 0.f)
            continue;
          mod = mod
                + (delayed[si][sj] ? fb[sj] : cur[sj]) * matrix[si][sj];
        }

        // Fixed-frequency operators ignore pitch EG and LFO pitch, as on the
        // DX7 -- that is the whole point of detaching them from the keyboard.
        const vec4 incNow
            = fixedOp[si] ? inc[si] : inc[si] * pitchFactor;

        // Modulated phase and its exact unwrapped increment. Deriving delta
        // analytically avoids ever having to unwrap p1 against p0, and it is
        // what drives both the ADAA quotient and the mip level.
        const vec4 theta = phase[si] + mod;
        const vec4 p1 = theta - kfr::floor(theta);
        const vec4 delta = incNow + (mod - prevMod[si]);

        const vec4 o
            = renderOperator(bp.mode, waves[si], p1, prevWrap[si], delta)
              * env[si];

        prevMod[si] = mod;
        prevWrap[si] = p1;
        cur[si] = o;
        acc = acc + o * carriers[si];

        const vec4 np = phase[si] + incNow;
        phase[si] = np - kfr::floor(np);
      }

      z2 = z1;
      z1 = cur;

      // --- 4. Horizontal sum across lanes.
      out[f] += acc[0] + acc[1] + acc[2] + acc[3];
    }
  }

  // Retire voices whose envelopes have finished. Called once per block.
  void reap() noexcept
  {
    for(int l = 0; l < Lanes; ++l)
    {
      const auto sl = std::size_t(l);
      if(!active[sl] || gate[sl])
        continue;

      bool finished = true;
      for(int op = 0; op < NumOps; ++op)
        finished &= envelopes[sl][std::size_t(op)].done();

      if(finished)
      {
        active[sl] = false;
        note[sl] = -1;
        for(int op = 0; op < NumOps; ++op)
        {
          const auto so = std::size_t(op);
          inc[so][l] = 0.f;
          phase[so][l] = 0.f;
          z1[so][l] = 0.f;
          z2[so][l] = 0.f;
          prevMod[so][l] = 0.f;
          prevWrap[so][l] = 0.f;
          opGain[so][l] = 0.f;
        }
      }
    }
  }
};

// ============================================================================
// Part 6 -- Avendish processor
// ============================================================================

// Per-operator port bank. Repetitive by nature: four identical groups of
// fifteen named, host-automatable parameters. A macro keeps the declaration
// honest rather than hiding it behind a vector port that hosts cannot name.
#define SYNTHIMI_OP_PORTS(IDX, NUM, RATIO_INIT)                                \
  halp::hslider_f32<"Op " NUM " Ratio", halp::range{0.5f, 32.f, RATIO_INIT}>   \
      ratio_##IDX;                                                             \
  halp::hslider_f32<"Op " NUM " Fixed Hz", halp::range{0.f, 8000.f, 0.f}>      \
      fixed_##IDX;                                                             \
  halp::hslider_f32<"Op " NUM " Detune", halp::range{-100.f, 100.f, 0.f}>      \
      detune_##IDX;                                                            \
  halp::enum_t<WaveformFM, "Op " NUM " Wave"> wave_##IDX;                        \
  halp::hslider_f32<"Op " NUM " Attack", halp::range{0.001f, 4.f, 0.01f}>      \
      attack_##IDX;                                                            \
  halp::hslider_f32<"Op " NUM " Decay", halp::range{0.001f, 8.f, 0.3f}>        \
      decay_##IDX;                                                             \
  halp::hslider_f32<"Op " NUM " Sustain", halp::range{0.f, 1.f, 0.7f}>         \
      sustain_##IDX;                                                           \
  halp::hslider_f32<"Op " NUM " Release", halp::range{0.001f, 8.f, 0.4f}>      \
      release_##IDX;                                                           \
  halp::hslider_f32<"Op " NUM " Vel Depth", halp::range{0.f, 1.f, 0.f}>        \
      vel_##IDX;                                                               \
  halp::hslider_f32<"Op " NUM " KLS Break", halp::range{0.f, 127.f, 60.f}>     \
      klsbrk_##IDX;                                                            \
  halp::hslider_f32<"Op " NUM " KLS dB/oct", halp::range{-24.f, 24.f, 0.f}>    \
      klsdep_##IDX;                                                            \
  halp::hslider_f32<"Op " NUM " Key Rate", halp::range{0.f, 2.f, 0.f}>         \
      krs_##IDX;                                                               \
  halp::hslider_f32<"Op " NUM " AM Depth", halp::range{0.f, 1.f, 0.f}>         \
      am_##IDX;                                                                \
  halp::enum_t<PhaseMode, "Op " NUM " Phase"> phase_##IDX;

// Matching block in the ui: one tab per operator.
#define SYNTHIMI_OP_TAB(IDX, LABEL)                                            \
  struct                                                                       \
  {                                                                            \
    halp_meta(name, LABEL)                                                     \
    halp_meta(layout, halp::layouts::vbox)                                     \
    struct                                                                     \
    {                                                                          \
      halp_meta(layout, halp::layouts::hbox)                                   \
      halp::control<&ins::wave_##IDX> wave;                                    \
      halp::control<&ins::ratio_##IDX> ratio;                                  \
      halp::control<&ins::fixed_##IDX> fixed;                                  \
      halp::control<&ins::detune_##IDX> detune;                                \
      halp::control<&ins::phase_##IDX> phase;                                  \
    } tuning;                                                                  \
    struct                                                                     \
    {                                                                          \
      halp_meta(layout, halp::layouts::hbox)                                   \
      halp::control<&ins::attack_##IDX> a;                                     \
      halp::control<&ins::decay_##IDX> d;                                      \
      halp::control<&ins::sustain_##IDX> s;                                    \
      halp::control<&ins::release_##IDX> r;                                    \
      halp::control<&ins::krs_##IDX> krs;                                      \
    } envelope;                                                                \
    struct                                                                     \
    {                                                                          \
      halp_meta(layout, halp::layouts::hbox)                                   \
      halp::control<&ins::vel_##IDX> vel;                                      \
      halp::control<&ins::klsbrk_##IDX> brk;                                   \
      halp::control<&ins::klsdep_##IDX> dep;                                   \
      halp::control<&ins::am_##IDX> am;                                        \
    } scaling;                                                                 \
  } op##IDX;

struct Fomo
{
  static consteval auto name() { return "FoMo"; }
  static consteval auto category() { return "Audio/Synth"; }
  static consteval auto c_name() { return "fomo"; }
  static consteval auto uuid() { return "e9f0a1b2-3c4d-5e6f-7a8b-9c0d1e2f3a4b"; }

  struct ins
  {
    halp::midi_bus<"MIDI"> midi;

    // The whole routing in one port: 4 rows x 5 columns, row-major, all cells
    // normalised to [0, 1] and mapped exponentially in operator().
    //   [row][0..3] FM index of operator `col` into operator `row`
    //   [row][4]    output level of operator `row`
    struct : halp::val_port<"Matrix", std::vector<float>>
    {
      enum widget
      {
        multi_slider  // fallback only; the custom UI below is the real editor
      };
      struct range
      {
        float min = 0.f, max = 1.f, init = 0.f;
      };
    } matrix;

    SYNTHIMI_OP_PORTS(0, "1", 1.f)
    SYNTHIMI_OP_PORTS(1, "2", 2.f)
    SYNTHIMI_OP_PORTS(2, "3", 3.f)
    SYNTHIMI_OP_PORTS(3, "4", 4.f)

    // Global pitch envelope
    halp::hslider_f32<"Pitch Atk", halp::range{0.001f, 4.f, 0.02f}> peg_attack;
    halp::hslider_f32<"Pitch Dcy", halp::range{0.001f, 4.f, 0.15f}> peg_decay;
    halp::hslider_f32<"Pitch Depth", halp::range{-24.f, 24.f, 0.f}> peg_depth;

    // Global LFO
    halp::enum_t<LfoShape, "LFO Shape"> lfo_shape;
    halp::hslider_f32<"LFO Rate", halp::range{0.01f, 40.f, 5.f}> lfo_rate;
    halp::hslider_f32<"LFO Delay", halp::range{0.f, 4.f, 0.f}> lfo_delay;
    halp::hslider_f32<"LFO Pitch", halp::range{0.f, 12.f, 0.f}> lfo_pitch;

    halp::enum_t<AntiAlias, "Antialias"> aa_mode;
    halp::hslider_f32<"Index Limit", halp::range{0.f, 1.f, 1.f}> index_limit;
    halp::hslider_f32<"Gain", halp::range{0.f, 1.f, 0.25f}> gain;
  } inputs;

  struct outs
  {
    halp::audio_bus<"Audio Out", float> audio;
  } outputs;

  std::array<VoiceGroup, NumGroups> groups;
  Lfo lfo;
  std::vector<float> pitchBuf;
  std::vector<float> lfoBuf;
  double sr = 44100.;
  std::uint64_t stamp = 0;

  void prepare(halp::setup s)
  {
    sr = (s.rate > 0.) ? s.rate : 44100.;
    tableSet();  // force table construction off the audio thread

    const auto n = std::size_t(s.frames > 0 ? s.frames : 4096);
    pitchBuf.assign(n, 1.f);
    lfoBuf.assign(n, 0.f);

    for(auto& g : groups)
      g.prepare(sr);
  }

  // Classic two-operator starting point: OP2 modulates OP1, OP1 is the
  // carrier. Cells are normalised, so 0.35 maps to an index of about 2.
  static std::vector<float> defaultMatrix()
  {
    std::vector<float> m(std::size_t(kMatrixSize), 0.f);
    m[std::size_t(0 * kMatrixCols + 1)] = 0.35f;
    m[std::size_t(0 * kMatrixCols + NumOps)] = 1.f;
    return m;
  }

  OpParamSet collectOpParams() const
  {
    OpParamSet p{};

#define SYNTHIMI_COLLECT(IDX)                                                  \
  {                                                                            \
    OpParams& o = p[IDX];                                                      \
    o.wave = inputs.wave_##IDX.value;                                          \
    o.ratio = inputs.ratio_##IDX.value;                                        \
    o.fixedHz = inputs.fixed_##IDX.value;                                      \
    o.detuneCents = inputs.detune_##IDX.value;                                 \
    o.env = EnvShape{                                                          \
        inputs.attack_##IDX.value, inputs.decay_##IDX.value,                   \
        inputs.sustain_##IDX.value, inputs.release_##IDX.value};               \
    o.velDepth = inputs.vel_##IDX.value;                                       \
    o.klsBreakpoint = inputs.klsbrk_##IDX.value;                               \
    o.klsDepth = inputs.klsdep_##IDX.value;                                    \
    o.keyRateScale = inputs.krs_##IDX.value;                                   \
    o.amDepth = inputs.am_##IDX.value;                                         \
    o.phaseMode = inputs.phase_##IDX.value;                                    \
  }

    SYNTHIMI_COLLECT(0)
    SYNTHIMI_COLLECT(1)
    SYNTHIMI_COLLECT(2)
    SYNTHIMI_COLLECT(3)
#undef SYNTHIMI_COLLECT

    return p;
  }

  // --------------------------------------------------------------------
  // Voice allocation: retrigger > free lane > oldest released > oldest.
  // --------------------------------------------------------------------
  std::pair<int, int> allocate(int midiNote) noexcept
  {
    int freeG = -1, freeL = -1;
    int relG = -1, relL = -1;
    std::uint64_t relAge = ~0ull;
    int oldG = 0, oldL = 0;
    std::uint64_t oldAge = ~0ull;

    for(int g = 0; g < NumGroups; ++g)
    {
      auto& grp = groups[std::size_t(g)];
      for(int l = 0; l < Lanes; ++l)
      {
        const auto sl = std::size_t(l);
        if(grp.active[sl] && grp.gate[sl] && grp.note[sl] == midiNote)
          return {g, l};  // retrigger the same key

        if(!grp.active[sl])
        {
          if(freeG < 0)
          {
            freeG = g;
            freeL = l;
          }
          continue;
        }

        if(!grp.gate[sl] && grp.age[sl] < relAge)
        {
          relAge = grp.age[sl];
          relG = g;
          relL = l;
        }
        if(grp.age[sl] < oldAge)
        {
          oldAge = grp.age[sl];
          oldG = g;
          oldL = l;
        }
      }
    }

    if(freeG >= 0)
      return {freeG, freeL};
    if(relG >= 0)
      return {relG, relL};
    return {oldG, oldL};
  }

  void noteOn(
      int midiNote, float vel, const OpParamSet& ops, const Matrix& matrix,
      float indexLimit) noexcept
  {
    // The pitch envelope is per-voice and retriggers inside VoiceGroup::noteOn,
    // so every note gets its own sweep. Only the LFO delay ramp is global, and
    // restarting that under sustaining notes would audibly duck them -- so it
    // restarts only when the synth was silent.
    bool wasSilent = true;
    for(const auto& g : groups)
      if(g.sounding())
      {
        wasSilent = false;
        break;
      }

    const auto [g, l] = allocate(midiNote);
    groups[std::size_t(g)].noteOn(
        l, midiNote, vel, sr, ops, matrix, indexLimit, stamp++);

    if(wasSilent)
      lfo.retrigger();
  }

  void noteOff(int midiNote) noexcept
  {
    for(auto& grp : groups)
      for(int l = 0; l < Lanes; ++l)
        if(grp.active[std::size_t(l)] && grp.gate[std::size_t(l)]
           && grp.note[std::size_t(l)] == midiNote)
          grp.noteOff(l);
  }

  void operator()(int frames)
  {
    const OpParamSet ops = collectOpParams();

    // --- Matrix first: note-on needs it for Carson index limiting.
    // matrix[i][j] == "operator j modulates operator i".
    auto& cells = inputs.matrix.value;
    if(cells.size() != std::size_t(kMatrixSize))
      cells = defaultMatrix();

    Matrix matrix{};
    OpGains carriers{};
    for(int i = 0; i < NumOps; ++i)
    {
      for(int j = 0; j < NumOps; ++j)
        matrix[std::size_t(i)][std::size_t(j)]
            = indexFromNorm(cells[std::size_t(i * kMatrixCols + j)])
              * kIndexToCycles;
      carriers[std::size_t(i)]
          = gainFromNorm(cells[std::size_t(i * kMatrixCols + NumOps)]);
    }

    const Routing routing = computeRouting(matrix);
    const float indexLimit = inputs.index_limit.value;

    // --- MIDI.
    // NOTE: every event is applied at the start of the block, so note timing
    // is quantised to the buffer size. For sample-accurate rendering, sort by
    // the message timestamp and split the render loop into segments.
    for(const auto& msg : inputs.midi)
    {
      if(msg.bytes.size() < 3)
        continue;

      const unsigned char status = msg.bytes[0] & 0xF0;
      const unsigned char d1 = msg.bytes[1] & 0x7F;
      const unsigned char d2 = msg.bytes[2] & 0x7F;

      switch(status)
      {
        case 0x90:
          if(d2 > 0)
            noteOn(d1, float(d2) / 127.f, ops, matrix, indexLimit);
          else
            noteOff(d1);
          break;
        case 0x80:
          noteOff(d1);
          break;
        default:
          break;
      }
    }

    // --- Global modulator buffers, one pass for the whole block.
    if(pitchBuf.size() < std::size_t(frames))
    {
      // Only reached if the host exceeds the frame count it announced in
      // prepare(); allocating here is not real-time safe, but losing the
      // block would be worse.
      pitchBuf.resize(std::size_t(frames), 1.f);
      lfoBuf.resize(std::size_t(frames), 0.f);
    }

    const float pegAtk = inputs.peg_attack.value;
    const float pegDcy = inputs.peg_decay.value;
    const float pegDepth = inputs.peg_depth.value;
    const float lfoRate = inputs.lfo_rate.value;
    const float lfoDelay = inputs.lfo_delay.value;
    const float lfoPitch = inputs.lfo_pitch.value;
    const LfoShape lfoShape = inputs.lfo_shape.value;

    for(int f = 0; f < frames; ++f)
    {
      const float l = lfo.tick(lfoRate, lfoShape, lfoDelay, sr);
      lfoBuf[std::size_t(f)] = l;
      // LFO pitch only; each voice folds in its own pitch envelope.
      pitchBuf[std::size_t(f)] = std::exp2(l * lfoPitch / 12.f);
    }

    const BlockParams bp{
        &matrix,       &carriers, &ops,     &routing,
        pitchBuf.data(), lfoBuf.data(),
        pegAtk,        pegDcy,    pegDepth, sr,
        inputs.aa_mode.value};

    // --- Render.
    // The ADAA division leaves denormals in long release tails; if your host
    // does not already set FTZ/DAZ, wrap this in kfr::scoped_flush_denormals.
    auto ch0 = outputs.audio.channel(0, frames);
    float* buf = ch0.data();
    for(int f = 0; f < frames; ++f)
      buf[f] = 0.f;

    for(auto& g : groups)
      g.render(buf, frames, bp);

    const float gain = inputs.gain.value;
    for(int f = 0; f < frames; ++f)
      buf[f] *= gain;

    for(int c = 1; c < outputs.audio.channels; ++c)
    {
      auto ch = outputs.audio.channel(c, frames);
      for(int f = 0; f < frames; ++f)
        ch[f] = buf[f];
    }

    for(auto& g : groups)
      g.reap();
  }

  // ======================================================================
  // Custom UI. The matrix is a halp::custom_control bound to the vector
  // port, exactly as examples/Advanced/UI/Multislider.hpp binds its cursors:
  // ModMatrixWidget owns the halp::transaction, so a drag is one undo step.
  // ======================================================================
  struct ui
  {
    using enum halp::colors;
    using enum halp::layouts;

    halp_meta(name, "Main")
    halp_meta(layout, vbox)
    halp_meta(background, background_dark)
    halp_meta(width, 380)
    halp_meta(height, 700)

    halp::custom_control<ModMatrixWidget, &ins::matrix> matrix;

    struct
    {
      halp_meta(layout, tabs)
      SYNTHIMI_OP_TAB(0, "Op 1")
      SYNTHIMI_OP_TAB(1, "Op 2")
      SYNTHIMI_OP_TAB(2, "Op 3")
      SYNTHIMI_OP_TAB(3, "Op 4")
    } operators;

    struct
    {
      halp_meta(name, "Global")
      halp_meta(layout, vbox)

      struct
      {
        halp_meta(layout, hbox)
        halp::control<&ins::peg_attack> a;
        halp::control<&ins::peg_decay> d;
        halp::control<&ins::peg_depth> depth;
      } pitchEnvelope;

      struct
      {
        halp_meta(layout, hbox)
        halp::control<&ins::lfo_shape> shape;
        halp::control<&ins::lfo_rate> rate;
        halp::control<&ins::lfo_delay> delay;
        halp::control<&ins::lfo_pitch> pitch;
      } lfo;

      struct
      {
        halp_meta(layout, hbox)
        halp::control<&ins::aa_mode> aa;
        halp::control<&ins::index_limit> limit;
        halp::control<&ins::gain> gain;
      } output;
    } global;

    void reset() { matrix.reset(); }
  };
};

#undef SYNTHIMI_OP_PORTS
#undef SYNTHIMI_OP_TAB

}