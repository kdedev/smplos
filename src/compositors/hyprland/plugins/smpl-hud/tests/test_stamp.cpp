#include "../stamp.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace smplhud;

namespace {

Tone tone() {
    return makeTone({1.0f, 0.70f, 0.10f, 1.f}, {1.0f, 0.40f, 0.0f, 1.f});
}

// Premultiplied RGBA, 0..255, over a window of window-relative px.
struct Canvas {
    int                x0, y0, w, h;
    std::vector<float> px;
    std::vector<int>   drawn; // how many pieces covered the pixel

    Canvas(int x0_, int y0_, int w_, int h_) : x0(x0_), y0(y0_), w(w_), h(h_), px(size_t(w_) * size_t(h_) * 4, 0.f), drawn(size_t(w_) * size_t(h_), 0) {}

    bool   has(int x, int y) const { return x >= x0 && y >= y0 && x < x0 + w && y < y0 + h; }
    size_t at(int x, int y) const { return size_t(y - y0) * size_t(w) + size_t(x - x0); }
};

void texel(const Image& img, int x, int y, float out[4]) {
    for (int k = 0; k < 4; ++k)
        out[k] = img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4 + size_t(k)];
}

// The pieces the way the compositor draws them: tiles 1:1, strips stretched
// along their run with the end colours blended linearly.
void drawPieces(const Stroke& s, const Tone& t, Canvas& cv) {
    for (const StripPiece& sp : s.strips) {
        const StripSpec spec = stripSpecOf(sp, s, t, t);
        const Image     img  = bakeStrip(spec);
        const int       len  = sp.vertical ? sp.box.h : sp.box.w;
        for (int i = 0; i < len; ++i) {
            const float f = (float(i) + 0.5f) / float(len);
            for (int j = 0; j < spec.size; ++j) {
                float a[4], b[4];
                texel(img, sp.vertical ? j : 0, sp.vertical ? 0 : j, a);
                texel(img, sp.vertical ? j : 1, sp.vertical ? 1 : j, b);
                const int x = sp.box.x + (sp.vertical ? j : i), y = sp.box.y + (sp.vertical ? i : j);
                CHECK(cv.has(x, y));
                if (!cv.has(x, y))
                    continue;
                const size_t at = cv.at(x, y);
                for (int k = 0; k < 4; ++k)
                    cv.px[at * 4 + size_t(k)] += a[k] + (b[k] - a[k]) * f;
                ++cv.drawn[at];
            }
        }
    }
    for (const TilePiece& tile : s.tiles) {
        const Image img = bakeTile(tileSpecOf(tile, s, t));
        CHECK(img.w == tile.box.w && img.h == tile.box.h);
        for (int j = 0; j < img.h; ++j) {
            for (int i = 0; i < img.w; ++i) {
                const int x = tile.box.x + i, y = tile.box.y + j;
                CHECK(cv.has(x, y));
                if (!cv.has(x, y))
                    continue;
                float c[4];
                texel(img, i, j, c);
                const size_t at = cv.at(x, y);
                for (int k = 0; k < 4; ++k)
                    cv.px[at * 4 + size_t(k)] += c[k];
                ++cv.drawn[at];
            }
        }
    }
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

// The stroke drawn straight from its definition: every pixel looks at the
// nearest visible segment, with no pieces and no images.
void drawReference(const Stroke& s, const Tone& t, Canvas& cv) {
    const double hw    = std::lround(s.halfWidth * 16) / 16.0;
    const double reach = std::lround(s.reach * 16) / 16.0;
    const double lineA = alpha8(s.alpha) / 255.0, haloA = alpha8(s.haloAlpha) / 255.0;
    const size_t n     = s.pts.size();
    const size_t count = s.closed ? n : n - 1;

    for (int y = cv.y0; y < cv.y0 + cv.h; ++y) {
        for (int x = cv.x0; x < cv.x0 + cv.w; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            double       best = std::numeric_limits<double>::infinity(), fade = 0;
            for (size_t i = 0; i < count; ++i) {
                const Vertex& a = s.pts[i];
                const Vertex& b = s.pts[(i + 1) % n];
                if (s.alpha * std::max(a.alpha, b.alpha) < kInvisible)
                    continue;
                const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
                const double f  = len2 > 0 ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
                const double ex = px - (a.x + dx * f), ey = py - (a.y + dy * f);
                const double d2 = ex * ex + ey * ey;
                if (d2 < best) {
                    best = d2;
                    fade = lerp(a.alpha, b.alpha, f);
                }
            }
            if (!std::isfinite(best))
                continue;
            const double d = std::sqrt(best), e = d - hw;
            const double core = std::clamp(hw - d + 0.5, 0.0, 1.0);
            const double halo = e <= 0 ? 1.0 : (reach <= 0 || e >= reach ? 0.0 : (1 - e / reach) * (1 - e / reach));
            const double c    = std::clamp(core * lineA * t.line[3] / 255.0, 0.0, 1.0);
            const double h    = std::clamp(halo * haloA * t.glow[3] / 255.0, 0.0, 1.0);
            const size_t at   = cv.at(x, y);
            cv.px[at * 4 + 3] = float((c + (1 - c) * h) * fade * 255);
            for (int k = 0; k < 3; ++k)
                cv.px[at * 4 + size_t(k)] = float((c * t.line[k] / 255.0 + (1 - c) * h * t.glow[k] / 255.0) * fade * 255);
        }
    }
}

struct Fixture {
    const char*         name;
    std::vector<Vertex> pts;
    bool                closed;
    float               alpha, haloAlpha;
    double              tolerance; // of 255
};

// Shapes like the ones the panel builder makes: runs along x or y and at 45
// degrees, whole numbers (shifted by half a px for lines of odd thickness).
const std::vector<Fixture>& fixtures() {
    static const std::vector<Fixture> list = {
        {"open polyline", {{20, 30}, {120, 30}, {160, 70}, {160, 150}, {100, 150}}, false, 1.f, 0.5f, 3},
        {"closed chamfered rectangle", {{30, 10}, {170, 10}, {190, 30}, {190, 110}, {170, 130}, {30, 130}, {10, 110}, {10, 30}}, true, 1.f, 0.55f, 3},
        {"closed rectangle", {{10, 10}, {110, 10}, {110, 60}, {10, 60}}, true, 0.95f, 0.5f, 3},
        {"closed with an invisible connector", {{30, 10}, {170, 10}, {190, 30}, {190, 110}, {170, 130, 0.f}, {30, 130, 0.f}, {10, 110}, {10, 30}}, true, 1.f, 0.55f, 3},
        {"open with a fade", {{10, 20, 1.f}, {110, 20, 1.f}, {210, 20, 0.2f}, {210, 90, 0.2f}}, false, 1.f, 0.5f, 3},
        {"dim trace", {{10, 40}, {60, 40}, {110, 90}, {110, 160}}, false, 0.55f, 0.5f, 3},
        {"vertical then diagonal", {{40, 10}, {40, 90}, {100, 150}, {180, 150}}, false, 1.f, 0.5f, 3},
    };
    return list;
}

Stroke strokeOf(const Fixture& f, double halfWidth, double reach) {
    Stroke s;
    const double shift = std::fmod(halfWidth * 2, 2.0) > 0.5 ? 0.5 : 0.0; // odd thickness: centres sit on pixel centres
    for (Vertex v : f.pts) {
        v.x += shift;
        v.y += shift;
        s.pts.push_back(v);
    }
    s.closed    = f.closed;
    s.alpha     = f.alpha;
    s.haloAlpha = f.haloAlpha;
    s.halfWidth = halfWidth;
    s.reach     = reach;
    cutStroke(s);
    return s;
}

Canvas canvasFor(const Stroke& s) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const Vertex& v : s.pts) {
        x0 = std::min(x0, v.x);
        y0 = std::min(y0, v.y);
        x1 = std::max(x1, v.x);
        y1 = std::max(y1, v.y);
    }
    const int room = roomAround(s.halfWidth, s.reach) + 2;
    return {int(std::floor(x0)) - room, int(std::floor(y0)) - room, int(std::ceil(x1 - x0)) + 2 * room, int(std::ceil(y1 - y0)) + 2 * room};
}

