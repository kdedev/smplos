#include "panel.hpp"

#include "stamp.hpp"

#include <algorithm>
#include <cmath>
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

// Frames smaller than this get no notches, no ghost line and no traces.
constexpr double kFancyWidth  = 480;
constexpr double kFancyHeight = 160;

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
        panel.runners.push_back({0, 0.0, 1.0, dot});
        panel.runners.push_back({0, 0.5, 1.0, dot});
    }

    if (f.fancy && f.tau > 0) {
        Trace t;
        const double speeds[3] = {0.6, 1.0, 0.8};
        int          n         = 0;
        const auto   add       = [&](bool ok) {
            const double speed = speeds[n++];
            if (!ok)
                return;
            panel.strokes.push_back(stroke(std::move(t.pts), false, style.lineAlpha * kTraceAlpha, kTraceReach * reach, kTraceHalo * halo));
            if (!beads)
                return;
            bead.pos = t.from;
            panel.beads.push_back(bead);
            bead.pos = t.to;
            panel.beads.push_back(bead);
            panel.runners.push_back({int(panel.strokes.size()) - 1, 0.0, speed, dot});
        };
        t = {};
        add(notchTrace(f, cuts, t));
        t = {};
        add(stepTrace(f, cuts, t));
        t = {};
        add(cornerTrace(f, cuts, t));
    }

    // Room for the outline, the traces and the beads, whatever the window size.
    const double lineRoom = roomAround(f.hw, reach);
    const double beadRoom = beads ? beadSize(lookOf(dot)) / 2.0 : 0.0;
    const double room     = std::max(lineRoom, beadRoom) + 1;
    const auto   logical  = [&](double phys) { return int(std::ceil(phys / f.s)) + 2; };
    const int    side     = logical(std::ceil(f.c) + room);
    out.extents           = {side, side, logical(std::ceil(f.c + f.tau) + room), side};

    const double clear = minClearance(f);
    const int    edge  = int(std::ceil(f.c + f.hw));
    out.sampleBox      = {-edge, -edge, p.width + 2 * edge, p.height + 2 * edge};
    out.centre         = {f.left, f.top, f.fw(), f.fh(), f.a, 1.0, std::max(0.0, clear - f.hw - 1), f.hw + reach};
    out.valid          = true;
}

RunnerPose runnerPose(const Panel& panel, const Runner& runner, double travelled) {
    if (runner.stroke < 0 || size_t(runner.stroke) >= panel.strokes.size())
        return {};
    const Stroke& stroke = panel.strokes[size_t(runner.stroke)];
    const double  len    = strokeLength(stroke);
    if (len <= 0)
        return {};

    double at = runner.offset * len + travelled * runner.speed;
    at -= std::floor(at / len) * len;
    const StrokePoint p = strokePointAt(stroke, at);

    float fade = 1.f;
    if (!stroke.closed) {
        const double room = std::max(1.0, std::min(len / 4, 16 * runner.look.radius));
        fade              = float(std::clamp(std::min(at, len - at) / room, 0.0, 1.0));
    }
    return {p.pos, std::clamp(p.alpha * runner.look.alpha * fade, 0.f, 1.f)};
}

} // namespace smplhud
