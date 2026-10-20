#include "panel.hpp"

#include "glyph.hpp"
#include "stamp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
constexpr double kGlyphReach  = 0.28; // how far the glow reaches past a stroke's edge
constexpr double kGlyphMin    = 6;    // px; no character is smaller

// Where a character can light up along a line: not where the line is all but
// invisible, not so near the end of a trace that it crowds the character on the
// end, and not where it would touch the window. Where the line dips towards the
// window (the notches, the raised baselines) the character stands a little
// outside it instead, as far as kGlyphNudge logical px.
constexpr float  kSlotMinAlpha = 0.3f;
constexpr double kSlotInset    = 1.1; // in pitches
constexpr double kGlyphClear   = 2.0; // px between a character and the window
constexpr double kGlyphNudge   = 6.0;

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

double segmentDistance(Vec2 q, Vec2 a, Vec2 b) {
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    const double t = len2 > 0 ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
    return std::hypot(q.x - (a.x + dx * t), q.y - (a.y + dy * t));
}

// How far a point is from the window, its corners cut at 45 degrees the way the
// outline is measured; 0 inside it.
double windowDistance(const Params& p, const Frame& f, Vec2 q) {
    const double W = p.width, H = p.height, c = f.cutEff;
    if (q.x >= 0 && q.y >= 0 && q.x <= W && q.y <= H && q.x + q.y >= c && (W - q.x) + q.y >= c && (W - q.x) + (H - q.y) >= c && q.x + (H - q.y) >= c)
        return 0;

    const Vec2 shape[8] = {{c, 0}, {W - c, 0}, {W, c}, {W, H - c}, {W - c, H}, {c, H}, {0, H - c}, {0, c}};
    double     best     = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 8; ++i)
        best = std::min(best, segmentDistance(q, shape[i], shape[(i + 1) % 8]));
    return best;
}

// How far out, in whole px, a character may step off the outline to clear the window.
int glyphNudge(const Frame& f) {
    return int(std::ceil(kGlyphNudge * f.s));
}

// Which way is outward, away from the window, at `s` px along a closed stroke:
// to the left of the way it runs, since it runs clockwise on the screen.
Vec2 outwardAt(const Stroke& stroke, double s) {
    const Vec2   a = strokePointAt(stroke, s - 1).pos, b = strokePointAt(stroke, s + 1).pos;
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    return len > 0 ? Vec2{(b.y - a.y) / len, -(b.x - a.x) / len} : Vec2{};
}

