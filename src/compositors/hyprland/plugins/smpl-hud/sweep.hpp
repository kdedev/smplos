#pragma once

// The glow that travels slowly along a window's HUD line.
//
// Pure geometry and pixel maths with no Hyprland dependency, so it is unit
// tested like geometry.cpp. The compositor side (timer, damage, texture upload
// and drawing) lives in HudDecoration and HudPassElement.
//
// Everything is in window-relative physical px, like the rings of a Layout.

#include "geometry.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace smplhud {

// The closed track the glow follows, clockwise on screen: the Centreline as a
// polyline, or the outline of a panel. The glow is dark wherever the line is
// (a segment's `alphaFrom`/`alphaTo` are how visible it is at its ends).
struct Path {
    struct Segment {
        Vec2   from;
        Vec2   dir; // unit vector along the segment
        double length = 0;
        double arc    = 0; // distance along the track where the segment starts
        float  alphaFrom = 1.f, alphaTo = 1.f;
    };

    std::vector<Segment> segments;
    double               length = 0; // the whole track

    bool valid() const { return length > 0 && !segments.empty(); }
};

struct PathPoint {
    Vec2 pos;
    Vec2 dir; // unit tangent
};

Path buildPath(const Centreline& centre);

// The track through `pts`, closed back to the first point. Clockwise on screen.
Path buildPath(std::span<const Vertex> pts);

// Position and direction `s` px along the track; `s` wraps around.
PathPoint pointAt(const Path& path, double s);

struct SweepShape {
    double halfLength = 0; // along the track
    double reachOut   = 0; // across the track, on the outside
    double reachIn    = 0; // across the track, on the window side
    float  whiten     = 0; // how far the core is mixed towards white
    float  alpha      = 0; // peak opacity

    bool operator==(const SweepShape&) const = default;
};

struct SweepColour {
    float r = 1, g = 1, b = 1; // straight (not premultiplied), 0..1
    float strength  = 1;       // scales the opacity
};

// A small premultiplied RGBA image of the glow. Its texels sit on a grid
// anchored at the window's top-left corner, so a glow that moves keeps
// sampling the same lattice. The GPU's bilinear filter smooths the upscale.
struct Sprite {
    IBox                 box; // px; x, y, w and h are multiples of `texel`
    int                  texel  = 1;
    int                  width  = 0; // texels
    int                  height = 0;
    std::vector<uint8_t> rgba; // premultiplied, width * height * 4

    bool empty() const { return width <= 0 || height <= 0; }
};

// Texel size in px: two logical px per texel, whatever the scale. The glow is
// soft and the GPU's bilinear filter smooths the upscale, while a quarter of
// the texels is a quarter of the work to rasterise and upload at every step.
int sweepTexel(double scale);

// The box a sprite for a glow centred `s` px along the track covers: the part
// of the track under the glow plus its reach, snapped outwards to the texel
// grid and kept inside `limit`. Empty (w or h 0) if there is nothing to draw.
IBox sweepBounds(const Path& path, double s, const SweepShape& shape, int texel, const IBox& limit);

// Draws that glow: an ellipse around the track point at `s`, brightest at its
// centre and fading smoothly to nothing, never beyond `sweepBounds`.
Sprite renderSweep(const Path& path, double s, const SweepShape& shape, const SweepColour& colour, int texel, const IBox& limit);

// Seconds between moves of the glow: about every 3.5 logical px of travel
// (20 a second at the Neon speed, an even three frames of a 60 Hz monitor),
// kept between 16 and 100 ms so a fast glow costs at most ~60 updates a
// second and a slow one still looks smooth.
double sweepInterval(double speed);

// Where the glow is, as a fraction of the track in [0, 1), after `seconds`
// at `speed` logical px per second. `trackLength` is in physical px.
double sweepAdvance(double phase, double seconds, double speed, double scale, double trackLength);

// The glow belongs to the focused window: it fades in when the window gains
// focus and out when it loses it. `level` in [0, 1] moves by `seconds` out of
// `duration` towards 1 (focused) or 0 (not), and stops there.
double sweepFade(double level, bool focused, double seconds, double duration);

// How strong the glow is at a fade level: eased, so it neither jumps in nor
// lingers at either end.
double sweepIntensity(double level);

} // namespace smplhud
