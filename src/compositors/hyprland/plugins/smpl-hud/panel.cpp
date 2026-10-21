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
constexpr double kFlareRadius = 1.7;
constexpr double kFlareReach  = 3.0;
constexpr double kFlareHalo   = 2.0;

// It is fully lit within kTouchNear px of a dot on a trace and not at all from
// kTouchFar px on (at scale 1).
constexpr double kTouchNear = 14;
constexpr double kTouchFar  = 70;

// It is lit by the other dot going round from kMergeShare of the outline's
// length away, but from no less than kMergeMin and no more than kMergeMax px
// (at scale 1) and never from more than kMergeCap of it, and fully so within a
// tenth of that.
constexpr double kMergeShare = 0.12;
constexpr double kMergeMin   = 200;
constexpr double kMergeMax   = 500;
constexpr double kMergeCap   = 0.35;

// Frames smaller than this get no notches, no ghost line and no traces.
constexpr double kFancyWidth  = 480;
constexpr double kFancyHeight = 160;

// The pod that holds the scanner at the bottom right corner, in place of the chamfer. The outline bulges
// out there by kPodPoke, with a corner of its own cut kPodCorner; the plate behind the scanner's picture
// reaches kPodMargin beyond it, and its bezel, on the window's side, has its corner cut kPodBezel. A
// window has a pod only if it is kPodRoom times as wide, and as high, as the picture, so that the pod never
// takes much of it.
constexpr double kPodPoke   = 8;
constexpr double kPodMargin = 3;
constexpr double kPodCorner = 14;
constexpr double kPodBezel  = 9;
constexpr double kPodRoom   = 5;
constexpr double kPodTrace  = 4; // how far above the pod a trace on the right side ends

// The characters of a glyph style, in terms of their height.
constexpr double kGlyphStroke = 0.05; // half the thickness of a stroke
constexpr double kGlyphMin    = 6;    // px; no character is smaller
constexpr double kGlyphHalo   = 1.25; // their glow is this much brighter than the line's

// The columns of characters beyond the first, in terms of the first: each looks
// further away than the one before it, being smaller, dimmer and more out of focus,
// and it is slower in px, though its characters fall as many rows a second. Every
// column but the last is a little see-through, so that the one behind it shows
// through where the two overlap; the last is only a faint haze.
constexpr double kNearDim = 0.8;

struct Further {
    double scale; // of the first column's characters
    double dim;   // how bright they are at most
    double soft;  // px of softness of a stroke's edge, at scale 1
};

constexpr Further kFurther[kGlyphColumns - 1] = {{0.5, 0.55, 0.6}, {0.34, 0.38, 0.9}};

// The columns the characters fall in: the first is on the line itself, its
// characters centred on it, as if they slid along it. Each of the others lies
// outside the line, partly under the one before it: only kColumnPeek of the width
// of one of its characters is beyond that column's outer edge. A column starts and
// ends a pitch short of the ends of the straight run, clear of the corners. One
// with fewer than kLaneMinCells characters is not worth having, and neither is a
// column where there is none before it. A column has one fall for every kLaneCells
// characters, but no more than kLaneMaxStreams of its own: the second column's
// smaller characters get twice as many as the first's, and the third's, a faint
// haze, only one more, since every character is a draw that the compositor has to
// make.
constexpr double kColumnPeek                    = 0.4;
constexpr double kLaneInset                     = 1.0; // in pitches
constexpr int    kLaneMinCells                  = 3;
constexpr double kLaneCells                     = 20;
constexpr int    kLaneMaxStreams[kGlyphColumns] = {3, 6, 4};

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

// How far the underglow takes to die away towards the corners of the window, as a multiple of its reach.
constexpr double kUnderglowTaper = 1.5;

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

// How far a pod pokes out beyond the frame, at this scale.
double podPoke(const Frame& f) {
    return std::max(2.0, std::round(kPodPoke * f.s));
}

