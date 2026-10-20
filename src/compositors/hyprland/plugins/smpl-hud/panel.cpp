#include "panel.hpp"

#include "glyph.hpp"
#include "stamp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace smplhud {

namespace {

constexpr double kSqrt2 = std::numbers::sqrt2;

// The traces are dimmer than the outline and keep part of its glow.
constexpr double kTraceAlpha = 0.55;
constexpr double kTraceReach = 0.6;
constexpr double kTraceHalo  = 0.5;

constexpr float kRampAlpha = 0.7f; // opacity at the top of the ramp that leads into the ghost line

constexpr double kBeadMin    = 1.5; // px
constexpr double kBeadReach  = 0.8; // of the outline's glow reach
constexpr double kRunnerGrow = 1.15;

// The two dots going round the outline run opposite ways, one a little slower
// than the other, so that they do not always meet at the same two places.
constexpr double kRoundSpeed = 1.0;
constexpr double kBackSpeed  = 0.85;

// The dots on the traces start in different places along them.
constexpr double kBounceStart[3] = {0.0, 0.4, 0.75};

// A dot going round swells into a flare, in terms of the dot's own sizes.
constexpr double kFlareRadius = 1.3;
constexpr double kFlareReach  = 2.0;
constexpr double kFlareHalo   = 1.7;

// It is fully lit within kTouchNear px of a dot on a trace and not at all from
// kTouchFar px on (at scale 1).
constexpr double kTouchNear = 14;
constexpr double kTouchFar  = 70;

// It is lit by the other dot going round from kMergeShare of the outline's
// length away, but from no less than kMergeMin and no more than kMergeMax px
// (at scale 1) and never from more than kMergeCap of it, and fully so within a
// tenth of that.
constexpr double kMergeShare = 0.10;
constexpr double kMergeMin   = 160;
constexpr double kMergeMax   = 400;
constexpr double kMergeCap   = 0.35;

// Frames smaller than this get no notches, no ghost line and no traces.
constexpr double kFancyWidth  = 480;
constexpr double kFancyHeight = 160;

// The characters of a glyph style, in terms of their height.
constexpr double kGlyphStroke = 0.05; // half the thickness of a stroke
constexpr double kGlyphMin    = 6;    // px; no character is smaller
constexpr double kGlyphHalo   = 1.25; // their glow is this much brighter than the line's

// The columns the characters fall in: the one on the window's side of the line
// is centred in the gap between the line and the window, the other is its mirror
// image across the line. A character keeps kGlyphGap px from the line and
// kGlyphClear px from the window, and a column starts and ends a pitch short of
// the ends of the straight run, clear of the corners. One with fewer than
// kLaneMinCells characters is not worth having. A column has one fall for every
// kLaneCells characters, but no more than kLaneMaxStreams.
constexpr double kGlyphGap       = 0.5;
constexpr double kGlyphClear     = 1.5;
constexpr double kLaneInset      = 1.0; // in pitches
constexpr int    kLaneMinCells   = 3;
constexpr double kLaneCells      = 20;
constexpr int    kLaneMaxStreams = 3;

// The lamps on the top and the bottom edge. An edge has room for one for every
// kLampRoom lamp lengths, but never more than the style asks for, and none if it
// is shorter than a lamp. Every lamp has a look of its own.
struct LampLook {
    double length, speed, offset, period, phase;
};

constexpr double   kLampRoom     = 1.6;
constexpr LampLook kLampsTop[2]    = {{1.0, 0.8, 0.0, 4.6, 0.0}, {0.65, -0.55, 0.3, 6.1, 0.45}};
constexpr LampLook kLampsBottom[2] = {{0.9, 0.7, 0.15, 5.3, 0.2}, {0.65, -0.5, 0.45, 4.2, 0.7}};
constexpr int      kLampMax      = 2;

constexpr double kLampStep         = 4.0;  // logical px: a lamp is redrawn only when it has gone this far
constexpr double kLampSwell        = 0.12; // a lamp is this much longer at the top of a pulse and shorter at the bottom
constexpr double kLampDim          = 0.35; // how strong it is at the bottom of a pulse
constexpr int    kLampLengthSteps  = 6;
constexpr int    kLampStrengthSteps = 12;

// The rectangle the outline follows, and the sizes that shape it.
struct Frame {
    double s    = 1;
    double hw   = 0.5; // half the line's thickness
    double c    = 0;   // distance of the centre line from the window box
    bool   even = true;

    double left = 0, top = 0, right = 0, bottom = 0;

