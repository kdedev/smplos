#pragma once

// Compositor-free HUD geometry.
//
// A HUD style plus a window size becomes a list of shapes (concentric
// chamfered border rings and flat fills) in physical pixels, relative to the
// top-left corner of the window box. Nothing here touches Hyprland, so it can
// be unit tested with a plain compiler.

#include <span>
#include <string_view>
#include <vector>

namespace smplhud {

struct IBox {
    int  x = 0, y = 0, w = 0, h = 0;
    bool operator==(const IBox&) const = default;
};

struct Vec2 {
    double x = 0, y = 0;
};

// How far the HUD paints past each side of the window box, in logical px.
struct Extents {
    int  left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const Extents&) const = default;
};

// Which colour set a shape takes. The plugin maps these onto the window's own
// border / glow gradients so the HUD follows the active theme and animations.
enum class Paint {
    Line,   // the window's border gradient
    Glow,   // the window's glow gradient
    Bright, // border gradient mixed towards white (brackets, tabs)
};

// A band drawn with the compositor's border shader.
struct Ring {
    IBox  inner;               // inner edge of the band
    int   borderSize  = 1;     // logical px; the renderer turns it into physical px
    int   thickness   = 1;     // the same thickness in physical px
    int   round       = 1;     // inner-edge corner leg, measured from the inner box
    int   outerRound  = 1;     // outer-edge corner leg, measured from the outer box
    Paint paint       = Paint::Line;
    float alpha       = 1.f;
    bool  clipped     = false; // only the part inside `clip` is drawn
    IBox  clip;
};

// A flat chamfered rectangle (edge tabs and their glow).
struct Fill {
    IBox  box;
    int   round = 1;
    Paint paint = Paint::Line;
    float alpha = 1.f;
};

// How a style is drawn.
enum class Kind {
    Rings, // concentric chamfered bands round the window, drawn with the compositor's border shader
    Panel, // a notched outline with traces and beads, drawn from small baked images (panel.hpp)
};

// A look. New HUDs are new entries in the registry in geometry.cpp.
struct Style {
    const char* name;

    float lineAlpha;

    // Glow halos on both sides of the line, drawn as bands with a quadratic
    // alpha falloff.
    float haloAlpha;       // alpha of the brightest band
    float haloReach;       // logical px the halo extends past the line
    float floatBoost;      // brightness factor for floating windows
    float haloInnerReach;  // logical px of halo on the window side of the line
    float haloInnerFactor; // inner halo brightness relative to the outer one

    // Corner brackets: the line, thicker and brighter, near each corner.
    float bracketLen; // logical px of each arm; 0 disables brackets
    int   bracketExtra;      // logical px thicker than the line on each side
    int   bracketGlowReach;  // logical px of glow around the brackets
    float bracketGlowAlpha;

    // Tabs in the middle of each edge.
    float tabLen; // logical px; 0 disables tabs
    int   tabExtra;
    float tabAlpha;
    int   tabGlowGrow[2];
    float tabGlowAlpha[2];

    // How far the bright colours are mixed towards white.
    float brightWhiten;

    // The glow that slowly travels along the line (theme `hud_sweep`).
    float sweepAlpha;  // peak opacity at its centre; 0 turns the sweep off for this style
    float sweepWhiten; // how far its core is mixed towards white

    // Panel styles only (Kind::Panel). They use lineAlpha, haloAlpha, haloReach
    // and floatBoost like the ring styles do.
    Kind  kind        = Kind::Rings;
    float notchDepth  = 0; // logical px the outline steps towards the window at a notch
    float traceOffset = 0; // logical px the outer traces run beyond the outline
    float beadRadius  = 0; // logical px; 0 draws no beads and no runners
    float runnerSpeed = 1; // speed of the runner dots relative to the travelling glow
};

struct Params {
    int    width  = 0; // window size in physical px
    int    height = 0;
    float  scale  = 1.f;
    int    cut    = 0;   // corner leg of the window itself, physical px
    float  roundingPower = 1.f;
    bool   floating = false;

    // theme knobs
    double gap            = 6;
    double lineWidth      = 3;
    double chamfer        = 10;
    double floatGlowRange = 14;
};