void testPiecesDrawTheStroke() {
    for (const Fixture& f : fixtures()) {
        for (double hw : {1.0, 1.5, 2.0}) {
            for (double reach : {9.0, 0.0}) {
                const Stroke s = strokeOf(f, hw, reach);
                CHECK(!s.strips.empty() || !s.tiles.empty());

                Canvas got = canvasFor(s), want = canvasFor(s);
                drawPieces(s, tone(), got);
                drawReference(s, tone(), want);

                int    overlaps = 0, bad = 0;
                double worst = 0;
                for (int y = got.y0; y < got.y0 + got.h; ++y) {
                    for (int x = got.x0; x < got.x0 + got.w; ++x) {
                        const size_t at = got.at(x, y);
                        overlaps += got.drawn[at] > 1;
                        for (size_t k = 0; k < 4; ++k) {
                            const double diff = std::fabs(got.px[at * 4 + k] - want.px[at * 4 + k]);
                            worst             = std::max(worst, diff);
                            if (diff > f.tolerance && bad++ == 0)
                                std::fprintf(stderr, "  %s hw=%g reach=%g: pixel %d,%d channel %zu: got %g, want %g\n", f.name, hw, reach, x, y, k, got.px[at * 4 + k], want.px[at * 4 + k]);
                        }
                    }
                }
                if (overlaps)
                    std::fprintf(stderr, "  %s hw=%g reach=%g: %d pixel(s) drawn by more than one piece\n", f.name, hw, reach, overlaps);
                CHECK(overlaps == 0);
                CHECK(bad == 0);
                CHECK(worst <= f.tolerance);
            }
        }
    }
}

