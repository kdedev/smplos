#pragma once

// The notched, beaded frame of a Kind::Panel style.
//
// A panel is a closed outline of straight and 45 degree runs round the window:
// chamfered corners, a notch and a step on the top edge, a raised and a ghost
// line on the bottom edge and a bigger cut at the bottom left. Three thin
// traces run beyond it and end in beads. While the window has focus, dots move
// along the lines: two go round the outline in opposite directions and one
// bounces back and forth between the beads of each trace. A dot going round
// glows brighter when a bouncing dot or the other one comes close. Everything
// is measured in physical px from the top-left corner of the window box and
// snapped so that lines land on whole pixels; see stamp.hpp for how the lines
// become images.
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

    // Where it is on its stroke, for whatever travels with it (the characters of a glyph style).
    int    stroke = -1;
    double at     = 0; // px from the start of the stroke
    int    dir    = 0; // +1 or -1 along the stroke for a dot going round, 0 for one running back and forth
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

// A character to draw. Characters never move: each sits in a place of the panel
// (a bead, or a slot along a line) and only its look changes, in steps coarse
// enough that nothing needs redrawing until a step is crossed.
struct GlyphDraw {
    int id    = 0; // ascending in a list; the beads come first, then the slots
    int x     = 0; // the middle of the character, whole px
    int y     = 0;
    int glyph = 0; // see glyph.hpp
    int alpha = 0; // 1..kGlyphAlphaSteps
    int heat  = 0; // 0..kGlyphHeatSteps: how far it is overdrawn in the bright colour

    bool operator==(const GlyphDraw&) const = default;
};

constexpr int kGlyphAlphaSteps = 32;
constexpr int kGlyphHeatSteps  = 8;

// The characters of a glyph style that are lit `clock` seconds after the window
// came to life, in ascending id order. `poses` are those of `runnerPoses`,
// `intensity` (0..1) is how far the window has focus and `seed` tells windows apart.
//
//  - Each bead is a character that breathes slowly and changes every breath. At
//    intensity 0 it just rests. It lights up when a dot on a trace reaches it.
//  - A dot leaves a trail of characters along its line: behind one going round,
//    on both sides of one running back and forth, longer and bigger where it is
//    glowing.
//  - A few characters along the lines light up and fade again on their own.
//
// Nothing is lit but the beads at intensity 0, and nothing at all if the panel
// has no characters (see GlyphFace::on).
std::vector<GlyphDraw> glyphDraws(const Panel& panel, std::span<const RunnerPose> poses, double clock, double intensity, unsigned seed);

// What to redraw to get from one list of `glyphDraws` to the next: every
// character that was added, removed or changed, as it is in the list it is in.
std::vector<GlyphDraw> glyphChanges(std::span<const GlyphDraw> before, std::span<const GlyphDraw> after);

} // namespace smplhud
