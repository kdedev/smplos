#include "../sweep.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace smplhud;

namespace {

constexpr double kPi = std::numbers::pi;

const Style& scifi() {
    return *styleFor("scifi");
}

// A Neon-like window: `cut` is the window's own corner leg at 1x.
Params windowParams(double scale = 1, double cut = 21, float power = 1, bool floating = false) {
    Params p;
    p.width          = int(std::lround(1200 * scale));
    p.height         = int(std::lround(800 * scale));
    p.scale          = float(scale);
    p.cut            = int(std::lround(cut * scale));
    p.roundingPower  = power;
    p.floating       = floating;
    p.floatGlowRange = 18;
    return p;
}

// What the plugin keeps in range: the layout's extents in physical px.
IBox limitOf(const Params& p, const Layout& l) {
    const int left   = int(std::ceil(l.extents.left * p.scale));
    const int top    = int(std::ceil(l.extents.top * p.scale));
    const int right  = int(std::ceil(l.extents.right * p.scale));
    const int bottom = int(std::ceil(l.extents.bottom * p.scale));
    return {-left, -top, p.width + left + right, p.height + top + bottom};
}

SweepShape shapeFor(const Layout& l, double scale, double logicalLength = 180) {
    return {logicalLength * scale / 2, l.centre.reachOut, l.centre.reachIn, scifi().sweepWhiten, scifi().sweepAlpha};
}

constexpr IBox kHugeLimit{-5000, -5000, 10000, 10000};

// ---- the track ----

Centreline centreline(double w, double h, double leg, double power) {
    Centreline c;
    c.x     = -6.5;
    c.y     = -6.5;
    c.w     = w;
    c.h     = h;
    c.leg   = leg;
    c.power = power;
    return c;
}

void testPathLength() {
    const double w = 1213, h = 813, leg = 25;
    const double straight = 2 * (w - 2 * leg) + 2 * (h - 2 * leg);

    // a chamfer is a straight cut across the corner
    CHECK_NEAR(buildPath(centreline(w, h, leg, 1)).length, straight + 4 * leg * std::sqrt(2.0), 1e-6);

    // a round corner is a quarter circle; the polyline is a hair shorter
    const double round = buildPath(centreline(w, h, leg, 2)).length;
    CHECK_NEAR(round, straight + 2 * kPi * leg, 0.01 * 2 * kPi * leg);
    CHECK(round < straight + 2 * kPi * leg);

    // a squircle sits between the chamfer and the square corner
    const double squircle = buildPath(centreline(w, h, leg, 4)).length;
    CHECK(squircle > round && squircle < 2 * (w + h));

    // a square window has just its perimeter
    CHECK_NEAR(buildPath(centreline(w, h, 0, 1)).length, 2 * (w + h), 1e-6);
}

void testPathIsClosedConvexAndClockwise() {
    for (double power : {1.0, 1.5, 2.0, 4.0, 10.0}) {
        for (double leg : {0.0, 3.0, 25.0, 60.0}) {
            const Centreline c = centreline(400, 300, leg, power);
            const Path       p = buildPath(c);
            CHECK(p.valid());
            if (!p.valid())
                continue;

            double area = 0, arc = 0;
            for (size_t i = 0; i < p.segments.size(); ++i) {
                const Path::Segment& a = p.segments[i];
                const Path::Segment& b = p.segments[(i + 1) % p.segments.size()];

                // consecutive segments join, arcs add up, directions are unit vectors
                CHECK_NEAR(a.from.x + a.dir.x * a.length, b.from.x, 1e-6);
                CHECK_NEAR(a.from.y + a.dir.y * a.length, b.from.y, 1e-6);
                CHECK_NEAR(a.arc, arc, 1e-9);
                CHECK_NEAR(std::hypot(a.dir.x, a.dir.y), 1.0, 1e-9);
                arc += a.length;

                // always turning right on screen: convex and clockwise
                CHECK(a.dir.x * b.dir.y - a.dir.y * b.dir.x >= -1e-9);

                area += a.from.x * b.from.y - b.from.x * a.from.y;

                // everything stays inside the box
                CHECK(a.from.x >= c.x - 1e-9 && a.from.x <= c.x + c.w + 1e-9);
                CHECK(a.from.y >= c.y - 1e-9 && a.from.y <= c.y + c.h + 1e-9);

                // the outward normal (dir.y, -dir.x) points away from the middle
                const double mx = a.from.x + a.dir.x * a.length / 2 - (c.x + c.w / 2);
                const double my = a.from.y + a.dir.y * a.length / 2 - (c.y + c.h / 2);
                CHECK(mx * a.dir.y - my * a.dir.x > 0);
            }
            CHECK(area > 0); // positive in y-down coordinates means clockwise on screen
            CHECK_NEAR(arc, p.length, 1e-9);
        }
    }
}

void testPointAtWrapsAndStaysOnTrack() {
    const Path p = buildPath(centreline(1213, 813, 25, 1));
    for (double s : {0.0, 1.0, 17.3, 400.0, 1000.0, p.length - 0.5}) {
        const PathPoint a = pointAt(p, s);
        const PathPoint b = pointAt(p, s + p.length);
        const PathPoint c = pointAt(p, s - 3 * p.length);
        CHECK_NEAR(a.pos.x, b.pos.x, 1e-6);
        CHECK_NEAR(a.pos.y, b.pos.y, 1e-6);
        CHECK_NEAR(a.pos.x, c.pos.x, 1e-6);
        CHECK_NEAR(a.pos.y, c.pos.y, 1e-6);
        CHECK_NEAR(a.dir.x, b.dir.x, 1e-9);
    }

    // the track starts where the top-right corner leaves the top edge, heading clockwise
    const PathPoint start = pointAt(p, 0);
    CHECK_NEAR(start.pos.x, -6.5 + 1213 - 25, 1e-9);
    CHECK_NEAR(start.pos.y, -6.5, 1e-9);
    CHECK(start.dir.x > 0 && start.dir.y > 0);

    // half way along the top-right chamfer
    const PathPoint mid = pointAt(p, p.segments[0].length / 2);
    CHECK_NEAR(mid.pos.x, -6.5 + 1213 - 25 / 2.0, 1e-9);
    CHECK_NEAR(mid.pos.y, -6.5 + 25 / 2.0, 1e-9);

    CHECK(!buildPath(centreline(0, 10, 1, 1)).valid());
    const Path empty;
    CHECK(pointAt(empty, 5).pos.x == 0);
}

// A clockwise rectangle's corners, each with how visible the line is there.
std::vector<Vertex> rectangle(double w, double h, float a0 = 1.f, float a1 = 1.f, float a2 = 1.f, float a3 = 1.f) {
    return {{0, 0, a0}, {w, 0, a1}, {w, h, a2}, {0, h, a3}};
}

void testPathThroughVertices() {
    const std::vector<Vertex> pts  = rectangle(400, 300, 1.f, 0.75f, 0.25f, 0.f);
    const Path                path = buildPath(pts);
    CHECK(path.valid());
    CHECK(path.segments.size() == 4);
    CHECK_NEAR(path.length, 2 * (400 + 300), 1e-9);

    // the segments run from vertex to vertex, closing back to the first, and
    // are as visible as the vertices at their ends
    double arc = 0;
    for (size_t i = 0; i < path.segments.size(); ++i) {
        const Path::Segment& seg = path.segments[i];
        CHECK_NEAR(seg.from.x, pts[i].x, 1e-9);
        CHECK_NEAR(seg.from.y, pts[i].y, 1e-9);
        CHECK_NEAR(seg.arc, arc, 1e-9);
        CHECK_NEAR(std::hypot(seg.dir.x, seg.dir.y), 1.0, 1e-9);
        CHECK_NEAR(seg.alphaFrom, pts[i].alpha, 1e-6);
        CHECK_NEAR(seg.alphaTo, pts[(i + 1) % pts.size()].alpha, 1e-6);
        arc += seg.length;
    }
    CHECK_NEAR(path.segments[0].dir.x, 1, 1e-9);  // along the top edge to the right
    CHECK_NEAR(path.segments[1].dir.y, 1, 1e-9);  // down the right edge
    CHECK_NEAR(path.segments[2].dir.x, -1, 1e-9); // back along the bottom
    CHECK_NEAR(path.segments[3].dir.y, -1, 1e-9); // and up the left

    const PathPoint p = pointAt(path, 400 + 150);
    CHECK_NEAR(p.pos.x, 400, 1e-9);
    CHECK_NEAR(p.pos.y, 150, 1e-9);

    // a repeated vertex adds nothing
    const std::vector<Vertex> twice = {{0, 0, 1.f}, {0, 0, 1.f}, {400, 0, 1.f}, {400, 300, 1.f}};
    const Path                skip  = buildPath(twice);
    CHECK(skip.segments.size() == 3);
    CHECK_NEAR(skip.length, 400 + 300 + 500, 1e-9);

    // nothing to follow
    CHECK(!buildPath(std::span<const Vertex>{}).valid());
    CHECK(!buildPath(std::span<const Vertex>(pts.data(), 1)).valid());
    CHECK(!buildPath(std::span<const Vertex>(twice.data(), 2)).valid());
}

void testVertexPathMatchesCentreline() {
    // the same track, given as the corners of a Centreline
    for (double power : {1.0, 2.0}) {
        const Path          round = buildPath(centreline(400, 300, 25, power));
        std::vector<Vertex> pts;
        for (const Path::Segment& seg : round.segments)
            pts.push_back({seg.from.x, seg.from.y, 1.f});

        const Path same = buildPath(pts);
        CHECK(same.segments.size() == round.segments.size());
        CHECK_NEAR(same.length, round.length, 1e-6);
        for (size_t i = 0; i < std::min(same.segments.size(), round.segments.size()); ++i) {
            CHECK_NEAR(same.segments[i].length, round.segments[i].length, 1e-6);
            CHECK_NEAR(same.segments[i].arc, round.segments[i].arc, 1e-6);
            CHECK_NEAR(same.segments[i].dir.x, round.segments[i].dir.x, 1e-9);
            CHECK_NEAR(same.segments[i].dir.y, round.segments[i].dir.y, 1e-9);
            CHECK(same.segments[i].alphaFrom == 1.f && same.segments[i].alphaTo == 1.f);
        }
    }
    // and a Centreline's own segments are fully visible
    for (const Path::Segment& seg : buildPath(centreline(400, 300, 25, 1)).segments)
        CHECK(seg.alphaFrom == 1.f && seg.alphaTo == 1.f);
}

// ---- the glow ----

void testSweepTexel() {
    // two logical px per texel, in whole physical px
    CHECK(sweepTexel(0.5) == 2);
    CHECK(sweepTexel(1) == 2);
    CHECK(sweepTexel(1.25) == 3);
    CHECK(sweepTexel(1.5) == 3);
    CHECK(sweepTexel(2) == 4);
    CHECK(sweepTexel(2.5) == 5);
    CHECK(sweepTexel(3) == 6);
    CHECK(sweepTexel(8) == 6);
}

uint8_t alphaAt(const Sprite& sp, int px, int py) {
    // `px`, `py`: window-relative px inside the sprite
    const int i = (px - sp.box.x) / sp.texel;
    const int j = (py - sp.box.y) / sp.texel;
    return sp.rgba[(size_t(j) * sp.width + i) * 4 + 3];
}

void testFormulaOnTheTopEdge() {
    // scale 1, a window 1200 wide: the middle of the top edge is x = 600
    const Params p = windowParams(1);
    const Layout l = buildLayout(scifi(), p);
    const Path   path = buildPath(l.centre);
    const SweepShape shape = shapeFor(l, 1);

    // the arc position of the middle of the top edge: the closing segment ends at the first corner
    const double topEdgeStart = path.segments.back().arc;
    const double s            = topEdgeStart + path.segments.back().length / 2;
    const PathPoint centre    = pointAt(path, s);
    CHECK_NEAR(centre.pos.x, 600, 1e-6);

    const SweepColour colour{0.5f, 0.25f, 1.f, 1.f};
    const Sprite      sp = renderSweep(path, s, shape, colour, 1, limitOf(p, l));
    CHECK(!sp.empty());
    CHECK(sp.rgba.size() == size_t(sp.width) * sp.height * 4);

    const double y = centre.pos.y;
    for (double along : {0.0, 20.0, 50.0, 80.0}) {
        for (double across : {-4.0, -1.0, 0.0, 2.0, 7.0, 12.0}) {
            // texel centres sit on half-integers; sample the one at (600.5 + along, y + 0.5 - across)
            const double px = std::floor(600 + along) + 0.5, py = std::floor(y - across) + 0.5;
            const double dAcross = y - py; // positive = outside (up on the top edge)
            const double dAlong  = px - 600;
            const double reach   = dAcross >= 0 ? shape.reachOut : shape.reachIn;
            const double rho2    = (dAlong / shape.halfLength) * (dAlong / shape.halfLength) + (dAcross / reach) * (dAcross / reach);
            const double expect  = rho2 >= 1 ? 0 : 255 * shape.alpha * (1 - rho2) * (1 - rho2);
            CHECK_NEAR(alphaAt(sp, int(std::floor(px)), int(std::floor(py))), expect, 1.01);
        }
    }

    // brightest at the centre, never above the peak opacity
    int peak = 0;
    for (size_t k = 3; k < sp.rgba.size(); k += 4)
        peak = std::max<int>(peak, sp.rgba[k]);
    CHECK(peak >= int(std::lround(255 * shape.alpha * 0.97)));
    CHECK(peak <= int(std::lround(255 * shape.alpha)));

    // premultiplied: no channel exceeds alpha
    for (size_t k = 0; k < sp.rgba.size(); k += 4) {
        CHECK(sp.rgba[k] <= sp.rgba[k + 3] && sp.rgba[k + 1] <= sp.rgba[k + 3] && sp.rgba[k + 2] <= sp.rgba[k + 3]);
    }
}

void testColourAndWhitening() {
    const Params p = windowParams(1);
    const Layout l = buildLayout(scifi(), p);
    const Path   path = buildPath(l.centre);
    SweepShape   shape = shapeFor(l, 1);

    const double s = path.segments.back().arc + path.segments.back().length / 2;

    // without whitening the core keeps the colour, at the full opacity
    shape.whiten = 0;
    Sprite sp = renderSweep(path, s, shape, {0.8f, 0.4f, 0.2f, 1.f}, 1, limitOf(p, l));
    int    best = 0;
    size_t at   = 0;
    for (size_t k = 3; k < sp.rgba.size(); k += 4)
        if (sp.rgba[k] > best) {
            best = sp.rgba[k];
            at   = k - 3;
        }
    CHECK_NEAR(sp.rgba[at] / double(best), 0.8, 0.02);
    CHECK_NEAR(sp.rgba[at + 1] / double(best), 0.4, 0.02);
    CHECK_NEAR(sp.rgba[at + 2] / double(best), 0.2, 0.02);

    // whitening mixes the core towards white, and only the core
    shape.whiten = 0.5f;
    sp           = renderSweep(path, s, shape, {0.8f, 0.4f, 0.2f, 1.f}, 1, limitOf(p, l));
    best         = 0;
    for (size_t k = 3; k < sp.rgba.size(); k += 4)
        if (sp.rgba[k] > best) {
            best = sp.rgba[k];
            at   = k - 3;
        }
    CHECK(sp.rgba[at + 2] / double(best) > 0.5); // blue 0.2 -> about 0.6
    CHECK(sp.rgba[at + 1] / double(best) > 0.65);

    // strength scales opacity; zero draws nothing
    sp = renderSweep(path, s, shape, {1.f, 1.f, 1.f, 0.5f}, 1, limitOf(p, l));
    best = 0;
    for (size_t k = 3; k < sp.rgba.size(); k += 4)
        best = std::max<int>(best, sp.rgba[k]);
    CHECK(best <= int(std::lround(255 * shape.alpha * 0.5)) && best >= int(std::lround(255 * shape.alpha * 0.5 * 0.97)));

    sp = renderSweep(path, s, shape, {1.f, 1.f, 1.f, 0.f}, 1, limitOf(p, l));
    CHECK(std::all_of(sp.rgba.begin(), sp.rgba.end(), [](uint8_t v) { return v == 0; }));
}

void testSymmetryAlongTheTrack() {
    for (double scale : {1.0, 2.0}) {
        const Params p = windowParams(scale);
        const Layout l = buildLayout(scifi(), p);
        const Path   path = buildPath(l.centre);
        const SweepShape shape = shapeFor(l, scale);

        const double s = path.segments.back().arc + path.segments.back().length / 2;
        const double cx = pointAt(path, s).pos.x;
        const int    q  = sweepTexel(scale);
        CHECK(std::fmod(cx, q) == 0); // the middle of the window is a lattice line

        const Sprite sp = renderSweep(path, s, shape, {0.6f, 0.7f, 1.f, 1.f}, q, limitOf(p, l));
        CHECK(!sp.empty());

        // texel columns mirror about x = cx
        const int mid = (int(cx) - sp.box.x) / q; // the first column right of the middle
        for (int j = 0; j < sp.height; ++j) {
            for (int d = 0; d < std::min(mid, sp.width - mid); ++d) {
                const uint8_t* a = &sp.rgba[(size_t(j) * sp.width + (mid + d)) * 4];
                const uint8_t* b = &sp.rgba[(size_t(j) * sp.width + (mid - 1 - d)) * 4];
                for (int k = 0; k < 4; ++k)
                    CHECK(std::abs(int(a[k]) - int(b[k])) <= 1);
            }
        }
    }
}

// Independent evaluation of one texel: the nearest of all segments, no culling.
struct Rgba {
    int r = 0, g = 0, b = 0, a = 0;
};

Rgba bruteTexel(const Path& path, double s, const SweepShape& shape, const SweepColour& col, double x, double y) {
    const double half = std::min(shape.halfLength, path.length / 2);

    double bestD2 = 1e300, bestArc = 0;
    bool   outside = true;
    for (const Path::Segment& seg : path.segments) {
        const double rx = x - seg.from.x, ry = y - seg.from.y;
        double       t  = rx * seg.dir.x + ry * seg.dir.y;
        t               = std::max(0.0, std::min(seg.length, t));
        const double dx = rx - seg.dir.x * t, dy = ry - seg.dir.y * t;
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2  = d2;
            bestArc = seg.arc + t;
            outside = dx * seg.dir.y - dy * seg.dir.x >= 0;
        }
    }