void testStripsAreStraightAndTilesStayInPlace() {
    for (const Fixture& f : fixtures()) {
        const Stroke s = strokeOf(f, 1.0, 9.0);
        for (const StripPiece& sp : s.strips) {
            CHECK(sp.box.w > 0 && sp.box.h > 0);
            // a strip is as thick as the line plus its glow, whatever its length
            const int across = sp.vertical ? sp.box.w : sp.box.h;
            CHECK(across >= 2 * 9 && across <= 2 * roomAround(1.0, 9.0) + 1);
            CHECK(sp.alphaA >= 0.f && sp.alphaA <= 1.f && sp.alphaB >= 0.f && sp.alphaB <= 1.f);
        }
        for (const TilePiece& t : s.tiles) {
            CHECK(t.box.w > 0 && t.box.h > 0);
            CHECK(!t.segs.empty());
        }
    }
}

void testRecutReplacesOldPieces() {
    const Fixture& f = fixtures()[1];
    Stroke         a = strokeOf(f, 1.0, 9.0);
    Stroke         b = a;
    cutStroke(b);
    CHECK(a.strips.size() == b.strips.size() && a.tiles.size() == b.tiles.size());
    for (size_t i = 0; i < a.strips.size(); ++i)
        CHECK(a.strips[i].box == b.strips[i].box);
    for (size_t i = 0; i < a.tiles.size(); ++i)
        CHECK(a.tiles[i].box == b.tiles[i].box);

    // a different line cuts differently
    b.pts.pop_back();
    cutStroke(b);
    CHECK(b.strips.size() != a.strips.size() || b.tiles.size() != a.tiles.size());
}

void testNothingToDraw() {
    Stroke s;
    s.halfWidth = 1;
    s.reach     = 6;
    cutStroke(s);
    CHECK(s.strips.empty() && s.tiles.empty());

    s.pts = {{5, 5}};
    cutStroke(s);
    CHECK(s.strips.empty() && s.tiles.empty());

    // a line that cannot be seen leaves no pieces
    s.pts   = {{5, 5}, {50, 5}, {50, 50}};
    s.alpha = 0.f;
    cutStroke(s);
    CHECK(s.strips.empty() && s.tiles.empty());

    s.alpha = 1.f;
    s.pts   = {{5, 5, 0.f}, {50, 5, 0.f}, {50, 50, 0.f}};
    cutStroke(s);
    CHECK(s.strips.empty() && s.tiles.empty());

    // a zero length segment does not crash
    s.pts = {{5, 5}, {5, 5}, {50, 5}};
    cutStroke(s);
    CHECK(true);
}

void testInvisibleStretchesDrawNothing() {
    // Two visible runs with an invisible one between them
    Stroke s;
    s.halfWidth = 1;
    s.reach     = 6;
    s.alpha     = 1.f;
    s.haloAlpha = 0.5f;
    s.pts       = {{10, 10}, {80, 10}, {80, 10, 0.f}, {160, 10, 0.f}, {160, 10}, {230, 10}};
    cutStroke(s);
    CHECK(!s.strips.empty());
    const int room = roomAround(1, 6);
    for (const StripPiece& sp : s.strips) {
        // nothing is drawn over the hidden middle, except the cap tiles' reach
        CHECK(sp.box.x + sp.box.w <= 80 + 1 || sp.box.x >= 160 - 1 || sp.alphaA > 0.f);
        CHECK(!(sp.box.x < 80 + 2 && sp.box.x + sp.box.w > 160 - 2));
    }
    for (const TilePiece& t : s.tiles)
        CHECK(!(t.box.x < 80 - room && t.box.x + t.box.w > 160 + room));
}