    double cutEff = 0; // the window's own corner, as the leg of the 45 degree cut that crosses it at the same place
    double a      = 0; // leg of the 45 degree corners
    double d      = 0; // how far the notches step towards the window
    double b      = 0; // leg of the cut at the bottom left
    double tau    = 0; // how far the outer traces run beyond the frame
    bool   fancy  = false;

    double fw() const { return right - left; }
    double fh() const { return bottom - top; }

    // A coordinate across a horizontal or vertical line, put where the line covers whole pixels.
    double snap(double v) const { return even ? std::round(v) : std::floor(v) + 0.5; }
};

Frame measure(const Style& st, const Params& p) {
    Frame f;
    f.s = p.scale;

    const int bs = std::max(1, int(std::lround(p.lineWidth)));
    const int T  = std::max(1, int(std::round(float(bs) * p.scale)));
    f.hw         = T / 2.0;
    f.even       = T % 2 == 0;
    f.c          = f.snap(std::max(p.gap * f.s, f.hw + 2.0));
    f.left = f.top = -f.c;
    f.right        = p.width + f.c;
    f.bottom       = p.height + f.c;

    const double power = std::max(1.0, double(p.roundingPower));
    f.cutEff           = 2.0 * p.cut * (1.0 - std::pow(std::cos(std::numbers::pi / 4), 2.0 / power));

    // Concentric with the window's corner, but not further in than the theme's chamfer
    // and never closer to the window than about 3 px.
    double a = std::max(p.chamfer * f.s, f.cutEff + f.c * concentricFactor(p.roundingPower));
    a        = std::min({a, f.cutEff + 2 * f.c - 4.2, std::floor(std::min(f.fw(), f.fh()) / 3.0)});
    f.a      = std::max(2.0, std::round(a));

    f.d = std::min(std::round(st.notchDepth * f.s), f.c - 4);
    if (f.d < 3)
        f.d = 0;
    f.b   = std::floor(std::min(3 * f.a, f.cutEff + 2 * f.c - f.d - 4.2));
    f.tau = st.traceOffset > 0 ? std::max(3.0, std::round(st.traceOffset * f.s)) : 0.0;
    return f;
}

// Where the notches, ramps and gaps sit along the top and bottom edges. The
// fractions are those of the reference artwork.
struct Cuts {
    double notchX     = 0; // where the ramp into the notch on the top edge starts
    double notchFloor = 0; // length of its floor
    double stepX      = 0; // where the ramp into the step near the top right starts
    double baseX2     = 0; // left end of the right-hand baseline
    double ghostX     = 0; // left end of the ghost line
    double endX1      = 0; // right end of the left-hand baseline, where it has faded out
    double baseX1     = 0; // left end of the left-hand baseline, where the ramp up to the recess starts
    double fade       = 0; // length over which the left-hand baseline fades out
};

// False when the features would collide.
bool placeCuts(const Frame& f, Cuts& k) {
    const double fw = f.fw(), s = f.s, d = f.d;
    k.notchX     = std::round(f.left + 0.357 * fw);
    k.notchFloor = std::round(std::clamp(0.097 * fw, 50 * s, 160 * s));
    k.stepX      = std::round(f.right - std::clamp(0.18 * fw, 80 * s, 240 * s));
    k.baseX2     = std::round(f.left + 0.659 * fw);
    k.ghostX     = k.baseX2 - d - std::round(std::clamp(0.13 * fw, 40 * s, 220 * s));
    k.endX1      = std::round(f.left + 0.593 * fw);
    k.baseX1     = std::round(f.left + 0.354 * fw) + d;
    k.fade       = std::round(14 * s);

    const double room = 10 * s;
    return k.notchX > f.left + f.a + room                                  //
        && k.notchX + 2 * d + k.notchFloor + room < k.stepX                //
        && k.stepX + room < f.right - f.a                                  //
        && f.left + f.b + room < k.baseX1 - d                              // recess line
        && k.baseX1 + room < k.endX1 - k.fade                              // left-hand baseline
        && k.ghostX > k.baseX1 - d + room                                  // ghost line starts after the recess line ends
        && k.ghostX + room < k.baseX2 - d                                  //
        && k.baseX2 + room < f.right - f.a;                                // right-hand baseline
}

std::vector<Vertex> plainOutline(const Frame& f) {
    const double a = f.a;
    return {
        {f.left + a, f.top, 1.f},        {f.right - a, f.top, 1.f},    {f.right, f.top + a, 1.f},   {f.right, f.bottom - a, 1.f},
        {f.right - a, f.bottom, 1.f},    {f.left + a, f.bottom, 1.f},  {f.left, f.bottom - a, 1.f}, {f.left, f.top + a, 1.f},
    };
}

// Clockwise from the top edge. The bottom edge doubles back across an invisible
// segment: the ghost line ends, the left-hand baseline begins.
std::vector<Vertex> fancyOutline(const Frame& f, const Cuts& k) {
    const double a = f.a, d = f.d, b = f.b;
    return {
        {f.left + a, f.top, 1.f},
        {k.notchX, f.top, 1.f},
        {k.notchX + d, f.top + d, 1.f},
        {k.notchX + d + k.notchFloor, f.top + d, 1.f},
        {k.notchX + 2 * d + k.notchFloor, f.top, 1.f},
        {k.stepX, f.top, 1.f},
        {k.stepX + d, f.top + d, 1.f},
        {f.right - a + d, f.top + d, 1.f},
        {f.right, f.top + a, 1.f},
        {f.right, f.bottom - a, 1.f},
        {f.right - a, f.bottom, 1.f},
        {k.baseX2, f.bottom, 1.f},
        {k.baseX2 - d, f.bottom - d, kRampAlpha},
        {k.ghostX, f.bottom - d, 0.f},
        {k.endX1, f.bottom, 0.f},
        {k.endX1 - k.fade, f.bottom, 1.f},
        {k.baseX1, f.bottom, 1.f},
        {k.baseX1 - d, f.bottom - d, 1.f},
        {f.left + b, f.bottom - d, 1.f},
        {f.left, f.bottom - d - b, 1.f},
        {f.left, f.top + a, 1.f},
    };
}

// A trace and the beads at its two ends.
struct Trace {
    std::vector<Vertex> pts;
    Vec2                from, to;
};

// In the notch on the top edge: two beads and a dim line between them.
bool notchTrace(const Frame& f, const Cuts& k, Trace& t) {
    const double s = f.s, d = f.d;
    const double floorStart = k.notchX + d, floorEnd = floorStart + k.notchFloor;
    const double x0 = floorStart + std::max(0.25 * d, 2 * s), x1 = floorEnd - std::max(0.3 * d, 2.5 * s);
    const double y  = f.snap(f.top + 0.35 * d);
    if (x1 - x0 < 14 * s)
        return false;
    t.pts  = {{x0 + 3 * s, y, 1.f}, {x1 - 2 * s, y, 1.f}};
    t.from = {x0, f.top + 0.1 * d};
    t.to   = {x1, f.top + 0.1 * d};
    return true;
}

// From the step near the top right, along the top, then down the right side
// beyond the frame, parallel to the corner cut.
bool stepTrace(const Frame& f, const Cuts& k, Trace& t) {
    const double s = f.s, d = f.d;
    const double bx = k.stepX + d + std::round(0.36 * d), by = f.top + 0.1 * d;
    const double xa = bx + 3.5 * s;
    const double yh = f.snap(f.top + 0.33 * d);
    const double xv = f.snap(f.right + f.tau);
    const double yv = f.snap(f.top + f.a - (kSqrt2 - 1) * f.tau);
    const double xh = xv - (yv - yh);
    const double ye = f.bottom - f.a;
    if (xh - xa < 10 * s || ye - yv < 10 * s)
        return false;
    t.pts  = {{xa, yh, 1.f}, {xh, yh, 1.f}, {xv, yv, 1.f}, {xv, ye, 1.f}};
    t.from = {bx, by};
    t.to   = {xv, ye};
    return true;
}

// From the left edge, parallel to the cut at the bottom left and beyond it, then along the bottom.
bool cornerTrace(const Frame& f, const Cuts& k, Trace& t) {
    const double s  = f.s;
    const double y0 = f.bottom - f.d - f.b + std::round(f.tau * kSqrt2);
    const double xd = f.left + (f.bottom - y0);
    const double xe = k.baseX1 - std::round(18 * s);
    if (y0 > f.bottom - 4 || xe - xd < 10 * s)
        return false;
    t.pts  = {{f.left, y0, 1.f}, {xd, f.bottom, 1.f}, {xe, f.bottom, 1.f}};
    t.from = {f.left, y0};
    t.to   = {xe, f.bottom};
    return true;
}

// How far outline and traces are from the window box at the nearest, so that
// the travelling glow can stay off the window.
double minClearance(const Frame& f) {
    double best = (f.cutEff + 2 * f.c - f.a) / kSqrt2;
    best        = std::min(best, f.c - (f.fancy ? f.d : 0.0));
    if (f.fancy)
        best = std::min(best, (f.cutEff + 2 * f.c - f.b - f.d) / kSqrt2);
    return best;
}

// The characters' size.
void placeFace(const Style& style, const Frame& f, double halo, Panel& panel) {
    GlyphFace& face = panel.glyphs;
    face.height     = std::max(kGlyphMin, std::round(style.glyphHeight * f.s));
    face.halfWidth  = kGlyphStroke * face.height;
    face.reach      = style.glyphGlow * face.height;
    face.haloAlpha  = float(std::min(1.0, halo * kGlyphHalo));
    face.pitch      = std::max(face.height * 0.5, std::round(style.glyphPitch * f.s));
    face.fall       = style.glyphFall;
    face.knock      = style.glyphKnock;
}

// Where the columns beside one straight side of the outline are. The line is at
// x = `line` and the window's edge at x = `window`. The first column is the one
// in the gap between the line and the window, if the characters fit in it; the
// second is on the other side of the line, as far from it as the first is. They
// depend on the style and the scale, never on how big the window is.
struct Columns {
    bool fits  = false;
    int  inner = 0, outer = 0;
};

Columns columnsOf(const GlyphFace& face, const Frame& f, double line, double window) {
    const double dir   = window > line ? 1.0 : -1.0;
    const double halfW = face.height * kGlyphW / kGlyphH / 2 + face.halfWidth;
    const double edge  = line + dir * f.hw; // the line's edge on the window's side

    Columns out;
    out.inner         = int(std::lround((edge + window) / 2));
    out.fits          = std::fabs(window - out.inner) >= halfW + kGlyphClear && std::fabs(out.inner - edge) >= halfW + kGlyphGap;
    const double away = std::max(std::fabs(line - out.inner), std::ceil(f.hw + halfW + kGlyphGap));
    out.outer         = int(std::lround(line - dir * away));
    return out;
}

// The columns of one straight side of the outline, between `top` and `bottom`.
// The second is half a pitch lower than the first.
void placeLanes(const GlyphFace& face, const Frame& f, double line, double window, double top, double bottom, std::vector<RainLane>& out) {
    const Columns cols = columnsOf(face, f, line, window);

    const double inset = kLaneInset * face.pitch;
    const auto   lane  = [&](int x, int y0) {
        const int cells = int(std::floor((bottom - inset - y0) / face.pitch + 1e-9)) + 1;
        if (cells >= kLaneMinCells)
            out.push_back({x, y0, cells, std::clamp(int(std::lround(cells / kLaneCells)), 1, kLaneMaxStreams)});
    };

    const int first = int(std::lround(top + inset));
    if (cols.fits)
        lane(cols.inner, first);
    lane(cols.outer, first + int(std::floor(face.pitch / 2)));
}

// The characters' size and the columns they fall in: down both sides, between
// the corners of the outline.
void placeRain(const Style& style, const Frame& f, double halo, Panel& panel) {
    if (style.glyphHeight <= 0)
        return;

    placeFace(style, f, halo, panel);
    const double leftEnd = f.fancy ? f.bottom - f.d - f.b : f.bottom - f.a;
    placeLanes(panel.glyphs, f, f.left, f.left + f.c, f.top + f.a, leftEnd, panel.lanes);
    placeLanes(panel.glyphs, f, f.right, f.right - f.c, f.top + f.a, f.bottom - f.a, panel.lanes);
}

// The track the lamps follow: the outline, dark where it runs down a side, so
// that no glow runs where the characters fall. The corners between a side and
// an edge fade the glow out.
std::vector<Vertex> lampTrack(std::vector<Vertex> pts, const Frame& f) {
    for (Vertex& v : pts) {
        if (v.x == f.left || v.x == f.right)
            v.alpha = 0.f;
    }
    return pts;
}

// The lamps of the top and the bottom edge, going to and fro between the first
// and the last vertex of the stretch of the track that makes up the edge.
void placeLamps(const Style& style, const Params& p, const Frame& f, std::vector<Vertex> outline, Panel& panel) {
    if (style.lamps <= 0)
        return;

    panel.track = lampTrack(std::move(outline), f);
    std::vector<double> at(panel.track.size(), 0.0); // px along the track at each vertex
    for (size_t i = 1; i < at.size(); ++i)
        at[i] = at[i - 1] + std::hypot(panel.track[i].x - panel.track[i - 1].x, panel.track[i].y - panel.track[i - 1].y);

    struct Edge {
        size_t          first, last;
        const LampLook* looks;
    };
    const Edge edges[2] = {f.fancy ? Edge{0, 7, kLampsTop} : Edge{0, 1, kLampsTop}, f.fancy ? Edge{10, 18, kLampsBottom} : Edge{4, 5, kLampsBottom}};

    const double longest = p.sweepLength * f.s;
    for (const Edge& edge : edges) {
        const double from = at[edge.first], to = at[edge.last];
        if (longest <= 0 || to - from < longest)
            continue;

        const int n = std::clamp(int(std::floor((to - from) / (kLampRoom * longest))), 1, std::min(style.lamps, kLampMax));
        for (int i = 0; i < n; ++i) {
            const LampLook& look = edge.looks[i];
            panel.lamps.push_back({from, to, look.length, look.speed, look.offset, look.period, look.phase, std::max(1.0, kLampStep * f.s)});
        }
    }
}

} // namespace