// The pod at the bottom right corner: the plate that holds the scanner's picture, whose right and bottom
// edge are the outline's, which goes round it.
struct PodShape {
    bool   on  = false;
    double p   = 0;           // how far it pokes out beyond the frame
    double k   = 0;           // leg of the cut at its outer corner
    double k2  = 0;           // and of the one at the top left corner of the bezel
    double m   = 0;           // margin of the plate round the picture
    double x1  = 0, y1 = 0;   // the plate's top left corner
    double xr  = 0, yb = 0;   // and its bottom right one
    IBox   picture;
};

// Off, if there is no scanner, if the window is too small for it, or if the pod would not fit the frame.
PodShape measurePod(const Frame& f, const Params& p) {
    PodShape pod;
    if (p.scannerW <= 0 || p.scannerH <= 0 || p.width < kPodRoom * p.scannerW || p.height < kPodRoom * p.scannerH)
        return pod;

    pod.p = podPoke(f);
    pod.m = std::max(std::round(kPodMargin * f.s), std::ceil(f.hw) + 2.0);
    const double w = p.scannerW + 2 * pod.m, h = p.scannerH + 2 * pod.m;
    pod.k          = std::min(std::round(kPodCorner * f.s), std::min(w, h) - pod.p - 2.0);
    pod.k2         = std::min(std::round(kPodBezel * f.s), std::floor(std::min(w, h) / 2.0) - 1.0);
    if (pod.k < 2 || pod.k2 < 1)
        return pod;

    pod.xr = f.right + pod.p;
    pod.yb = f.bottom + pod.p;
    pod.x1 = pod.xr - w;
    pod.y1 = pod.yb - h;
    if (pod.x1 < f.left + f.a + 10 * f.s || pod.y1 < f.top + f.a + 10 * f.s)
        return pod;

    pod.picture = {int(std::lround(pod.x1 + pod.m)), int(std::lround(pod.y1 + pod.m)), p.scannerW, p.scannerH};
    pod.on      = true;
    return pod;
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

// False when the features would collide. `rightEnd` is where the right-hand baseline has to end: where the
// corner at the bottom right starts.
bool placeCuts(const Frame& f, double rightEnd, Cuts& k) {
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
        && k.baseX2 + room < rightEnd;                                     // right-hand baseline
}

// The outline, and which of its vertices the top and the bottom edge, which the lamps follow, begin and
// end at.
struct Outline {
    std::vector<Vertex> pts;
    size_t              topFrom = 0, topTo = 0, bottomFrom = 0, bottomTo = 0;
};

// The bottom right corner of the outline, clockwise: cut off, or, round a pod, which steps out from the
// right side, runs down along the plate's right edge, round its corner and back along its bottom, and
// steps in again to the bottom edge. Ends where the bottom edge begins.
void bottomRight(const Frame& f, const PodShape& pod, std::vector<Vertex>& pts) {
    if (!pod.on) {
        pts.push_back({f.right, f.bottom - f.a, 1.f});
        pts.push_back({f.right - f.a, f.bottom, 1.f});
        return;
    }
    pts.push_back({f.right, pod.y1, 1.f});
    pts.push_back({pod.xr, pod.y1 + pod.p, 1.f});
    pts.push_back({pod.xr, pod.yb - pod.k, 1.f});
    pts.push_back({pod.xr - pod.k, pod.yb, 1.f});
    pts.push_back({pod.x1 + pod.p, pod.yb, 1.f});
    pts.push_back({pod.x1, f.bottom, 1.f});
}

Outline plainOutline(const Frame& f, const PodShape& pod) {
    const double a = f.a;
    Outline      o;
    o.pts   = {{f.left + a, f.top, 1.f}, {f.right - a, f.top, 1.f}, {f.right, f.top + a, 1.f}};
    o.topTo = 1;
    bottomRight(f, pod, o.pts);
    o.bottomFrom = o.pts.size() - 1;
    o.pts.push_back({f.left + a, f.bottom, 1.f});
    o.bottomTo = o.pts.size() - 1;
    o.pts.push_back({f.left, f.bottom - a, 1.f});
    o.pts.push_back({f.left, f.top + a, 1.f});
    return o;
}

// Clockwise from the top edge. The bottom edge doubles back across an invisible
// segment: the ghost line ends, the left-hand baseline begins.
Outline fancyOutline(const Frame& f, const Cuts& k, const PodShape& pod) {
    const double a = f.a, d = f.d, b = f.b;
    Outline      o;
    o.pts = {
        {f.left + a, f.top, 1.f},
        {k.notchX, f.top, 1.f},
        {k.notchX + d, f.top + d, 1.f},
        {k.notchX + d + k.notchFloor, f.top + d, 1.f},
        {k.notchX + 2 * d + k.notchFloor, f.top, 1.f},
        {k.stepX, f.top, 1.f},
        {k.stepX + d, f.top + d, 1.f},
        {f.right - a + d, f.top + d, 1.f},
        {f.right, f.top + a, 1.f},
    };
    o.topTo = o.pts.size() - 2;
    bottomRight(f, pod, o.pts);
    o.bottomFrom = o.pts.size() - 1;
    o.pts.insert(o.pts.end(), {
        {k.baseX2, f.bottom, 1.f},
        {k.baseX2 - d, f.bottom - d, kRampAlpha},
        {k.ghostX, f.bottom - d, 0.f},
        {k.endX1, f.bottom, 0.f},
        {k.endX1 - k.fade, f.bottom, 1.f},
        {k.baseX1, f.bottom, 1.f},
        {k.baseX1 - d, f.bottom - d, 1.f},
        {f.left + b, f.bottom - d, 1.f},
    });
    o.bottomTo = o.pts.size() - 1;
    o.pts.push_back({f.left, f.bottom - d - b, 1.f});
    o.pts.push_back({f.left, f.top + a, 1.f});
    return o;
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
// beyond the frame, parallel to the corner cut, to where the corner at the bottom right starts, or the
// pod, which it ends above.
bool stepTrace(const Frame& f, const Cuts& k, const PodShape& pod, Trace& t) {
    const double s = f.s, d = f.d;
    const double bx = k.stepX + d + std::round(0.36 * d), by = f.top + 0.1 * d;
    const double xa = bx + 3.5 * s;
    const double yh = f.snap(f.top + 0.33 * d);
    const double xv = f.snap(f.right + f.tau);
    const double yv = f.snap(f.top + f.a - (kSqrt2 - 1) * f.tau);
    const double xh = xv - (yv - yh);
    const double ye = pod.on ? f.snap(pod.y1 - kPodTrace * s) : f.bottom - f.a;
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

// The characters' sizes: those of the first column, and those of each one beyond it,
// which is smaller, dimmer, softer and slower in px, but has as many rows a second go by.
void placeFace(const Style& style, const Frame& f, double halo, Panel& panel) {
    GlyphFace& face = panel.glyphs;
    face.height     = std::max(kGlyphMin, std::round(style.glyphHeight * f.s));
    face.halfWidth  = kGlyphStroke * face.height;
    face.reach      = style.glyphGlow * face.height;
    face.haloAlpha  = float(std::min(1.0, halo * kGlyphHalo));
    face.pitch      = std::max(face.height * 0.5, std::round(style.glyphPitch * f.s));
    face.fall       = style.glyphFall;
    face.knock      = style.glyphKnock;
    face.dim        = float(kNearDim);
    face.soft       = 0;

    GlyphFace* const further[kGlyphColumns - 1] = {&panel.farGlyphs, &panel.deepGlyphs};
    for (int c = 1; c < kGlyphColumns; ++c) {
        GlyphFace& out = *further[c - 1];
        out            = {};
        if (style.glyphColumns <= c)
            continue;

        const Further& look = kFurther[c - 1];
        out                 = face;
        out.height *= look.scale;
        out.halfWidth *= look.scale;
        out.reach *= look.scale;
        out.pitch *= look.scale;
        out.fall *= look.scale;
        out.dim  = float(look.dim);
        out.soft = look.soft * f.s;
    }
}

double halfWidthOf(const GlyphFace& face) {
    return face.height * kGlyphW / kGlyphH / 2 + face.halfWidth;
}

// Where the columns of characters on one straight side of the outline are.
// The line is at x = `line` and `away` (1 or -1) points from the window outwards:
// the first column is centred on the line, each of the others lies beyond it,
// partly under the one before. They depend on the style and the scale, never on
// how big the window is.
struct Columns {
    int n                = 0;
    int x[kGlyphColumns] = {}; // the middle of the characters of each
};

Columns columnsOf(const Panel& panel, double line, double away) {
    Columns out;
    double  edge = line; // how far the columns reach outwards so far
    for (int c = 0; c < kGlyphColumns; ++c) {
        const GlyphFace& face = panel.face(c);
        if (!face.on())
            break;
        const double half = halfWidthOf(face);
        double       x    = std::round(line); // on the line, to the nearest whole px
        if (out.n > 0) {
            const double at = edge + away * (2 * kColumnPeek - 1) * half;
            x               = away > 0 ? std::ceil(at) : std::floor(at); // whole px, never nearer than asked
        }
        out.x[out.n++] = int(x);
        edge           = x + away * half;
    }
    return out;
}

// The columns of one straight side of the outline, between `top` and `bottom`.
void placeLanes(const Panel& panel, double line, double away, double top, double bottom, std::vector<RainLane>& out) {
    const Columns cols = columnsOf(panel, line, away);
    for (int c = 0; c < cols.n; ++c) {
        const GlyphFace& face  = panel.face(c);
        const double     inset = kLaneInset * face.pitch;
        const int        y0    = int(std::lround(top + inset));
        const int        cells = int(std::floor((bottom - inset - y0) / face.pitch + 1e-9)) + 1;
        if (cells < kLaneMinCells)
            break; // and no column beyond one that is not there

        out.push_back({cols.x[c], y0, cells, std::clamp(int(std::lround(cells / kLaneCells)), 1, kLaneMaxStreams[c]), c});
    }
}

// The characters' sizes and the columns they fall in: down both sides, between
// the corners of the outline.
void placeRain(const Style& style, const Frame& f, const PodShape& pod, double halo, Panel& panel) {
    if (style.glyphHeight <= 0)
        return;

    placeFace(style, f, halo, panel);
    const double leftEnd  = f.fancy ? f.bottom - f.d - f.b : f.bottom - f.a;
    const double rightEnd = pod.on ? pod.y1 : f.bottom - f.a;
    placeLanes(panel, f.left, -1.0, f.top + f.a, leftEnd, panel.lanes);
    placeLanes(panel, f.right, 1.0, f.top + f.a, rightEnd, panel.lanes);
}

// A vertex of the outline that stands on the left or the right side, or on the pod's.
bool onSide(const Vertex& v, const Frame& f) {
    return v.x >= f.right || v.x <= f.left;
}

// The outline with its sides masked out: what runs along the top and the bottom edge,
// for the lamps to follow, so that none of them goes where the characters fall. The
// corners between a side and an edge fade the glow out.
std::vector<Vertex> edgeTrack(std::vector<Vertex> pts, const Frame& f) {
    for (Vertex& v : pts) {
        if (onSide(v, f))
            v.alpha = 0.f;
    }
    return pts;
}

// The outline as visible as the theme says, the glow beside the line with it: `side`
// down its left and right sides, `edge` along the top and the bottom. The corners
// between a side and an edge fade from one to the other.
std::vector<Vertex> dimOutline(std::vector<Vertex> pts, const Frame& f, double side, double edge) {
    for (Vertex& v : pts)
        v.alpha *= float(std::clamp(onSide(v, f) ? side : edge, 0.0, 1.0));
    return pts;
}

// A glow with no line to it along the window's own edge `y`, which is not where the outline is: it
// stands off from the window, and the light is to come from under the window. It is strongest
// between the corners and dies away over `taper` towards them, where the characters fall.
std::vector<Vertex> underglowRun(double width, double y, double taper) {
    const double half = width / 2.0;
    taper             = std::max(std::round(taper), 1.0); // whole px, so that no run of it is shorter than one
    if (taper >= half)
        return {{0, y, 0.f}, {half, y, 1.f}, {width, y, 0.f}};
    return {{0, y, 0.f}, {taper, y, 1.f}, {width - taper, y, 1.f}, {width, y, 0.f}};
}

// The lamps of the top and the bottom edge, going to and fro between the first
// and the last vertex of the stretch of the track that makes up the edge.
void placeLamps(const Style& style, const Params& p, const Frame& f, const Outline& outline, Panel& panel) {
    if (style.lamps <= 0)
        return;

    panel.track = edgeTrack(outline.pts, f);
    std::vector<double> at(panel.track.size(), 0.0); // px along the track at each vertex
    for (size_t i = 1; i < at.size(); ++i)
        at[i] = at[i - 1] + std::hypot(panel.track[i].x - panel.track[i - 1].x, panel.track[i].y - panel.track[i - 1].y);

    struct Edge {
        size_t          first, last;
        const LampLook* looks;
    };
    const Edge edges[2] = {{outline.topFrom, outline.topTo, kLampsTop}, {outline.bottomFrom, outline.bottomTo, kLampsBottom}};

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
    Frame    f = measure(style, p);
    Cuts     cuts;
    PodShape pod = measurePod(f, p);

    // The notches and ramps need room beside the pod; a pod that would leave none of it gives way to them.
    const auto fancyWith = [&](const PodShape& with) {
        return f.d > 0 && f.fw() >= kFancyWidth * f.s && f.fh() >= kFancyHeight * f.s && f.b >= f.a && f.a >= f.d + 6 * f.s &&
            f.fh() - f.a - f.d - f.b >= 8 * f.s && placeCuts(f, with.on ? with.x1 : f.right - f.a, cuts);
    };
    f.fancy = fancyWith(pod);
    if (!f.fancy && pod.on && fancyWith(PodShape{})) {
        pod     = {};
        f.fancy = true;
    }

    out.pod = {};
    if (pod.on) {
        out.pod.on        = true;
        out.pod.picture   = pod.picture;
        out.pod.halfWidth = f.hw;
        out.pod.plate     = {{pod.x1, f.bottom},
                             {pod.x1, pod.y1 + pod.k2},
                             {pod.x1 + pod.k2, pod.y1},
                             {f.right, pod.y1},
                             {pod.xr, pod.y1 + pod.p},
                             {pod.xr, pod.yb - pod.k},
                             {pod.xr - pod.k, pod.yb},
                             {pod.x1 + pod.p, pod.yb}};
        out.pod.bezel     = 3;
    }

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
    Outline outline = f.fancy ? fancyOutline(f, cuts, pod) : plainOutline(f, pod);
    panel.strokes.push_back(stroke(dimOutline(outline.pts, f, p.sideOpacity, p.edgeOpacity), true, style.lineAlpha, reach, halo));

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
        add(stepTrace(f, cuts, pod, t));
        t = {};
        add(cornerTrace(f, cuts, t));
    }

    // Last, so that the runners, which name their strokes by place, find the ones they run on: a glow
    // with no line, as far out as the outline's own, along the window's top and bottom edge, whatever
    // the outline itself is like. One too faint to see has no stroke at all.
    const double underglow = std::clamp(p.underglow * boost, 0.0, 1.0);
    if (underglow >= kInvisible && reach > 0) {
        for (const double y : {f.snap(0), f.snap(p.height)})
            panel.strokes.push_back(stroke(underglowRun(p.width, y, kUnderglowTaper * reach), false, 0.0, reach, underglow));
    }

    // Room for the outline, the traces, the beads and the characters, whatever the window size.
    placeRain(style, f, pod, halo, panel);
    placeLamps(style, p, f, outline, panel);

    const double lineRoom = roomAround(f.hw, reach);
    const double dotRoom  = beads ? std::max(beadSize(lookOf(dot)), beadSize(lookOf(panel.flare))) / 2.0 : 0.0;
    const double room     = std::max(lineRoom, dotRoom) + 1;
    const auto   logical  = [&](double phys) { return int(std::ceil(phys / f.s)) + 2; };

    // The characters of a column stick out beyond the window box, and so does their glow, whether or not
    // this window is tall enough for the column.
    double columns = 0;
    if (panel.glyphs.on()) {
        const Columns cols = columnsOf(panel, f.left, -1.0);
        for (int c = 0; c < cols.n; ++c)
            columns = std::max(columns, glyphSize(panel.face(c)).w / 2.0 + 1 - cols.x[c]); // 0 is the window's edge
    }

    // The pod sticks out beyond the frame at the right and at the bottom, whether or not this window is big
    // enough for one.
    const double poke  = p.scannerW > 0 ? podPoke(f) : 0.0;
    const int    side  = logical(std::ceil(f.c) + room);
    const int    right = std::max({logical(std::ceil(f.c + f.tau) + room), logical(std::ceil(f.c + poke) + room), logical(columns)});
    out.extents        = {std::max(side, logical(columns)), side, right, logical(std::ceil(f.c + poke) + room)};

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

// A column of characters has a few falls. A fall is a head, one bright character
// that glides down the column, never up, and a tail: the characters the head
// leaves in the cells it passes, which come up as it gets clear of them and dim
// with every row it gets further. It begins at the top of the column, runs off
// the bottom, and after a pause begins again. Every fall has its own speed, and
// every pass of it a tail of its own: a long one, a short one, and everything
// between.
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
constexpr double kEdgeRows      = 2.5; // a fall fades in and out over this many rows at the ends of the column

// A head keeps its character for kBandRows rows, or one more, then turns into
// another over kBandBlend rows. How far into the first of them it begins differs
// from pass to pass.
constexpr int    kBandRows  = 4;
constexpr double kBandBlend = 0.7;

// Heads that are closer than kYieldFar rows to each other get in each other's
// way: of two, the later one in the column's list of falls is drawn in full,
// the other fades out the closer they get, and is gone within kYieldNear rows.
constexpr double kYieldNear = 0.3;
constexpr double kYieldFar  = 1.0;

// The character left in a cell comes up between kBirthStart and kBirthStart +
// kBirthSpan rows behind the head, which is as soon as the head's own character
// is clear of the cell. It is overdrawn in the bright colour for the first
// kHeatRows rows after kHeatStart, and is never as bright as a head.
constexpr double kBirthStart = 0.8;
constexpr double kBirthSpan  = 0.8;
constexpr double kHeatStart  = 1.0;
constexpr double kHeatRows   = 1.5;
constexpr double kTrailPeak  = 0.85;
constexpr double kTrailHeat  = 0.9;

// No two characters are quite as bright: a head is at least kHeadBrightMin of
// the full brightness, a character left behind kTrailBrightMin of its own.
constexpr double kHeadBrightMin  = 0.85;
constexpr double kTrailBrightMin = 0.7;

// The sizes of the characters, as shares of the face's own; the smallest is
// kSmallShare of them, the middle one kMediumShare and the largest the rest.
constexpr double kGlyphScale[kGlyphSizes] = {0.84, 0.92, 1.0};
constexpr double kSmallShare              = 0.3;
constexpr double kMediumShare             = 0.4;

// Different things to draw from the same seed.
enum Salt : uint32_t {
    kSaltSpeed = 1,
    kSaltTail,
    kSaltPause,
    kSaltPhase,
    kSaltGlyph,
    kSaltStart = 8,
    kSaltBeat,
    kSaltPeriod,
    kSaltBand,
    kSaltBright,
    kSaltTrail,
    kSaltTrailBright,
    kSaltSize,
};

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

// The stretches of a pass in which a head keeps one character.
struct Bands {
    int    rows   = kBandRows;
    double offset = 0; // rows into the first one at the top of the column
};

// A fall at one moment.
struct Fall {
    uint32_t  key   = 0;
    double    speed = 1; // relative to the column's
    double    cycle = 1; // rows from the head entering the column to the next head doing so
    double    head  = 0; // rows from the first cell: where the head's character is
    long long pass  = 0;
    int       tail  = 0; // rows, in this pass
    Bands     bands;
};

Bands bandsOf(uint32_t seed, uint32_t key, long long pass) {
    const uint32_t h    = hashOf(seed, key, kSaltBand, uint32_t(pass));
    const int      rows = kBandRows + int(h & 1);
    return {rows, unit(mix(h)) * rows};
}

Fall fallAt(uint32_t seed, uint32_t key, int cells, double fallen) {
    Fall f;
    f.key   = key;
    f.speed = kFallSpeedMin + kFallSpeedSpan * unit(hashOf(seed, key, kSaltSpeed));

    const int pause = int(std::floor(cells * (kPauseMin + kPauseSpan * unit(hashOf(seed, key, kSaltPause))))) + kPauseRows;

    // A pass is as long as the longest tail allows, so that a short tail only rests for longer.
    const int tailMax = std::clamp(int(std::floor(cells * kTailOfColumn)), kTailLeast, kTailMax);
    const int tailMin = std::min(kTailMin, tailMax);
    f.cycle           = double(cells) + tailMax + pause;

    const double run  = fallen * f.speed / f.cycle + unit(hashOf(seed, key, kSaltPhase));
    const double pass = std::floor(run);
    f.pass            = (long long)pass;
    f.head            = (run - pass) * f.cycle;
    f.tail            = tailMin + int(hashOf(seed, key, kSaltTail, uint32_t(f.pass)) % uint32_t(tailMax - tailMin + 1));
    f.bands           = bandsOf(seed, key, f.pass);
    return f;
}

// The band of the pass that a head `row` rows down is in, and how many rows into it.
int bandAt(const Bands& bands, double row, double& into) {
    const double at   = (row + bands.offset) / bands.rows;
    const double band = std::floor(at);
    into              = (at - band) * bands.rows;
    return int(band);
}

// What a head looks like in a band of a pass.
uint32_t bandSalt(long long pass, int band) {
    return mix(uint32_t(pass)) + uint32_t(band);
}

int shapeOf(uint32_t seed, uint32_t key, long long pass, int band) {
    return int(hashOf(seed, key, kSaltGlyph, bandSalt(pass, band)) % uint32_t(glyphCount()));
}

int sizeOfRoll(uint32_t h) {
    const double v = unit(h);
    return v < kSmallShare ? 0 : v < kSmallShare + kMediumShare ? 1 : 2;
}

// 1 in the middle of a column, falling to 0 at the first and the last cell
// and beyond.
double edgeFade(double row, int cells) {
    return smooth((row + 0.5) / kEdgeRows) * smooth((cells - row - 0.5) / kEdgeRows);
}

} // namespace

GlyphFace glyphFaceOf(const GlyphFace& face, int size) {
    const double scale = kGlyphScale[std::clamp(size, 0, kGlyphSizes - 1)];
    GlyphFace    out   = face;
    out.height *= scale;
    out.halfWidth *= scale;
    out.reach *= scale;
    return out;
}

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

std::vector<GlyphDraw> glyphDraws(const Panel& panel, double travelled, double intensity, unsigned seed) {
    std::vector<GlyphDraw> out, heads;
    intensity = std::clamp(intensity, 0.0, 1.0);
    if (!panel.glyphs.on() || panel.glyphs.pitch <= 0 || intensity <= 0)
        return out;

    std::vector<Fall> falls;
    uint32_t          base = 0; // the id of the first cell of the column
    uint32_t          next = 0; // the number of the next fall, which its head's ids come from
    for (size_t l = 0; l < panel.lanes.size(); ++l) {
        const RainLane&  lane = panel.lanes[l];
        const GlyphFace& face = panel.face(lane.column);
        if (lane.cells <= 0 || !face.on() || face.pitch <= 0)
            continue;
        const double fallen = std::max(travelled, 0.0) * face.fall / face.pitch; // rows
        const auto   y      = [&](double row) { return int(std::lround(lane.y0 + row * face.pitch)); };

        falls.clear();
        for (int k = 0; k < std::max(1, lane.streams); ++k)
            falls.push_back(fallAt(seed, uint32_t(l) * 16 + uint32_t(k), lane.cells, fallen));

        for (int row = 0; row < lane.cells; ++row) {
            // A cell has what the head that has passed it last left in it. One that has not
            // got to it yet is in its pause, and what it left has gone out long since.
            const Fall* owner = nullptr;
            double      ago = 0, age = 0; // rows the head has gone beyond the cell, and the time that took
            for (const Fall& f : falls) {
                const double rows = f.head >= row ? f.head - row : f.head - row + f.cycle;
                if (!owner || rows / f.speed < age) {
                    owner = &f;
                    ago   = rows;
                    age   = rows / f.speed;
                }
            }
            if (ago >= owner->tail)
                continue;
            const double rise = smooth((ago - kBirthStart) / kBirthSpan);
            if (rise <= 0)
                continue;

            const uint32_t id    = base + uint32_t(row);
            const uint32_t visit = mix(uint32_t(owner->pass)) + owner->key;
            double         look  = kTrailPeak * rise * std::pow(1.0 - ago / owner->tail, kFall) * edgeFade(row, lane.cells) * face.dim;
            look *= kTrailBrightMin + (1 - kTrailBrightMin) * unit(hashOf(seed, id, kSaltTrailBright, visit));
            for (const Fall& f : falls) {
                // What a head is about to take the place of fades out as it gets there.
                const double ahead = row - f.head;
                if (&f != owner && ahead > 0 && ahead < 1)
                    look *= smooth(ahead);
            }
            const int a = steps(look * intensity, kGlyphAlphaSteps);
            if (a <= 0)
                continue;

            // Never the character of the head that left it.
            double    into  = 0;
            int       glyph = int(hashOf(seed, id, kSaltTrail, visit) % uint32_t(glyphCount()));
            if (glyph == shapeOf(seed, owner->key, owner->pass, bandAt(owner->bands, row, into)))
                glyph = (glyph + 1) % glyphCount();

            const double heat = kTrailHeat * rise * smooth(1.0 - (ago - kHeatStart) / kHeatRows);
            out.push_back({int(id), lane.x, y(row), glyph, a, steps(heat * intensity, kGlyphHeatSteps), sizeOfRoll(hashOf(seed, id, kSaltSize, visit)), lane.column});
        }
        base += uint32_t(lane.cells);

        for (size_t i = 0; i < falls.size(); ++i, ++next) {
            const Fall& f    = falls[i];
            double      look = edgeFade(f.head, lane.cells) * face.dim;
            for (size_t j = i + 1; j < falls.size(); ++j)
                look *= smooth((std::fabs(f.head - falls[j].head) - kYieldNear) / (kYieldFar - kYieldNear));
            look *= kHeadBrightMin + (1 - kHeadBrightMin) * unit(hashOf(seed, f.key, kSaltBright, uint32_t(f.pass)));
            if (look <= 0)
                continue;

            // For a moment, as it turns into the character of the next band, it is both.
            double       into   = 0;
            const int    band   = bandAt(f.bands, f.head, into);
            const double turned = band > 0 ? smooth(into / kBandBlend) : 1.0;
            const auto   put    = [&](uint32_t which, int shape, double share) {
                const int a = steps(look * share * intensity, kGlyphAlphaSteps);
                if (a > 0)
                    heads.push_back({kGlyphHeadId + int(2 * next + which), lane.x, y(f.head), shapeOf(seed, f.key, f.pass, shape), a, steps(intensity, kGlyphHeatSteps),
                                     sizeOfRoll(hashOf(seed, f.key, kSaltSize, bandSalt(f.pass, shape))), lane.column});
            };
            if (turned < 1)
                put(0, band - 1, 1 - turned);
            put(1, band, turned);
        }
    }

    out.insert(out.end(), heads.begin(), heads.end());
    return out;
}

std::vector<size_t> glyphOrder(std::span<const GlyphDraw> draws) {
    std::vector<size_t> out;
    out.reserve(draws.size());
    for (int column = kGlyphColumns - 1; column >= 0; --column)
        for (size_t i = 0; i < draws.size(); ++i)
            if (std::clamp(draws[i].column, 0, kGlyphColumns - 1) == column)
                out.push_back(i);
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
            if (!(before[i] == after[j])) {
                if (before[i].x != after[j].x || before[i].y != after[j].y)
                    out.push_back(before[i]);
                out.push_back(after[j]);
            }
            ++i;
            ++j;
        }
    }
    return out;
}

} // namespace smplhud
