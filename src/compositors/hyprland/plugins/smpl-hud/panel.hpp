#pragma once

// The notched frame of a Kind::Panel style.
//
// A panel is a closed outline of straight and 45 degree runs round the window:
// chamfered corners, a notch and a step on the top edge, a raised and a ghost
// line on the bottom edge and a bigger cut at the bottom left. Everything is
// measured in physical px from the top-left corner of the window box and
// snapped so that lines land on whole pixels; see stamp.hpp for how the lines
// become images. Two looks are made of it:
//
//  - Amber: three thin traces run beyond the outline and end in beads. While
//    the window has focus, dots move along the lines: two go round the outline
//    in opposite directions and one bounces back and forth between the beads of
//    each trace. A dot going round glows brighter when a bouncing dot or the
//    other one comes close.
//
//  - Matrix: the outline alone. Characters fall down both straight sides, two
//    columns a side, one on each side of the line, inside its glow. Along the
//    top and the bottom edge a few glows, the lamps, go slowly to and fro and
//    pulse. All of it only while the window has focus.
//
// Nothing here touches Hyprland, so it is unit tested with a plain compiler.

#include "geometry.hpp"

#include <span>
#include <vector>

namespace smplhud {

// Fills `out.panel`, `out.centre`, `out.sampleBox`, `out.extents` and `out.valid`
// for a style of Kind::Panel. The extents do not depend on the window size.
void buildPanel(const Style& style, const Params& params, Layout& out);

struct RunnerPose {
    Vec2  pos;
    float alpha = 0.f; // 0 where the line under the dot is invisible
    float glow  = 0.f; // 0..1, how far the dot has swelled into the panel's flare
};

// How much a dot running back and forth slows down towards the ends of its
// line: at the ends it goes (1 - kBounceEase) times as fast as it does on
// average, half way along (1 + kBounceEase) times.
constexpr double kBounceEase = 0.4;

// Where `runner` is once the dots have travelled `travelled` px at speed 1.
// On a closed stroke it goes round and round, the other way if its speed is
// negative. On an open one it goes back and forth between the ends, never
// fading, and takes `length / speed` to get from one end to the other. Its
// glow is not known without the other dots; see runnerPoses.
RunnerPose runnerPose(const Panel& panel, const Runner& runner, double travelled);

// The poses of all of `panel.runners`, in that order, with the glow of the ones
// going round: it rises as they come close to a bouncing dot or to each other
// and is 1 where they touch.
std::vector<RunnerPose> runnerPoses(const Panel& panel, double travelled);

// Where a lamp is, how long and how strong, once the lamps have travelled
// `travelled` px at speed 1 and `clock` seconds have gone by since the window
// came to life. It goes to and fro between the two ends of its stretch of the
// track, slowing down towards them like a bouncing dot does, and pulses.
// `index` is the lamp's place in `panel.lamps` and `seed` tells windows apart:
// together they make every lamp of every window start somewhere else.
//
// The pose is in steps coarse enough that nothing needs redrawing until a step
// is crossed: the place is a multiple of `Lamp::step`, the length changes in a
// few steps and the strength in a dozen.
struct LampPose {
    double at       = 0;   // px along Panel::track
    float  length   = 1.f; // times the theme's glow length: the lamp's own, swelling a little with the pulse
    float  strength = 1.f; // 0..1, how strongly it glows

    bool operator==(const LampPose&) const = default;
};

LampPose lampPose(const Panel& panel, size_t index, double travelled, double clock, unsigned seed);

// A character to draw. Characters never move: each sits in a cell of a column
// and only its look changes, in steps coarse enough that nothing needs
// redrawing until a step is crossed.
struct GlyphDraw {
    int id    = 0; // ascending in a list; the cells of the first column come first, top to bottom, then those of the next
    int x     = 0; // the middle of the character, whole px
    int y     = 0;
    int glyph = 0; // see glyph.hpp
    int alpha = 0; // 1..kGlyphAlphaSteps
    int heat  = 0; // 0..kGlyphHeatSteps: how far it is overdrawn in the bright colour

    bool operator==(const GlyphDraw&) const = default;
};

constexpr int kGlyphAlphaSteps = 32;
constexpr int kGlyphHeatSteps  = 8;

// The characters of a glyph style that are lit when the columns have fallen
// `travelled` px at speed 1, `clock` seconds after the window came to life, in
// ascending id order. `intensity` (0..1) is how far the window has focus and
// `seed` tells windows apart.
//
// Each column has a few falls at once: a bright head that goes down a row at a
// time, never up, and a tail of characters that dim as they get further behind
// it. A fall begins at the top of the column, grows its tail, runs off the
// bottom, and after a pause begins again, each time with a tail of another
// length (eight to twenty-eight rows, less in a short column). Every character
// changes into another one now and then, the head faster. At intensity 0
// nothing is lit, and nothing is if the panel has no characters (see
// GlyphFace::on).
std::vector<GlyphDraw> glyphDraws(const Panel& panel, double travelled, double clock, double intensity, unsigned seed);

// What to redraw to get from one list of `glyphDraws` to the next: every
// character that was added, removed or changed, as it is in the list it is in.
std::vector<GlyphDraw> glyphChanges(std::span<const GlyphDraw> before, std::span<const GlyphDraw> after);

} // namespace smplhud