void buildPanel(const Style& style, const Params& p, Layout& out) {
    Frame f = measure(style, p);
    Cuts  cuts;
    f.fancy = f.d > 0 && f.fw() >= kFancyWidth * f.s && f.fh() >= kFancyHeight * f.s && f.b >= f.a && f.a >= f.d + 6 * f.s &&
        f.fh() - f.a - f.d - f.b >= 8 * f.s && placeCuts(f, cuts);

    const double boost = p.floating ? style.floatBoost : 1.0;
    const double reach = std::max(0.0, (p.floating ? std::max<double>(style.haloReach, p.floatGlowRange) : style.haloReach) * f.s);
    const double halo  = std::clamp(style.haloAlpha * boost, 0.0, 1.0);

    const auto stroke = [&](std::vector<Vertex> pts, bool closed, double alpha, double strokeReach, double strokeHalo) {
        Stroke s;
        s.pts       = std::move(pts);
        s.closed    = closed;
        s.alpha     = float(alpha);
        s.halfWidth = f.hw;
        s.reach     = strokeReach;
        s.haloAlpha = float(strokeHalo);
        cutStroke(s);
        return s;
    };

    Panel& panel = out.panel;
    panel        = {};
    std::vector<Vertex> outline = f.fancy ? fancyOutline(f, cuts) : plainOutline(f);
    panel.strokes.push_back(stroke(outline, true, style.lineAlpha, reach, halo));

    // Beads sit at the ends of the traces; dots travel along the outline and the traces.
    Bead bead;
    bead.radius    = std::max(style.beadRadius * f.s, kBeadMin);
    bead.reach     = kBeadReach * reach;
    bead.haloAlpha = float(halo);
    Bead dot       = bead;
    dot.radius *= kRunnerGrow;

    const bool beads = style.beadRadius > 0;
    if (beads) {
        panel.runners.push_back({0, 0.0, kRoundSpeed, dot});
        panel.runners.push_back({0, 0.5, -kBackSpeed, dot});

        panel.flare           = dot;
        panel.flare.radius    = dot.radius * kFlareRadius;
        panel.flare.reach     = dot.reach * kFlareReach;
        panel.flare.haloAlpha = float(std::min(1.0, halo * kFlareHalo));
        panel.touch           = {kTouchNear * f.s, kTouchFar * f.s};

        const double outline = strokeLength(panel.strokes[0]);
        const double far     = std::min(std::clamp(kMergeShare * outline, kMergeMin * f.s, kMergeMax * f.s), kMergeCap * outline);
        panel.merge          = {0.1 * far, far};
    }

    if (f.fancy && f.tau > 0) {
        Trace t;
        const double speeds[3] = {0.6, 1.0, 0.8};
        int          n         = 0;
        const auto   add       = [&](bool ok) {
            const double speed = speeds[n];
            const double start = kBounceStart[n++];
            if (!ok)
                return;
            panel.strokes.push_back(stroke(std::move(t.pts), false, style.lineAlpha * kTraceAlpha, kTraceReach * reach, kTraceHalo * halo));
            if (!beads)
                return;
            bead.pos = t.from;
            panel.beads.push_back(bead);
            bead.pos = t.to;
            panel.beads.push_back(bead);
            panel.runners.push_back({int(panel.strokes.size()) - 1, start, speed, dot});
        };
        t = {};
        add(notchTrace(f, cuts, t));
        t = {};
        add(stepTrace(f, cuts, t));
        t = {};
        add(cornerTrace(f, cuts, t));
    }

    // Room for the outline, the traces, the beads and the characters, whatever the window size.
    placeRain(style, f, halo, panel);
    placeLamps(style, p, f, std::move(outline), panel);

    const double lineRoom = roomAround(f.hw, reach);
    const double dotRoom  = beads ? std::max(beadSize(lookOf(dot)), beadSize(lookOf(panel.flare))) / 2.0 : 0.0;
    const double room     = std::max(lineRoom, dotRoom) + 1;
    const auto   logical  = [&](double phys) { return int(std::ceil(phys / f.s)) + 2; };

    // The characters of a column stick out beyond the window box, and so does their glow, whether or not
    // this window is tall enough for the column.
    double columns = 0;
    if (panel.glyphs.on()) {
        const Columns cols = columnsOf(panel.glyphs, f, f.left, f.left + f.c);
        const double  half = glyphSize(panel.glyphs).w / 2.0 + 1;
        columns            = half - std::min(cols.fits ? double(cols.inner) : 0.0, double(cols.outer)); // 0 is the window's edge
    }

    const int side = logical(std::ceil(f.c) + room);
    out.extents    = {std::max(side, logical(columns)), side, std::max(logical(std::ceil(f.c + f.tau) + room), logical(columns)), side};

    const double clear = minClearance(f);
    const int    edge  = int(std::ceil(f.c + f.hw));
    out.sampleBox      = {-edge, -edge, p.width + 2 * edge, p.height + 2 * edge};
    out.centre         = {f.left, f.top, f.fw(), f.fh(), f.a, 1.0, std::max(0.0, clear - f.hw - 1), f.hw + reach};
    out.valid          = true;
}

