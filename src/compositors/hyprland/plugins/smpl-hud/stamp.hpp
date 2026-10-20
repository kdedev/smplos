#pragma once

// Strokes and beads as small images.
//
// A panel (see panel.hpp) is a line of straight and 45 degree runs plus a few
// dots. The compositor can only draw textured quads, so each stroke is cut
// into pieces that are baked once and then reused from a cache:
//
//   strips  straight horizontal or vertical runs. The image is 2 texels long:
//           one column holds the colour at each end and the GPU's bilinear
//           filter blends between them, so one image serves a run of any length.
//   tiles   bitmaps of the places where a line turns, steps or ends. They are
//           drawn 1:1, so they are exact whatever the angle.
//   beads   soft round dots.
//
// Pieces of one stroke never overlap: a strip stops where a tile starts and a
// tile reaches exactly as far as the line's glow does. Nothing here touches
// Hyprland, so it is unit tested with a plain compiler.

#include "geometry.hpp"
#include "gradient.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace smplhud {

// A stroke segment is invisible when its opacity is below this.
constexpr float kInvisible = 0.004f;

// How far the image of a line reaches from its centre, in whole px: half its
// thickness, its glow and a px of slack.
int roomAround(double halfWidth, double reach);

// Cuts `stroke.pts` into `stroke.strips` and `stroke.tiles`. The points must
// sit where the panel builder puts them: whole or half px, 45 degrees apart.
void cutStroke(Stroke& stroke);

// Length of a stroke in px; a closed stroke counts its closing segment.
double strokeLength(const Stroke& stroke);

struct StrokePoint {
    Vec2  pos;
    float alpha = 1.f; // how visible the line is there, before the stroke's own opacity
};

// The point `s` px along a stroke. A closed stroke wraps around, an open one stops at its ends.
StrokePoint strokePointAt(const Stroke& stroke, double s);

// A premultiplied RGBA image, row by row from the top.
struct Image {
    int                  w = 0, h = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return w <= 0 || h <= 0; }
};

// How a line looks across itself. Whole numbers only, so that equal looks
// bake equal images and can share one cache entry.
struct Look {
    int halfWidth16 = 16; // half the line's thickness, in 1/16 px
    int reach16     = 0;  // how far the halo reaches past the line's edge
    int lineAlpha   = 255;
    int haloAlpha   = 0;  // the halo's opacity at the line's edge

    bool operator==(const Look&) const = default;
};

Look lookOf(const Stroke& stroke);
Look lookOf(const Bead& bead);
Look lookOf(const GlyphFace& face); // the core is half the thickness of a stroke of a character

// The colours of the line and of its glow at a place: straight (not
// premultiplied) 8 bit RGBA, rounded to 5 bits a channel (`levels` + 1 steps) so
// that a slowly changing gradient does not bake a new image at every step.
struct Tone {
    uint8_t line[4] = {255, 255, 255, 255};
    uint8_t glow[4] = {255, 255, 255, 255};

    bool operator==(const Tone&) const = default;
};

constexpr int kToneLevels = 31;

Tone makeTone(const Rgba& line, const Rgba& glow, int levels = kToneLevels);

// Opacity 0..1 as 8 bit.
int alpha8(float alpha);

struct StripSpec {
    bool  vertical = false;
    int   size     = 0; // texels across the line
    int   centre2  = 0; // twice the line's centre, in texels from the first row or column
    Look  look;
    Tone  a, b;         // colours at the two ends (the smaller coordinate is `a`)
    int   alphaA = 255, alphaB = 255;
};

// 2 texels by `size` for a horizontal strip, `size` by 2 for a vertical one.
// A strip is drawn on a quad twice as long as the run, so that the run's ends
// sit at the two texel centres and the colour changes linearly from one to the other.
Image       bakeStrip(const StripSpec& spec);
std::string keyOf(const StripSpec& spec);

// The spec of a strip piece of `stroke`, with the colours at its two ends.
StripSpec stripSpecOf(const StripPiece& strip, const Stroke& stroke, const Tone& a, const Tone& b);

struct Quad {
    double x = 0, y = 0, w = 0, h = 0;
};

// Where the image of a strip goes: a quad twice as long as the run and centred
// on it. The run itself is its middle half, `strip.box`, and that is the only
// part to paint (as the damage): there the filter blends from the colour at one
// end of the run to the colour at the other.
Quad stripQuad(const StripPiece& strip);

struct TileSegSpec {
    int ax = 0, ay = 0, bx = 0, by = 0; // 1/16 px, relative to the tile's corner
    int alphaA = 255, alphaB = 255;
};

struct TileSpec {
    int                     w = 0, h = 0;
    Look                    look;
    Tone                    tone;
    std::vector<TileSegSpec> segs;
};

TileSpec    tileSpecOf(const TilePiece& tile, const Stroke& stroke, const Tone& tone);
Image       bakeTile(const TileSpec& spec);
std::string keyOf(const TileSpec& spec);

struct BeadSpec {
    Look look; // the core is the line, the halo is the halo
    Tone tone; // `line` colours the core, `glow` the halo
};

// Side of a bead's image in px, always even: the dot sits on the middle corner.
int beadSize(const Look& look);

Image       bakeBead(const BeadSpec& spec);
std::string keyOf(const BeadSpec& spec);

// A Matrix glyph (glyph.hpp) as an image: its strokes get the core and the halo
// of a line, and the whole is mirrored left to right. Under the strokes goes a
// soft black plate, so that a line that runs behind the glyph is dimmed there
// rather than striking it through.
struct GlyphSpec {
    int  glyph    = 0; // any whole number; it wraps around the glyphs there are
    int  height16 = 0; // the glyph's height in 1/16 px; its width follows from the grid
    Look look;         // the core is half the thickness of the strokes
    Tone tone;
    int  knock    = 0; // the plate's opacity, 0..255; 0 has none
};

struct GlyphSize {
    int w = 0, h = 0;
};

// Size of a glyph's image in px, both even: the glyph sits on the middle
// corner, so a glyph drawn at whole px lands on whole px. Empty for no height.
GlyphSize glyphSize(int height16, const Look& look);

// The same for a character of a panel's face.
GlyphSize glyphSize(const GlyphFace& face);
GlyphSpec glyphSpecOf(const GlyphFace& face, int glyph, const Tone& tone);

Image       bakeGlyph(const GlyphSpec& spec);
std::string keyOf(const GlyphSpec& spec);

} // namespace smplhud