// The rounded rectangle through the middle of the line: the track the sweep
// follows, in the same window-relative physical px as the rings.
struct Centreline {
    double x = 0, y = 0, w = 0, h = 0;
    double leg      = 0; // corner leg
    double power    = 1; // corner rounding power
    double reachIn  = 0; // room for glow on the window side, without covering the window
    double reachOut = 0; // room for glow outside, up to where the outer halo ends
};

// A point of a stroke; `alpha` fades the line along its length.
struct Vertex {
    double x = 0, y = 0;
    float  alpha = 1.f;
};

// A straight run of a stroke, drawn as a 2 x N image stretched along it: the
// first texel column holds the colour at one end, the second the colour at the
// other, and N rows hold what the line looks like across itself.
struct StripPiece {
    IBox   box;        // along the run and, across it, at natural size
    bool   vertical = false;
    Vec2   a, b;       // the ends of the run on the centre line
    float  alphaA = 1.f, alphaB = 1.f;
    double centre = 0; // coordinate of the centre line across the run
};

// A piece of a stroke inside a tile, with the line's opacity at both ends.
struct TileSeg {
    Vec2  a, b;
    float alphaA = 1.f, alphaB = 1.f;
};

// A small image of a stroke where it turns: the part of the stroke near
// `box`, running out to the box's edges.
struct TilePiece {
    IBox                 box;
    std::vector<TileSeg> segs;
};

// A line made of straight and 45 degree runs, in window-relative physical px.
// A closed stroke is cut open where it is invisible, or else half way along
// its longest straight run.
struct Stroke {
    std::vector<Vertex> pts;
    bool                closed = false;
    float               alpha  = 1.f; // overall opacity of the line
    Paint               core   = Paint::Line;
    Paint               halo   = Paint::Glow;
    double              halfWidth = 1; // half the line's thickness, px
    double              reach     = 0; // how far the halo reaches past the line's edge, px
    float               haloAlpha = 0; // halo opacity at the line's edge

    // Where the stroke is cut into drawable pieces (see stamp.hpp).
    std::vector<StripPiece> strips;
    std::vector<TilePiece>  tiles;
};

// A dot: a bright core with a soft halo.
struct Bead {
    Vec2   pos;
    double radius    = 1;
    double reach     = 0;
    float  haloAlpha = 0;
    float  alpha     = 1.f;
};

// A dot that travels along a stroke while its window has focus: round and round
// a closed stroke, back and forth on an open one.
struct Runner {
    int    stroke = 0;   // index into Panel::strokes
    double offset = 0;   // where it starts, as a fraction of the stroke (of one way, on an open one)
    double speed  = 1;   // relative to the other runners; a negative one runs the other way
    Bead   look;         // `pos` is unused
};

// How far apart two dots light each other up: fully within `near` px, not at
// all from `far` px on, smoothly in between.
struct Proximity {
    double near = 0;
    double far  = 0;
};

struct Panel {
    std::vector<Stroke> strokes; // strokes[0] is the closed outline round the window
    std::vector<Bead>   beads;   // dots that stay where they are
    std::vector<Runner> runners;

    // A dot going round a closed stroke swells into `flare` (drawn over it) when
    // another dot is near: one running on an open stroke (`touch`, measured in a
    // straight line) or the other one going round (`merge`, measured along the
    // stroke). A flare of radius 0 turns this off.
    Bead      flare = {{}, 0.0, 0.0, 0.f, 1.f}; // no radius: none
    Proximity touch;
    Proximity merge;

    bool empty() const { return strokes.empty(); }
};

struct Layout {
    bool              valid = false;
    std::vector<Ring> rings;  // drawn in order
    std::vector<Fill> fills;  // drawn after all rings, in order
    Panel             panel;  // the shapes of a Kind::Panel style; empty for ring styles
    IBox              sampleBox; // box the line gradient is normalised over
    Centreline        centre;
    Extents           extents;
};

constexpr int kMinWindow = 40; // physical px; smaller windows get no HUD

// Corner-leg change per pixel of outward offset (1 for round corners,
// 2 - sqrt(2) for 45 degree chamfers), the same rule Hyprland's own border uses.
double concentricFactor(double roundingPower);

// Looks up a style by name; nullptr when unknown.
const Style* styleFor(std::string_view name);

// Every registered style, so the tests cover a new look without being edited.
std::span<const Style> allStyles();

Layout  buildLayout(const Style& style, const Params& params);

// Extents do not depend on the window size, only on style, scale and theme.
Extents extentsFor(const Style& style, const Params& params);

} // namespace smplhud