namespace {

// Where a runner is, before the dots near it have had their say.
struct Placed {
    RunnerPose pose;
    int        stroke = -1;
    bool       closed = false;
    double     at     = 0; // px from the start of its stroke
    double     len    = 0; // length of its stroke
};

double wrap(double v, double period) {
    return v - std::floor(v / period) * period;
}

// How far along a line (0..1) a dot is that runs back and forth on it, `phase`
// being 0 at the start of the way out, 1 at the end of it and 2 when it is back.
double bounce(double phase) {
    constexpr double tau = 2 * std::numbers::pi;
    const double     q   = wrap(phase, 2.0);
    const double     x   = q <= 1 ? q : 2 - q;
    return std::clamp(x - kBounceEase / tau * std::sin(tau * x), 0.0, 1.0);
}

Placed place(const Panel& panel, const Runner& runner, double travelled) {
    Placed out;
    if (runner.stroke < 0 || size_t(runner.stroke) >= panel.strokes.size())
        return out;
    const Stroke& stroke = panel.strokes[size_t(runner.stroke)];
    const double  len    = strokeLength(stroke);
    if (len <= 0)
        return out;

    const double at = stroke.closed ? wrap(runner.offset * len + travelled * runner.speed, len)
                                    : bounce(runner.offset + travelled * runner.speed / len) * len;
    const StrokePoint p = strokePointAt(stroke, at);

    out.pose   = {p.pos, std::clamp(p.alpha * runner.look.alpha, 0.f, 1.f), 0.f};
    out.stroke = runner.stroke;
    out.closed = stroke.closed;
    out.at     = at;
    out.len    = len;
    return out;
}

// 1 within `prox.near`, 0 from `prox.far` on, smooth in between.
double nearness(double dist, const Proximity& prox) {
    if (prox.far <= 0 || dist >= prox.far)
        return 0;
    if (dist <= prox.near)
        return 1;
    const double t = (prox.far - dist) / (prox.far - prox.near);
    return t * t * (3 - 2 * t);
}

} // namespace