void testStrokeLengthAndPoints() {
    Stroke sq;
    sq.pts    = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    sq.closed = true;
    CHECK_NEAR(strokeLength(sq), 400, 1e-9);

    const auto at = [&](const Stroke& s, double d) { return strokePointAt(s, d); };
    CHECK_NEAR(at(sq, 0).pos.x, 0, 1e-9);
    CHECK_NEAR(at(sq, 0).pos.y, 0, 1e-9);
    CHECK_NEAR(at(sq, 150).pos.x, 100, 1e-9);
    CHECK_NEAR(at(sq, 150).pos.y, 50, 1e-9);
    CHECK_NEAR(at(sq, 400).pos.x, 0, 1e-9); // wraps round
    CHECK_NEAR(at(sq, 400).pos.y, 0, 1e-9);
    CHECK_NEAR(at(sq, 350).pos.x, 0, 1e-9);
    CHECK_NEAR(at(sq, 350).pos.y, 50, 1e-9);
    CHECK_NEAR(at(sq, -50).pos.y, 50, 1e-9); // and backwards

    Stroke line;
    line.pts = {{0, 0, 1.f}, {100, 0, 0.f}, {100, 50, 0.f}};
    CHECK_NEAR(strokeLength(line), 150, 1e-9); // an open line has no closing segment
    CHECK_NEAR(at(line, 25).pos.x, 25, 1e-9);
    CHECK_NEAR(at(line, 25).alpha, 0.75, 1e-6);
    CHECK_NEAR(at(line, 125).pos.x, 100, 1e-9);
    CHECK_NEAR(at(line, 125).pos.y, 25, 1e-9);
    CHECK_NEAR(at(line, -10).pos.x, 0, 1e-9); // an open line stops at its ends
    CHECK_NEAR(at(line, 1e9).pos.y, 50, 1e-9);

    const Stroke none;
    CHECK_NEAR(strokeLength(none), 0, 1e-9);
    CHECK_NEAR(at(none, 5).alpha, 1.0, 1e-9);
}

void testRoomAround() {
    CHECK(roomAround(1, 8) == 10);
    CHECK(roomAround(1.5, 8.3) == 11);
    CHECK(roomAround(0, 0) == 1);
}

void testToneIsQuantised() {
    const Tone a = makeTone({0.60f, 0.20f, 0.90f, 1.f}, {0.30f, 0.30f, 0.30f, 0.5f});
    const Tone b = makeTone({0.605f, 0.201f, 0.899f, 1.f}, {0.301f, 0.299f, 0.30f, 0.5f});
    CHECK(a == b); // a slowly drifting gradient keeps the same image
    const Tone c = makeTone({0.70f, 0.20f, 0.90f, 1.f}, {0.30f, 0.30f, 0.30f, 0.5f});
    CHECK(!(a == c));

    const Tone e = makeTone({0.f, 1.f, 0.5f, 1.f}, {2.f, -1.f, 0.f, 0.f}); // out of range is clamped
    CHECK(e.line[0] == 0 && e.line[1] == 255 && e.line[3] == 255);
    CHECK(e.glow[0] == 255 && e.glow[1] == 0 && e.glow[3] == 0);
    CHECK_NEAR(e.line[2], 127.5, 4.6); // 5 bits a channel: a step is 8.2 of 255
    CHECK_NEAR(a.line[0], 0.60 * 255, 4.6);
}

void testLooks() {
    Stroke s;
    s.halfWidth = 1.5;
    s.reach     = 9.25;
    s.alpha     = 0.55f;
    s.haloAlpha = 0.5f;
    const Look l = lookOf(s);
    CHECK(l.halfWidth16 == 24 && l.reach16 == 148);
    CHECK(l.lineAlpha == alpha8(0.55f) && l.haloAlpha == alpha8(0.5f));

    Bead b;
    b.radius    = 2.5;
    b.reach     = 2;
    b.haloAlpha = 0.5f;
    const Look lb = lookOf(b);
    CHECK(lb.halfWidth16 == 40 && lb.reach16 == 32 && lb.lineAlpha == 255);

    CHECK(alpha8(0.f) == 0 && alpha8(1.f) == 255 && alpha8(-3.f) == 0 && alpha8(7.f) == 255);
}

template <class Spec, class Mutator>
void differs(const Spec& base, Mutator&& change) {
    Spec other = base;
    change(other);
    CHECK(keyOf(base) != keyOf(other));
}

