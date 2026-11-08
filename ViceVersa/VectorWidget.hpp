#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The diamond of the four sources, A west, B north, C east, D south, bound to
 * the mix envelope's points (Dsp/Vector.hpp, a std::vector<float>), the
 * Position (a 2D point), and the envelope's loop menus. It shows the five
 * points and the path through them, the loop brighter, the Position as a
 * cross, and the point of each sounding voice.
 *
 * Two buttons on the left pick what a drag in the diamond moves: the
 * envelope's points, or the Position, as the VS's joystick. The loop and the
 * times of the moves are menus and knobs of their own.
 */

#include <ViceVersa/Dsp/Vector.hpp>
#include <avnd/concepts/painter.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>
#include <halp/value_types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace viceversa
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

struct VectorPad
{
  static constexpr double kPad = 186.;   // the diamond's square
  static constexpr double kMargin = 12.;
  static constexpr double kButton = 24.; // the drag target's buttons
  static constexpr double kTools = kButton + 6.;
  static constexpr double kHit = 9.;

  static constexpr double width() { return kTools + kMargin * 2. + kPad; }
  static constexpr double height() { return kMargin * 2. + kPad; }

  static constexpr double cx() { return kTools + kMargin + kPad * 0.5; }
  static constexpr double cy() { return kMargin + kPad * 0.5; }
  static constexpr double radius() { return kPad * 0.5; }
  //! The drag target's buttons, stacked at the top left
  static constexpr double buttonX() { return 2.; }
  static constexpr double buttonY(int i) { return kMargin + i * (kButton + 6.); }

  using Point = halp::xy_type<float>;

  // The ports' values, kept up to date by the host: points, Position, loop
  // mode, loop from, loop to (menu items). Points and Position are edited.
  std::tuple<std::vector<float>, Point, int, int, int> values{
      {}, Point{0.f, 0.f}, 0, 1, 2};
  std::tuple<halp::transaction<std::vector<float>>, halp::transaction<Point>> transactions;
  std::function<void()> update;

  VecEnvSpec spec = VecEnvSpec::centred();
  LivePoints live; // from the processor

  enum Target
  {
    Points,
    Position
  } target{Points};
  int drag{-1};          // a point
  bool dragging{false};  // the Position

  auto& envelope() { return std::get<0>(transactions); }
  auto& position() { return std::get<1>(transactions); }
  const Point& pos() const { return std::get<1>(values); }

  //! The decoded envelope follows the ports, except the points during a
  //! drag, where they lead
  void sync()
  {
    auto next = VecEnvSpec::decode(std::get<0>(values), VecEnvSpec::centred());
    if(drag >= 0)
    {
      next.x = spec.x;
      next.y = spec.y;
    }
    next.setLoop(VecLoop(std::get<2>(values)), std::get<3>(values), std::get<4>(values) + 1);
    spec = next;
  }

  static double px(float x) { return cx() + double(x) * radius(); }
  static double py(float y) { return cy() - double(y) * radius(); }