RunnerPose runnerPose(const Panel& panel, const Runner& runner, double travelled) {
    return place(panel, runner, travelled).pose;
}

std::vector<RunnerPose> runnerPoses(const Panel& panel, double travelled) {
    std::vector<Placed> all;
    all.reserve(panel.runners.size());
    for (const Runner& runner : panel.runners)
        all.push_back(place(panel, runner, travelled));

    std::vector<RunnerPose> poses;
    poses.reserve(all.size());
    for (const Placed& p : all)
        poses.push_back(p.pose);
    if (panel.flare.radius <= 0)
        return poses;

    for (size_t i = 0; i < all.size(); ++i) {
        const Placed& a = all[i];
        if (!a.closed || a.pose.alpha <= 0)
            continue;

        // Each dot that is near adds its light, as far as it is visible itself.
        double dark = 1.0;
        for (size_t j = 0; j < all.size(); ++j) {
            const Placed& b = all[j];
            if (j == i || b.pose.alpha <= 0)
                continue;

            double lit = 0;
            if (!b.closed) {
                lit = nearness(std::hypot(a.pose.pos.x - b.pose.pos.x, a.pose.pos.y - b.pose.pos.y), panel.touch);
            } else if (b.stroke == a.stroke) {
                const double gap = std::fabs(a.at - b.at);
                lit              = nearness(std::min(gap, a.len - gap), panel.merge);
            }
            dark *= 1.0 - lit * b.pose.alpha;
        }
        poses[i].glow = float(std::clamp(1.0 - dark, 0.0, 1.0));
    }
    return poses;
}

