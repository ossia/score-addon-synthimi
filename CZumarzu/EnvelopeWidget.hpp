#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The graph of an eight-step envelope (Dsp/Envelope.hpp), bound to its four
 * ports: it edits the steps' rates and levels, and shows the sustain, end and
 * loop, which are menus and a toggle of their own.
 *
 * The envelope is the CZ's: each step a rate and a level, and a step's time
 * is its rate times the distance it moves. So it is edited as the CZ shows
 * it, one column per step: the point's height is the level, its place across
 * the column the rate, from fastest at the left to slowest at the right.
 * Dragging anywhere in a column moves its point by as much as the mouse moves:
 * a click alone changes nothing. Each column draws its step's slope, from
 * where the step before left off; the steps past the end are dimmed.
 *
 * Under the columns, read only, the envelope as it plays over time: what the
 * rates and levels add up to. The loop's steps are numbered in colour.
 */

#include <CZumarzu/Dsp/Envelope.hpp>
#include <avnd/concepts/painter.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace czumarzu
{
using Rgba = std::array<unsigned char, 4>;

struct Palette
{
  Rgba well{24, 25, 25, 255};
  Rgba edge{55, 55, 55, 255};
  Rgba label{127, 127, 127, 255};
  Rgba text{200, 200, 200, 255};
  Rgba accent{224, 176, 30, 255};
  Rgba hilite{255, 255, 255, 255};

  template <typename Ctx>
  static Palette from(Ctx& ctx)
  {
    Palette p;
    if constexpr(requires { ctx.to_rgba(halp::colors::light); })
    {
      auto get = [&](halp::colors c) {
        const auto v = ctx.to_rgba(c);
        return Rgba{v.r, v.g, v.b, v.a};
      };
      p.well = get(halp::colors::background_dark);
      p.edge = get(halp::colors::darker);
      p.label = get(halp::colors::mid);
      p.text = get(halp::colors::lighter);
      p.accent = get(halp::colors::runtime_value_mid);
      p.hilite = get(halp::colors::light);
    }
    return p;
  }
};

template <typename Ctx>
inline void fill(Ctx& ctx, const Rgba& c)
{
  ctx.set_fill_color({c[0], c[1], c[2], c[3]});
}
template <typename Ctx>
inline void stroke(Ctx& ctx, const Rgba& c)
{
  ctx.set_stroke_color({c[0], c[1], c[2], c[3]});
}

/*! Kind gives:
 *   static constexpr const char* title;
 *   static EnvSpec fallback();
 *   static std::string levelText(float);
 */
template <typename Kind>
struct StepEnvelopeWidget
{
  // ---- geometry -------------------------------------------------------
  static constexpr double kW = 280.;
  static constexpr double kTitleH = 15.;
  static constexpr double kPlotX = 1.;
  static constexpr double kPlotY = kTitleH;
  static constexpr double kCol = (kW - 2.) / kSteps;
  static constexpr double kPlotH = 66.;
  static constexpr double kNumH = 12.;  // step numbers under the columns
  static constexpr double kPreviewY = kPlotY + kPlotH + kNumH + 2.;
  static constexpr double kPreviewH = 26.;
  static constexpr double kInset = 4.; // of a point from its column's edges

  static constexpr double width() { return kW; }
  static constexpr double height() { return kPreviewY + kPreviewH + 2.; }

  // ---- state ----------------------------------------------------------
  //! The ports: steps, sustain menu, end menu, loop. The host keeps them up
  //! to date, on a preset, an undo, automation
  std::tuple<std::vector<float>, int, int, bool> values{{}, 0, 0, false};
  //! Only the steps are edited here
  std::tuple<halp::transaction<std::vector<float>>> transactions;

  EnvSpec spec = Kind::fallback();

  int drag{-1};
  int shown{-1};
  // Where the drag started, and its point then
  double fromX{}, fromY{};
  float fromRate{}, fromLevel{};

  auto& transaction() { return std::get<0>(transactions); }

  //! The decoded copy follows the ports, except the dragged point, which leads
  void sync()
  {
    const auto [points, sustain, end, loop] = values;
    auto next = EnvSpec::from(points, sustain, end, loop, Kind::fallback());
    if(drag >= 0)
    {
      next.rate = spec.rate;
      next.level = spec.level;
    }
    spec = next;
  }

  // ---- geometry of a step ----------------------------------------------
  static double colX(int i) { return kPlotX + kCol * i; }
  //! Fastest at the left of the column, slowest at the right
  static double rateX(int i, float rate)
  {
    return colX(i) + kInset + (kCol - 2. * kInset) * (1. - std::clamp(double(rate), 0., 1.));
  }
  static double levelY(float level)
  {
    return kPlotY + 3. + (kPlotH - 6.) * (1. - std::clamp(double(level), 0., 1.));
  }

  //! A step's time: its distance at its rate
  double seconds(int i) const
  {
    const float prev = i == 0 ? 0.f : spec.level[std::size_t(i - 1)];
    return std::abs(spec.level[std::size_t(i)] - prev) * stepTime(spec.rate[std::size_t(i)]);
  }

  static std::string secondsText(double t)
  {
    char buf[24];
    if(t < 0.0095)
      std::snprintf(buf, sizeof(buf), "%.1f ms", t * 1000.);
    else if(t < 0.95)
      std::snprintf(buf, sizeof(buf), "%.0f ms", t * 1000.);
    else
      std::snprintf(buf, sizeof(buf), "%.1f s", t);
    return buf;
  }

  // ---- painting -------------------------------------------------------
  void paint(avnd::painter auto ctx)
  {
    sync();
    const auto pal = Palette::from(ctx);

    // Title, and the step being edited
    {
      std::string title = Kind::title;
      if(shown >= 0 && shown <= spec.end)
      {
        const auto i = std::size_t(shown);
        title += "   " + std::to_string(shown + 1) + ": " + Kind::levelText(spec.level[i])
                 + ", rate " + std::to_string(int(std::lround(spec.rate[i] * 99.)))
                 + " (" + secondsText(seconds(shown)) + ")";
      }
      else
      {
        // What the steps add up to, over time
        double attack = 0., release = 0.;
        for(int i = 0; i <= spec.end; i++)
          (spec.sustain >= 0 && i > spec.sustain ? release : attack) += seconds(i);
        if(attack + release > 0.)
          title += "   " + secondsText(attack)
                   + (spec.sustain >= 0 ? ", release " + secondsText(release) : std::string{});
      }
      ctx.begin_path();
      fill(ctx, pal.label);
      ctx.draw_text(kPlotX, kTitleH - 4., title);
      ctx.fill();
    }

    // Columns: the ones in use outlined, the sustain shaded
    for(int i = 0; i < kSteps; i++)
    {
      const bool used = i <= spec.end;
      ctx.begin_path();
      ctx.set_stroke_width(1.);
      stroke(ctx, used ? pal.edge : Rgba{pal.edge[0], pal.edge[1], pal.edge[2], 90});
      ctx.draw_rect(colX(i) + 0.5, kPlotY, kCol - 1., kPlotH);
      ctx.stroke();
      if(i == spec.sustain)
      {
        // The step the key holds
        ctx.begin_path();
        fill(ctx, pal.edge);
        ctx.draw_rect(colX(i) + 0.5, kPlotY, kCol - 1., kPlotH);
        ctx.fill();
      }
    }

    // Each step's slope, from the level before it to its own, then its level
    // held to the column's edge
    ctx.begin_path();
    ctx.set_stroke_width(1.5);
    stroke(ctx, pal.accent);
    for(int i = 0; i <= spec.end; i++)
    {
      const auto ii = std::size_t(i);
      const float prev = i == 0 ? 0.f : spec.level[ii - 1];
      const double x = rateX(i, spec.rate[ii]), y = levelY(spec.level[ii]);
      ctx.draw_line(colX(i) + kInset, levelY(prev), x, y);
      ctx.draw_line(x, y, colX(i + 1) - kInset + (i < spec.end ? kInset * 2. : 0.), y);
    }
    ctx.stroke();

    for(int i = 0; i < kSteps; i++)
    {
      const auto ii = std::size_t(i);
      const bool used = i <= spec.end;
      ctx.begin_path();
      fill(ctx, i == drag ? pal.hilite : used ? pal.text : pal.edge);
      ctx.draw_circle(rateX(i, spec.rate[ii]), levelY(spec.level[ii]), i == drag ? 4. : 3.);
      ctx.fill();

      // Step number, or S for the sustain; in colour, the loop's
      const bool looped = spec.loop && i <= spec.loopEnd();
      ctx.begin_path();
      fill(ctx, i == spec.sustain || looped ? pal.accent : used ? pal.label : pal.edge);
      ctx.draw_text(
          colX(i) + kCol * 0.5 - 3., kPlotY + kPlotH + kNumH - 2.,
          i == spec.sustain ? std::string("S") : std::to_string(i + 1));
      ctx.fill();
    }

    paintPreview(ctx, pal);
  }

  //! The envelope over time, read only: attack up to the sustain, a held
  //! stretch, then the release. A loop is played twice, then released.
  void paintPreview(auto& ctx, const Palette& pal)
  {
    ctx.begin_path();
    ctx.set_stroke_width(1.);
    stroke(ctx, pal.edge);
    ctx.draw_rect(kPlotX + 0.5, kPreviewY, kW - 3., kPreviewH);
    ctx.stroke();

    // The steps in the order they play, -1 for the held stretch
    std::array<int, 3 * kSteps> order{};
    int count = 0;
    const int passes = spec.loop ? 2 : 1;
    for(int pass = 0; pass < passes; pass++)
      for(int i = 0; i <= spec.loopEnd(); i++)
        order[std::size_t(count++)] = i;
    if(spec.sustain >= 0 && !spec.loop)
      order[std::size_t(count++)] = -1;
    for(int i = spec.loopEnd() + 1; i <= spec.end; i++)
      order[std::size_t(count++)] = i;

    // A step's time from wherever the one before left off
    auto length = [&](int i, float from) {
      return std::abs(spec.level[std::size_t(i)] - from) * stepTime(spec.rate[std::size_t(i)]);
    };
    double moving = 0.;
    float lv = 0.f;
    for(int k = 0; k < count; k++)
    {
      if(const int i = order[std::size_t(k)]; i >= 0)
      {
        moving += length(i, lv);
        lv = spec.level[std::size_t(i)];
      }
    }
    const double hold = 0.15 * moving + 1e-3;
    const double total = moving + (spec.sustain >= 0 && !spec.loop ? hold : 0.);
    if(total <= 0.)
      return;
    const double sx = (kW - 6.) / total;
    const auto py = [&](float l) { return kPreviewY + 2. + (kPreviewH - 4.) * (1. - l); };

    ctx.begin_path();
    ctx.set_stroke_width(1.);
    stroke(ctx, pal.accent);
    double t = 0.;
    lv = 0.f;
    for(int k = 0; k < count; k++)
    {
      const int i = order[std::size_t(k)];
      const float to = i >= 0 ? spec.level[std::size_t(i)] : lv;
      const double d = i >= 0 ? length(i, lv) : hold;
      ctx.draw_line(kPlotX + 2. + t * sx, py(lv), kPlotX + 2. + (t + d) * sx, py(to));
      t += d;
      lv = to;
    }
    ctx.stroke();
  }

  // ---- interaction ----------------------------------------------------
  static int columnAt(double x)
  {
    return std::clamp(int((x - kPlotX) / kCol), 0, kSteps - 1);
  }

  bool mouse_press(double x, double y)
  {
    sync();
    if(y < kPlotY || y > kPlotY + kPlotH + kNumH)
      return false;
    const int i = columnAt(x);
    if(i > spec.end)
      return false;
    shown = drag = i;
    fromX = x;
    fromY = y;
    fromRate = spec.rate[std::size_t(i)];
    fromLevel = spec.level[std::size_t(i)];
    if(transaction().start)
      transaction().start();
    return true;
  }

  bool mouse_move(double x, double y)
  {
    if(drag < 0)
      return false;
    const auto i = std::size_t(drag);
    // Right is slower, up is louder: the scales of rateX and levelY
    spec.rate[i] = float(std::clamp(
        double(fromRate) - (x - fromX) / (kCol - 2. * kInset), 0., 1.));
    spec.level[i] = float(std::clamp(
        double(fromLevel) - (y - fromY) / (kPlotH - 6.), 0., 1.));
    if(transaction().update)
      transaction().update(spec.points());
    return true;
  }

  bool mouse_release(double, double)
  {
    if(drag >= 0 && transaction().commit)
      transaction().commit();
    drag = -1;
    return false;
  }

  void reset() { drag = -1; }
};

struct DcaKind
{
  static constexpr const char* title = "DCA";
  static EnvSpec fallback();
  static std::string levelText(float l)
  {
    if(l <= 0.f)
      return "-inf";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f dB", double((l - 1.f) * 78.27f));
    return buf;
  }
};
struct DcwKind
{
  static constexpr const char* title = "DCW";
  static EnvSpec fallback();
  static std::string levelText(float l)
  {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", double(l * 100.f));
    return buf;
  }
};
struct DcoKind
{
  static constexpr const char* title = "DCO";
  static EnvSpec fallback();
  static std::string levelText(float l) { return DcwKind::levelText(l); }
};
}
