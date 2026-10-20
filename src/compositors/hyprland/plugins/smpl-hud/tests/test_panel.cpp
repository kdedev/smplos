#include "../glyph.hpp"
#include "../panel.hpp"
#include "../stamp.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace smplhud;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

const Style& panelStyle() {
    return *styleFor("panel");
}

const Style& matrixStyle() {
    return *styleFor("matrix");
}

Params params(int w, int h, float scale = 1.f, bool floating = false, double lineWidth = 2) {
    Params p;
    p.width          = w;
    p.height         = h;
    p.scale          = scale;
    p.floating       = floating;
    p.cut            = int(std::lround(21 * scale));
    p.roundingPower  = 1.f;
    p.gap            = 15;
    p.lineWidth      = lineWidth;
    p.chamfer        = 10;
    p.floatGlowRange = 14;
    return p;
}

// From the smallest window that gets a HUD to a 4K monitor.
constexpr int   kSizes[][2] = {{40, 40}, {41, 300}, {300, 41}, {100, 100}, {400, 300}, {640, 200}, {700, 400}, {1000, 100}, {1920, 1080}, {3840, 2160}};
constexpr float kScales[]   = {1.f, 1.25f, 1.5f, 2.f};

std::string label(const Params& p) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%dx%d @%g%s, line %g", p.width, p.height, double(p.scale), p.floating ? ", floating" : "", p.lineWidth);
    return buf;
}

// The thickness of the line in physical px, the way the builder rounds it.
int thickness(const Params& p) {
    const int bs = std::max(1, int(std::lround(p.lineWidth)));
    return std::max(1, int(std::round(float(bs) * p.scale)));
}

bool whole(double v) {
    return std::fabs(v - std::round(v)) < 1e-9;
}

bool half(double v) {
    return std::fabs(v - std::floor(v) - 0.5) < 1e-9;
}

bool visible(const Stroke& s, const Vertex& a, const Vertex& b) {
    return s.alpha * std::max(a.alpha, b.alpha) >= kInvisible;
}

// Calls fn(a, b) for every segment, including the closing one of a closed stroke.
template <class Fn>
void eachSegment(const Stroke& s, Fn fn) {
    const size_t n = s.pts.size(), count = s.closed ? n : (n ? n - 1 : 0);
    for (size_t i = 0; i < count; ++i)
        fn(s.pts[i], s.pts[(i + 1) % n]);
}

double distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    const double t = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

double distToStroke(const Stroke& s, Vec2 p) {
    double best = kInf;
    eachSegment(s, [&](const Vertex& a, const Vertex& b) { best = std::min(best, distToSegment(p, {a.x, a.y}, {b.x, b.y})); });
    return best;
}

