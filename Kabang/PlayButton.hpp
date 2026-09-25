#pragma once

#include <avnd/concepts/painter.hpp>
#include <halp/layout.hpp>

#include <array>
#include <functional>

namespace Synthimi
{
// on_pressed is set by the ui bus
struct PlayButton
{
  static constexpr double width() { return 16.; }
  static constexpr double height() { return 14.; }

  std::function<void()> on_pressed;
  bool pressed{};

  void paint(avnd::painter auto ctx)
  {
    std::array<unsigned char, 4> well{44, 45, 46, 255};
    std::array<unsigned char, 4> icon{224, 176, 30, 255};
    if constexpr(requires { ctx.to_rgba(halp::colors::light); })
    {
      auto get = [&](halp::colors c) {
        const auto v = ctx.to_rgba(c);
        return std::array<unsigned char, 4>{v.r, v.g, v.b, v.a};
      };
      using enum halp::colors;
      well = get(pressed ? runtime_value_mid : background_light);
      icon = get(pressed ? background_dark : runtime_value_mid);
    }

    ctx.begin_path();
    ctx.set_fill_color({well[0], well[1], well[2], well[3]});
    ctx.draw_rounded_rect(0., 0., width(), height(), 2.);
    ctx.fill();

    ctx.begin_path();
    ctx.set_fill_color({icon[0], icon[1], icon[2], icon[3]});
    ctx.move_to(5.5, 3.5);
    ctx.line_to(11.5, 7.);
    ctx.line_to(5.5, 10.5);
    ctx.close_path();
    ctx.fill();
  }

  bool mouse_press(double, double)
  {
    pressed = true;
    if(on_pressed)
      on_pressed();
    return true;
  }
  void mouse_move(double, double) { }
  void mouse_release(double, double) { pressed = false; }
};
}
