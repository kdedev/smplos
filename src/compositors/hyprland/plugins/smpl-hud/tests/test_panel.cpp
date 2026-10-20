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

    // The characters of a glyph style stay inside the extents, on whole px and on a line,
    // whatever they are doing. A style without characters has no places for them.
    const GlyphFace& face = lay.panel.glyphs;
    CHECK(face.on() || lay.panel.slots.empty());
    if (face.on()) {
        const GlyphSize gs = glyphSize(face);
        CHECK(gs.w > 0 && gs.h > 0 && gs.w % 2 == 0 && gs.h % 2 == 0);
        const auto glyphFits = [&](Vec2 at) {
            const double x = std::round(at.x), y = std::round(at.y);
            return inside(x - gs.w / 2.0, y - gs.h / 2.0, x + gs.w / 2.0, y + gs.h / 2.0);
        };
        for (const Bead& b : lay.panel.beads)
            CHECK(glyphFits(b.pos));

        // Where the line dips towards the window a character stands off it, outwards, by up to this much.
        const double     stepOut = std::ceil(6 * s);
        const GlyphSlot* last    = nullptr;
        for (const GlyphSlot& slot : lay.panel.slots) {
            CHECK(whole(slot.pos.x) && whole(slot.pos.y));
            CHECK(glyphFits(slot.pos));
            CHECK(slot.stroke >= 0 && size_t(slot.stroke) < lay.panel.strokes.size());
            if (slot.stroke < 0 || size_t(slot.stroke) >= lay.panel.strokes.size())
                continue;
            const Stroke& line = lay.panel.strokes[size_t(slot.stroke)];
            CHECK(slot.at >= 0 && slot.at <= strokeLength(line));
            CHECK(slot.alpha >= 0.3f - 1e-6f && slot.alpha <= 1.f);
            CHECK(distToStroke(line, slot.pos) <= (line.closed ? stepOut : 0.0) + 0.75);
            if (last)
                CHECK(slot.stroke > last->stroke || (slot.stroke == last->stroke && slot.at > last->at));
            last = &slot;
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
    CHECK(st->glyphHeight == 0 && st->glyphPitch == 0 && st->glyphPeriod == 0);

    // The matrix style is a panel that draws characters where the panel style draws dots.
    const Style* mx = styleFor("matrix");
    CHECK(mx != nullptr);
    if (!mx)
        return;
    CHECK(mx->kind == Kind::Panel);
    CHECK(mx->notchDepth > 0 && mx->traceOffset > 0 && mx->beadRadius > 0 && mx->runnerSpeed > 0);
    CHECK(mx->lineAlpha > 0 && mx->lineAlpha <= 1 && mx->haloAlpha > 0 && mx->haloAlpha <= 1);
    CHECK(mx->haloReach > 0 && mx->floatBoost >= 1);
    CHECK(mx->glyphHeight > 0 && mx->glyphPitch >= mx->glyphHeight / 2 && mx->glyphPeriod > 0);
    CHECK(mx->sweepAlpha > 0); // the characters travel while the window sweeps
    CHECK(mx->glyphKnock > 0 && mx->glyphKnock <= 1); // each stands on a dark plate, so the line does not strike it through

    // Only the panel styles are panels, and only the matrix one draws characters; the others stay bands.
    for (const Style& other : allStyles()) {
        const std::string name = other.name;
        CHECK((other.kind == Kind::Panel) == (name == "panel" || name == "matrix"));
        CHECK((other.glyphHeight > 0) == (name == "matrix"));
        CHECK((other.glyphPitch > 0) == (name == "matrix"));
        CHECK((other.glyphPeriod > 0) == (name == "matrix"));
        CHECK((other.glyphKnock > 0) == (name == "matrix"));
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
    CHECK(!plain.panel.glyphs.on() && plain.panel.slots.empty() && plain.panel.glyphs.knock == 0);
    CHECK(glyphDraws(plain.panel, runnerPoses(plain.panel, 50), 3.0, 1.0, 1).empty());

    const Style& st = matrixStyle();
    for (float s : kScales) {
        const GlyphFace docked = buildLayout(st, params(900, 600, s)).panel.glyphs;
        const GlyphFace afloat = buildLayout(st, params(900, 600, s, true)).panel.glyphs;
        CHECK(docked.on());
        CHECK_NEAR(docked.height, std::max(6.0, std::round(st.glyphHeight * double(s))), 1e-9);
        CHECK_NEAR(docked.pitch, std::max(docked.height / 2, std::round(st.glyphPitch * double(s))), 1e-9);
        CHECK_NEAR(docked.period, st.glyphPeriod, 1e-9);
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

// No character touches the window, even where the line comes close to it, and
// one that stands off the line stands outside it.
void testGlyphsClearOfWindow() {
    const Style& st      = matrixStyle();
    int          stepped = 0;
    for (const auto& size : {std::pair{100, 100}, {240, 180}, {400, 300}, {700, 400}, {900, 600}, {1920, 1080}})
        for (float s : kScales)
            for (bool floating : {false, true}) {
                const int        before = g_failures;
                const Params     p      = params(size.first, size.second, s, floating);
                const Layout     lay    = buildLayout(st, p);
                const Panel&     pn     = lay.panel;
                const GlyphFace& face   = pn.glyphs;
                CHECK(pn.slots.size() > 10);

                // The strokes of a character reach this far from its middle.
                const double hx = face.height * kGlyphW / kGlyphH / 2 + face.halfWidth, hy = face.height / 2 + face.halfWidth;
                double       nearest = kInf;
                int          onOutline = 0;
                for (const GlyphSlot& slot : pn.slots) {
                    for (int i = -4; i <= 4; ++i)
                        for (int j = -4; j <= 4; ++j)
                            nearest = std::min(nearest, distToWindow({slot.pos.x + hx * i / 4, slot.pos.y + hy * j / 4}, p.width, p.height, p.cut));

                    const Stroke& line = pn.strokes[size_t(slot.stroke)];
                    if (!line.closed)
                        continue;
                    ++onOutline;
                    const Vec2 on = strokePointAt(line, slot.at).pos;
                    if (std::hypot(slot.pos.x - on.x, slot.pos.y - on.y) < 1.5)
                        continue;
                    ++stepped;
                    CHECK(distToWindow(slot.pos, p.width, p.height, p.cut) > distToWindow(on, p.width, p.height, p.cut));
                }
                CHECK(nearest >= 1.5);

                // The outline keeps most of its characters; none are closer than a pitch, or
                // nearly so, and none sit on the bead at the end of a trace.
                CHECK(onOutline >= 0.7 * std::lround(strokeLength(pn.strokes[0]) / face.pitch));
                const GlyphSlot* prev = nullptr;
                for (const GlyphSlot& slot : pn.slots) {
                    if (prev && prev->stroke == slot.stroke)
                        CHECK(slot.at - prev->at >= 0.8 * face.pitch);
                    prev = &slot;

                    const Stroke& line = pn.strokes[size_t(slot.stroke)];
                    if (!line.closed)
                        for (const Vertex& end : {line.pts.front(), line.pts.back()})
                            CHECK(std::hypot(slot.pos.x - end.x, slot.pos.y - end.y) >= 0.5 * face.pitch);
                }
                if (g_failures != before)
                    std::fprintf(stderr, "  ^ %s, nearest %g\n", label(p).c_str(), nearest);
            }
    CHECK(stepped > 0); // where the notches dip towards the window some characters step off the line
}

// Characters are drawn over the frame the panel style draws: nothing of the
// frame moves for them, and it only gets more room.
void testGlyphsLeaveTheFrame() {
    Style plain = panelStyle(), fancy = plain;
    fancy.glyphHeight = matrixStyle().glyphHeight;
    fancy.glyphPitch  = matrixStyle().glyphPitch;
    fancy.glyphPeriod = matrixStyle().glyphPeriod;
    for (const auto& size : kSizes)
        for (float s : kScales)
            for (bool floating : {false, true}) {
                const Params p = params(size[0], size[1], s, floating);
                const Layout a = buildLayout(plain, p), b = buildLayout(fancy, p);
                CHECK(a.valid && b.valid);
                CHECK(a.panel.strokes.size() == b.panel.strokes.size());
                for (size_t i = 0; i < std::min(a.panel.strokes.size(), b.panel.strokes.size()); ++i)
                    CHECK(same(a.panel.strokes[i], b.panel.strokes[i]));
                CHECK(a.panel.beads.size() == b.panel.beads.size());
                for (size_t i = 0; i < std::min(a.panel.beads.size(), b.panel.beads.size()); ++i)
                    CHECK(a.panel.beads[i].pos.x == b.panel.beads[i].pos.x && a.panel.beads[i].pos.y == b.panel.beads[i].pos.y && a.panel.beads[i].radius == b.panel.beads[i].radius);
                CHECK(a.panel.runners.size() == b.panel.runners.size());
                for (size_t i = 0; i < std::min(a.panel.runners.size(), b.panel.runners.size()); ++i)
                    CHECK(a.panel.runners[i].stroke == b.panel.runners[i].stroke && a.panel.runners[i].offset == b.panel.runners[i].offset && a.panel.runners[i].speed == b.panel.runners[i].speed);
                CHECK(a.sampleBox == b.sampleBox);
                CHECK(b.extents.left >= a.extents.left && b.extents.top >= a.extents.top && b.extents.right >= a.extents.right && b.extents.bottom >= a.extents.bottom);
                CHECK(a.panel.slots.empty() && !b.panel.slots.empty());
            }
}

// Where a dot is on its line, for the characters that follow it.
void testRunnerPoseExtras() {
    const Layout lay = buildLayout(matrixStyle(), params(900, 600));
    const Panel& pn  = lay.panel;
    CHECK(pn.runners.size() >= 3);
    for (double t : {0.0, 13.0, 211.5, 1234.0, 5000.0}) {
        const auto poses = runnerPoses(pn, t);
        CHECK(poses.size() == pn.runners.size());
        if (poses.size() != pn.runners.size())
            return;
        for (size_t i = 0; i < pn.runners.size(); ++i) {
            const Runner&    r = pn.runners[i];
            const Stroke&    s = pn.strokes[size_t(r.stroke)];
            const RunnerPose a = runnerPose(pn, r, t);
            CHECK(a.stroke == r.stroke && poses[i].stroke == r.stroke);
            CHECK(poses[i].at == a.at && poses[i].dir == a.dir);
            CHECK(a.at >= 0 && a.at <= strokeLength(s) + 1e-9);
            CHECK(a.dir == (s.closed ? (r.speed > 0 ? 1 : -1) : 0));
            const Vec2 on = strokePointAt(s, a.at).pos;
            CHECK_NEAR(on.x, a.pos.x, 1e-6);
            CHECK_NEAR(on.y, a.pos.y, 1e-6);
        }
    }

    // A dot that points nowhere is nowhere.
    Runner lost = pn.runners.front();
    lost.stroke = 99;
    const RunnerPose none = runnerPose(pn, lost, 5);
    CHECK(none.stroke == -1 && none.dir == 0 && none.alpha == 0);
}

// A 400 px square outline with a trace 10 px above its top edge, a bead at each
// end of the trace and a place for a character every 40 px (a pitch) along both
// lines: 40 round the square, 7 along the trace.
Panel glyphSquare() {
    Panel pn  = square();
    pn.glyphs = {12, 0.6, 3.4, 0.5f, 40, 4};
    pn.strokes.push_back(lineOf({{50, -10, 1.f}, {350, -10, 1.f}}, false));
    for (Vec2 end : {Vec2{50, -10}, Vec2{350, -10}}) {
        Bead b;
        b.pos = end;
        pn.beads.push_back(b);
    }
    for (size_t k = 0; k < pn.strokes.size(); ++k)
        for (double at = 20; at < strokeLength(pn.strokes[k]); at += 40)
            pn.slots.push_back({strokePointAt(pn.strokes[k], at).pos, int(k), at, 1.f});
    return pn;
}

// The id of the character at `at` px along a stroke, or -1.
int slotId(const Panel& pn, int stroke, double at) {
    for (size_t j = 0; j < pn.slots.size(); ++j)
        if (pn.slots[j].stroke == stroke && std::fabs(pn.slots[j].at - at) < 1e-9)
            return int(pn.beads.size() + j);
    return -1;
}

RunnerPose poseAt(const Panel& pn, int stroke, double at, int dir, float glow = 0.f) {
    RunnerPose pose;
    pose.pos    = strokePointAt(pn.strokes[size_t(stroke)], at).pos;
    pose.alpha  = 1.f;
    pose.glow   = glow;
    pose.stroke = stroke;
    pose.at     = at;
    pose.dir    = dir;
    return pose;
}

using Drawn = std::map<int, GlyphDraw>;

Drawn drawn(const std::vector<GlyphDraw>& draws) {
    Drawn out;
    for (const GlyphDraw& d : draws)
        out[d.id] = d;
    return out;
}

// How far a character is lit, in steps; 0 when it is not drawn at all.
int litAt(const Drawn& d, int id) {
    const auto it = d.find(id);
    return it == d.end() ? 0 : it->second.alpha;
}

int heatAt(const Drawn& d, int id) {
    const auto it = d.find(id);
    return it == d.end() ? 0 : it->second.heat;
}

// A seed for which no character along the lines lights up by itself at that
// time, so that what the dots do shows on its own.
unsigned quietSeed(const Panel& pn, double clock) {
    for (unsigned seed = 0; seed < 500; ++seed) {
        bool quiet = true;
        for (const GlyphDraw& d : glyphDraws(pn, {}, clock, 1.0, seed))
            quiet = quiet && size_t(d.id) < pn.beads.size();
        if (quiet)
            return seed;
    }
    CHECK(!"a quiet seed");
    return 0;
}

// On the real panels: what is drawn is well formed, deterministic, only a part
// of all there is, and every place along the lines lights up when a dot goes by.
void testGlyphDrawsOnPanels() {
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
        const double speed  = std::fabs(pn.runners[1].speed);
        const double lap    = strokeLength(pn.strokes[0]) / speed;
        const size_t places = pn.beads.size() + pn.slots.size();

        std::vector<char> seen(pn.slots.size(), 0);
        size_t            most = 0;
        for (double t = 0; t < 2 * lap; t += 2 * pn.glyphs.pitch / speed) {
            const auto poses = runnerPoses(pn, t);
            for (double intensity : {0.0, 0.5, 1.0}) {
                const double clock = t * 0.05 + intensity;
                const auto   draws = glyphDraws(pn, poses, clock, intensity, 12345);
                CHECK(draws == glyphDraws(pn, poses, clock, intensity, 12345));
                most = std::max(most, draws.size());

                int last = -1;
                for (const GlyphDraw& d : draws) {
                    CHECK(d.id > last && size_t(d.id) < places);
                    last = d.id;
                    if (size_t(d.id) >= places)
                        continue;
                    const bool isBead = size_t(d.id) < pn.beads.size();
                    const Vec2 where  = isBead ? pn.beads[size_t(d.id)].pos : pn.slots[size_t(d.id) - pn.beads.size()].pos;
                    CHECK(d.x == std::lround(where.x) && d.y == std::lround(where.y));
                    CHECK(d.glyph >= 0 && d.glyph < glyphCount());
                    CHECK(d.alpha >= 1 && d.alpha <= kGlyphAlphaSteps && d.heat >= 0 && d.heat <= kGlyphHeatSteps);
                    if (intensity == 0.0)
                        CHECK(isBead && d.heat == 0); // without focus nothing but the beads rest there
                    if (!isBead && intensity == 1.0)
                        seen[size_t(d.id) - pn.beads.size()] = 1;
                }
            }
        }
        CHECK(most <= pn.beads.size() + pn.slots.size() / 2);
        for (size_t j = 0; j < pn.slots.size(); ++j)
            CHECK(seen[j]);
        if (g_failures != before)
            std::fprintf(stderr, "  ^ %s\n", label(p).c_str());
    }

    // Windows are told apart by their seeds.
    const Layout lay = buildLayout(matrixStyle(), params(900, 600));
    CHECK(glyphDraws(lay.panel, {}, 5.0, 1.0, 1) != glyphDraws(lay.panel, {}, 5.0, 1.0, 2));

    // Nothing to draw where there are no characters, or no places for them.
    CHECK(glyphDraws(Panel{}, {}, 1.0, 1.0, 1).empty());
    Panel bare = square();
    bare.glyphs = {12, 0.6, 3.4, 0.5f, 40, 4};
    CHECK(glyphDraws(bare, {}, 1.0, 1.0, 1).empty());
}

// A dot going round leaves a trail behind it, a short way ahead of it, and one
// running back and forth leaves it on both sides, the same on both.
void testGlyphTrails() {
    const Panel    pn    = glyphSquare();
    const double   clock = 3.0;
    const unsigned seed  = quietSeed(pn, clock);
    const int      beads = int(pn.beads.size());

    const auto reach = [&](const Drawn& d, int stroke, double at, double step, int dir) {
        int far = 0;
        for (int k = 1; k <= 16; ++k)
            if (litAt(d, slotId(pn, stroke, at + dir * step * k)) > 0)
                far = k;
        return far;
    };

    // Clockwise: lit under the dot, fading with the distance behind it.
    const RunnerPose cw[]  = {poseAt(pn, 0, 820, 1)};
    const Drawn      trail = drawn(glyphDraws(pn, cw, clock, 1.0, seed));
    const int        head  = slotId(pn, 0, 820);
    CHECK(head >= 0);
    CHECK(litAt(trail, head) == kGlyphAlphaSteps && heatAt(trail, head) == kGlyphHeatSteps);

    int prev = kGlyphAlphaSteps;
    for (int k = 1; k <= 16; ++k) {
        const int a = litAt(trail, slotId(pn, 0, 820 - 40.0 * k));
        CHECK(a <= prev);
        prev = a;
    }
    CHECK(litAt(trail, slotId(pn, 0, 780)) > litAt(trail, slotId(pn, 0, 740)));
    CHECK(litAt(trail, slotId(pn, 0, 740)) > litAt(trail, slotId(pn, 0, 660)));
    CHECK(heatAt(trail, slotId(pn, 0, 780)) == 0); // only the head is hot
    const int behind = reach(trail, 0, 820, 40, -1), ahead = reach(trail, 0, 820, 40, +1);
    CHECK(behind >= 5 && behind <= 8);
    CHECK(ahead >= 1 && ahead <= 2);
    CHECK(behind >= 3 * ahead);

    // Nothing else is lit but the dot's own trail.
    for (const auto& [id, d] : trail)
        if (id >= beads)
            CHECK(std::fabs(pn.slots[size_t(id - beads)].at - 820) <= 8 * 40 && pn.slots[size_t(id - beads)].stroke == 0);

    // Counter-clockwise the same, the other way round.
    const RunnerPose ccw[] = {poseAt(pn, 0, 820, -1)};
    const Drawn      back  = drawn(glyphDraws(pn, ccw, clock, 1.0, seed));
    CHECK(reach(back, 0, 820, 40, +1) == behind && reach(back, 0, 820, 40, -1) == ahead);
    for (int k = -4; k <= 4; ++k)
        CHECK(litAt(back, slotId(pn, 0, 820 + 40.0 * k)) == litAt(trail, slotId(pn, 0, 820 - 40.0 * k)));

    // The trail goes on across the end of the line, where it closes.
    const Drawn wrap = drawn(glyphDraws(pn, std::vector{poseAt(pn, 0, 20, 1)}, clock, 1.0, seed));
    CHECK(litAt(wrap, slotId(pn, 0, 1580)) > litAt(wrap, slotId(pn, 0, 1540)) && litAt(wrap, slotId(pn, 0, 1540)) > 0);

    // Running back and forth on the trace: lit on both sides alike, a few pitches out.
    const RunnerPose bounce[] = {poseAt(pn, 1, 140, 0)};
    const Drawn      both     = drawn(glyphDraws(pn, bounce, clock, 1.0, seed));
    CHECK(litAt(both, slotId(pn, 1, 140)) == kGlyphAlphaSteps);
    for (int k = 1; k <= 3; ++k)
        CHECK(litAt(both, slotId(pn, 1, 140 + 40.0 * k)) == litAt(both, slotId(pn, 1, 140 - 40.0 * k)));
    CHECK(litAt(both, slotId(pn, 1, 100)) > litAt(both, slotId(pn, 1, 60)) && litAt(both, slotId(pn, 1, 60)) > 0);
    CHECK(litAt(both, slotId(pn, 1, 20)) == 0 && litAt(both, slotId(pn, 1, 260)) == 0);

    // Where the dot glows the trail is longer and brighter.
    const RunnerPose hot[]  = {poseAt(pn, 0, 820, 1, 1.f)};
    const Drawn      glows  = drawn(glyphDraws(pn, hot, clock, 1.0, seed));
    CHECK(reach(glows, 0, 820, 40, -1) > behind);
    for (int k = 1; k <= 5; ++k)
        CHECK(litAt(glows, slotId(pn, 0, 820 - 40.0 * k)) >= litAt(trail, slotId(pn, 0, 820 - 40.0 * k)));

    // A dot that is not on a line lights nothing along it.
    RunnerPose astray = poseAt(pn, 0, 820, 1);
    astray.stroke     = -1;
    CHECK(drawn(glyphDraws(pn, std::vector{astray}, clock, 1.0, seed)) == drawn(glyphDraws(pn, {}, clock, 1.0, seed)));

    // Less focus, a fainter trail, none at all without.
    const RunnerPose go[] = {poseAt(pn, 0, 820, 1)};
    const Drawn      half = drawn(glyphDraws(pn, go, clock, 0.5, seed));
    CHECK(litAt(half, head) > 0 && litAt(half, head) < litAt(trail, head));
    CHECK(litAt(half, slotId(pn, 0, 780)) < litAt(trail, slotId(pn, 0, 780)));
    for (const GlyphDraw& d : glyphDraws(pn, go, clock, 0.0, seed))
        CHECK(d.id < beads);
    CHECK(glyphDraws(pn, go, clock, 2.0, seed) == glyphDraws(pn, go, clock, 1.0, seed)); // out of range is clamped
    CHECK(glyphDraws(pn, go, clock, -1.0, seed) == glyphDraws(pn, go, clock, 0.0, seed));
}

// The character at the end of a trace lights up when a dot reaches it.
void testGlyphHits() {
    const Panel    pn    = glyphSquare();
    const double   clock = 3.0;
    const unsigned seed  = 5;
    const Drawn    rest  = drawn(glyphDraws(pn, {}, clock, 1.0, seed));
    CHECK(rest.count(0) && rest.count(1));
    CHECK(litAt(rest, 0) < kGlyphAlphaSteps || heatAt(rest, 0) < kGlyphHeatSteps);

    // A dot right on the first bead: fully lit; the other bead, far away, does not care.
    const RunnerPose on[] = {poseAt(pn, 1, 0, 0)};
    const Drawn      hit  = drawn(glyphDraws(pn, on, clock, 1.0, seed));
    CHECK(litAt(hit, 0) == kGlyphAlphaSteps && heatAt(hit, 0) == kGlyphHeatSteps);
    CHECK(hit.at(1) == rest.at(1));
    CHECK(hit.at(0).glyph == rest.at(0).glyph && hit.at(0).x == rest.at(0).x);

    // The nearer the dot the brighter, none from two pitches and a bit off.
    int last = kGlyphAlphaSteps;
    for (double dist : {10.0, 25.0, 40.0, 60.0, 80.0}) {
        const RunnerPose near[] = {poseAt(pn, 1, dist, 0)};
        const Drawn      d      = drawn(glyphDraws(pn, near, clock, 1.0, seed));
        CHECK(litAt(d, 0) <= last && litAt(d, 0) >= litAt(rest, 0));
        CHECK(heatAt(d, 0) >= heatAt(rest, 0));
        last = litAt(d, 0);
    }
    const RunnerPose far[] = {poseAt(pn, 1, 89, 0)};
    CHECK(drawn(glyphDraws(pn, far, clock, 1.0, seed)).at(0) == rest.at(0));

    // A dot that cannot be seen, one that is on no line, or one while nobody is looking lights nothing.
    RunnerPose hidden = poseAt(pn, 1, 0, 0);
    hidden.alpha      = 0.f;
    CHECK(drawn(glyphDraws(pn, std::vector{hidden}, clock, 1.0, seed)).at(0) == rest.at(0));
    RunnerPose astray = poseAt(pn, 1, 0, 0);
    astray.stroke     = -1;
    CHECK(drawn(glyphDraws(pn, std::vector{astray}, clock, 1.0, seed)).at(0) == rest.at(0));
    CHECK(drawn(glyphDraws(pn, on, clock, 0.0, seed)).at(0) == drawn(glyphDraws(pn, {}, clock, 0.0, seed)).at(0));
}

// A character at the end of a trace breathes slowly, and changes into another
// at the dimmest point of a breath. Without focus it rests.
void testGlyphBreathing() {
    const Panel    pn   = glyphSquare();
    const unsigned seed = 3;

    const std::vector<GlyphDraw> rest = glyphDraws(pn, {}, 0.0, 0.0, seed);
    CHECK(rest.size() == pn.beads.size());
    for (double clock = 0; clock < 30; clock += 0.37) {
        const auto d = glyphDraws(pn, {}, clock, 0.0, seed);
        CHECK(d.size() == rest.size());
        for (size_t i = 0; i < std::min(d.size(), rest.size()); ++i)
            CHECK(d[i].alpha == rest[i].alpha && d[i].heat == 0);
    }

    // It goes from dim to bright and back, a step or two at a time, and changes only when dim.
    int    lo = kGlyphAlphaSteps, hi = 0, hottest = 0, changes = 0, step = 0, glyph = -1, alpha = -1;
    double dimmestChange = 0;
    for (double clock = 0; clock < 20; clock += 0.01) {
        const Drawn d = drawn(glyphDraws(pn, {}, clock, 1.0, seed));
        CHECK(d.count(0));
        if (!d.count(0))
            return;
        const GlyphDraw& g = d.at(0);
        lo                 = std::min(lo, g.alpha);
        hi                 = std::max(hi, g.alpha);
        hottest            = std::max(hottest, g.heat);
        if (alpha >= 0)
            step = std::max(step, std::abs(g.alpha - alpha));
        if (glyph >= 0 && g.glyph != glyph) {
            ++changes;
            dimmestChange = std::max(dimmestChange, double(g.alpha));
        }
        glyph = g.glyph;
        alpha = g.alpha;
    }
    CHECK(lo <= 6 && hi >= 28 && hottest >= 2);
    CHECK(step <= 1);
    CHECK(changes >= 3 && changes <= 7);
    CHECK(dimmestChange <= 6);

    // Not every window breathes together.
    bool apart = false;
    for (double clock = 0; clock < 20; clock += 0.5)
        apart = apart || litAt(drawn(glyphDraws(pn, {}, clock, 1.0, 3)), 0) != litAt(drawn(glyphDraws(pn, {}, clock, 1.0, 4)), 0);
    CHECK(apart);
}

// The head of a trail flickers through characters, the rest of it changes now
// and then, and a few places light up on their own.
void testGlyphRhythm() {
    const Panel pn   = glyphSquare();
    const int   head = slotId(pn, 0, 820), far = slotId(pn, 0, 660);
    const RunnerPose cw[] = {poseAt(pn, 0, 820, 1)};

    int flicks = 0, drifts = 0, glyph = -1, other = -1;
    for (double clock = 3; clock < 5; clock += 0.005) {
        const Drawn d = drawn(glyphDraws(pn, cw, clock, 1.0, 11));
        CHECK(d.count(head) && d.count(far));
        if (!d.count(head) || !d.count(far))
            return;
        if (glyph >= 0 && d.at(head).glyph != glyph)
            ++flicks;
        if (other >= 0 && d.at(far).glyph != other)
            ++drifts;
        glyph = d.at(head).glyph;
        other = d.at(far).glyph;
    }
    CHECK(flicks >= 9 && flicks <= 16); // about seven a second
    CHECK(drifts <= 1);

    // On their own, a few places along the lines light up, faintly, and fade again.
    int              lit = 0;
    std::vector<int> places;
    for (double clock = 0; clock < 60; clock += 0.25)
        for (const GlyphDraw& d : glyphDraws(pn, {}, clock, 1.0, 9)) {
            if (size_t(d.id) < pn.beads.size())
                continue;
            CHECK(d.heat == 0 && d.alpha <= 18);
            ++lit;
            if (std::find(places.begin(), places.end(), d.id) == places.end())
                places.push_back(d.id);
        }
    CHECK(lit > 0);
    CHECK(places.size() >= 2 && places.size() <= pn.slots.size() / 4);
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
    testGlyphsClearOfWindow();
    testGlyphsLeaveTheFrame();
    testRunnerPoseExtras();
    testGlyphDrawsOnPanels();
    testGlyphTrails();
    testGlyphHits();
    testGlyphBreathing();
    testGlyphRhythm();
    testGlyphChanges();
    return finish("test_panel");
}