namespace {

constexpr double kTwoPi = 2 * std::numbers::pi;

// A column of characters has a few falls. A fall is a bright head that goes down
// a row at a time, never up, with a tail behind it that dims with every row. It
// begins at the top of the column, grows its tail, runs off the bottom, and
// after a pause begins again. Every fall has its own speed, and every pass of it
// a tail of its own: a long one, a short one, and everything between.
constexpr double kFallSpeedMin  = 0.75; // relative to the speed of the column
constexpr double kFallSpeedSpan = 0.6;
constexpr int    kTailMin       = 8; // rows
constexpr int    kTailMax       = 28;
constexpr double kTailOfColumn  = 0.6; // no tail is longer than this part of its column ...
constexpr int    kTailLeast     = 3;   // ... but a tail may always be this long
constexpr double kPauseMin      = 0.2; // of the length of the column ...
constexpr double kPauseSpan     = 0.5;
constexpr int    kPauseRows     = 2; // ... and this many rows more
constexpr double kFall          = 1.3; // how fast a tail dims: 1 is linearly
constexpr double kHeadRows      = 2.5; // the rows this near the head are also drawn in the bright colour
constexpr double kEdgeRows      = 2.5; // a fall fades in and out over this many rows at the ends of the column

// A character changes into another one every few seconds, each at its own pace;
// the rain is meant to stay at the edge of the eye without drawing it, so both
// the changes and the fall are as slow as the lamps are.
constexpr double    kChangeMin   = 2.5; // seconds
constexpr double    kChangeSpan  = 5.0;
constexpr double    kFlickerHz   = 3.0; // how often a head changes
constexpr double    kFlickerHeat = 0.4; // how hot a character must be to do so
constexpr long long kFlickerBase = 1LL << 40;

// Different things to draw from the same seed.
enum Salt : uint32_t { kSaltSpeed = 1, kSaltTail, kSaltPause, kSaltPhase, kSaltGlyph, kSaltChange, kSaltRhythm, kSaltStart, kSaltBeat, kSaltPeriod };

uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint32_t hashOf(uint32_t seed, uint32_t id, uint32_t salt, uint32_t extra = 0) {
    uint32_t h = mix(seed ^ 0x9e3779b9U);
    h          = mix(h ^ id);
    h          = mix(h ^ (salt * 0x85ebca6bU));
    return mix(h ^ extra);
}

// 0 up to but not including 1.
double unit(uint32_t h) {
    return double(h >> 8) / 16777216.0;
}

double frac(double v) {
    return v - std::floor(v);
}

int steps(double v, int n) {
    return std::clamp(int(std::lround(v * n)), 0, n);
}

double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

int glyphAt(uint32_t seed, uint32_t id, long long epoch) {
    return int(hashOf(seed, id, kSaltGlyph, uint32_t(epoch)) % uint32_t(glyphCount()));
}

} // namespace