// Positive when the polygon runs clockwise on the screen (y points down).
double signedArea(const std::vector<Vertex>& pts) {
    double sum = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vertex& a = pts[i];
        const Vertex& b = pts[(i + 1) % pts.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum / 2;
}

// How far a point is from a window whose corners are cut at 45 degrees (the
// shape of a window at rounding_power 1); 0 inside it.
double distToWindow(Vec2 q, int w, int h, int cut) {
    const double W = w, H = h, c = cut;
    const bool   inside = q.x >= 0 && q.y >= 0 && q.x <= W && q.y <= H && q.x + q.y >= c && (W - q.x) + q.y >= c && (W - q.x) + (H - q.y) >= c && q.x + (H - q.y) >= c;
    if (inside)
        return 0;
    const Vec2 shape[8] = {{c, 0}, {W - c, 0}, {W, c}, {W, H - c}, {W - c, H}, {c, H}, {0, H - c}, {0, c}};
    double     best     = kInf;
    for (int i = 0; i < 8; ++i)
        best = std::min(best, distToSegment(q, shape[i], shape[(i + 1) % 8]));
    return best;
}

bool overlap(const IBox& a, const IBox& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The indices of the visible segments whose centre line passes through a point.
std::vector<size_t> segmentsAt(const Stroke& s, Vec2 p) {
    std::vector<size_t> found;
    const size_t        n = s.pts.size(), count = s.closed ? n : (n ? n - 1 : 0);
    for (size_t i = 0; i < count; ++i) {
        const Vertex& a = s.pts[i];
        const Vertex& b = s.pts[(i + 1) % n];
        if (visible(s, a, b) && distToSegment(p, {a.x, a.y}, {b.x, b.y}) < 0.51)
            found.push_back(i);
    }
    return found;
}

// Adds the segments that a piece of line from a to b lies on, found by sampling it.
void addSegments(std::vector<size_t>& into, const Stroke& s, Vec2 a, Vec2 b) {
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
        for (size_t i : segmentsAt(s, {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}))
            into.push_back(i);
}

// How many segments apart two segments are along the stroke.
size_t apart(const Stroke& s, size_t i, size_t j) {
    const size_t d = i > j ? i - j : j - i;
    return s.closed ? std::min(d, s.pts.size() - d) : d;
}

// Every visible segment is horizontal, vertical or at 45 degrees and sits on
// whole pixels: flat and upright lines on whole or half px across them,
// depending on their thickness.
void checkSegments(const Stroke& s, const Params& p) {
    const bool odd = thickness(p) % 2 == 1;
    eachSegment(s, [&](const Vertex& a, const Vertex& b) {
        if (!visible(s, a, b))
            return;
        const double dx = b.x - a.x, dy = b.y - a.y;
        CHECK(std::hypot(dx, dy) >= 1);
        if (std::fabs(dy) < 1e-9) {
            CHECK(odd ? half(a.y) : whole(a.y));
        } else if (std::fabs(dx) < 1e-9) {
            CHECK(odd ? half(a.x) : whole(a.x));
        } else {
            CHECK_NEAR(std::fabs(dx), std::fabs(dy), 1e-9);
            CHECK(whole(dx));
        }
    });
}

void checkLayout(const Layout& lay, const Params& p) {
    const int before = g_failures;

    CHECK(lay.valid);
    CHECK(lay.rings.empty() && lay.fills.empty());
    CHECK(!lay.panel.empty());
    if (lay.panel.empty()) {
        std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
        return;
    }

    const Stroke& outline = lay.panel.strokes[0];
    CHECK(outline.closed);
    CHECK(signedArea(outline.pts) > 0);
    CHECK_NEAR(outline.halfWidth, thickness(p) / 2.0, 1e-9);

    const Extents& e = lay.extents;
    const double   s = p.scale;
    const auto     inside = [&](double x0, double y0, double x1, double y1) {
        return x0 >= -double(e.left) * s - 1e-9 && y0 >= -double(e.top) * s - 1e-9 && x1 <= p.width + double(e.right) * s + 1e-9 && y1 <= p.height + double(e.bottom) * s + 1e-9;
    };
    const auto fits = [&](const IBox& b) { return b.w > 0 && b.h > 0 && inside(b.x, b.y, b.x + b.w, b.y + b.h); };

    for (const Stroke& st : lay.panel.strokes) {
        CHECK_NEAR(st.halfWidth, outline.halfWidth, 1e-9);
        CHECK(st.reach >= 0 && st.haloAlpha >= 0 && st.haloAlpha <= 1 && st.alpha > 0 && st.alpha <= 1);
        checkSegments(st, p);
        CHECK(!st.strips.empty() || !st.tiles.empty());

        // Pieces on the same or on neighbouring segments never overlap, or the
        // joint between them would be drawn twice. Pieces further apart may: the
        // ghost line sits just above the end of the baseline and their glow mixes.
        // All pieces stay inside the extents.
        struct Piece {
            IBox                box;
            std::vector<size_t> segs;
        };
        std::vector<Piece> pieces;
        for (const StripPiece& piece : st.strips) {
            CHECK(fits(piece.box));
            Piece out{piece.box, {}};
            addSegments(out.segs, st, piece.a, piece.b);
            CHECK(!out.segs.empty());
            pieces.push_back(std::move(out));
        }
        for (const TilePiece& tile : st.tiles) {
            CHECK(fits(tile.box));
            CHECK(!tile.segs.empty());
            Piece out{tile.box, {}};
            for (const TileSeg& seg : tile.segs)
                addSegments(out.segs, st, seg.a, seg.b);
            CHECK(!out.segs.empty());
            pieces.push_back(std::move(out));
        }
        for (size_t i = 0; i < pieces.size(); ++i)
            for (size_t j = i + 1; j < pieces.size(); ++j) {
                if (!overlap(pieces[i].box, pieces[j].box))
                    continue;
                bool crowded = false;
                for (size_t a : pieces[i].segs)
                    for (size_t b : pieces[j].segs)
                        crowded = crowded || apart(st, a, b) < 2;
                CHECK(!crowded);
            }
    }

    // The gradient is measured over a box that holds the whole outline.
    const IBox& sb = lay.sampleBox;
    for (const Vertex& v : outline.pts)
        CHECK(v.x >= sb.x && v.y >= sb.y && v.x <= sb.x + sb.w && v.y <= sb.y + sb.h);

    // The centre line is the rectangle the outline runs round.
    double x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
    for (const Vertex& v : outline.pts) {
        x0 = std::min(x0, v.x);
        y0 = std::min(y0, v.y);
        x1 = std::max(x1, v.x);
        y1 = std::max(y1, v.y);
    }
    CHECK_NEAR(lay.centre.x, x0, 1e-9);
    CHECK_NEAR(lay.centre.y, y0, 1e-9);
    CHECK_NEAR(lay.centre.w, x1 - x0, 1e-9);
    CHECK_NEAR(lay.centre.h, y1 - y0, 1e-9);
    CHECK(lay.centre.reachIn >= 0 && lay.centre.reachOut > outline.halfWidth);

    for (const Bead& b : lay.panel.beads) {
        const double r = beadSize(lookOf(b)) / 2.0;
        CHECK(inside(b.pos.x - r, b.pos.y - r, b.pos.x + r, b.pos.y + r));
    }

    // A dot, with the flare over it, stays inside the extents wherever it is on its line.
    for (const Runner& run : lay.panel.runners) {
        CHECK(run.stroke >= 0 && size_t(run.stroke) < lay.panel.strokes.size());
        if (run.stroke < 0 || size_t(run.stroke) >= lay.panel.strokes.size())
            continue;
        const double r = std::max(beadSize(lookOf(run.look)), beadSize(lookOf(lay.panel.flare))) / 2.0;
        eachSegment(lay.panel.strokes[size_t(run.stroke)], [&](const Vertex& a, const Vertex& b) {
            for (const Vertex& v : {a, b})
                CHECK(inside(v.x - r, v.y - r, v.x + r, v.y + r));
        });
    }

    // The characters of a glyph style fall in columns that stay inside the extents, whatever they
    // are doing. A style without characters has no columns.
    const GlyphFace& face = lay.panel.glyphs;
    CHECK(face.on() || lay.panel.lanes.empty());
    if (face.on()) {
        const GlyphSize gs = glyphSize(face);
        CHECK(gs.w > 0 && gs.h > 0 && gs.w % 2 == 0 && gs.h % 2 == 0);
        for (const RainLane& lane : lay.panel.lanes) {
            CHECK(lane.cells >= 3 && lane.streams >= 1);
            const long last = std::lround(lane.y0 + (lane.cells - 1) * face.pitch);
            CHECK(inside(lane.x - gs.w / 2.0, lane.y0 - gs.h / 2.0, lane.x + gs.w / 2.0, double(last) + gs.h / 2.0));
        }
    }

    if (g_failures != before)
        std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
}

void testStyle() {
    const Style* st = styleFor("panel");
    CHECK(st != nullptr);
    if (!st)
        return;
    CHECK(st->kind == Kind::Panel);
    CHECK(st->notchDepth > 0 && st->traceOffset > 0 && st->beadRadius > 0 && st->runnerSpeed > 0);
    CHECK(st->lineAlpha > 0 && st->lineAlpha <= 1 && st->haloAlpha > 0 && st->haloAlpha <= 1);
    CHECK(st->haloReach > 0 && st->floatBoost >= 1);
    CHECK(st->glyphHeight == 0 && st->glyphPitch == 0 && st->glyphFall == 0 && st->lamps == 0);

    // The matrix style is the panel's outline alone, with characters falling down its sides
    // and glows going to and fro along its top and bottom.
    const Style* mx = styleFor("matrix");
    CHECK(mx != nullptr);
    if (!mx)
        return;
    CHECK(mx->kind == Kind::Panel);
    CHECK(mx->notchDepth > 0 && mx->traceOffset == 0 && mx->beadRadius == 0);
    CHECK(mx->lineAlpha > 0 && mx->lineAlpha <= 1 && mx->haloAlpha > 0 && mx->haloAlpha <= 1);
    CHECK(mx->haloReach > 0 && mx->floatBoost >= 1);
    CHECK(mx->glyphHeight > 0 && mx->glyphPitch >= mx->glyphHeight / 2 && mx->glyphFall > 0 && mx->glyphGlow > 0);
    CHECK(mx->lamps > 0 && mx->sweepAlpha > 0); // a lamp is drawn the way the sweep is

    // Only the panel styles are panels, and only the matrix one has characters and lamps; the others stay bands.
    for (const Style& other : allStyles()) {
        const std::string name = other.name;
        CHECK((other.kind == Kind::Panel) == (name == "panel" || name == "matrix"));
        CHECK((other.glyphHeight > 0) == (name == "matrix"));
        CHECK((other.glyphPitch > 0) == (name == "matrix"));
        CHECK((other.glyphFall > 0) == (name == "matrix"));
        CHECK((other.lamps > 0) == (name == "matrix"));
    }
}

void testTooSmall() {
    const Style& st = panelStyle();
    CHECK(buildLayout(st, params(40, 40)).valid);
    CHECK(!buildLayout(st, params(39, 300)).valid);
    CHECK(!buildLayout(st, params(300, 39)).valid);
    CHECK(!buildLayout(st, params(300, 300, 0.f)).valid);
}

void testLayouts() {
    for (const Style* st : {&panelStyle(), &matrixStyle()})
        for (const auto& size : kSizes)
            for (float s : kScales)
                for (bool floating : {false, true})
                    for (double lineWidth : {2.0, 3.0}) {
                        const Params p = params(size[0], size[1], s, floating, lineWidth);
                        checkLayout(buildLayout(*st, p), p);
                    }
}

void testFrameKinds() {
    const Style& st = panelStyle();

    // Too small for the notches: a chamfered frame and nothing else, but two dots.
    for (const auto& size : {std::pair{400, 300}, {1000, 100}, {40, 40}, {440, 600}}) {
        const Layout lay = buildLayout(st, params(size.first, size.second));
        CHECK(lay.valid);
        CHECK(lay.panel.strokes.size() == 1);
        CHECK(lay.panel.strokes[0].pts.size() == 8);
        CHECK(lay.panel.beads.empty());
        CHECK(lay.panel.runners.size() == 2);
        for (const Runner& r : lay.panel.runners)
            CHECK(r.stroke == 0);
    }

    // Big enough: the notched outline and three traces with a bead at each end.
    struct Case {
        int   w, h;
        float scale;
    };
    for (const Case& c : {Case{700, 400, 1.f}, {900, 600, 1.f}, {1920, 1080, 1.f}, {1920, 1080, 1.25f}, {1920, 1080, 1.5f}, {1920, 1080, 2.f}, {3840, 2160, 1.f}, {3840, 2160, 2.f}}) {
        const Params p   = params(c.w, c.h, c.scale);
        const Layout lay = buildLayout(st, p);
        const Panel& pn  = lay.panel;
        CHECK(pn.strokes.size() == 4);
        CHECK(pn.strokes[0].pts.size() > 8);
        const size_t traces = pn.strokes.size() - 1;
        CHECK(pn.beads.size() == 2 * traces);
        CHECK(pn.runners.size() == 2 + traces);
        if (pn.strokes.size() != 4)
            std::fprintf(stderr, "  ^ %s\n", label(p).c_str());

        // The traces are dimmer than the outline.
        for (size_t i = 1; i < pn.strokes.size(); ++i) {
            CHECK(!pn.strokes[i].closed);
            CHECK(pn.strokes[i].alpha < pn.strokes[0].alpha);
            CHECK(pn.strokes[i].reach < pn.strokes[0].reach);
        }
    }
}

// The line never touches the window, and the room the travelling glow is
// allowed inside the line is really free.
void testClearOfWindow() {
    const Style& st = panelStyle();
    for (const auto& size : {std::pair{100, 100}, {240, 180}, {700, 400}, {900, 600}, {1920, 1080}})
        for (float s : kScales)
            for (bool floating : {false, true})
                for (double lineWidth : {2.0, 3.0}) {
                    const Params p   = params(size.first, size.second, s, floating, lineWidth);
                    const Layout lay = buildLayout(st, p);
                    double       nearest = kInf;
                    for (const Stroke& stroke : lay.panel.strokes)
                        eachSegment(stroke, [&](const Vertex& a, const Vertex& b) {
                            if (!visible(stroke, a, b))
                                return;
                            const int n = int(std::ceil(std::hypot(b.x - a.x, b.y - a.y) * 4));
                            for (int i = 0; i <= n; ++i) {
                                const double t = double(i) / n;
                                nearest        = std::min(nearest, distToWindow({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, p.width, p.height, p.cut));
                            }
                        });
                    const double hw = lay.panel.strokes[0].halfWidth;
                    CHECK(nearest >= hw + 0.5);
                    if (lay.centre.reachIn > 0)
                        CHECK(lay.centre.reachIn + hw + 1 <= nearest + 0.2);
                    if (g_failures)
                        std::fprintf(stderr, "  %s: nearest %g, reachIn %g, half width %g\n", label(p).c_str(), nearest, lay.centre.reachIn, hw);
                }
}

void testBeadsAndRunners() {
    const Layout lay = buildLayout(panelStyle(), params(900, 600));
    const Panel& pn  = lay.panel;
    CHECK(pn.strokes.size() > 1);

    // A bead sits at each end of every trace.
    for (const Bead& b : pn.beads) {
        double nearest = kInf;
        for (size_t i = 1; i < pn.strokes.size(); ++i) {
            const auto& pts = pn.strokes[i].pts;
            for (const Vertex& end : {pts.front(), pts.back()})
                nearest = std::min(nearest, std::hypot(b.pos.x - end.x, b.pos.y - end.y));
        }
        CHECK(nearest <= 8);
        CHECK(b.radius >= 1.5);
        CHECK(b.reach > 0 && b.haloAlpha > 0 && b.haloAlpha <= 1 && b.alpha > 0);
    }

    // Two dots on the outline, opposite each other and going opposite ways, and
    // one on each trace.
    std::vector<int> perStroke(pn.strokes.size(), 0);
    for (const Runner& r : pn.runners) {
        CHECK(r.stroke >= 0 && size_t(r.stroke) < pn.strokes.size());
        if (r.stroke >= 0 && size_t(r.stroke) < perStroke.size())
            ++perStroke[size_t(r.stroke)];
        CHECK(r.speed != 0 && r.offset >= 0 && r.offset < 1);
        CHECK(r.look.radius > pn.beads.front().radius); // a travelling dot is a little bigger than a resting one
    }
    CHECK(perStroke[0] == 2);
    for (size_t i = 1; i < perStroke.size(); ++i)
        CHECK(perStroke[i] == 1);
    CHECK_NEAR(pn.runners[0].offset, 0.0, 1e-9);
    CHECK_NEAR(pn.runners[1].offset, 0.5, 1e-9);
    CHECK(pn.runners[0].speed > 0 && pn.runners[1].speed < 0);

    // The dots on the traces start in different places, so they do not all hit the same end together.
    for (size_t i = 2; i < pn.runners.size(); ++i) {
        CHECK(pn.runners[i].speed > 0);
        for (size_t j = i + 1; j < pn.runners.size(); ++j)
            CHECK(std::fabs(pn.runners[i].offset - pn.runners[j].offset) > 0.1);
    }

    // A dot going round swells into a bigger flare with a wider and brighter halo near other dots.
    const Bead& dot = pn.runners[0].look;
    CHECK(pn.flare.radius > dot.radius);
    CHECK(pn.flare.reach > dot.reach);
    CHECK(pn.flare.haloAlpha >= dot.haloAlpha && pn.flare.haloAlpha <= 1);
    CHECK(pn.touch.near > 0 && pn.touch.far > pn.touch.near);
    CHECK(pn.merge.near > 0 && pn.merge.far > pn.merge.near);
    CHECK(pn.merge.far <= 0.35 * strokeLength(pn.strokes[0]) + 1e-9);
}

// How many times a dot goes round the middle of the window over one lap, counted
// clockwise on the screen.
double turns(const Panel& pn, const Runner& r, Vec2 mid, double lap) {
    constexpr int steps = 4000;
    double        sum   = 0;
    Vec2          prev  = runnerPose(pn, r, 0).pos;
    for (int i = 1; i <= steps; ++i) {
        const Vec2   cur = runnerPose(pn, r, lap * i / steps).pos;
        const double ax = prev.x - mid.x, ay = prev.y - mid.y, bx = cur.x - mid.x, by = cur.y - mid.y;
        sum += std::atan2(ax * by - ay * bx, ax * bx + ay * by);
        prev = cur;
    }
    return sum / (2 * std::numbers::pi);
}

double stepOf(const Panel& pn, const Runner& r, double t, double dt = 1.0) {
    const RunnerPose a = runnerPose(pn, r, t), b = runnerPose(pn, r, t + dt);
    return std::hypot(b.pos.x - a.pos.x, b.pos.y - a.pos.y);
}

void testRunnerPose() {
    const Layout lay = buildLayout(panelStyle(), params(900, 600));
    const Panel& pn  = lay.panel;
    CHECK(pn.runners.size() >= 3);
    const Vec2 mid{450, 300};

    int  shown = 0, total = 0;
    bool hidden = false;
    for (const Runner& r : pn.runners) {
        if (r.stroke < 0 || size_t(r.stroke) >= pn.strokes.size())
            continue;
        const Stroke& s   = pn.strokes[size_t(r.stroke)];
        const double  len = strokeLength(s), lap = len / std::fabs(r.speed);
        CHECK(len > 0);

        for (double t = 0; t < 3 * lap; t += 5.7) {
            const RunnerPose pose = runnerPose(pn, r, t);
            CHECK(pose.alpha >= 0 && pose.alpha <= 1);
            CHECK(distToStroke(s, pose.pos) < 1e-6);
            CHECK(pose.glow == 0); // that needs the other dots; see runnerPoses
            if (r.stroke == 0) {
                ++total;
                if (pose.alpha > 0)
                    ++shown;
                else
                    hidden = true;
            }
        }

        if (s.closed) {
            // After a lap it is where it was, having gone once round the window, the way its speed says.
            const RunnerPose a = runnerPose(pn, r, 37.0), b = runnerPose(pn, r, 37.0 + lap);
            CHECK_NEAR(a.pos.x, b.pos.x, 1e-6);
            CHECK_NEAR(a.pos.y, b.pos.y, 1e-6);
            CHECK_NEAR(a.alpha, b.alpha, 1e-6);
            CHECK_NEAR(turns(pn, r, mid, lap), r.speed > 0 ? 1.0 : -1.0, 1e-3);

            // It moves along the line at its own speed.
            for (double t = 0; t < lap; t += 11.3) {
                const double step = stepOf(pn, r, t);
                CHECK(step > 0 && step <= std::fabs(r.speed) + 1e-6);
            }
            continue;
        }

        // On an open line it goes back and forth, never fading, and is back where it was after there and back.
        const RunnerPose a = runnerPose(pn, r, 37.0), b = runnerPose(pn, r, 37.0 + 2 * lap);
        CHECK_NEAR(a.pos.x, b.pos.x, 1e-6);
        CHECK_NEAR(a.pos.y, b.pos.y, 1e-6);
        for (double t = 0; t < 4 * lap; t += 7.9)
            CHECK_NEAR(runnerPose(pn, r, t).alpha, 1, 1e-6);

        // It reaches both ends and turns round there.
        const Vertex &first = s.pts.front(), &last = s.pts.back();
        const double  toEnd = (1.0 - r.offset) * lap;
        CHECK_NEAR(runnerPose(pn, r, toEnd).pos.x, last.x, 1e-6);
        CHECK_NEAR(runnerPose(pn, r, toEnd).pos.y, last.y, 1e-6);
        CHECK_NEAR(runnerPose(pn, r, toEnd + lap).pos.x, first.x, 1e-6);
        CHECK_NEAR(runnerPose(pn, r, toEnd + lap).pos.y, first.y, 1e-6);
        for (double d : {3.0, 17.0, 40.0}) {
            CHECK_NEAR(runnerPose(pn, r, toEnd - d).pos.x, runnerPose(pn, r, toEnd + d).pos.x, 1e-6);
            CHECK_NEAR(runnerPose(pn, r, toEnd - d).pos.y, runnerPose(pn, r, toEnd + d).pos.y, 1e-6);
        }

        // It never runs faster than the ease allows.
        for (double t = 0; t < 2 * lap; t += 3.1)
            CHECK(stepOf(pn, r, t) <= (1 + kBounceEase) * r.speed + 1e-6);
    }
    CHECK(hidden); // on the outline the dots vanish where the line does
    CHECK(shown > total / 2);

    // It slows down towards the ends: on the straight trace in the notch it covers
    // less in a step up to the turn than in one half way along.
    const Runner& notch = pn.runners[2];
    const double  lap   = strokeLength(pn.strokes[size_t(notch.stroke)]) / notch.speed;
    const double  turn  = (1.0 - notch.offset) * lap;
    CHECK(stepOf(pn, notch, turn - 1.0) < 0.6 * stepOf(pn, notch, turn - lap / 2 - 0.5));

    // On a plain frame nothing is invisible, and the two dots still go opposite ways.
    const Layout plain = buildLayout(panelStyle(), params(400, 300));
    CHECK(plain.panel.runners.size() == 2);
    for (const Runner& r : plain.panel.runners) {
        for (double t = 0; t < 2000; t += 13)
            CHECK_NEAR(runnerPose(plain.panel, r, t).alpha, 1, 1e-6);
        CHECK_NEAR(turns(plain.panel, r, {200, 150}, strokeLength(plain.panel.strokes[0]) / std::fabs(r.speed)), r.speed > 0 ? 1.0 : -1.0, 1e-3);
    }

    // Nothing for a runner that points nowhere.
    Runner lost = pn.runners.front();
    lost.stroke = 99;
    CHECK(runnerPose(pn, lost, 5).alpha == 0);
    lost.stroke = -1;
    CHECK(runnerPose(pn, lost, 5).alpha == 0);

    Panel withLost = pn;
    withLost.runners.push_back(lost);
    const auto poses = runnerPoses(withLost, 5);
    CHECK(poses.size() == withLost.runners.size());
    if (poses.size() == withLost.runners.size()) {
        CHECK(poses.back().alpha == 0 && poses.back().glow == 0);
        for (size_t i = 0; i + 1 < poses.size(); ++i)
            CHECK(poses[i].alpha == runnerPose(pn, pn.runners[i], 5).alpha);
    }
}

Stroke lineOf(std::vector<Vertex> pts, bool closed) {
    Stroke s;
    s.pts    = std::move(pts);
    s.closed = closed;
    return s;
}

Runner runner(int stroke, double offset, double speed) {
    Runner r;
    r.stroke = stroke;
    r.offset = offset;
    r.speed  = speed;
    return r;
}

// A square outline 400 px a side, 1600 px round, clockwise from its top left corner.
Panel square() {
    Panel pn;
    pn.strokes.push_back(lineOf({{0, 0, 1.f}, {400, 0, 1.f}, {400, 400, 1.f}, {0, 400, 1.f}}, true));
    pn.flare.radius = 4;
    pn.touch        = {14, 90};
    pn.merge        = {20, 200};
    return pn;
}

// The glow is a float, so it is only as exact as that.
constexpr double kGlowTol = 1e-6;

// What the proximity ranges make of a distance.
double lit(double dist, const Proximity& range) {
    const double t = std::clamp((range.far - dist) / (range.far - range.near), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// A dot going round lights up when a dot on a trace is near it, the more the nearer it is.
void testGlowNearTraceDots() {
    Panel pn = square();
    pn.strokes.push_back(lineOf({{50, -10, 1.f}, {350, -10, 1.f}}, false));
    // The dot going round clockwise starts at the top left corner, the one going the other way
    // at the bottom right; the third stands still half way along the trace: at (200, -10).
    pn.runners = {runner(0, 0.0, 1.0), runner(0, 0.5, -1.0), runner(1, 0.5, 0.0)};

    // Right under it, 10 px away.
    auto poses = runnerPoses(pn, 200);
    CHECK(poses.size() == 3);
    CHECK_NEAR(poses[0].glow, 1, kGlowTol);
    CHECK_NEAR(poses[1].glow, 0, kGlowTol); // the other one is far from everything
    CHECK_NEAR(poses[2].glow, 0, kGlowTol); // dots on traces do not glow
    CHECK_NEAR(poses[2].alpha, 1, 1e-9);

    // Further off the glow is lower, and exactly half the way from 14 px to 90 px.
    for (double dist : {20.0, 40.0, 52.0, 70.0, 89.0}) {
        const double dx = std::sqrt(dist * dist - 100);
        poses           = runnerPoses(pn, 200 + dx);
        CHECK_NEAR(poses[0].glow, lit(dist, pn.touch), kGlowTol);
        CHECK_NEAR(poses[1].glow, 0, kGlowTol);
    }
    CHECK_NEAR(lit(52, pn.touch), 0.5, 1e-9);

    // Beyond 90 px there is none.
    CHECK_NEAR(runnerPoses(pn, 0)[0].glow, 0, kGlowTol);
    CHECK_NEAR(runnerPoses(pn, 300)[0].glow, 0, kGlowTol);

    // It rises as the dot comes nearer and falls as it goes away again.
    double last = -1;
    bool   rose = true, fell = true;
    for (double t = 100; t <= 200; t += 2.5) {
        const double g = runnerPoses(pn, t)[0].glow;
        rose           = rose && g >= last - 1e-12;
        last           = g;
    }
    for (double t = 200; t <= 300; t += 2.5) {
        const double g = runnerPoses(pn, t)[0].glow;
        fell           = fell && g <= last + 1e-12;
        last           = g;
    }
    CHECK(rose && fell);

    // Without a flare nothing glows.
    Panel off = pn;
    off.flare.radius = 0;
    for (const RunnerPose& p : runnerPoses(off, 200))
        CHECK(p.glow == 0);

    // A dot that cannot be seen lights nothing up, one that half shows lights it half.
    pn.strokes[1].pts = {{50, -10, 0.5f}, {350, -10, 0.5f}};
    CHECK_NEAR(runnerPoses(pn, 200)[0].glow, 0.5, kGlowTol);
    pn.strokes[1].pts = {{50, -10, 0.f}, {350, -10, 0.f}};
    CHECK_NEAR(runnerPoses(pn, 200)[0].glow, 0, kGlowTol);

    // Two dots near it add up: two halves make three quarters.
    pn.strokes[1].pts = {{50, -52, 1.f}, {350, -52, 1.f}};
    pn.strokes.push_back(lineOf({{50, 52, 1.f}, {350, 52, 1.f}}, false));
    pn.runners.push_back(runner(2, 0.5, 0.0));
    poses = runnerPoses(pn, 200);
    CHECK(poses.size() == 4);
    CHECK_NEAR(poses[0].glow, 0.75, kGlowTol);
}

// The two dots going round light each other up as they near, measured along the line.
void testGlowBetweenCirclers() {
    Panel pn   = square();
    pn.runners = {runner(0, 0.0, 1.0), runner(0, 0.5, -1.0)};

    // They meet after 400 px, 800 px from where one started and where the other did.
    auto poses = runnerPoses(pn, 400);
    CHECK_NEAR(poses[0].glow, 1, kGlowTol);
    CHECK_NEAR(poses[1].glow, 1, kGlowTol);

    // 200 px apart along the line is as far as they light each other, 20 px as near.
    CHECK_NEAR(runnerPoses(pn, 300)[0].glow, 0, kGlowTol);
    CHECK_NEAR(runnerPoses(pn, 390)[0].glow, 1, kGlowTol);
    for (double t : {310.0, 330.0, 350.0, 372.0}) {
        poses = runnerPoses(pn, t);
        CHECK_NEAR(poses[0].glow, lit(800 - 2 * t, pn.merge), kGlowTol);
        CHECK_NEAR(poses[0].glow, poses[1].glow, kGlowTol);
        CHECK(poses[0].glow > 0 && poses[0].glow < 1);
    }

    // They glow after they have passed each other as well, and again when they meet on the far side.
    CHECK_NEAR(runnerPoses(pn, 430)[0].glow, lit(60, pn.merge), kGlowTol);
    CHECK_NEAR(runnerPoses(pn, 1200)[0].glow, 1, kGlowTol);
    CHECK_NEAR(runnerPoses(pn, 800)[0].glow, 0, kGlowTol);

    // The distance is the short way round, across the end of the line.
    pn.runners = {runner(0, 0.0, 0.0), runner(0, 1590.0 / 1600, 0.0)};
    poses      = runnerPoses(pn, 0);
    CHECK_NEAR(poses[0].glow, 1, kGlowTol);
    CHECK_NEAR(poses[1].glow, 1, kGlowTol);
    pn.runners = {runner(0, 0.0, 0.0), runner(0, 120.0 / 1600, 0.0)};
    poses      = runnerPoses(pn, 0);
    CHECK_NEAR(poses[0].glow, lit(120, pn.merge), kGlowTol);
    pn.runners = {runner(0, 0.0, 0.0), runner(0, 1480.0 / 1600, 0.0)};
    CHECK_NEAR(runnerPoses(pn, 0)[0].glow, lit(120, pn.merge), kGlowTol);

    // One that cannot be seen does not draw the other on: the second dot sits 5 px on, where the line
    // is gone. With the line there it would light the first up all the way.
    pn.strokes[0].pts = {{0, 0, 1.f}, {200, 0, 1.f}, {205, 0, 0.f}, {400, 0, 0.f}, {400, 400, 1.f}, {0, 400, 1.f}};
    pn.runners        = {runner(0, 200.0 / 1600, 0.0), runner(0, 205.0 / 1600, 0.0)};
    poses             = runnerPoses(pn, 0);
    CHECK_NEAR(poses[1].alpha, 0, 1e-9);
    CHECK_NEAR(poses[0].glow, 0, kGlowTol);
    pn.strokes[0].pts[2].alpha = 1.f;
    CHECK_NEAR(runnerPoses(pn, 0)[0].glow, 1, kGlowTol);
}

// On the real layouts: the glow stays in range, belongs to the dots going round and
// does what it should near the dots on the traces.
void testGlowOnPanels() {
    struct Case {
        int   w, h;
        float scale;
        bool  floating;
    };
    for (const Case& c : {Case{900, 600, 1.f, false}, {1100, 640, 1.f, false}, {1920, 1080, 1.f, true}, {1920, 1080, 1.5f, false}, {3840, 2160, 2.f, false}, {400, 300, 1.f, false}}) {
        const int    before = g_failures;
        const Params p      = params(c.w, c.h, c.scale, c.floating);
        const Layout lay    = buildLayout(panelStyle(), p);
        const Panel& pn     = lay.panel;
        const double lap    = strokeLength(pn.strokes[0]) / std::fabs(pn.runners[1].speed);

        double maxGlow = 0, minGlow = 1;
        int    nearTrace = 0;
        for (double t = 0; t < 40 * lap; t += 2.9) {
            const auto poses = runnerPoses(pn, t);
            CHECK(poses.size() == pn.runners.size());
            if (poses.size() != pn.runners.size())
                return;

            for (size_t i = 0; i < poses.size(); ++i) {
                CHECK(poses[i].glow >= 0 && poses[i].glow <= 1);
                CHECK_NEAR(poses[i].alpha, runnerPose(pn, pn.runners[i], t).alpha, 1e-6);
                if (pn.runners[i].stroke != 0) {
                    CHECK(poses[i].glow == 0);
                    continue;
                }
                if (poses[i].alpha <= 0)
                    continue;
                maxGlow = std::max(maxGlow, double(poses[i].glow));
                minGlow = std::min(minGlow, double(poses[i].glow));

                // Within reach of the dot on a trace it is lit all the way.
                for (size_t j = 0; j < poses.size(); ++j) {
                    if (pn.runners[j].stroke == 0 || poses[j].alpha < 1)
                        continue;
                    if (std::hypot(poses[i].pos.x - poses[j].pos.x, poses[i].pos.y - poses[j].pos.y) <= pn.touch.near) {
                        ++nearTrace;
                        CHECK_NEAR(poses[i].glow, 1, 1e-6);
                    }
                }
            }
        }
        CHECK(maxGlow > 0.99 && minGlow < 0.01); // it comes and goes
        if (pn.strokes.size() > 1)
            CHECK(nearTrace > 0);
        if (g_failures != before)
            std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
    }

    // On a plain frame there are no traces, and the two dots light each other up in turn.
    const Layout plain = buildLayout(panelStyle(), params(400, 300));
    const double lap   = strokeLength(plain.panel.strokes[0]) / std::fabs(plain.panel.runners[1].speed);
    for (double t = 0; t < 4 * lap; t += 3.3) {
        const auto poses = runnerPoses(plain.panel, t);
        CHECK(poses.size() == 2);
        if (poses.size() == 2)
            CHECK_NEAR(poses[0].glow, poses[1].glow, 1e-6);
    }
}

void testKnobs() {
    const Params p = params(900, 600);

    Style st = panelStyle();
    st.beadRadius = 0;
    Layout lay    = buildLayout(st, p);
    CHECK(lay.valid && lay.panel.beads.empty() && lay.panel.runners.empty());
    CHECK(lay.panel.flare.radius == 0 && runnerPoses(lay.panel, 10).empty());
    CHECK(lay.panel.strokes.size() == 4); // the traces stay

    st             = panelStyle();
    st.traceOffset = 0;
    lay            = buildLayout(st, p);
    CHECK(lay.valid && lay.panel.strokes.size() == 1);
    CHECK(lay.panel.strokes[0].pts.size() > 8); // still notched
    CHECK(lay.panel.beads.empty() && lay.panel.runners.size() == 2);

    st            = panelStyle();
    st.notchDepth = 0;
    lay           = buildLayout(st, p);
    CHECK(lay.valid && lay.panel.strokes.size() == 1 && lay.panel.strokes[0].pts.size() == 8);

    // A thicker line makes a thicker stroke.
    CHECK_NEAR(buildLayout(panelStyle(), params(900, 600, 1.f, false, 4)).panel.strokes[0].halfWidth, 2.0, 1e-9);
    CHECK_NEAR(buildLayout(panelStyle(), params(900, 600, 2.f, false, 2)).panel.strokes[0].halfWidth, 2.0, 1e-9);
}

void testFloating() {
    const Style& st = panelStyle();
    const Layout docked = buildLayout(st, params(900, 600)), floating = buildLayout(st, params(900, 600, 1.f, true));
    CHECK(floating.panel.strokes[0].reach > docked.panel.strokes[0].reach);
    CHECK(floating.panel.strokes[0].haloAlpha >= docked.panel.strokes[0].haloAlpha);
    CHECK(floating.extents.left >= docked.extents.left && floating.extents.right >= docked.extents.right);
    CHECK(floating.centre.reachOut > docked.centre.reachOut);
}

bool same(const Stroke& a, const Stroke& b) {
    if (a.pts.size() != b.pts.size() || a.strips.size() != b.strips.size() || a.tiles.size() != b.tiles.size())
        return false;
    for (size_t i = 0; i < a.pts.size(); ++i)
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y || a.pts[i].alpha != b.pts[i].alpha)
            return false;
    for (size_t i = 0; i < a.strips.size(); ++i)
        if (!(a.strips[i].box == b.strips[i].box) || a.strips[i].alphaA != b.strips[i].alphaA || a.strips[i].alphaB != b.strips[i].alphaB)
            return false;
    for (size_t i = 0; i < a.tiles.size(); ++i)
        if (!(a.tiles[i].box == b.tiles[i].box) || a.tiles[i].segs.size() != b.tiles[i].segs.size())
            return false;
    return true;
}

// The same window always gives the same pieces, so the baked images are reused.
void testDeterministic() {
    const Params p = params(1280, 720, 1.25f);
    const Layout a = buildLayout(panelStyle(), p), b = buildLayout(panelStyle(), p);
    CHECK(a.panel.strokes.size() == b.panel.strokes.size());
    for (size_t i = 0; i < std::min(a.panel.strokes.size(), b.panel.strokes.size()); ++i)
        CHECK(same(a.panel.strokes[i], b.panel.strokes[i]));
    CHECK(a.extents == b.extents);
}

void testExtents() {
    for (const Style* st : {&panelStyle(), &matrixStyle()})
        for (float s : kScales)
            for (bool floating : {false, true}) {
                const Extents e = extentsFor(*st, params(1000, 700, s, floating));
                CHECK(e.left > 0 && e.top > 0 && e.right > 0 && e.bottom > 0);
                CHECK(e.right >= e.left); // the traces run out on the right
                for (const auto& size : kSizes)
                    CHECK(buildLayout(*st, params(size[0], size[1], s, floating)).extents == e);
            }
}

// ---------------------------------------------------------------------------
// The characters of a glyph style.

void testGlyphFace() {
    const Layout plain = buildLayout(panelStyle(), params(900, 600));
    CHECK(!plain.panel.glyphs.on() && plain.panel.lanes.empty() && plain.panel.glyphs.knock == 0);
    CHECK(glyphDraws(plain.panel, 50.0, 3.0, 1.0, 1).empty());

    const Style& st = matrixStyle();
    for (float s : kScales) {
        const GlyphFace docked = buildLayout(st, params(900, 600, s)).panel.glyphs;
        const GlyphFace afloat = buildLayout(st, params(900, 600, s, true)).panel.glyphs;
        CHECK(docked.on());
        CHECK_NEAR(docked.height, std::max(6.0, std::round(st.glyphHeight * double(s))), 1e-9);
        CHECK_NEAR(docked.pitch, std::max(docked.height / 2, std::round(st.glyphPitch * double(s))), 1e-9);
        CHECK_NEAR(docked.fall, st.glyphFall, 1e-9);
        CHECK(docked.halfWidth > 0 && 4 * docked.halfWidth < docked.height && docked.reach > 0);
        CHECK(docked.haloAlpha > 0 && docked.haloAlpha <= 1);
        CHECK_NEAR(docked.knock, st.glyphKnock, 1e-9);

        // The face belongs to the style and the scale, not to the window; floating makes the glow stronger.
        CHECK(afloat.height == docked.height && afloat.pitch == docked.pitch && afloat.halfWidth == docked.halfWidth);
        CHECK(afloat.haloAlpha >= docked.haloAlpha && afloat.haloAlpha <= 1);
        CHECK(afloat.knock == docked.knock);
        for (const auto& size : kSizes) {
            const GlyphFace other = buildLayout(st, params(size[0], size[1], s)).panel.glyphs;
            CHECK(other.height == docked.height && other.pitch == docked.pitch && other.reach == docked.reach && other.haloAlpha == docked.haloAlpha);
            CHECK(other.knock == docked.knock);
        }
    }
    CHECK(buildLayout(st, params(900, 600, 2.f)).panel.glyphs.height > buildLayout(st, params(900, 600, 1.f)).panel.glyphs.height);
}

// The characters fall in columns down both sides of the window: one in the gap between the line and the
// window, if it fits there, and one on the other side of the line, as far from it and half a pitch lower.
// All of them are in the line's glow, off the line and the window, and beside one straight run of the line.
void testLanes() {
    const Style& st = matrixStyle();
    for (const auto& size : kSizes)
        for (float s : kScales)
            for (bool floating : {false, true})
                for (double lineWidth : {2.0, 3.0})
                    for (double gap : {6.0, 15.0}) {
                        const int        before = g_failures;
                        Params           p      = params(size[0], size[1], s, floating, lineWidth);
                        p.gap                   = gap;
                        const Layout     lay    = buildLayout(st, p);
                        const Panel&     pn     = lay.panel;
                        const GlyphFace& face   = pn.glyphs;
                        const Stroke&    line   = pn.strokes[0];
                        const double     halfX  = face.height * kGlyphW / kGlyphH / 2 + face.halfWidth, halfY = face.height / 2 + face.halfWidth;
                        const auto       lineAt = [&](const RainLane& lane) { return lane.x < p.width / 2 ? lay.centre.x : lay.centre.x + lay.centre.w; };
                        CHECK(pn.lanes.size() <= 4);

                        for (size_t i = 0; i < pn.lanes.size(); ++i) {
                            const RainLane& lane   = pn.lanes[i];
                            const bool      right  = lane.x >= p.width / 2;
                            const double    lineX  = lineAt(lane);
                            const double    off    = std::fabs(lane.x - lineX);
                            const long      bottom = std::lround(lane.y0 + (lane.cells - 1) * face.pitch);
                            CHECK(lane.cells >= 3 && lane.streams >= 1 && lane.streams <= 3);
                            CHECK(lane.cells > 20 || lane.streams == 1); // a short column has one fall
                            CHECK(lane.cells < 50 || lane.streams == 3); // a long one has the most

                            // Beside the line, not on it, and in its glow.
                            CHECK(off - line.halfWidth - halfX >= 0.5 - 1e-9);
                            CHECK(off + halfX <= line.halfWidth + line.reach + 1);

                            // The rows lie along one straight run of the line, with half a pitch to spare at both ends.
                            for (double y = lane.y0 - face.pitch / 2; y <= double(bottom) + face.pitch / 2; ++y)
                                CHECK(distToStroke(line, {lineX, y}) < 1e-6);

                            // And off the window.
                            for (int row = 0; row < lane.cells; ++row) {
                                const double y = double(std::lround(lane.y0 + row * face.pitch));
                                for (double fx : {-1.0, 1.0})
                                    for (double fy : {-1.0, 1.0})
                                        CHECK(distToWindow({lane.x + fx * halfX, y + fy * halfY}, p.width, p.height, p.cut) >= 1.5 - 1e-9);
                            }

                            // The left side's columns come first; of two on one side the one in the gap does.
                            if (i == 0)
                                continue;
                            const RainLane& prev = pn.lanes[i - 1];
                            CHECK(prev.x < p.width / 2 || right);
                            if ((prev.x >= p.width / 2) == right) {
                                CHECK(right ? (prev.x > p.width && prev.x < lineX && lane.x > lineX) : (prev.x < 0 && prev.x > lineX && lane.x < lineX));
                                CHECK(off >= std::fabs(prev.x - lineX));
                                CHECK(lane.y0 == prev.y0 + int(std::floor(face.pitch / 2)));
                                CHECK(lane.cells <= prev.cells && lane.cells + 1 >= prev.cells);
                            }
                        }
                        if (g_failures != before)
                            std::fprintf(stderr, "  ^ %s, gap %g\n", label(p).c_str(), gap);
                    }

    // Where there is room there are both columns on both sides; where the gap is too narrow for characters, only the outer ones.
    for (const auto& size : {std::pair{400, 300}, {900, 600}, {1920, 1080}, {3840, 2160}})
        for (float s : kScales) {
            const Layout roomy = buildLayout(st, params(size.first, size.second, s));
            CHECK(roomy.panel.lanes.size() == 4);
            Params narrow = params(size.first, size.second, s);
            narrow.gap    = 6;
            CHECK(buildLayout(st, narrow).panel.lanes.size() == 2);
        }

    // A window too short for three characters between the corners has no columns.
    CHECK(buildLayout(st, params(300, 41)).panel.lanes.empty());

    // Longer columns have more falls.
    const Layout tall = buildLayout(st, params(900, 2000)), low = buildLayout(st, params(900, 200));
    CHECK(!tall.panel.lanes.empty() && !low.panel.lanes.empty());
    if (!tall.panel.lanes.empty() && !low.panel.lanes.empty()) {
        CHECK(tall.panel.lanes[0].cells > low.panel.lanes[0].cells);
        CHECK(tall.panel.lanes[0].streams >= low.panel.lanes[0].streams);
    }
}

// The matrix style draws the panel style's outline and nothing else of its frame: the traces, beads and
// dots belong to the Amber look.
void testMatrixFrame() {
    for (const auto& size : kSizes)
        for (float s : kScales)
            for (bool floating : {false, true}) {
                const int    before = g_failures;
                const Params p      = params(size[0], size[1], s, floating);
                const Layout a = buildLayout(panelStyle(), p), b = buildLayout(matrixStyle(), p);
                CHECK(a.valid && b.valid);
                CHECK(!a.panel.strokes.empty() && b.panel.strokes.size() == 1);
                CHECK(b.panel.beads.empty() && b.panel.runners.empty());
                CHECK(b.panel.flare.radius == 0 && b.panel.touch.far == 0 && b.panel.merge.far == 0);
                if (!a.panel.strokes.empty() && !b.panel.strokes.empty()) {
                    const auto& pa = a.panel.strokes[0].pts;
                    const auto& pb = b.panel.strokes[0].pts;
                    CHECK(pa.size() == pb.size());
                    for (size_t i = 0; i < std::min(pa.size(), pb.size()); ++i)
                        CHECK(pa[i].x == pb[i].x && pa[i].y == pb[i].y && pa[i].alpha == pb[i].alpha);
                }
                CHECK(a.sampleBox == b.sampleBox);
                CHECK(b.extents.left == b.extents.right);

                // The panel style has no characters and no lamps.
                CHECK(!a.panel.glyphs.on() && a.panel.lanes.empty() && a.panel.lamps.empty() && a.panel.track.empty());
                if (g_failures != before)
                    std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
            }
}

// The lamps go along the top and the bottom edge, as many as the edge has room for, and never down a side.
void testLamps() {
    for (const auto& size : kSizes)
        for (float s : kScales)
            for (bool floating : {false, true}) {
                const int    before = g_failures;
                const Params p      = params(size[0], size[1], s, floating);

                const Layout amber = buildLayout(panelStyle(), p);
                CHECK(amber.panel.lamps.empty() && amber.panel.track.empty());

                const Layout lay   = buildLayout(matrixStyle(), p);
                const auto&  track = lay.panel.track;
                const auto&  lamps = lay.panel.lamps;
                CHECK(track.size() == 8 || track.size() == 21); // the plain outline or the notched one
                const bool   fancy = track.size() == 21;
                const double left = lay.centre.x, right = lay.centre.x + lay.centre.w;

                std::vector<double> at(track.size(), 0.0); // px along the track at each vertex
                for (size_t i = 1; i < at.size(); ++i)
                    at[i] = at[i - 1] + std::hypot(track[i].x - track[i - 1].x, track[i].y - track[i - 1].y);

                // The sides are dark. Nothing else is, but the gap in the bottom edge of the notched outline.
                size_t dark = 0;
                for (const Vertex& v : track) {
                    const bool side = v.x == left || v.x == right;
                    CHECK(v.alpha >= 0.f && v.alpha <= 1.f && (!side || v.alpha == 0.f));
                    dark += !side && v.alpha == 0.f;
                }
                CHECK(dark == (fancy ? 2u : 0u));

                // From the first to the last vertex of the stretch that is the top, and of the one that is the bottom.
                const size_t edges[2][2] = {{0, fancy ? 7u : 1u}, {fancy ? 10u : 4u, fancy ? 18u : 5u}};
                const double longest     = p.sweepLength * double(s);
                size_t       k           = 0;
                for (const auto& edge : edges) {
                    const double span = at[edge[1]] - at[edge[0]];
                    for (size_t v = edge[0]; v <= edge[1] && v < track.size(); ++v)
                        CHECK(track[v].x != left && track[v].x != right);

                    size_t n = 0;
                    while (k + n < lamps.size() && std::fabs(lamps[k + n].from - at[edge[0]]) < 1e-9 && std::fabs(lamps[k + n].to - at[edge[1]]) < 1e-9)
                        ++n;

                    // An edge shorter than a glow has none and a long one has two, going opposite ways.
                    CHECK(n <= 2 && (n == 0) == (span < longest));
                    CHECK(span < 4 * longest || n == 2);
                    CHECK(span >= 2 * longest || n <= 1); // two lamps would crowd it
                    for (size_t i = 0; i < n; ++i) {
                        const Lamp& lamp = lamps[k + i];
                        CHECK(lamp.length > 0 && lamp.period > 0 && lamp.step >= 1 && lamp.step <= 8 * double(s));
                        CHECK(lamp.phase >= 0 && lamp.phase < 1 && lamp.offset >= 0 && lamp.offset < 1);
                        CHECK(lamp.speed != 0 && (lamp.speed > 0) == (i == 0));
                    }
                    k += n;
                }
                CHECK(k == lamps.size());
                if (g_failures != before)
                    std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
            }
}

// A lamp goes to and fro between the ends of its stretch, a step at a time, and pulses. Where it is
// depends on how far the lamps have travelled and how strongly it glows only on the time.
void testLampPose() {
    const Layout lay = buildLayout(matrixStyle(), params(1920, 1080));
    const Panel& pn  = lay.panel;
    CHECK(pn.lamps.size() == 4);

    // A lamp that is not there is a plain one.
    CHECK(LampPose{}.at == 0 && LampPose{}.length == 1.f && LampPose{}.strength == 1.f);
    CHECK(lampPose(pn, pn.lamps.size(), 5, 5, 1) == LampPose{} && lampPose(Panel{}, 0, 5, 5, 1) == LampPose{});

    for (size_t i = 0; i < pn.lamps.size(); ++i) {
        const Lamp&  lamp  = pn.lamps[i];
        const double span  = lamp.to - lamp.from;
        const double cycle = 2 * span / std::fabs(lamp.speed); // the px it takes to go there and back

        // On its way it stays on its stretch, on a multiple of the step, and gets to both ends.
        double lo = kInf, hi = -kInf;
        for (int j = 0; j <= 20000; ++j) {
            const double   travelled = 1.3 * cycle * j / 20000;
            const LampPose pose      = lampPose(pn, i, travelled, 0.0, 7);
            CHECK(pose == lampPose(pn, i, travelled, 0.0, 7));
            CHECK(pose.at >= lamp.from - 1e-9 && pose.at <= lamp.to + 1e-9);
            const double steps = (pose.at - lamp.from) / lamp.step;
            CHECK_NEAR(steps, std::round(steps), 1e-6);
            lo = std::min(lo, pose.at);
            hi = std::max(hi, pose.at);
        }
        CHECK(lo - lamp.from <= lamp.step + 1e-9 && lamp.to - hi <= lamp.step + 1e-9);
        for (double travelled : {-1.0, -250.0, -9999.5}) {
            const double at = lampPose(pn, i, travelled, 0.0, 7).at;
            CHECK(at >= lamp.from - 1e-9 && at <= lamp.to + 1e-9);
        }

        // Where it is does not depend on the time, how it glows not on how far it has gone.
        for (double travelled : {0.0, 123.0, 4567.0})
            for (double clock : {0.5, 3.0, 17.3})
                CHECK(lampPose(pn, i, travelled, clock, 7).at == lampPose(pn, i, travelled, 0.0, 7).at);
        for (double clock : {0.0, 0.9, 4.4})
            for (double travelled : {10.0, 999.0}) {
                const LampPose a = lampPose(pn, i, 0.0, clock, 7), b = lampPose(pn, i, travelled, clock, 7);
                CHECK(a.strength == b.strength && a.length == b.length);
            }

        // It pulses between dim and full, slowly and in a few steps, and swells a little with the pulse.
        std::set<float> strengths, lengths;
        float           dimmest = 1.f, brightest = 0.f, before = -1.f;
        double          fastest = 0;
        for (double clock = 0; clock < 60; clock += 0.01) {
            const LampPose pose = lampPose(pn, i, 0.0, clock, 7);
            CHECK(pose.strength > 0.f && pose.strength <= 1.f);
            CHECK(pose.length >= lamp.length * 0.85 && pose.length <= lamp.length * 1.15);
            strengths.insert(pose.strength);
            lengths.insert(pose.length);
            dimmest   = std::min(dimmest, pose.strength);
            brightest = std::max(brightest, pose.strength);
            if (before >= 0.f)
                fastest = std::max(fastest, double(std::fabs(pose.strength - before)));
            before = pose.strength;
        }
        CHECK(dimmest <= 0.5f && brightest >= 0.99f);
        CHECK(strengths.size() >= 6 && strengths.size() <= 13);
        CHECK(lengths.size() >= 3 && lengths.size() <= 7);
        CHECK(fastest <= 0.1);
    }

    // Every window and every lamp starts somewhere else.
    bool apart = false;
    for (size_t i = 0; i < pn.lamps.size(); ++i)
        apart = apart || lampPose(pn, i, 400, 2.0, 1) != lampPose(pn, i, 400, 2.0, 2);
    CHECK(apart);
    CHECK(lampPose(pn, 0, 400, 2.0, 1) != lampPose(pn, 1, 400, 2.0, 1));
    CHECK(lampPose(pn, 0, 100, -5.0, 1) == lampPose(pn, 0, 100, 0.0, 1)); // time does not run backwards
}

using Drawn = std::map<int, GlyphDraw>;

Drawn drawn(const std::vector<GlyphDraw>& draws) {
    Drawn out;
    for (const GlyphDraw& d : draws)
        out[d.id] = d;
    return out;
}

// Every character of every column of a panel, in the order of their ids.
struct RainCell {
    int    x, y;
    size_t lane;
};

std::vector<RainCell> cellsOf(const Panel& pn) {
    std::vector<RainCell> out;
    for (size_t l = 0; l < pn.lanes.size(); ++l)
        for (int row = 0; row < pn.lanes[l].cells; ++row)
            out.push_back({pn.lanes[l].x, int(std::lround(pn.lanes[l].y0 + row * pn.glyphs.pitch)), l});
    return out;
}

// No fall has a tail of fewer rows than this, unless its column is too short for it, or of more than this many.
constexpr int kShortestTail = 8;
constexpr int kLongestTail  = 28;

// On the real panels: what is drawn is well formed, deterministic, only a part of every column, and
// every character of every column is lit by some fall.
void testRainDraws() {
    struct Case {
        int   w, h;
        float scale;
        bool  floating;
    };
    for (const Case& c : {Case{400, 300, 1.f, false}, {900, 600, 1.f, false}, {1920, 1080, 1.f, true}, {1920, 1080, 1.5f, false}, {3840, 2160, 2.f, false}}) {
        const int    before = g_failures;
        const Params p      = params(c.w, c.h, c.scale, c.floating);
        const Layout lay    = buildLayout(matrixStyle(), p);
        const Panel& pn     = lay.panel;
        const auto   cells  = cellsOf(pn);
        CHECK(pn.lanes.size() == 4 && !cells.empty());

        const double      row    = pn.glyphs.pitch / pn.glyphs.fall; // the px a fall takes to go down a row
        std::vector<char> seen(cells.size(), 0);
        size_t            unseen = cells.size();
        int               n      = 0;
        for (double t = 0; unseen > 0 && t < 5000 * row; t += row / 4, ++n) {
            const double clock = 0.013 * n;
            const auto   draws = glyphDraws(pn, t, clock, 1.0, 12345);

            std::vector<int> litIn(pn.lanes.size(), 0);
            int              last = -1;
            for (const GlyphDraw& d : draws) {
                CHECK(d.id > last && size_t(d.id) < cells.size());
                last = d.id;
                if (size_t(d.id) >= cells.size())
                    continue;
                const RainCell& cell = cells[size_t(d.id)];
                CHECK(d.x == cell.x && d.y == cell.y);
                CHECK(d.glyph >= 0 && d.glyph < glyphCount());
                CHECK(d.alpha >= 1 && d.alpha <= kGlyphAlphaSteps && d.heat >= 0 && d.heat <= kGlyphHeatSteps);
                ++litIn[cell.lane];
                if (!seen[size_t(d.id)]) {
                    seen[size_t(d.id)] = 1;
                    --unseen;
                }
            }
            for (size_t l = 0; l < litIn.size(); ++l)
                CHECK(litIn[l] <= pn.lanes[l].streams * kLongestTail);

            if (n % 5 != 0)
                continue;
            CHECK(draws == glyphDraws(pn, t, clock, 1.0, 12345));

            // Less focus is a fainter picture of the same: the same characters, less lit and less hot.
            const Drawn all = drawn(draws);
            for (const GlyphDraw& d : glyphDraws(pn, t, clock, 0.5, 12345)) {
                const auto it = all.find(d.id);
                CHECK(it != all.end());
                if (it == all.end())
                    continue;
                CHECK(d.glyph == it->second.glyph && d.x == it->second.x && d.y == it->second.y);
                CHECK(d.alpha <= it->second.alpha && d.alpha <= kGlyphAlphaSteps / 2 && d.heat <= it->second.heat);
            }
            CHECK(glyphDraws(pn, t, clock, 0.0, 12345).empty() && glyphDraws(pn, t, clock, -1.0, 12345).empty());
            CHECK(glyphDraws(pn, t, clock, 2.0, 12345) == draws); // out of range is clamped

            // The rain goes with how far the columns have fallen; only the choice of characters goes with the time.
            const auto later = glyphDraws(pn, t, clock + 9.7, 1.0, 12345);
            CHECK(later.size() == draws.size());
            for (size_t i = 0; i < std::min(later.size(), draws.size()); ++i)
                CHECK(later[i].id == draws[i].id && later[i].alpha == draws[i].alpha && later[i].heat == draws[i].heat);
        }
        CHECK(unseen == 0);
        if (g_failures != before)
            std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
    }

    const Layout lay   = buildLayout(matrixStyle(), params(900, 600));
    const Panel& pn    = lay.panel;
    const auto   cells = cellsOf(pn);
    CHECK(pn.lanes.size() >= 2);

    // Windows are told apart by their seeds, and the columns of one window do not fall in step.
    bool seeds = false, apart = false;
    for (double t = 0; t < 4000; t += 130) {
        seeds = seeds || glyphDraws(pn, t, 5.0, 1.0, 1) != glyphDraws(pn, t, 5.0, 1.0, 2);

        std::vector<int> rows[2];
        for (const GlyphDraw& d : glyphDraws(pn, t, 0.0, 1.0, 1)) {
            const size_t lane = cells[size_t(d.id)].lane;
            if (lane < 2)
                rows[lane].push_back(d.id - (lane == 0 ? 0 : pn.lanes[0].cells));
        }
        apart = apart || rows[0] != rows[1];
    }
    CHECK(seeds && apart);

    // Time and the distance fallen do not run backwards.
    CHECK(glyphDraws(pn, -500.0, 3.0, 1.0, 1) == glyphDraws(pn, 0.0, 3.0, 1.0, 1));
    CHECK(glyphDraws(pn, 400.0, -3.0, 1.0, 1) == glyphDraws(pn, 400.0, 0.0, 1.0, 1));

    // Most of a long column is dark at any time.
    const Layout tall    = buildLayout(matrixStyle(), params(900, 2000));
    size_t       total   = 0, lit = 0, samples = 0;
    for (const RainLane& lane : tall.panel.lanes)
        total += size_t(lane.cells);
    for (double t = 0; t < 40000; t += 37, ++samples)
        lit += glyphDraws(tall.panel, t, 0.0, 1.0, 3).size();
    CHECK(total > 0 && 2 * lit < total * samples);

    // Nothing to draw where there are no characters, or no columns for them.
    CHECK(glyphDraws(Panel{}, 1.0, 1.0, 1.0, 1).empty());
    CHECK(glyphDraws(buildLayout(panelStyle(), params(900, 600)).panel, 1.0, 1.0, 1.0, 1).empty());
    Panel bare;
    bare.glyphs = {12, 0.6, 3.4, 0.5f, 40, 4};
    CHECK(glyphDraws(bare, 1.0, 1.0, 1.0, 1).empty());
}

// A column of characters by itself, that falls a row for every 10 px of travel.
Panel oneColumn(int cells, int streams = 1) {
    Panel pn;
    pn.glyphs = {12, 0.6, 3.4, 0.5f, 40, 4};
    pn.lanes  = {{100, 20, cells, streams}};
    return pn;
}

// What is lit in such a column: how much of every row, which rows are first and last, how many, and in how many pieces.
struct Look {
    std::vector<int> alpha, heat;
    int              first = -1, last = -1, lit = 0, pieces = 0;
};

Look lookAt(const Panel& pn, double travelled, double clock, unsigned seed) {
    Look out;
    out.alpha.assign(size_t(pn.lanes[0].cells), 0);
    out.heat = out.alpha;
    for (const GlyphDraw& d : glyphDraws(pn, travelled, clock, 1.0, seed)) {
        out.alpha[size_t(d.id)] = d.alpha;
        out.heat[size_t(d.id)]  = d.heat;
        out.pieces += (out.lit == 0 || out.last != d.id - 1) ? 1 : 0;
        out.first = out.lit == 0 ? d.id : out.first;
        out.last  = d.id;
        ++out.lit;
    }
    return out;
}

// A fall starts at the top of its column, goes down and never up, with a tail that dims towards its end, runs off the
// bottom, rests, and starts again. It fades in and out at both ends of the column.
void testRainFalls() {
    constexpr int kCells = 30, kEnd = kCells - 1;
    constexpr int kLongest = kCells * 6 / 10; // no tail is longer than three fifths of its column
    const Panel   pn       = oneColumn(kCells);

    for (unsigned seed : {1u, 2u, 3u, 77u}) {
        const int        before = g_failures;
        int              passes = 0, rest = 0, shortest = 1 << 30, longest = 0;
        bool             whole  = false;      // the pass that goes on has been seen from its start
        std::vector<int> arrival(kCells, -1); // how bright its head was when it got to a row
        double           reached[2] = {-1, -1};
        Look             prev;

        for (double t = 0; t <= 5000; t += 5) {
            const Look now = lookAt(pn, t, 0.0, seed);

            if (now.lit == 0) {
                if (prev.lit > 0 && whole) {
                    // It came in dim, was at full strength in the middle and went out dim.
                    CHECK(arrival[0] > 0 && arrival[0] < arrival[1] && arrival[1] < arrival[2] && arrival[2] == kGlyphAlphaSteps);
                    CHECK(arrival[kEnd] > 0 && arrival[kEnd] < arrival[kEnd - 1] && arrival[kEnd - 1] < arrival[kEnd - 2] &&
                          arrival[kEnd - 2] == kGlyphAlphaSteps);

                    // It fell at the speed of the column, give or take a third that is its own.
                    const double rate = (kEnd - 4) / ((reached[1] - reached[0]) / 10);
                    CHECK(rate >= 0.7 && rate <= 1.4);
                }
                whole = false;
                ++rest;
                prev = now;
                continue;
            }

            if (prev.lit == 0 && rest > 0) {
                // A pass begins with its head at the top of the column.
                CHECK(now.last == 0 && now.lit == 1);
                if (passes > 0) {
                    shortest = std::min(shortest, rest);
                    longest  = std::max(longest, rest);
                }
                ++passes;
                rest  = 0;
                whole = true;
                arrival.assign(kCells, -1);
                reached[0] = reached[1] = -1;
            }

            CHECK(now.lit <= kLongestTail && now.pieces == 1);
            if (prev.lit > 0) {
                // The same pass as a step ago: it goes down, and what is lit only dims.
                CHECK(now.first >= prev.first);
                if (prev.heat[size_t(prev.last)] > 0) // while its head is on the column
                    CHECK(now.last >= prev.last);
                for (size_t r = 0; r < size_t(kCells); ++r)
                    CHECK(prev.alpha[r] == 0 || now.alpha[r] <= prev.alpha[r]);
            }

            if (whole && now.heat[size_t(now.last)] > 0 && arrival[size_t(now.last)] < 0) {
                arrival[size_t(now.last)] = now.alpha[size_t(now.last)];
                if (now.last == 2)
                    reached[0] = t;
                if (now.last == kEnd - 2)
                    reached[1] = t;
            }

            if (now.first >= 3 && now.last <= kCells - 4) {
                // Well inside the column that is all of a fall: a tail of eight characters or more, dimmer and
                // dimmer behind a head at full strength, and only the head and the two behind it hot.
                CHECK(now.lit >= kShortestTail && now.lit <= kLongest && now.last - now.first + 1 == now.lit);
                CHECK(now.alpha[size_t(now.last)] == kGlyphAlphaSteps && now.heat[size_t(now.last)] == kGlyphHeatSteps);
                CHECK(now.alpha[size_t(now.last - 1)] < now.alpha[size_t(now.last)]);
                CHECK(now.heat[size_t(now.last - 3)] == 0);
                CHECK(now.alpha[size_t(now.last - now.lit / 2)] < kGlyphAlphaSteps / 2); // it dims faster than linearly
                for (int r = now.first; r < now.last; ++r)
                    CHECK(now.alpha[size_t(r)] <= now.alpha[size_t(r + 1)] && now.heat[size_t(r)] <= now.heat[size_t(r + 1)]);
            }
            prev = now;
        }
        CHECK(passes >= 3);
        CHECK(shortest >= 6 && longest <= 100); // a rest is not over at once, and does not leave the column dark for long
        if (g_failures != before)
            std::fprintf(stderr, "  ^ seed %u\n", seed);
    }

    // A long column has several falls at once, each at its own pace: never more than it should, and sometimes two or more.
    const Panel wide   = oneColumn(120, 3);
    int         most   = 0, pieces = 0;
    for (unsigned seed : {1u, 2u, 3u})
        for (double t = 0; t <= 8000; t += 7) {
            const Look now = lookAt(wide, t, 0.0, seed);
            most           = std::max(most, now.lit);
            pieces         = std::max(pieces, now.pieces);
        }
    CHECK(most <= 3 * kLongestTail && pieces >= 2 && pieces <= 3);
}

// Every pass of a fall has a tail of its own, eight rows to twenty-eight: a column does not fall the same way twice,
// short tails and long ones both come, and no tail is longer than its column allows.
void testRainTails() {
    // How many characters are lit at most in every pass of a column, for the passes in 20000 px of travel.
    const auto tailsOf = [](const Panel& pn, unsigned seed) {
        std::vector<int> tails;
        int              most = 0;
        for (double t = 0; t <= 20000; t += 5) {
            const int lit = lookAt(pn, t, 0.0, seed).lit;
            if (lit == 0 && most > 0) {
                tails.push_back(most);
                most = 0;
            }
            most = std::max(most, lit);
        }
        return tails;
    };

    std::set<int> every;
    for (unsigned seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
        const std::vector<int> tails = tailsOf(oneColumn(60), seed);
        const std::set<int>    kinds(tails.begin(), tails.end());
        CHECK(tails.size() >= 8 && kinds.size() >= 4);
        if (kinds.empty())
            continue;
        CHECK(*kinds.begin() >= kShortestTail && *kinds.rbegin() <= kLongestTail);
        every.insert(kinds.begin(), kinds.end());
    }
    CHECK(!every.empty() && *every.begin() <= kShortestTail + 2 && *every.rbegin() >= kLongestTail - 4);

    // A column that is too short for the longest tail has one of three fifths of it, or of three rows.
    for (int cells : {1, 2, 4, 10, 20, 45, 120}) {
        const int cap  = std::clamp(cells * 6 / 10, 3, kLongestTail);
        int       most = 0;
        for (unsigned seed : {1u, 2u, 3u})
            for (int tail : tailsOf(oneColumn(cells), seed))
                most = std::max(most, tail);
        CHECK(most <= cap);
        if (cells == 10 || cells == 20)
            CHECK(most == cap);
    }

    // A fall is never cut short, whatever its tail: it has left the column before the next one begins, and so what is
    // lit only ever goes down. A short rest after a long tail is what could go wrong, so look at many columns.
    for (int cells : {12, 30, 45})
        for (unsigned seed = 1; seed <= 60; ++seed) {
            const Panel pn = oneColumn(cells);
            Look        prev;
            for (double t = 0; t <= 6000; t += 5) {
                const Look now = lookAt(pn, t, 0.0, seed);
                if (prev.lit > 0 && now.lit > 0)
                    CHECK(now.first >= prev.first);
                prev = now;
            }
        }
}

// The head of a fall and the character behind it flicker through others three times a second, the rest of the tail
// changes now and then, each character at its own slow pace.
void testRainRhythm() {
    const Panel    pn   = oneColumn(30);
    const unsigned seed = 11;

    // Stop the rain at a moment when its head is well inside the column.
    double stop = -1;
    int    head = -1;
    for (double t = 0; t <= 3000 && stop < 0; t += 5) {
        const Look look = lookAt(pn, t, 0.0, seed);
        if (look.first >= 3 && look.last <= 24 && look.heat[size_t(look.last)] > 0) {
            stop = t;
            head = look.last;
        }
    }
    CHECK(stop >= 0 && head >= 8);
    if (stop < 0 || head < 8)
        return;

    const int rows[]       = {head, head - 1, head - 2, head - 5};
    int       changes[4]   = {0, 0, 0, 0};
    int       was[4]       = {-1, -1, -1, -1};
    for (double clock = 0; clock < 2; clock += 0.005) {
        const Drawn d = drawn(glyphDraws(pn, stop, clock, 1.0, seed));
        for (size_t k = 0; k < 4; ++k) {
            const auto it = d.find(rows[k]);
            CHECK(it != d.end());
            if (it == d.end())
                return;
            if (was[k] >= 0 && it->second.glyph != was[k])
                ++changes[k];
            was[k] = it->second.glyph;
        }
    }
    CHECK(changes[0] >= 4 && changes[0] <= 7 && changes[1] >= 4 && changes[1] <= 7); // about three a second
    CHECK(changes[2] <= 1 && changes[3] <= 1);

    // Further back a character changes now and then, never in a hurry, each at a pace of its own.
    const Look       tail = lookAt(pn, stop, 0.0, seed);
    std::vector<int> back; // the rows that are lit but too cool to flicker
    for (int row = 0; row < int(tail.alpha.size()); ++row)
        if (tail.alpha[size_t(row)] > 0 && tail.heat[size_t(row)] <= 2)
            back.push_back(row);
    CHECK(back.size() >= 5);

    std::map<int, int>    drifts, glyph;
    std::map<int, double> since;
    for (double clock = 0; clock < 40; clock += 0.01) {
        const Drawn d = drawn(glyphDraws(pn, stop, clock, 1.0, seed));
        for (const int row : back) {
            const auto it = d.find(row);
            CHECK(it != d.end());
            if (it == d.end())
                return;
            if (glyph.count(row) && it->second.glyph != glyph[row]) {
                ++drifts[row];
                if (since.count(row))
                    CHECK(clock - since[row] >= 2.4);
                since[row] = clock;
            }
            glyph[row] = it->second.glyph;
        }
    }
    std::set<int> paces;
    for (const int row : back) {
        CHECK(drifts[row] >= 4 && drifts[row] <= 17);
        paces.insert(drifts[row]);
    }
    CHECK(paces.size() >= 3);
}

// The rain is meant to stay at the edge of the eye: at the speed matrix-hud asks for, the fastest fall of the real
// panel goes down no more rows a second than the fastest lamp goes along its edge, and none stands still.
void testRainIsCalm() {
    constexpr double kThemeSpeed = 60; // px a second, as `hud_sweep_speed` of matrix-hud
    constexpr double kSeconds    = 5;
    Panel            pn          = buildLayout(matrixStyle(), params(1920, 1080)).panel;
    CHECK(!pn.lanes.empty() && !pn.lamps.empty() && pn.glyphs.on());
    if (pn.lanes.empty() || pn.lamps.empty() || !pn.glyphs.on())
        return;

    double lamp = 0;
    for (const Lamp& l : pn.lamps)
        lamp = std::max(lamp, std::abs(l.speed));
    const double lampRows = lamp * kThemeSpeed / pn.glyphs.pitch; // rows a second

    pn.lanes = {{pn.lanes[0].x, pn.lanes[0].y0, 60, 1}}; // one fall, so that its head is the last row lit
    double slowest = 1e9, fastest = 0;
    int    measured = 0;
    for (unsigned seed = 1; seed <= 40; ++seed)
        for (double travelled = 0; travelled < 6000; travelled += 15) {
            // From a head well inside the column to five seconds later: it cannot have run off the end by then.
            const Look from = lookAt(pn, travelled, 0.0, seed);
            if (from.last < 8 || from.last > 34)
                continue;
            const Look   to   = lookAt(pn, travelled + kThemeSpeed * kSeconds, 0.0, seed);
            const double rate = (to.last - from.last) / kSeconds;
            slowest           = std::min(slowest, rate);
            fastest           = std::max(fastest, rate);
            ++measured;
            break;
        }
    CHECK(measured == 40);
    CHECK(slowest >= 1.0);
    CHECK(fastest <= lampRows + 0.25);
}

void testGlyphChanges() {
    const auto draw = [](int id, int alpha, int heat = 0, int glyph = 1) { return GlyphDraw{id, 10 * id, 20, glyph, alpha, heat}; };

    const std::vector<GlyphDraw> before = {draw(0, 5), draw(2, 8), draw(3, 9, 1), draw(6, 12)};
    const std::vector<GlyphDraw> after  = {draw(0, 5), draw(2, 9), draw(4, 7), draw(6, 12, 0, 2), draw(7, 3)};

    // 2 changed, 3 went, 4 came, 6 became another character, 7 came: each as it is in the list it is in.
    const std::vector<GlyphDraw> expect = {draw(2, 9), draw(3, 9, 1), draw(4, 7), draw(6, 12, 0, 2), draw(7, 3)};
    CHECK(glyphChanges(before, after) == expect);

    CHECK(glyphChanges(before, before).empty());
    CHECK(glyphChanges({}, {}).empty());
    CHECK(glyphChanges({}, after) == after);
    CHECK(glyphChanges(before, {}) == before);

    // Applying the changes to the old list gives the new one.
    std::map<int, GlyphDraw> state = drawn(before);
    for (const GlyphDraw& d : glyphChanges(before, after)) {
        const bool stays = std::any_of(after.begin(), after.end(), [&](const GlyphDraw& a) { return a.id == d.id; });
        if (stays)
            state[d.id] = d;
        else
            state.erase(d.id);
    }
    CHECK(state == drawn(after));
}

} // namespace

int main() {
    if (!styleFor("panel") || !styleFor("matrix")) {
        std::fprintf(stderr, "the panel or the matrix style is not registered\n");
        return 1;
    }
    testStyle();
    testTooSmall();
    testLayouts();
    testFrameKinds();
    testClearOfWindow();
    testBeadsAndRunners();
    testRunnerPose();
    testGlowNearTraceDots();
    testGlowBetweenCirclers();
    testGlowOnPanels();
    testKnobs();
    testFloating();
    testDeterministic();
    testExtents();
    testGlyphFace();
    testLanes();
    testMatrixFrame();
    testLamps();
    testLampPose();
    testRainDraws();
    testRainFalls();
    testRainTails();
    testRainRhythm();
    testRainIsCalm();
    testGlyphChanges();
    return finish("test_panel");
}