void testKeysTellImagesApart() {
    StripSpec strip;
    strip.size    = 20;
    strip.centre2 = 21;
    strip.look    = {16, 128, 255, 128};
    strip.a = strip.b = tone();
    CHECK(keyOf(strip) == keyOf(StripSpec(strip)));
    differs(strip, [](StripSpec& s) { s.vertical = true; });
    differs(strip, [](StripSpec& s) { s.size = 21; });
    differs(strip, [](StripSpec& s) { s.centre2 = 20; });
    differs(strip, [](StripSpec& s) { s.look.halfWidth16 = 24; });
    differs(strip, [](StripSpec& s) { s.look.reach16 = 129; });
    differs(strip, [](StripSpec& s) { s.look.lineAlpha = 200; });
    differs(strip, [](StripSpec& s) { s.look.haloAlpha = 100; });
    differs(strip, [](StripSpec& s) { s.a.line[0] ^= 8; });
    differs(strip, [](StripSpec& s) { s.b.glow[2] ^= 8; });
    differs(strip, [](StripSpec& s) { s.alphaA = 10; });
    differs(strip, [](StripSpec& s) { s.alphaB = 10; });

    TileSpec tile;
    tile.w = tile.h = 30;
    tile.look       = {16, 128, 255, 128};
    tile.tone       = tone();
    tile.segs       = {{0, 0, 480, 480, 255, 255}};
    CHECK(keyOf(tile) == keyOf(TileSpec(tile)));
    differs(tile, [](TileSpec& s) { s.w = 31; });
    differs(tile, [](TileSpec& s) { s.h = 31; });
    differs(tile, [](TileSpec& s) { s.look.reach16 = 130; });
    differs(tile, [](TileSpec& s) { s.tone.line[1] ^= 8; });
    differs(tile, [](TileSpec& s) { s.segs[0].bx = 481; });
    differs(tile, [](TileSpec& s) { s.segs[0].alphaB = 0; });
    differs(tile, [](TileSpec& s) { s.segs.push_back({0, 0, 16, 16, 255, 255}); });

    BeadSpec bead{{40, 32, 255, 128}, tone()};
    CHECK(keyOf(bead) == keyOf(BeadSpec(bead)));
    differs(bead, [](BeadSpec& s) { s.look.halfWidth16 = 41; });
    differs(bead, [](BeadSpec& s) { s.tone.glow[0] ^= 8; });

    // different kinds of image never share a cache entry
    CHECK(keyOf(StripSpec{}) != keyOf(TileSpec{}));
    CHECK(keyOf(TileSpec{}) != keyOf(BeadSpec{}));
    CHECK(keyOf(StripSpec{}) != keyOf(BeadSpec{}));
}

void testStripFadesAlongItsRun() {
    StripSpec spec;
    spec.size    = 12;
    spec.centre2 = 12; // the line's centre is the middle of the 12 rows
    spec.look    = {16, 96, 255, 255};
    spec.a = spec.b = tone();
    spec.alphaA     = 255;
    spec.alphaB     = 0;
    const Image img = bakeStrip(spec);
    CHECK(img.w == 2 && img.h == 12);
    float a[4], b[4];
    texel(img, 0, 6, a);
    texel(img, 1, 6, b);
    CHECK(a[3] > 250);
    CHECK(b[3] == 0);

    // no halo outside the line and the glow it reaches
    texel(img, 0, 0, a);
    CHECK(a[3] < 40);

    StripSpec tall = spec;
    tall.vertical  = true;
    const Image v  = bakeStrip(tall);
    CHECK(v.w == 12 && v.h == 2);
    texel(v, 6, 0, a);
    texel(v, 6, 1, b);
    CHECK(a[3] > 250 && b[3] == 0);

    CHECK(bakeStrip(spec).rgba == img.rgba); // the same spec bakes the same image
}

// What the GPU does with a 2 texel strip on its quad: the colour at the centre
// of the pixel at `at`, along the run, with clamped bilinear filtering.
double filtered(const StripPiece& strip, double at) {
    const Quad   q    = stripQuad(strip);
    const double from = strip.vertical ? q.y : q.x, size = strip.vertical ? q.h : q.w;
    const double u    = (at - from) / size * 2; // texel coordinates: the texels' centres are 0.5 and 1.5
    return std::clamp(u - 0.5, 0.0, 1.0);        // the weight of the second texel
}