// The characters' size, and where along the lines they can light up: evenly
// spaced round a closed stroke, evenly spaced between the two ends of an open
// one, in whole px, and only where the line can be seen and the character
// clears the window (stepping outward off the outline where it must).
void placeGlyphs(const Style& style, const Params& p, const Frame& f, double halo, Panel& panel) {
    if (style.glyphHeight <= 0)
        return;

    GlyphFace& face = panel.glyphs;
    face.height     = std::max(kGlyphMin, std::round(style.glyphHeight * f.s));
    face.halfWidth  = kGlyphStroke * face.height;
    face.reach      = kGlyphReach * face.height;
    face.haloAlpha  = float(halo);
    face.pitch      = std::max(face.height * 0.5, std::round(style.glyphPitch * f.s));
    face.period     = style.glyphPeriod;
    face.knock      = style.glyphKnock;

    // The farthest a character's strokes reach from its middle, plus the room it keeps.
    const double clear = std::hypot(face.height * kGlyphW / kGlyphH / 2 + face.halfWidth, face.height / 2 + face.halfWidth) + kGlyphClear;

    for (size_t k = 0; k < panel.strokes.size(); ++k) {
        const Stroke& stroke = panel.strokes[k];
        const double  len    = strokeLength(stroke);
        const double  inset  = stroke.closed ? 0.0 : kSlotInset * face.pitch;
        const double  span   = len - 2 * inset;
        if (span < 0)
            continue;

        const int n = stroke.closed ? std::max(1, int(std::lround(len / face.pitch))) : int(std::floor(span / face.pitch + 1e-9)) + 1;
        for (int i = 0; i < n; ++i) {
            const double      at    = stroke.closed ? (i + 0.5) * len / n : (n == 1 ? len / 2 : inset + i * span / (n - 1));
            const StrokePoint point = strokePointAt(stroke, at);
            const float       alpha = point.alpha * stroke.alpha;
            if (alpha < kSlotMinAlpha)
                continue;

            const Vec2 way = stroke.closed ? outwardAt(stroke, at) : Vec2{};
            for (int nudge = 0; nudge <= glyphNudge(f); ++nudge) {
                const Vec2 pos = {std::round(point.pos.x + way.x * nudge), std::round(point.pos.y + way.y * nudge)};
                if (windowDistance(p, f, pos) >= clear) {
                    panel.slots.push_back({pos, int(k), at, alpha});
                    break;
                }
            }
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
    panel.strokes.push_back(stroke(f.fancy ? fancyOutline(f, cuts) : plainOutline(f), true, style.lineAlpha, reach, halo));

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
    placeGlyphs(style, p, f, halo, panel);
    const double    lineRoom  = roomAround(f.hw, reach);
    const double    dotRoom   = beads ? std::max(beadSize(lookOf(dot)), beadSize(lookOf(panel.flare))) / 2.0 : 0.0;
    const GlyphSize glyph     = glyphSize(panel.glyphs);
    const double    glyphRoom = panel.glyphs.on() ? std::max(glyph.w, glyph.h) / 2.0 + glyphNudge(f) : 0.0;
    const double    room      = std::max({lineRoom, dotRoom, glyphRoom}) + 1;
    const auto   logical   = [&](double phys) { return int(std::ceil(phys / f.s)) + 2; };
    const int    side      = logical(std::ceil(f.c) + room);
    out.extents            = {side, side, logical(std::ceil(f.c + f.tau) + room), side};

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

    out.pose   = {p.pos, std::clamp(p.alpha * runner.look.alpha, 0.f, 1.f), 0.f, runner.stroke, at, stroke.closed ? (runner.speed < 0 ? -1 : 1) : 0};
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

// A dot leaves a trail of characters along its line, in pitches (the distance
// between two characters).
constexpr double kLead  = 1.5; // in front of a dot going round
constexpr double kTail  = 8.0; // behind it
constexpr double kSides = 3.0; // either side of one running back and forth
constexpr double kHead  = 1.0; // the characters this near are also drawn in the bright colour
constexpr double kFall  = 1.3; // how fast a trail fades: 1 is linearly
constexpr double kSwell = 0.6; // how much longer a trail gets, and how much bigger its head, where its dot glows

// A character at the end of a trace.
constexpr double kRest       = 0.55; // how bright it is without focus
constexpr double kDimmest    = 0.12; // how bright at the start of a breath, when it changes into another one
constexpr double kBreathHeat = 0.5;  // the share of the bright colour at the top of a breath
constexpr double kHitNear    = 0.5;  // in pitches: a dot this near lights it fully ...
constexpr double kHitFar     = 2.2;  // ... and one this far not at all

// A character along a line changes into another one every few seconds, each
// at its own pace. One in kAmbientOneIn also lights up and fades again between
// two changes without a dot going by.
constexpr double   kChangeMin    = 4.0; // seconds
constexpr double   kChangeSpan   = 4.0;
constexpr unsigned kAmbientOneIn = 8;
constexpr double   kAmbientPeak  = 0.55;
constexpr double   kAmbientLit   = 0.65; // the share of the time between two changes that it is lit
constexpr double   kFlickerHz    = 7.0;  // how often the head of a trail changes
constexpr double   kFlickerHeat  = 0.4;  // how hot a character must be to do so
constexpr long long kFlickerBase = 1LL << 40;

// Different things to draw from the same seed.
enum Salt : uint32_t { kSaltPeriod = 1, kSaltPhase, kSaltGlyph, kSaltLength, kSaltRhythm, kSaltAmbient };

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

std::vector<GlyphDraw> glyphDraws(const Panel& panel, std::span<const RunnerPose> poses, double clock, double intensity, unsigned seed) {
    std::vector<GlyphDraw> out;
    const GlyphFace&       face = panel.glyphs;
    if (!face.on())
        return out;
    intensity = std::clamp(intensity, 0.0, 1.0);
    clock     = std::max(clock, 0.0);
    out.reserve(panel.beads.size() + panel.slots.size() / 4);

    const auto put = [&](size_t id, Vec2 pos, int glyph, double alpha, double heat) {
        const int a = steps(alpha, kGlyphAlphaSteps);
        if (a > 0)
            out.push_back({int(id), int(std::lround(pos.x)), int(std::lround(pos.y)), glyph, a, steps(heat, kGlyphHeatSteps)});
    };

    const Proximity hit = {kHitNear * face.pitch, kHitFar * face.pitch};
    for (size_t i = 0; i < panel.beads.size(); ++i) {
        const Bead&    bead   = panel.beads[i];
        const uint32_t id     = uint32_t(i);
        const double   period = std::max(0.1, face.period) * (0.85 + 0.35 * unit(hashOf(seed, id, kSaltPeriod)));
        const double   cycle  = clock / period + unit(hashOf(seed, id, kSaltPhase));
        const double   wave   = 0.5 - 0.5 * std::cos(kTwoPi * frac(cycle));

        double alpha = std::lerp(kRest, kDimmest + (1 - kDimmest) * wave, intensity);
        double heat  = intensity * kBreathHeat * wave * wave * wave;
        for (const RunnerPose& pose : poses) {
            if (pose.stroke < 0)
                continue;
            const double lit = nearness(std::hypot(pose.pos.x - bead.pos.x, pose.pos.y - bead.pos.y), hit) * pose.alpha * intensity;
            alpha            = std::max(alpha, lit);
            heat             = std::max(heat, lit);
        }
        put(i, bead.pos, glyphAt(seed, id, (long long)std::floor(cycle)), alpha * bead.alpha, heat);
    }

    std::vector<double> lengths;
    lengths.reserve(panel.strokes.size());
    for (const Stroke& stroke : panel.strokes)
        lengths.push_back(strokeLength(stroke));

    for (size_t j = 0; j < panel.slots.size(); ++j) {
        const GlyphSlot& slot = panel.slots[j];
        const uint32_t   id   = uint32_t(j);
        if (slot.stroke < 0 || size_t(slot.stroke) >= lengths.size())
            continue;
        const bool   closed = panel.strokes[size_t(slot.stroke)].closed;
        const double len    = lengths[size_t(slot.stroke)];

        const double change = kChangeMin + kChangeSpan * unit(hashOf(seed, id, kSaltLength));
        const double cycle  = clock / change + unit(hashOf(seed, id, kSaltRhythm));
        const double within = frac(cycle);

        // Every dot on this line lights the characters near it, each as far as it is lit.
        double dark = 1.0, heat = 0.0;
        if (intensity > 0 && len > 0) {
            for (const RunnerPose& pose : poses) {
                if (pose.stroke != slot.stroke)
                    continue;
                double d = slot.at - pose.at;
                if (closed)
                    d -= len * std::round(d / len);

                const double swell = 1 + kSwell * pose.glow;
                double       x     = std::fabs(d) / (kSides * face.pitch * swell);
                if (pose.dir != 0) {
                    const double ahead = d * pose.dir;
                    x                  = ahead >= 0 ? ahead / (kLead * face.pitch * swell) : -ahead / (kTail * face.pitch * swell);
                }
                if (x >= 1)
                    continue;
                dark *= 1.0 - std::pow(1.0 - x, kFall) * intensity;
                heat = std::max(heat, smooth(1.0 - std::fabs(d) / (kHead * face.pitch * swell)));
            }
        }

        double ambient = 0;
        if (intensity > 0 && hashOf(seed, id, kSaltAmbient) % kAmbientOneIn == 0 && within < kAmbientLit) {
            const double s = std::sin(std::numbers::pi * within / kAmbientLit);
            ambient        = kAmbientPeak * s * s * intensity;
        }

        const double alpha = 1.0 - dark * (1.0 - ambient);
        long long    epoch = (long long)std::floor(cycle);
        if (heat >= kFlickerHeat)
            epoch = kFlickerBase + (long long)std::floor(clock * kFlickerHz);
        put(panel.beads.size() + j, slot.pos, glyphAt(seed, id, epoch), alpha * slot.alpha, heat * intensity);
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
