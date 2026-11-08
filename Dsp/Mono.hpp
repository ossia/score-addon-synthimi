#pragma once

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <array>
#include <cstddef>

namespace synthimi
{
/*! A key released in Mono or Legato. Voice 0 plays the mono line: it goes
 *  back to the newest key still held in `stack`, or, with none left, is
 *  released or held by the pedal. The other voices are notes left from
 *  Poly, released as Poly would: those of this key and channel, unless the
 *  pedal holds them.
 *
 *  Voice: state (Voice::Active), note, channel, held, sustained, target.
 *  Stack: notes, an array of (note, channel), and size, the newest last.
 */
template <typename Voice, std::size_t N, typename Stack, typename Release>
void monoNoteOff(
    std::array<Voice, N>& voices, const Stack& stack, bool pedal, int note, int ch,
    Release&& release) noexcept
{
  auto let = [&](Voice& v) {
    v.held = false;
    if(pedal)
      v.sustained = true;
    else
      release(v);
  };
  for(std::size_t i = 1; i < N; i++)
  {
    auto& w = voices[i];
    if(w.state == Voice::Active && w.note == note && w.channel == ch && w.held)
      let(w);
  }
  auto& v = voices[0];
  if(v.state != Voice::Active || v.note != note || v.channel != ch)
    return;
  if(stack.size > 0)
  {
    // Back to the newest still held, as a legato slide
    const auto [n, c] = stack.notes[std::size_t(stack.size - 1)];
    v.note = n;
    v.channel = c;
    v.target = float(n);
    return;
  }
  let(v);
}
}
