#pragma once
#include <defines.h>
#include <string>
#include <string_view>
#include <vector>

// Shared "average glyph width" estimate every screen wrapping/centering
// text falls back to, since renderer_frontend.h's renderer_draw_text()
// doesn't expose any real text-measurement API (see wrap_text()'s own
// comment for why) -- vulkan_text_shader.cpp's default font is 28px tall;
// ~15px is a reasonable proportional-font guess at that size. Shared as
// one constant (rather than each screen picking its own, the way
// game_menus.cpp's old kIntertitleCharWidth and menu_system.cpp's kRowWidth
// used to independently) so a wrapped line break and any horizontal
// centering computed against the same string agree with each other.
constexpr f32 kAverageCharWidth = 15.0f;

// Greedily word-wraps text into lines that each fit within max_width_px --
// see kAverageCharWidth's own comment for why "fit" here means an estimate
// (line.size() * avg_char_width_px), not an exact measurement; nudge
// avg_char_width_px at a call site if its particular text/font combination
// visibly overflows or under-fills once you can see it rendered. Splits
// only on whitespace runs, never mid-word: a single word longer than
// max_width_px on its own is still emitted as its own (overflowing) line
// rather than hyphen-broken, since breaking mid-word well needs real glyph
// metrics this engine doesn't expose. Collapses runs of whitespace down to
// single spaces between words (matches how .conversation answer/question
// text is normally authored -- see resources/conversation.h). Returns a
// single-element vector containing an empty string if text is empty or
// all whitespace, so callers can always safely index result[0] for e.g. a
// first-line prefix.
std::vector<std::string> wrap_text(std::string_view text, f32 max_width_px,
                                   f32 avg_char_width_px = kAverageCharWidth);
