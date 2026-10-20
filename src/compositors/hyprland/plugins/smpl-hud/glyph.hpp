#pragma once

// The characters of the Matrix glyph rain, as strokes.
//
// Half-width katakana, a few digits and a few signs, each a handful of straight
// strokes on a small grid. Drawing them from strokes, not from a font, keeps
// the HUD free of any font or text library, makes every glyph look the same on
// every machine, and lets the baker (stamp.hpp) give them the same bright core
// and soft halo as the lines of the frame. They are drawn mirrored, like the
// characters of the film.
//
// Nothing here touches Hyprland, so it is unit tested with a plain compiler.

#include <span>
#include <vector>

namespace smplhud {

// Every glyph sits on a grid of kGlyphW by kGlyphH units, corner to corner.
constexpr int kGlyphW = 4;
constexpr int kGlyphH = 6;

struct GlyphPoint {
    int x = 0, y = 0;
};

// A polyline; a stroke of two equal points is a dot.
using GlyphStroke = std::vector<GlyphPoint>;

int glyphCount();

// The strokes of glyph `index`. Any whole number will do: it wraps around, so a
// hash can be used as it is.
std::span<const GlyphStroke> glyphStrokes(int index);

// The glyph a whole number stands for, in [0, glyphCount()).
int glyphIndex(long long index);

} // namespace smplhud