    double along = std::fmod(bestArc - s, path.length);
    if (along > path.length / 2)
        along -= path.length;
    if (along < -path.length / 2)
        along += path.length;

    const double reach = outside ? shape.reachOut : shape.reachIn;
    if (reach <= 0)
        return {};
    const double rho2 = bestD2 / (reach * reach) + (along / half) * (along / half);
    if (rho2 >= 1)
        return {};

    const double inten = (1 - rho2) * (1 - rho2);
    const double alpha = std::min(1.0, shape.alpha * col.strength * inten);
    const double white = std::min(1.0, shape.whiten * inten * inten);
    const auto   mix   = [&](double c) { return int(std::lround(255 * (c + (1 - c) * white) * alpha)); };
    return {mix(col.r), mix(col.g), mix(col.b), int(std::lround(255 * alpha))};
}

// Compares a sprite with the brute force over a square around the glow, so
// that a too-small box or a wrongly culled segment shows up as a mismatch.
void compareWithBruteForce(const Path& path, double s, const SweepShape& shape, int texel, const char* what) {
    const SweepColour col{0.55f, 0.65f, 1.f, 0.9f};
    const Sprite      sp = renderSweep(path, s, shape, col, texel, kHugeLimit);
    CHECK(!sp.empty());
    if (sp.empty())
        return;

    const PathPoint centre = pointAt(path, s);
    const double    radius = std::min(shape.halfLength, path.length / 2) + std::max(shape.reachOut, shape.reachIn) + 20;
    const int       q      = texel;

    int mismatches = 0, lit = 0;
    for (int y = int(std::floor((centre.pos.y - radius) / q)) * q; y < centre.pos.y + radius; y += q) {
        for (int x = int(std::floor((centre.pos.x - radius) / q)) * q; x < centre.pos.x + radius; x += q) {
            const Rgba want = bruteTexel(path, s, shape, col, x + q / 2.0, y + q / 2.0);
            Rgba       got;
            const bool inBox = x >= sp.box.x && y >= sp.box.y && x + q <= sp.box.x + sp.box.w && y + q <= sp.box.y + sp.box.h;
            if (inBox) {
                const uint8_t* px = &sp.rgba[(size_t((y - sp.box.y) / q) * sp.width + (x - sp.box.x) / q) * 4];
                got               = {px[0], px[1], px[2], px[3]};
            }
            lit += want.a > 0;
            if (std::abs(got.r - want.r) > 1 || std::abs(got.g - want.g) > 1 || std::abs(got.b - want.b) > 1 || std::abs(got.a - want.a) > 1) {
                if (++mismatches <= 3)
                    std::fprintf(stderr, "  %s: texel (%d, %d) %s: want %d,%d,%d,%d got %d,%d,%d,%d\n", what, x, y, inBox ? "in the box" : "OUTSIDE the box", want.r, want.g, want.b, want.a,
                                 got.r, got.g, got.b, got.a);
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(lit > 100); // the comparison really covered a glow
}

void testMatchesBruteForce() {
    for (double power : {1.0, 2.0, 4.0}) {
        for (double scale : {1.0, 2.0}) {
            const Params p    = windowParams(scale, 21, float(power));
            const Layout l    = buildLayout(scifi(), p);
            const Path   path = buildPath(l.centre);
            const SweepShape shape = shapeFor(l, scale);
            const int    q = sweepTexel(scale);

            // along the top edge, around each corner, across the wrap and mid-corner
            const double seg0 = path.segments.front().length;
            for (double s : {0.0, seg0 / 2, path.length / 8, path.length / 4, path.length / 2 + 3.3, path.length - 7.0, path.length - 120 * scale, 100.0 * scale}) {
                compareWithBruteForce(path, s, shape, q, "scifi");
            }
        }
    }

    // a glow longer than the whole track wraps onto itself
    const Params p    = windowParams(1);
    const Layout l    = buildLayout(scifi(), p);
    const Path   path = buildPath(l.centre);
    SweepShape   huge = shapeFor(l, 1, 100000);
    CHECK(huge.halfLength > path.length / 2);
    compareWithBruteForce(path, 640, huge, 1, "whole track");
}

// ---- following the line's visibility ----

// The glow is as strong as the line is visible where it is nearest: it follows
// a line that fades along a segment and stays dark where the line is not there.
void testGlowFollowsTheLinesVisibility() {
    const double      w = 400, h = 300;
    const int         q = 2;
    const SweepShape  shape{200, 30, 20, 0.f, 0.8f};
    const SweepColour col{1.f, 0.5f, 0.25f, 0.9f};

    // The top edge is fully visible, the right one fades out going down, the
    // bottom one is invisible and the left one fades back in going up.
    const Path path = buildPath(rectangle(w, h, 1.f, 1.f, 0.f, 0.f));

    // Beside a vertical edge, `s` px along the track from its start: the glow
    // is its formula times the visibility, which is 1 - y / h on both edges.
    const auto checkEdge = [&](double s, double x0, double x1, double edgeX, double reach, const char* what) {
        const Sprite sp = renderSweep(path, s, shape, col, q, kHugeLimit);
        CHECK(!sp.empty());
        int lit = 0, wrong = 0;
        for (int j = 0; j < sp.height; ++j) {
            for (int i = 0; i < sp.width; ++i) {
                const double x = sp.box.x + (i + 0.5) * q, y = sp.box.y + (j + 0.5) * q;
                if (!(x > x0 && x < x1 && y >= 40 && y <= h - 40))
                    continue;
                const double   across = (x - edgeX) / reach, along = (y - h / 2) / shape.halfLength;
                const double   rho2   = across * across + along * along;
                const double   inten  = rho2 < 1 ? (1 - rho2) * (1 - rho2) : 0.0;
                const double   alpha  = shape.alpha * col.strength * inten * (1 - y / h);
                const uint8_t* px     = &sp.rgba[(size_t(j) * sp.width + i) * 4];
                const bool     right  = std::abs(px[3] - int(std::lround(255 * alpha))) <= 1 && std::abs(px[0] - int(std::lround(255 * col.r * alpha))) <= 1 &&
                    std::abs(px[1] - int(std::lround(255 * col.g * alpha))) <= 1 && std::abs(px[2] - int(std::lround(255 * col.b * alpha))) <= 1;
                wrong += !right;
                lit += px[3] > 0;
            }
        }
        if (wrong)
            std::fprintf(stderr, "  %s: %d texels differ\n", what, wrong);
        CHECK(wrong == 0);
        CHECK(lit > 200);
    };
    checkEdge(w + h / 2, w, w + shape.reachOut, w, shape.reachOut, "right edge, outside");
    checkEdge(w + h / 2, w - shape.reachIn, w, w, shape.reachIn, "right edge, inside");
    checkEdge(2 * w + h + h / 2, -shape.reachOut, 0, 0, shape.reachOut, "left edge, outside");
    checkEdge(2 * w + h + h / 2, 0, shape.reachIn, 0, shape.reachIn, "left edge, inside");

    // Nothing at all along the invisible bottom edge, on either side of it...
    const double bottomMiddle = w + h + w / 2;
    const auto   litAlongBottom = [&](const Path& track) {
        const Sprite sp  = renderSweep(track, bottomMiddle, shape, col, q, kHugeLimit);
        int          lit = 0;
        for (int j = 0; j < sp.height; ++j) {
            for (int i = 0; i < sp.width; ++i) {
                const double x = sp.box.x + (i + 0.5) * q, y = sp.box.y + (j + 0.5) * q;
                if (x > 50 && x < w - 50 && y > h - shape.reachIn && y < h + shape.reachOut)
                    lit += sp.rgba[(size_t(j) * sp.width + i) * 4 + 3] > 0;
            }
        }
        return lit;
    };
    CHECK(litAlongBottom(path) == 0);

    // ...though the same glow lights that stretch when the line is there
    CHECK(litAlongBottom(buildPath(rectangle(w, h))) > 200);
}

// ---- keeping off the window ----

// Distance from a window-relative point to the window's boundary (brute force).
double distanceToWindow(double x, double y, const Params& p) {
    // fold into the top-left quadrant
    x = std::min(x, p.width - x);
    y = std::min(y, p.height - y);

    const double cut = p.cut, power = std::max(1.0, double(p.roundingPower));
    double       best = 1e18;
    const auto   consider = [&](double bx, double by) { best = std::min(best, std::hypot(x - bx, y - by)); };
    for (int i = 0; i <= 1200; ++i) {
        consider(0, cut + i * 0.25);
        consider(cut + i * 0.25, 0);
    }
    for (int i = 0; i <= 1000; ++i) {
        const double phi = (kPi / 2) * i / 1000;
        consider(cut * (1 - std::pow(std::cos(phi), 2 / power)), cut * (1 - std::pow(std::sin(phi), 2 / power)));
    }
    return best;
}

bool insideWindow(double x, double y, const Params& p) {
    x = std::min(x, p.width - x);
    y = std::min(y, p.height - y);
    if (x < 0 || y < 0)
        return false;
    const double cut = p.cut, power = std::max(1.0, double(p.roundingPower));
    if (x >= cut || y >= cut)
        return true;
    return std::pow((cut - x) / cut, power) + std::pow((cut - y) / cut, power) <= 1.0;
}

// Whether a window-relative point is strictly inside the (convex, clockwise) track.
bool insideTrack(const Path& path, double x, double y) {
    for (const Path::Segment& seg : path.segments) {
        if ((x - seg.from.x) * seg.dir.y - (y - seg.from.y) * seg.dir.x > -1e-9)
            return false;
    }
    return true;
}

void testGlowNeverReachesTheWindow() {
    struct Case {
        double cut;
        float  power;
        double chamfer;
        bool   trackCrossesWindow; // the theme's chamfer is deeper than the window's own corner
    };
    const Case cases[] = {
        {21, 1.f, 10, false}, // Neon
        {0, 1.f, 10, false},  // square window, the theme's chamfer cuts deeper than the window
        {0, 1.f, 30, true},   // the line itself crosses the window's corner
        {21, 2.f, 10, false},
        {0, 2.f, 10, false},
        {30, 4.f, 10, false},
    };

    for (const Case& cs : cases) {
        for (double scale : {1.0, 2.0}) {
            Params p       = windowParams(scale, cs.cut, cs.power);
            p.chamfer      = float(cs.chamfer);
            const Layout l = buildLayout(scifi(), p);
            const Path   path = buildPath(l.centre);
            const SweepShape shape = shapeFor(l, scale);
            const int    q = sweepTexel(scale);

            // when the track runs over the window, the glow must keep to its outer side
            if (cs.trackCrossesWindow)
                CHECK(shape.reachIn == 0);

            int lit = 0, bad = 0;
            for (int k = 0; k < 48; ++k) {
                const double s  = path.length * k / 48.0;
                const Sprite sp = renderSweep(path, s, shape, {1.f, 1.f, 1.f, 1.f}, q, limitOf(p, l));
                for (int j = 0; j < sp.height; ++j) {
                    for (int i = 0; i < sp.width; ++i) {
                        if (sp.rgba[(size_t(j) * sp.width + i) * 4 + 3] == 0)
                            continue;
                        ++lit;
                        const double x = sp.box.x + (i + 0.5) * q, y = sp.box.y + (j + 0.5) * q;
                        const bool   crossing = insideWindow(x, y, p) || distanceToWindow(x, y, p) < 0.35;
                        // never inside, and clear of the edge by half a px or more; where the
                        // line itself crosses the window, never on the inner side of it
                        if (cs.trackCrossesWindow ? (crossing && insideTrack(path, x, y)) : crossing)
                            ++bad;
                    }
                }
            }
            if (bad)
                std::fprintf(stderr, "  cut %.0f power %.0f chamfer %.0f scale %.0f: %d texel(s) on the window\n", cs.cut, double(cs.power), cs.chamfer, scale, bad);
            CHECK(bad == 0);
            CHECK(lit > 1000);
        }
    }
}

void testGlowFollowsTheChamfer() {
    const Params p    = windowParams(1);
    const Layout l    = buildLayout(scifi(), p);
    const Path   path = buildPath(l.centre);
    const SweepShape shape = shapeFor(l, 1);
    const double leg  = l.centre.leg;

    // centred on the middle of the top-right chamfer
    const double s  = path.segments[0].length / 2;
    const Sprite sp = renderSweep(path, s, shape, {1.f, 1.f, 1.f, 1.f}, 1, limitOf(p, l));
    CHECK(!sp.empty());

    const double mx = l.centre.x + l.centre.w - leg / 2, my = l.centre.y + leg / 2;
    CHECK(alphaAt(sp, int(std::floor(mx)), int(std::floor(my))) >= int(255 * shape.alpha * 0.9));

    // the box corner is farther from the chamfer than the glow reaches
    const double cornerX = l.centre.x + l.centre.w, cornerY = l.centre.y;
    CHECK(std::hypot(cornerX - mx, cornerY - my) > shape.reachOut);
    CHECK(alphaAt(sp, int(std::floor(cornerX - 1)), int(std::floor(cornerY + 1))) == 0);
}

void testBoundsAreTightAndInsideTheLimit() {
    for (double scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        for (float power : {1.f, 2.f}) {
            for (bool floating : {false, true}) {
                const Params     p     = windowParams(scale, 21, power, floating);
                const Layout     l     = buildLayout(scifi(), p);
                const Path       path  = buildPath(l.centre);
                const SweepShape shape = shapeFor(l, scale);
                const IBox       limit = limitOf(p, l);
                const int        q     = sweepTexel(scale);

                for (int k = -5; k < 100; ++k) {
                    const double s   = path.length * k / 53.0;
                    const IBox   box = sweepBounds(path, s, shape, q, limit);
                    CHECK(box.w > 0 && box.h > 0);
                    CHECK(box.x >= limit.x && box.y >= limit.y && box.x + box.w <= limit.x + limit.w && box.y + box.h <= limit.y + limit.h);
                    CHECK(box.x % q == 0 && box.y % q == 0 && box.w % q == 0 && box.h % q == 0);

                    // Tight: a stretch of track of length 2 * halfLength spans at most that
                    // much each way, plus the reach and its padding, and a texel of rounding
                    // on either side.
                    const double reach = std::max(shape.reachOut, shape.reachIn);
                    const double most  = 2 * shape.halfLength + 2 * (reach + q / 2.0 + 0.5) + 2 * q;
                    CHECK(box.w <= most && box.h <= most);
                }
            }
        }
    }
}

void testSpriteRimIsDark() {
    // Where the box is not cut by the limit, the sprite's outermost texels must
    // be fully transparent, or the sprite's edge would show as a hard line.
    for (double scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        for (float power : {1.f, 2.f, 4.f}) {
            const Params     p     = windowParams(scale, 21, power);
            const Layout     l     = buildLayout(scifi(), p);
            const Path       path  = buildPath(l.centre);
            const SweepShape shape = shapeFor(l, scale);
            const int        q     = sweepTexel(scale);

            for (int k = 0; k < 40; ++k) {
                const double s  = path.length * k / 40.0 + 0.37;
                const Sprite sp = renderSweep(path, s, shape, {1.f, 1.f, 1.f, 1.f}, q, kHugeLimit);
                CHECK(!sp.empty());
                if (sp.empty())
                    continue;

                int lit = 0;
                for (int j = 0; j < sp.height; ++j) {
                    for (int i = 0; i < sp.width; ++i) {
                        const uint8_t* px = &sp.rgba[(size_t(j) * sp.width + i) * 4];
                        lit += px[3] > 0;
                        if (i == 0 || j == 0 || i == sp.width - 1 || j == sp.height - 1)
                            CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0);
                    }
                }
                CHECK(lit > 0);
            }
        }
    }
}

void testEmptyCases() {
    const Params p    = windowParams(1);
    const Layout l    = buildLayout(scifi(), p);
    const Path   path = buildPath(l.centre);
    const SweepShape shape = shapeFor(l, 1);
    const SweepColour colour;

    // no track, no glow
    CHECK(renderSweep(Path{}, 10, shape, colour, 1, limitOf(p, l)).empty());
    CHECK(sweepBounds(Path{}, 10, shape, 1, limitOf(p, l)).w == 0);

    SweepShape none = shape;
    none.halfLength = 0;
    CHECK(renderSweep(path, 10, none, colour, 1, limitOf(p, l)).empty());

    none            = shape;
    none.reachOut   = 0;
    none.reachIn    = 0;
    CHECK(renderSweep(path, 10, none, colour, 1, limitOf(p, l)).empty());

    // nothing fits in an empty limit
    CHECK(renderSweep(path, 10, shape, colour, 1, IBox{0, 0, 0, 0}).empty());

    // no room inside: only the outside is lit
    SweepShape outer = shape;
    outer.reachIn    = 0;
    const Sprite sp  = renderSweep(path, path.segments.back().arc + 300, outer, {1.f, 1.f, 1.f, 1.f}, 1, limitOf(p, l));
    CHECK(!sp.empty());
    const double y = l.centre.y;
    CHECK(alphaAt(sp, 600, int(std::floor(y - 3))) > 0);
    CHECK(alphaAt(sp, 600, int(std::floor(y + 3))) == 0);
}

// ---- timing ----

void testInterval() {
    CHECK_NEAR(sweepInterval(80), 3.5 / 80, 1e-12); // a step every 3.5 px
    CHECK_NEAR(sweepInterval(70), 0.05, 1e-12);     // the Neon speed: 20 a second, three frames at 60 Hz
    CHECK_NEAR(sweepInterval(25), 0.1, 1e-12);      // slow: never rarer than 10 Hz
    CHECK_NEAR(sweepInterval(10), 0.1, 1e-12);
    CHECK_NEAR(sweepInterval(400), 0.016, 1e-12);   // fast: never more often than ~60 Hz
    CHECK_NEAR(sweepInterval(5000), 0.016, 1e-12);

    // nonsense still gives a usable, slow timer
    CHECK_NEAR(sweepInterval(0), 0.1, 1e-12);
    CHECK_NEAR(sweepInterval(-30), 0.1, 1e-12);

    // the step is 3.5 px, finer for slow glows (rate capped), coarser for fast ones (rate capped)
    for (double speed = 10; speed <= 400; speed += 10) {
        const double dt   = sweepInterval(speed);
        const double step = speed * dt;
        CHECK(dt >= 0.016 - 1e-12 && dt <= 0.1 + 1e-12);
        if (dt >= 0.1 - 1e-12)
            CHECK(step <= 3.5 + 1e-9);
        else if (dt <= 0.016 + 1e-12)
            CHECK(step >= 3.5 - 1e-9);
        else
            CHECK_NEAR(step, 3.5, 1e-9);
    }
}

void testAdvance() {
    // 100 logical px/s at 2x over a 1000 px track: 20% of the track per second
    CHECK_NEAR(sweepAdvance(0.5, 1, 100, 2, 1000), 0.7, 1e-12);
    CHECK_NEAR(sweepAdvance(0.5, 0.25, 100, 2, 1000), 0.55, 1e-12);

    // wraps round the track
    CHECK_NEAR(sweepAdvance(0.9, 1, 100, 1, 500), 0.1, 1e-12);
    CHECK_NEAR(sweepAdvance(0.0, 7.5, 100, 1, 500), 0.5, 1e-12);

    // a longer track means a slower phase for the same speed
    CHECK(sweepAdvance(0, 1, 100, 1, 2000) < sweepAdvance(0, 1, 100, 1, 1000));

    // the same speed in logical px is the same distance in physical px at any scale
    for (const double scale : {1.0, 1.25, 2.0, 3.0}) {
        const double track = 3000 * scale;
        CHECK_NEAR(sweepAdvance(0, 2, 60, scale, track), 120.0 / 3000, 1e-12);
    }

    // nothing moves for a missing duration, speed, track or scale
    CHECK_NEAR(sweepAdvance(0.3, 0, 100, 1, 500), 0.3, 1e-12);
    CHECK_NEAR(sweepAdvance(0.3, -1, 100, 1, 500), 0.3, 1e-12);
    CHECK_NEAR(sweepAdvance(0.3, 1, 0, 1, 500), 0.3, 1e-12);
    CHECK_NEAR(sweepAdvance(0.3, 1, 100, 1, 0), 0.3, 1e-12);
    CHECK_NEAR(sweepAdvance(0.3, 1, 100, 0, 500), 0.3, 1e-12);

    // always in [0, 1), whatever comes in
    for (const double phase : {-2.7, -0.3, 0.0, 0.5, 0.999999, 1.0, 3.25, std::nan("")}) {
        for (const double seconds : {0.0, 0.016, 0.25, 1000.0}) {
            const double out = sweepAdvance(phase, seconds, 80, 1, 900);
            CHECK(out >= 0 && out < 1);
        }
    }

    // many small steps agree with one big one
    double phase = 0.125;
    for (int i = 0; i < 1000; ++i)
        phase = sweepAdvance(phase, 0.016, 80, 1.25, 2400);
    CHECK_NEAR(phase, std::fmod(0.125 + 16.0 * 80 * 1.25 / 2400, 1.0), 1e-9);
}

void testFade() {
    // a focused window's glow fades in, an unfocused one's fades out, both in `duration` seconds
    CHECK_NEAR(sweepFade(0.0, true, 0.1, 0.5), 0.2, 1e-12);
    CHECK_NEAR(sweepFade(1.0, false, 0.1, 0.5), 0.8, 1e-12);
    CHECK_NEAR(sweepFade(0.5, true, 0.1, 0.5), 0.7, 1e-12);
    CHECK_NEAR(sweepFade(0.5, false, 0.1, 0.5), 0.3, 1e-12);

    // it never leaves [0, 1], and stays put once there
    CHECK_NEAR(sweepFade(0.95, true, 0.25, 0.5), 1.0, 1e-12);
    CHECK_NEAR(sweepFade(0.05, false, 0.25, 0.5), 0.0, 1e-12);
    CHECK_NEAR(sweepFade(1.0, true, 0.25, 0.5), 1.0, 1e-12);
    CHECK_NEAR(sweepFade(0.0, false, 0.25, 0.5), 0.0, 1e-12);
    CHECK_NEAR(sweepFade(7.0, true, 0.0, 0.5), 1.0, 1e-12);
    CHECK_NEAR(sweepFade(-3.0, false, 0.0, 0.5), 0.0, 1e-12);

    // a whole duration is a whole fade, from either end
    double level = 0;
    for (int i = 0; i < 50; ++i)
        level = sweepFade(level, true, 0.01, 0.5);
    CHECK_NEAR(level, 1.0, 1e-9);
    for (int i = 0; i < 50; ++i)
        level = sweepFade(level, false, 0.01, 0.5);
    CHECK_NEAR(level, 0.0, 1e-9);

    // focus coming back half way through a fade-out turns round from where it is
    level = 1;
    for (int i = 0; i < 25; ++i)
        level = sweepFade(level, false, 0.01, 0.5);
    CHECK_NEAR(level, 0.5, 1e-9);
    for (int i = 0; i < 25; ++i)
        level = sweepFade(level, true, 0.01, 0.5);
    CHECK_NEAR(level, 1.0, 1e-9);

    // no time passing changes nothing; no duration is instant
    CHECK_NEAR(sweepFade(0.4, true, 0, 0.5), 0.4, 1e-12);
    CHECK_NEAR(sweepFade(0.4, true, -1, 0.5), 0.4, 1e-12);
    CHECK_NEAR(sweepFade(0.4, true, 0.01, 0), 1.0, 1e-12);
    CHECK_NEAR(sweepFade(0.4, false, 0.01, 0), 0.0, 1e-12);
    CHECK_NEAR(sweepFade(0.4, false, 0.01, -2), 0.0, 1e-12);

    // nonsense in is the target out, never nonsense
    const double nan = std::nan("");
    CHECK_NEAR(sweepFade(nan, true, 0.01, 0.5), 1.0, 1e-12);
    CHECK_NEAR(sweepFade(nan, false, 0.01, 0.5), 0.0, 1e-12);
    CHECK_NEAR(sweepFade(0.4, true, nan, 0.5), 0.4, 1e-12);
    CHECK_NEAR(sweepFade(0.4, true, 0.01, nan), 1.0, 1e-12);
    CHECK_NEAR(sweepFade(0.4, false, 0.01, nan), 0.0, 1e-12);
}

void testIntensity() {
    CHECK_NEAR(sweepIntensity(0.0), 0.0, 1e-12);
    CHECK_NEAR(sweepIntensity(0.5), 0.5, 1e-12);
    CHECK_NEAR(sweepIntensity(1.0), 1.0, 1e-12);

    // eased: gentler than the level at both ends
    CHECK(sweepIntensity(0.1) < 0.1);
    CHECK(sweepIntensity(0.9) > 0.9);

    // never goes backwards, never leaves [0, 1]
    double prev = 0;
    for (int i = 0; i <= 100; ++i) {
        const double out = sweepIntensity(i / 100.0);
        CHECK(out >= prev - 1e-12);
        CHECK(out >= 0 && out <= 1);
        prev = out;
    }
    CHECK_NEAR(sweepIntensity(-1.0), 0.0, 1e-12);
    CHECK_NEAR(sweepIntensity(2.0), 1.0, 1e-12);
    CHECK_NEAR(sweepIntensity(std::nan("")), 0.0, 1e-12);
}

} // namespace

int main() {
    testPathLength();
    testPathIsClosedConvexAndClockwise();
    testPointAtWrapsAndStaysOnTrack();
    testPathThroughVertices();
    testVertexPathMatchesCentreline();
    testSweepTexel();
    testFormulaOnTheTopEdge();
    testColourAndWhitening();
    testSymmetryAlongTheTrack();
    testMatchesBruteForce();
    testGlowFollowsTheLinesVisibility();
    testGlowNeverReachesTheWindow();
    testGlowFollowsTheChamfer();
    testBoundsAreTightAndInsideTheLimit();
    testSpriteRimIsDark();
    testEmptyCases();
    testInterval();
    testAdvance();
    testFade();
    testIntensity();
    return finish("test_sweep");
}