  // ---- painting -------------------------------------------------------
  void paint(avnd::painter auto ctx)
  {
    sync();
    const auto pal = Palette::from(ctx);

    // The diamond, and its axes
    ctx.begin_path();
    fill(ctx, pal.well);
    ctx.move_to(px(-1.f), py(0.f));
    ctx.line_to(px(0.f), py(1.f));
    ctx.line_to(px(1.f), py(0.f));
    ctx.line_to(px(0.f), py(-1.f));
    ctx.close_path();
    ctx.fill();
    ctx.begin_path();
    ctx.set_stroke_width(1.);
    stroke(ctx, pal.edge);
    ctx.move_to(px(-1.f), py(0.f));
    ctx.line_to(px(0.f), py(1.f));
    ctx.line_to(px(1.f), py(0.f));
    ctx.line_to(px(0.f), py(-1.f));
    ctx.close_path();
    ctx.draw_line(px(-1.f), py(0.f), px(1.f), py(0.f));
    ctx.draw_line(px(0.f), py(-1.f), px(0.f), py(1.f));
    ctx.stroke();

    ctx.begin_path();
    fill(ctx, pal.text);
    ctx.draw_text(px(-1.f) - 10., py(0.f) + 4., "A");
    ctx.draw_text(px(0.f) - 3., py(1.f) - 2., "B");
    ctx.draw_text(px(1.f) + 3., py(0.f) + 4., "C");
    ctx.draw_text(px(0.f) - 3., py(-1.f) + 11., "D");
    ctx.fill();

    // The envelope, dimmed while the drag moves the Position
    const bool points = target == Points;
    const Rgba& line = points ? pal.label : pal.edge;
    const Rgba& loopLine = points ? pal.accent : pal.label;

    // The path: the moves, the loop brighter, the release to 4 dashed
    for(int i = 0; i < 4; i++)
    {
      const auto a = std::size_t(i), b = std::size_t(i + 1);
      const bool looped = spec.loop != VecLoop::Off && i >= spec.from && i + 1 <= spec.to;
      ctx.begin_path();
      ctx.set_stroke_width(looped ? 2.5 : 1.5);
      stroke(ctx, looped ? loopLine : line);
      ctx.draw_line(px(spec.x[a]), py(spec.y[a]), px(spec.x[b]), py(spec.y[b]));
      ctx.stroke();
    }
    if(spec.loop == VecLoop::Round && spec.to - spec.from > 1)
    {
      // Round: back from the last point of the loop to its first
      const auto a = std::size_t(spec.to), b = std::size_t(spec.from);
      ctx.begin_path();
      ctx.set_stroke_width(1.);
      stroke(ctx, loopLine);
      dashed(ctx, px(spec.x[a]), py(spec.y[a]), px(spec.x[b]), py(spec.y[b]));
      ctx.stroke();
    }

    // The Position: a cross through a ring, under the points so that one
    // there stays readable; bold while it is what the drag moves
    {
      const double x = px(pos().x), y = py(pos().y);
      ctx.begin_path();
      ctx.set_stroke_width(points ? 1. : 2.);
      stroke(ctx, points ? pal.text : pal.hilite);
      ctx.draw_line(x - 14., y, x - 6., y);
      ctx.draw_line(x + 6., y, x + 14., y);
      ctx.draw_line(x, y - 14., x, y - 6.);
      ctx.draw_line(x, y + 6., x, y + 14.);
      ctx.draw_circle(x, y, 9.);
      ctx.stroke();
    }

    for(int i = 0; i < kVecPoints; i++)
    {
      const auto ii = std::size_t(i);
      const double x = px(spec.x[ii]), y = py(spec.y[ii]);
      ctx.begin_path();
      fill(ctx, i == drag ? pal.hilite : points ? pal.accent : pal.label);
      ctx.draw_circle(x, y, i == drag ? 7. : 6.);
      ctx.fill();
      ctx.begin_path();
      fill(ctx, pal.well);
      ctx.draw_text(x - 3., y + 4., std::to_string(i));
      ctx.fill();
    }

    // Where the sounding voices are
    for(int i = 0; i < std::min(live.count, int(live.x.size())); i++)
    {
      const auto ii = std::size_t(i);
      ctx.begin_path();
      fill(ctx, pal.hilite);
      ctx.draw_circle(px(live.x[ii]), py(live.y[ii]), 3.);
      ctx.fill();
    }

    // What a drag moves: two buttons, the chosen one lit
    paintButton(ctx, pal, Points);
    paintButton(ctx, pal, Position);
  }