void testStripQuadPutsTheEndsOnTheTexelCentres() {
    for (const bool vertical : {false, true}) {
        StripPiece strip;
        strip.vertical = vertical;
        strip.box      = vertical ? IBox{30, 100, 9, 80} : IBox{100, 30, 80, 9};

        const Quad q = stripQuad(strip);
        const int  len = vertical ? strip.box.h : strip.box.w;
        if (vertical) {
            CHECK(q.x == strip.box.x && q.w == strip.box.w); // 1:1 across the line
            CHECK(q.y == strip.box.y - len / 2.0 && q.h == 2.0 * len);
        } else {
            CHECK(q.y == strip.box.y && q.h == strip.box.h);
            CHECK(q.x == strip.box.x - len / 2.0 && q.w == 2.0 * len);
        }

        // across the run: the first pixel's centre is a half pixel in, the last one a half pixel short of the end
        const int first = vertical ? strip.box.y : strip.box.x;
        for (int i = 0; i < len; ++i)
            CHECK_NEAR(filtered(strip, first + i + 0.5), (i + 0.5) / len, 1e-9);
        CHECK_NEAR(filtered(strip, first), 0.0, 1e-9);
        CHECK_NEAR(filtered(strip, first + len), 1.0, 1e-9);

        // all of the run is inside the part of the quad where the filter blends, never where it clamps
        CHECK(filtered(strip, first) >= 0.0 && filtered(strip, first + len) <= 1.0);
        const double inner = (first - (vertical ? q.y : q.x)) / (vertical ? q.h : q.w);
        CHECK_NEAR(inner, 0.25, 1e-9);
    }
}

void testTileKeepsItsLineInside() {
    // a 45 degree segment through a 30 px tile
    TileSpec spec;
    spec.w = spec.h = 30;
    spec.look       = {16, 96, 255, 128};
    spec.tone       = tone();
    spec.segs       = {{0, 0, 30 * 16, 30 * 16, 255, 255}};
    const Image img = bakeTile(spec);
    CHECK(img.w == 30 && img.h == 30);
    float c[4];
    texel(img, 15, 15, c);
    CHECK(c[3] > 250); // on the line
    texel(img, 15, 14, c);
    CHECK(c[3] > 150); // beside it
    texel(img, 29, 0, c);
    CHECK(c[3] == 0); // far corner

    TileSpec none = spec;
    none.segs.clear();
    for (uint8_t v : bakeTile(none).rgba)
        CHECK(v == 0);

    TileSpec empty;
    CHECK(bakeTile(empty).empty());
}

void testBeadIsSymmetricAndSoft() {
    Bead b;
    b.radius    = 2.5;
    b.reach     = 2;
    b.haloAlpha = 0.5f;
    const Look  look = lookOf(b);
    const Image img  = bakeBead({look, tone()});
    CHECK(img.w == img.h);
    CHECK(img.w % 2 == 0);
    CHECK(img.w == beadSize(look));
    CHECK(beadSize(look) == 12); // 2 * ceil(2.5 + 2 + 1)

    for (int j = 0; j < img.h; ++j) {
        for (int i = 0; i < img.w; ++i) {
            float p[4], m[4], n[4];
            texel(img, i, j, p);
            texel(img, img.w - 1 - i, j, m);
            texel(img, i, img.h - 1 - j, n);
            for (int k = 0; k < 4; ++k) {
                CHECK(p[k] == m[k]);
                CHECK(p[k] == n[k]);
            }
        }
    }
    float mid[4], corner[4], rim[4];
    texel(img, img.w / 2, img.h / 2, mid);
    texel(img, 0, 0, corner);
    texel(img, img.w / 2 + 3, img.h / 2, rim);
    CHECK(mid[3] == 255);
    CHECK(corner[3] == 0);
    CHECK(rim[3] > 0 && rim[3] < mid[3]); // the halo fades out

    // a wider halo makes a bigger image
    Bead wide = b;
    wide.reach = 6;
    CHECK(beadSize(lookOf(wide)) > beadSize(look));
}

} // namespace

int main() {
    testPiecesDrawTheStroke();
    testStripsAreStraightAndTilesStayInPlace();
    testRecutReplacesOldPieces();
    testNothingToDraw();
    testInvisibleStretchesDrawNothing();
    testStrokeLengthAndPoints();
    testRoomAround();
    testToneIsQuantised();
    testLooks();
    testKeysTellImagesApart();
    testStripFadesAlongItsRun();
    testStripQuadPutsTheEndsOnTheTexelCentres();
    testTileKeepsItsLineInside();
    testBeadIsSymmetricAndSoft();
    return finish("test_stamp");
}
