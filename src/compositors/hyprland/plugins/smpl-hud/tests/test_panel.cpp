#include "../panel.hpp"
#include "../stamp.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

using namespace smplhud;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

const Style& panelStyle() {
    return *styleFor("panel");
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

    // Only the panel style is a panel; the others stay bands.
    for (const Style& other : allStyles())
        CHECK((other.kind == Kind::Panel) == (std::string(other.name) == "panel"));
}

void testTooSmall() {
    const Style& st = panelStyle();
    CHECK(buildLayout(st, params(40, 40)).valid);
    CHECK(!buildLayout(st, params(39, 300)).valid);
    CHECK(!buildLayout(st, params(300, 39)).valid);
    CHECK(!buildLayout(st, params(300, 300, 0.f)).valid);
}

void testLayouts() {
    const Style& st = panelStyle();
    for (const auto& size : kSizes)
        for (float s : kScales)
            for (bool floating : {false, true})
                for (double lineWidth : {2.0, 3.0}) {
                    const Params p = params(size[0], size[1], s, floating, lineWidth);
                    checkLayout(buildLayout(st, p), p);
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
    const Style& st = panelStyle();
    for (float s : kScales)
        for (bool floating : {false, true}) {
            const Extents e = extentsFor(st, params(1000, 700, s, floating));
            CHECK(e.left > 0 && e.top > 0 && e.right > 0 && e.bottom > 0);
            CHECK(e.right >= e.left); // the traces run out on the right
            for (const auto& size : kSizes)
                CHECK(buildLayout(st, params(size[0], size[1], s, floating)).extents == e);
        }
}

} // namespace

int main() {
    if (!styleFor("panel")) {
        std::fprintf(stderr, "the panel style is not registered\n");
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
    return finish("test_panel");
}