  void paintButton(auto& ctx, const Palette& pal, Target t)
  {
    const bool on = target == t;
    const double bx = buttonX(), by = buttonY(int(t)), b = kButton;
    ctx.begin_path();
    fill(ctx, on ? pal.accent : pal.well);
    ctx.draw_rounded_rect(bx, by, b, b, 3.);
    ctx.fill();
    ctx.begin_path();
    ctx.set_stroke_width(1.);
    stroke(ctx, on ? pal.accent : pal.edge);
    ctx.draw_rounded_rect(bx + 0.5, by + 0.5, b - 1., b - 1., 3.);
    ctx.stroke();

    const Rgba& ink = on ? pal.well : pal.text;
    if(t == Points)
    {
      // A path through numbered points
      const double px[3] = {bx + 6., bx + 12., bx + 18.};
      const double py[3] = {by + 17., by + 7., by + 14.};
      ctx.begin_path();
      ctx.set_stroke_width(1.5);
      stroke(ctx, ink);
      ctx.draw_line(px[0], py[0], px[1], py[1]);
      ctx.draw_line(px[1], py[1], px[2], py[2]);
      ctx.stroke();
      ctx.begin_path();
      fill(ctx, ink);
      for(int i = 0; i < 3; i++)
        ctx.draw_circle(px[i], py[i], 2.5);
      ctx.fill();
    }
    else
    {
      // The Position's cross
      const double x = bx + b * 0.5, y = by + b * 0.5;
      ctx.begin_path();
      ctx.set_stroke_width(1.5);
      stroke(ctx, ink);
      ctx.draw_line(x - 9., y, x - 4., y);
      ctx.draw_line(x + 4., y, x + 9., y);
      ctx.draw_line(x, y - 9., x, y - 4.);
      ctx.draw_line(x, y + 4., x, y + 9.);
      ctx.draw_circle(x, y, 5.5);
      ctx.stroke();
    }
  }

  static void dashed(auto& ctx, double x0, double y0, double x1, double y1)
  {
    const double len = std::hypot(x1 - x0, y1 - y0);
    const int n = std::max(1, int(len / 6.));
    for(int i = 0; i < n; i += 2)
    {
      const double a = double(i) / n, b = double(std::min(i + 1, n)) / n;
      ctx.draw_line(x0 + a * (x1 - x0), y0 + a * (y1 - y0), x0 + b * (x1 - x0),
                    y0 + b * (y1 - y0));
    }
  }

  // ---- interaction ----------------------------------------------------
  //! The point of the diamond under the cursor: outside it, the mix would
  //! not change
  static Point at(double x, double y)
  {
    float vx = float((x - cx()) / radius()), vy = float((cy() - y) / radius());
    const float m = std::abs(vx) + std::abs(vy);
    if(m > 1.f)
    {
      vx /= m;
      vy /= m;
    }
    return {vx, vy};
  }

  void send()
  {
    if(envelope().update)
      envelope().update(spec.points());
  }
  void movePosition(double x, double y)
  {
    if(position().update)
      position().update(at(x, y));
  }
  void repaint()
  {
    if(update)
      update();
  }

  //! The drag target's button under the cursor, -1 if none
  static int buttonAt(double x, double y)
  {
    for(int i = 0; i < 2; i++)
      if(x >= buttonX() && x <= buttonX() + kButton && y >= buttonY(i)
         && y <= buttonY(i) + kButton)
        return i;
    return -1;
  }

  static bool inDiamond(double x, double y)
  {
    return std::abs(x - cx()) + std::abs(y - cy()) <= radius() + kHit;
  }

  bool mouse_press(double x, double y)
  {
    sync();
    if(const int b = buttonAt(x, y); b >= 0)
    {
      target = Target(b);
      repaint();
      return true;
    }
    if(target == Position && inDiamond(x, y))
    {
      dragging = true;
      if(position().start)
        position().start();
      movePosition(x, y);
      return true;
    }
    if(target == Points)
    {
      // The topmost point under the cursor
      for(int i = kVecPoints - 1; i >= 0; i--)
      {
        const auto ii = std::size_t(i);
        if(std::hypot(x - px(spec.x[ii]), y - py(spec.y[ii])) <= kHit)
        {
          drag = i;
          if(envelope().start)
            envelope().start();
          return true;
        }
      }
    }
    return false;
  }

  bool mouse_move(double x, double y)
  {
    if(dragging)
    {
      movePosition(x, y);
      return true;
    }
    if(drag < 0)
      return false;
    const auto p = at(x, y);
    spec.x[std::size_t(drag)] = p.x;
    spec.y[std::size_t(drag)] = p.y;
    send();
    return true;
  }

  bool mouse_release(double, double)
  {
    if(dragging && position().commit)
      position().commit();
    if(drag >= 0 && envelope().commit)
      envelope().commit();
    drag = -1;
    dragging = false;
    return false;
  }

  void reset()
  {
    drag = -1;
    dragging = false;
  }
};
}