LampPose lampPose(const Panel& panel, size_t index, double travelled, double clock, unsigned seed) {
    LampPose out;
    if (index >= panel.lamps.size())
        return out;
    const Lamp&  lamp = panel.lamps[index];
    const double span = std::max(lamp.to - lamp.from, 0.0);
    const double step = std::max(lamp.step, 1.0);

    // Every window starts its lamps somewhere else, and keeps the way they are placed against each other.
    const double offset = 2 * frac(lamp.offset + unit(hashOf(seed, 0, kSaltStart)));
    const double trip   = span > 0 ? bounce(offset + travelled * lamp.speed / span) * span : 0.0;
    out.at              = lamp.from + std::min(std::round(trip / step), std::floor(span / step)) * step;

    const double period = std::max(0.5, lamp.period) * (0.9 + 0.2 * unit(hashOf(seed, uint32_t(index), kSaltPeriod)));
    const double beat   = std::max(clock, 0.0) / period + frac(lamp.phase + unit(hashOf(seed, 0, kSaltBeat)));
    const double wave   = 0.5 - 0.5 * std::cos(kTwoPi * frac(beat));
    out.strength        = float(kLampDim + (1 - kLampDim) * steps(wave, kLampStrengthSteps) / double(kLampStrengthSteps));
    out.length          = float(lamp.length * (1 + kLampSwell * (2.0 * steps(wave, kLampLengthSteps) / kLampLengthSteps - 1)));
    return out;
}

std::vector<GlyphDraw> glyphDraws(const Panel& panel, double travelled, double clock, double intensity, unsigned seed) {
    std::vector<GlyphDraw> out;
    const GlyphFace&       face = panel.glyphs;
    intensity                   = std::clamp(intensity, 0.0, 1.0);
    if (!face.on() || face.pitch <= 0 || intensity <= 0)
        return out;
    clock           = std::max(clock, 0.0);
    const double fallen = std::max(travelled, 0.0) * face.fall / face.pitch; // rows

    std::vector<double> alpha, heat;
    uint32_t            base = 0; // the id of the first character of the column
    for (size_t l = 0; l < panel.lanes.size(); ++l) {
        const RainLane& lane = panel.lanes[l];
        if (lane.cells <= 0)
            continue;
        const uint32_t cells = uint32_t(lane.cells);
        alpha.assign(cells, 0.0);
        heat.assign(cells, 0.0);

        for (int k = 0; k < std::max(1, lane.streams); ++k) {
            const uint32_t key   = uint32_t(l) * 16 + uint32_t(k);
            const double   speed = kFallSpeedMin + kFallSpeedSpan * unit(hashOf(seed, key, kSaltSpeed));
            const int      pause = int(std::floor(cells * (kPauseMin + kPauseSpan * unit(hashOf(seed, key, kSaltPause))))) + kPauseRows;

            // A pass is as long as the longest tail allows, so that a short tail only rests for longer.
            const int      tailMax = std::clamp(int(std::floor(cells * kTailOfColumn)), kTailLeast, kTailMax);
            const int      tailMin = std::min(kTailMin, tailMax);
            const double   cycle   = double(cells) + tailMax + pause;
            const double   run     = fallen * speed / cycle + unit(hashOf(seed, key, kSaltPhase));
            const double   pass    = std::floor(run);
            const int      head    = int(std::floor((run - pass) * cycle));
            const int      tail    = tailMin + int(hashOf(seed, key, kSaltTail, uint32_t((long long)pass)) % uint32_t(tailMax - tailMin + 1));

            for (int i = 0; i < tail; ++i) {
                const int row = head - i;
                if (row < 0 || row >= lane.cells)
                    continue;
                const double fade = smooth((row + 0.5) / kEdgeRows) * smooth((lane.cells - row - 0.5) / kEdgeRows);
                alpha[size_t(row)] = std::max(alpha[size_t(row)], std::pow(1.0 - double(i) / tail, kFall) * fade);
                heat[size_t(row)]  = std::max(heat[size_t(row)], smooth(1.0 - i / kHeadRows) * fade);
            }
        }

        for (int row = 0; row < lane.cells; ++row) {
            const int a = steps(alpha[size_t(row)] * intensity, kGlyphAlphaSteps);
            if (a <= 0)
                continue;

            const uint32_t id     = base + uint32_t(row);
            const double   change = kChangeMin + kChangeSpan * unit(hashOf(seed, id, kSaltChange));
            const double   cycle  = clock / change + unit(hashOf(seed, id, kSaltRhythm));
            long long      epoch  = (long long)std::floor(cycle);
            if (heat[size_t(row)] >= kFlickerHeat)
                epoch = kFlickerBase + (long long)std::floor(clock * kFlickerHz);
            out.push_back({int(id), lane.x, int(std::lround(lane.y0 + row * face.pitch)), glyphAt(seed, id, epoch), a, steps(heat[size_t(row)] * intensity, kGlyphHeatSteps)});
        }
        base += cells;
    }
    return out;
}

std::vector<GlyphDraw> glyphChanges(std::span<const GlyphDraw> before, std::span<const GlyphDraw> after) {
    std::vector<GlyphDraw> out;
    size_t                 i = 0, j = 0;
    while (i < before.size() || j < after.size()) {
        if (j == after.size() || (i < before.size() && before[i].id < after[j].id)) {
            out.push_back(before[i++]);
        } else if (i == before.size() || after[j].id < before[i].id) {
            out.push_back(after[j++]);
        } else {
            if (!(before[i] == after[j]))
                out.push_back(after[j]);
            ++i;
            ++j;
        }
    }
    return out;
}

} // namespace smplhud
