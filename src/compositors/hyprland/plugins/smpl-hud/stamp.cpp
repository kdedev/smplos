#include "stamp.hpp"

#include "glyph.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace smplhud {
namespace {

constexpr double kEps = 1e-6;

// Tile boxes that come closer than this are merged, so no sliver of a strip is left between them.
constexpr int kMergeGap = 4;

int floorI(double v) {
    return int(std::floor(v + kEps));
}

int ceilI(double v) {
    return int(std::ceil(v - kEps));
}

// Nearest whole number, halves up, so that two runs meeting at one point agree on where it is.
int roundI(double v) {
    return int(std::floor(v + 0.5 + kEps));
}

int round16(double v) {
    return int(std::lround(v * 16.0));
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

// [x0, x1) x [y0, y1)
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    int  w() const { return x1 - x0; }
    int  h() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// The box of a segment (or a point, with a == b) grown by `pad`, snapped outwards.
Box boxAround(Vec2 a, Vec2 b, int pad) {
    return {floorI(std::min(a.x, b.x) - pad), floorI(std::min(a.y, b.y) - pad), ceilI(std::max(a.x, b.x) + pad), ceilI(std::max(a.y, b.y) + pad)};
}

bool closerThan(const Box& a, const Box& b, int gap) {
    return a.x0 - gap < b.x1 && b.x0 - gap < a.x1 && a.y0 - gap < b.y1 && b.y0 - gap < a.y1;
}

Box unionOf(const Box& a, const Box& b) {
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// What is left of `a` when `b` is cut out of it.
void subtract(const Box& a, const Box& b, std::vector<Box>& out) {
    if (!closerThan(a, b, 0)) {
        out.push_back(a);
        return;
    }
    if (a.y0 < b.y0)
        out.push_back({a.x0, a.y0, a.x1, b.y0});
    if (b.y1 < a.y1)
        out.push_back({a.x0, b.y1, a.x1, a.y1});
    const int y0 = std::max(a.y0, b.y0), y1 = std::min(a.y1, b.y1);
    if (a.x0 < b.x0)
        out.push_back({a.x0, y0, b.x0, y1});
    if (b.x1 < a.x1)
        out.push_back({b.x1, y0, a.x1, y1});
}

bool axisAligned(const Vertex& a, const Vertex& b) {
    return std::fabs(a.x - b.x) < kEps || std::fabs(a.y - b.y) < kEps;
}

bool horizontal(const Vertex& a, const Vertex& b) {
    return std::fabs(a.y - b.y) < kEps;
}

bool continues(const Vertex& a, const Vertex& b, const Vertex& c) {
    const double ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
    const double cross = ux * vy - uy * vx, dot = ux * vx + uy * vy;
    return std::fabs(cross) < 1e-6 * std::hypot(ux, uy) * std::hypot(vx, vy) && dot > 0;
}

bool segmentVisible(const Stroke& stroke, const Vertex& a, const Vertex& b) {
    return stroke.alpha * std::max(a.alpha, b.alpha) >= kInvisible;
}

bool endVisible(const Stroke& stroke, const Vertex& v) {
    return stroke.alpha * v.alpha >= kInvisible;
}

// A stretch of a stroke that can be seen.
struct Run {
    std::vector<Vertex> pts;
    bool                capFront = false; // the line ends here in plain view, so the end needs a rounded tile
    bool                capBack  = false;
};

// The visible stretches of an open polyline.
void collectRuns(const Stroke& stroke, const std::vector<Vertex>& pts, std::vector<Run>& runs) {
    Run  run;
    auto flush = [&] {
        if (run.pts.size() >= 2) {
            run.capFront = endVisible(stroke, run.pts.front());
            run.capBack  = endVisible(stroke, run.pts.back());
            runs.push_back(std::move(run));
        }
        run = {};
    };
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        if (!segmentVisible(stroke, pts[i], pts[i + 1])) {
            flush();
            continue;
        }
        if (run.pts.empty())
            run.pts.push_back(pts[i]);
        run.pts.push_back(pts[i + 1]);
    }
    flush();
}

std::vector<Run> splitRuns(const Stroke& stroke) {
    std::vector<Run> runs;
    const auto&      p = stroke.pts;
    const size_t     n = p.size();
    if (n < 2)
        return runs;
    if (!stroke.closed) {
        collectRuns(stroke, p, runs);
        return runs;
    }

    // A closed stroke starts again right after an invisible segment.
    for (size_t k = 0; k < n; ++k) {
        if (segmentVisible(stroke, p[k], p[(k + 1) % n]))
            continue;
        std::vector<Vertex> rotated;
        for (size_t i = 0; i < n; ++i)
            rotated.push_back(p[(k + 1 + i) % n]);
        collectRuns(stroke, rotated, runs); // its closing segment is the invisible one
        return runs;
    }

    // All of it is visible: cut it half way along its longest straight segment,
    // where the two halves meet end to end.
    size_t best    = n;
    double bestLen = 0;
    for (size_t i = 0; i < n; ++i) {
        const Vertex& a = p[i];
        const Vertex& b = p[(i + 1) % n];
        if (!axisAligned(a, b))
            continue;
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len > bestLen) {
            bestLen = len;
            best    = i;
        }
    }

    Run run;
    if (best < n && bestLen >= 2) {
        const Vertex& a = p[best];
        const Vertex& b = p[(best + 1) % n];
        Vertex        mid = a;
        double        t;
        if (horizontal(a, b)) {
            mid.x = std::round((a.x + b.x) / 2);
            t     = (mid.x - a.x) / (b.x - a.x);
        } else {
            mid.y = std::round((a.y + b.y) / 2);
            t     = (mid.y - a.y) / (b.y - a.y);
        }
        mid.alpha = float(lerp(a.alpha, b.alpha, t));
        run.pts.push_back(mid);
        for (size_t i = 1; i <= n; ++i)
            run.pts.push_back(p[(best + i) % n]);
        run.pts.push_back(mid);
    } else {
        for (size_t i = 0; i <= n; ++i)
            run.pts.push_back(p[i % n]);
        run.capFront = run.capBack = true;
    }
    runs.push_back(std::move(run));
    return runs;
}

// The boxes of the tiles of a run: round every place where it turns or ends in
// plain view and every segment that is not horizontal or vertical. Boxes that
// touch are merged, so none of the returned boxes overlap.
std::vector<Box> tileBoxes(const Run& run, int reach) {
    const auto&      p = run.pts;
    std::vector<Box> boxes;
    auto             around = [&](const Vertex& v) { return boxAround({v.x, v.y}, {v.x, v.y}, reach); };
    if (run.capFront)
        boxes.push_back(around(p.front()));
    if (run.capBack)
        boxes.push_back(around(p.back()));
    for (size_t i = 1; i + 1 < p.size(); ++i)
        if (!continues(p[i - 1], p[i], p[i + 1]))
            boxes.push_back(around(p[i]));
    for (size_t i = 0; i + 1 < p.size(); ++i)
        if (!axisAligned(p[i], p[i + 1]))
            boxes.push_back(boxAround({p[i].x, p[i].y}, {p[i + 1].x, p[i + 1].y}, reach));

    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < boxes.size() && !merged; ++i) {
            for (size_t j = i + 1; j < boxes.size() && !merged; ++j) {
                if (!closerThan(boxes[i], boxes[j], kMergeGap))
                    continue;
                boxes[i] = unionOf(boxes[i], boxes[j]);
                boxes.erase(boxes.begin() + long(j));
                merged = true;
            }
        }
    }
    return boxes;
}

// Liang-Barsky: the part of a-b inside the rectangle, with the line's opacity at its ends.
bool clipSegment(const Vertex& a, const Vertex& b, const Box& rect, TileSeg& out) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    double       t0 = 0, t1 = 1;
    auto         clip = [&](double p, double q) {
        if (std::fabs(p) < kEps)
            return q >= 0;
        const double r = q / p;
        if (p < 0) {
            if (r > t1)
                return false;
            t0 = std::max(t0, r);
        } else {
            if (r < t0)
                return false;
            t1 = std::min(t1, r);
        }
        return true;
    };
    if (!clip(-dx, a.x - rect.x0) || !clip(dx, rect.x1 - a.x) || !clip(-dy, a.y - rect.y0) || !clip(dy, rect.y1 - a.y) || t1 - t0 < kEps)
        return false;
    out.a      = {a.x + dx * t0, a.y + dy * t0};
    out.b      = {a.x + dx * t1, a.y + dy * t1};
    out.alphaA = float(lerp(a.alpha, b.alpha, t0));
    out.alphaB = float(lerp(a.alpha, b.alpha, t1));
    return true;
}

void addTile(Stroke& stroke, const Run& run, const Box& box, int reach) {
    TilePiece tile;
    tile.box = {box.x0, box.y0, box.w(), box.h()};
    const Box rect{box.x0 - reach, box.y0 - reach, box.x1 + reach, box.y1 + reach};
    for (size_t i = 0; i + 1 < run.pts.size(); ++i) {
        TileSeg seg;
        if (clipSegment(run.pts[i], run.pts[i + 1], rect, seg))
            tile.segs.push_back(seg);
    }
    if (!tile.segs.empty())
        stroke.tiles.push_back(std::move(tile));
}

// The strips of a horizontal or vertical segment: the band the line and its
// glow cover, minus the tiles.
void addStrips(Stroke& stroke, const Vertex& a, const Vertex& b, int reach, const std::vector<Box>& tiles) {
    const bool   flat  = horizontal(a, b);
    const double from  = flat ? a.x : a.y;
    const double to    = flat ? b.x : b.y;
    const double lo    = std::min(from, to), hi = std::max(from, to);
    const double c     = flat ? a.y : a.x;
    const Box    band  = flat ? Box{roundI(lo), floorI(c - reach), roundI(hi), ceilI(c + reach)} : Box{floorI(c - reach), roundI(lo), ceilI(c + reach), roundI(hi)};

    std::vector<Box> pieces{band};
    for (const Box& tile : tiles) {
        std::vector<Box> next;
        for (const Box& piece : pieces)
            subtract(piece, tile, next);
        pieces = std::move(next);
    }

    auto opacity = [&](double at) {
        const double t = std::clamp((at - from) / (to - from), 0.0, 1.0);
        return float(lerp(a.alpha, b.alpha, t));
    };

    for (const Box& piece : pieces) {
        const int len = flat ? piece.w() : piece.h();
        if (piece.empty() || len < 1)
            continue;
        StripPiece strip;
        strip.box      = {piece.x0, piece.y0, piece.w(), piece.h()};
        strip.vertical = !flat;
        strip.centre   = c;
        strip.a        = flat ? Vec2{double(piece.x0), c} : Vec2{c, double(piece.y0)};
        strip.b        = flat ? Vec2{double(piece.x1), c} : Vec2{c, double(piece.y1)};
        strip.alphaA   = opacity(flat ? piece.x0 : piece.y0);
        strip.alphaB   = opacity(flat ? piece.x1 : piece.y1);
        if (stroke.alpha * std::max(strip.alphaA, strip.alphaB) < kInvisible)
            continue;
        stroke.strips.push_back(strip);
    }
}

// The line across itself at distance `d` from its centre: how much of the pixel
// the line covers, and how bright the halo is (before the opacities).
void profile(const Look& look, double d, float& core, float& halo) {
    const double hw = look.halfWidth16 / 16.0, reach = look.reach16 / 16.0;
    core           = float(std::clamp(hw - d + 0.5, 0.0, 1.0));
    const double e = d - hw;
    if (e <= 0) {
        halo = 1;
    } else if (reach <= 0 || e >= reach) {
        halo = 0;
    } else {
        const double t = 1 - e / reach;
        halo           = float(t * t);
    }
}

// Writes a premultiplied pixel: the glow under the line, all of it scaled by
// `fade`, so that blending two such pixels linearly is exactly a fade.
void put(uint8_t* px, const Tone& tone, float core, float halo, float fade) {
    const float c = std::clamp(core * tone.line[3] / 255.f, 0.f, 1.f);
    const float h = std::clamp(halo * tone.glow[3] / 255.f, 0.f, 1.f);
    const float f = std::clamp(fade, 0.f, 1.f);
    const int   a = int(std::lround((c + (1 - c) * h) * f * 255));
    for (int k = 0; k < 3; ++k) {
        const float v = (c * tone.line[k] / 255.f + (1 - c) * h * tone.glow[k] / 255.f) * f;
        px[k]         = uint8_t(std::min(int(std::lround(std::clamp(v, 0.f, 1.f) * 255)), a));
    }
    px[3] = uint8_t(a);
}

class Key {
  public:
    Key& add(long long v) {
        m_s.append(reinterpret_cast<const char*>(&v), sizeof v);
        return *this;
    }
    Key& add(const Look& look) { return add(look.halfWidth16).add(look.reach16).add(look.lineAlpha).add(look.haloAlpha); }
    Key& add(const Tone& tone) {
        for (uint8_t v : tone.line)
            add(v);
        for (uint8_t v : tone.glow)
            add(v);
        return *this;
    }
    std::string str() { return std::move(m_s); }

  private:
    std::string m_s;
};

} // namespace

int roomAround(double halfWidth, double reach) {
    return int(std::ceil(halfWidth + reach + 1));
}

int alpha8(float alpha) {
    return int(std::lround(std::clamp(alpha, 0.f, 1.f) * 255));
}

Look lookOf(const Stroke& stroke) {
    return {round16(stroke.halfWidth), round16(stroke.reach), alpha8(stroke.alpha), alpha8(stroke.haloAlpha)};
}

Look lookOf(const Bead& bead) {
    return {round16(bead.radius), round16(bead.reach), 255, alpha8(bead.haloAlpha)};
}

Tone makeTone(const Rgba& line, const Rgba& glow, int levels) {
    levels         = std::max(levels, 1);
    auto quantised = [levels](float v) { return uint8_t(std::lround(std::lround(std::clamp(v, 0.f, 1.f) * levels) * 255.0 / levels)); };
    Tone tone;
    const float lc[4] = {line.r, line.g, line.b, line.a};
    const float gc[4] = {glow.r, glow.g, glow.b, glow.a};
    for (int k = 0; k < 4; ++k) {
        tone.line[k] = quantised(lc[k]);
        tone.glow[k] = quantised(gc[k]);
    }
    return tone;
}

void cutStroke(Stroke& stroke) {
    stroke.strips.clear();
    stroke.tiles.clear();
    const int reach = roomAround(stroke.halfWidth, stroke.reach);
    for (const Run& run : splitRuns(stroke)) {
        const std::vector<Box> boxes = tileBoxes(run, reach);
        for (const Box& box : boxes)
            addTile(stroke, run, box, reach);
        for (size_t i = 0; i + 1 < run.pts.size(); ++i)
            if (axisAligned(run.pts[i], run.pts[i + 1]))
                addStrips(stroke, run.pts[i], run.pts[i + 1], reach, boxes);
    }
}

double strokeLength(const Stroke& stroke) {
    const size_t n     = stroke.pts.size();
    const size_t count = stroke.closed ? n : (n ? n - 1 : 0);
    double       len   = 0;
    for (size_t i = 0; i < count; ++i) {
        const Vertex& a = stroke.pts[i];
        const Vertex& b = stroke.pts[(i + 1) % n];
        len += std::hypot(b.x - a.x, b.y - a.y);
    }
    return len;
}

StrokePoint strokePointAt(const Stroke& stroke, double s) {
    const size_t n = stroke.pts.size();
    if (n == 0)
        return {};
    const double total = strokeLength(stroke);
    if (n == 1 || total <= 0)
        return {{stroke.pts[0].x, stroke.pts[0].y}, stroke.pts[0].alpha};

    s = stroke.closed ? s - std::floor(s / total) * total : std::clamp(s, 0.0, total);
    const size_t count = stroke.closed ? n : n - 1;
    for (size_t i = 0; i < count; ++i) {
        const Vertex& a   = stroke.pts[i];
        const Vertex& b   = stroke.pts[(i + 1) % n];
        const double  len = std::hypot(b.x - a.x, b.y - a.y);
        if (s > len && i + 1 < count) {
            s -= len;
            continue;
        }
        const double t = len > 0 ? std::clamp(s / len, 0.0, 1.0) : 0;
        return {{lerp(a.x, b.x, t), lerp(a.y, b.y, t)}, float(lerp(a.alpha, b.alpha, t))};
    }
    return {{stroke.pts[0].x, stroke.pts[0].y}, stroke.pts[0].alpha};
}

Image bakeStrip(const StripSpec& spec) {
    Image     img;
    const int size = std::max(spec.size, 1);
    img.w          = spec.vertical ? size : 2;
    img.h          = spec.vertical ? 2 : size;
    img.rgba.assign(size_t(img.w) * size_t(img.h) * 4, 0);
    const double centre = spec.centre2 / 2.0;
    for (int k = 0; k < size; ++k) {
        float core = 0, halo = 0;
        profile(spec.look, std::fabs(k + 0.5 - centre), core, halo);
        for (int end = 0; end < 2; ++end) {
            const float  fade = (end ? spec.alphaB : spec.alphaA) / 255.f;
            const size_t at   = spec.vertical ? size_t(end) * size_t(size) + size_t(k) : size_t(k) * 2 + size_t(end);
            put(&img.rgba[at * 4], end ? spec.b : spec.a, core * spec.look.lineAlpha / 255.f, halo * spec.look.haloAlpha / 255.f, fade);
        }
    }
    return img;
}

std::string keyOf(const StripSpec& spec) {
    return Key().add(1).add(spec.vertical).add(spec.size).add(spec.centre2).add(spec.look).add(spec.a).add(spec.b).add(spec.alphaA).add(spec.alphaB).str();
}

StripSpec stripSpecOf(const StripPiece& strip, const Stroke& stroke, const Tone& a, const Tone& b) {
    StripSpec spec;
    spec.vertical = strip.vertical;
    spec.size     = strip.vertical ? strip.box.w : strip.box.h;
    spec.centre2  = int(std::lround(2 * (strip.centre - (strip.vertical ? strip.box.x : strip.box.y))));
    spec.look     = lookOf(stroke);
    spec.a        = a;
    spec.b        = b;
    spec.alphaA   = alpha8(strip.alphaA);
    spec.alphaB   = alpha8(strip.alphaB);
    return spec;
}

Quad stripQuad(const StripPiece& strip) {
    const double x = strip.box.x, y = strip.box.y, w = strip.box.w, h = strip.box.h;
    return strip.vertical ? Quad{x, y - h / 2, w, 2 * h} : Quad{x - w / 2, y, 2 * w, h};
}

TileSpec tileSpecOf(const TilePiece& tile, const Stroke& stroke, const Tone& tone) {
    TileSpec spec;
    spec.w    = tile.box.w;
    spec.h    = tile.box.h;
    spec.look = lookOf(stroke);
    spec.tone = tone;
    for (const TileSeg& s : tile.segs)
        spec.segs.push_back({round16(s.a.x - tile.box.x), round16(s.a.y - tile.box.y), round16(s.b.x - tile.box.x), round16(s.b.y - tile.box.y), alpha8(s.alphaA), alpha8(s.alphaB)});
    return spec;
}

Image bakeTile(const TileSpec& spec) {
    Image img;
    img.w = std::max(spec.w, 0);
    img.h = std::max(spec.h, 0);
    img.rgba.assign(size_t(img.w) * size_t(img.h) * 4, 0);
    if (img.empty())
        return img;

    // The nearest segment of every pixel, and the line's opacity at that point
    std::vector<float> nearest(size_t(img.w) * size_t(img.h), std::numeric_limits<float>::infinity());
    std::vector<float> fade(nearest.size(), 0.f);
    const double       pad = (spec.look.halfWidth16 + spec.look.reach16) / 16.0 + 1;
    for (const TileSegSpec& s : spec.segs) {
        const double ax = s.ax / 16.0, ay = s.ay / 16.0, dx = s.bx / 16.0 - ax, dy = s.by / 16.0 - ay;
        const double len2 = dx * dx + dy * dy;
        const int    i0   = std::max(0, floorI(std::min(ax, ax + dx) - pad)), i1 = std::min(img.w, ceilI(std::max(ax, ax + dx) + pad));
        const int    j0   = std::max(0, floorI(std::min(ay, ay + dy) - pad)), j1 = std::min(img.h, ceilI(std::max(ay, ay + dy) + pad));
        for (int j = j0; j < j1; ++j) {
            for (int i = i0; i < i1; ++i) {
                const double px = i + 0.5 - ax, py = j + 0.5 - ay;
                const double t  = len2 > 0 ? std::clamp((px * dx + py * dy) / len2, 0.0, 1.0) : 0.0;
                const double ex = px - dx * t, ey = py - dy * t;
                const float  d2 = float(ex * ex + ey * ey);
                const size_t at = size_t(j) * size_t(img.w) + size_t(i);
                if (d2 < nearest[at]) {
                    nearest[at] = d2;
                    fade[at]    = float(lerp(s.alphaA, s.alphaB, t) / 255.0);
                }
            }
        }
    }

    for (size_t at = 0; at < nearest.size(); ++at) {
        if (!std::isfinite(nearest[at]))
            continue;
        float core = 0, halo = 0;
        profile(spec.look, std::sqrt(double(nearest[at])), core, halo);
        put(&img.rgba[at * 4], spec.tone, core * spec.look.lineAlpha / 255.f, halo * spec.look.haloAlpha / 255.f, fade[at]);
    }
    return img;
}

std::string keyOf(const TileSpec& spec) {
    Key key;
    key.add(2).add(spec.w).add(spec.h).add(spec.look).add(spec.tone).add(static_cast<long long>(spec.segs.size()));
    for (const TileSegSpec& s : spec.segs)
        key.add(s.ax).add(s.ay).add(s.bx).add(s.by).add(s.alphaA).add(s.alphaB);
    return key.str();
}

int beadSize(const Look& look) {
    return 2 * int(std::ceil((look.halfWidth16 + look.reach16) / 16.0 + 1));
}

Image bakeBead(const BeadSpec& spec) {
    Image     img;
    const int n = beadSize(spec.look);
    img.w = img.h = n;
    img.rgba.assign(size_t(n) * size_t(n) * 4, 0);
    const double mid = n / 2.0;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            float core = 0, halo = 0;
            profile(spec.look, std::hypot(i + 0.5 - mid, j + 0.5 - mid), core, halo);
            put(&img.rgba[(size_t(j) * size_t(n) + size_t(i)) * 4], spec.tone, core * spec.look.lineAlpha / 255.f, halo * spec.look.haloAlpha / 255.f, 1.f);
        }
    }
    return img;
}

std::string keyOf(const BeadSpec& spec) {
    return Key().add(3).add(spec.look).add(spec.tone).str();
}

GlyphSize glyphSize(int height16, const Look& look) {
    if (height16 <= 0)
        return {};
    const double pad = (look.halfWidth16 + look.reach16) / 16.0 + 1;
    const double w   = height16 / 16.0 * kGlyphW / kGlyphH, h = height16 / 16.0;
    return {2 * ceilI(w / 2 + pad), 2 * ceilI(h / 2 + pad)};
}

Look lookOf(const GlyphFace& face) {
    return {round16(face.halfWidth), round16(face.reach), 255, alpha8(face.haloAlpha)};
}

GlyphSize glyphSize(const GlyphFace& face) {
    return glyphSize(round16(face.height), lookOf(face));
}

GlyphSpec glyphSpecOf(const GlyphFace& face, int glyph, const Tone& tone) {
    return {glyph, round16(face.height), lookOf(face), tone, alpha8(face.knock)};
}

namespace {

// The plate behind a glyph, in terms of the glyph's height: how far it reaches
// past the glyph's box on each side, how soft its edge is, and how round its corners.
constexpr double kPlatePadX    = 0.10;
constexpr double kPlatePadY    = 0.08;
constexpr double kPlateFeather = 0.16;
constexpr double kPlateRound   = 0.12;

// Puts black under an image of the glyph: a rounded box with half sides `hx` by
// `hy` around the image's middle, `feather` px soft over its edge, `strength`
// (0..1) dark. The image is premultiplied, so black under it adds opacity only.
void plateUnder(Image& img, double hx, double hy, double feather, double round, double strength) {
    // Never reach past the image: its border stays clear.
    hx = std::min(hx, img.w / 2.0 - 1 - feather / 2);
    hy = std::min(hy, img.h / 2.0 - 1 - feather / 2);
    if (hx <= 0 || hy <= 0 || strength <= 0)
        return;

    const double r = std::min({round, hx, hy});
    for (int j = 0; j < img.h; ++j) {
        for (int i = 0; i < img.w; ++i) {
            const double qx = std::fabs(i + 0.5 - img.w / 2.0) - (hx - r), qy = std::fabs(j + 0.5 - img.h / 2.0) - (hy - r);
            const double sd = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
            const double t  = std::clamp(0.5 - sd / feather, 0.0, 1.0);
            const double a  = strength * t * t * (3 - 2 * t);

            uint8_t* px = &img.rgba[(size_t(j) * size_t(img.w) + size_t(i)) * 4 + 3];
            *px         = uint8_t(std::lround(*px + 255.0 * a * (1.0 - *px / 255.0)));
        }
    }
}

} // namespace

Image bakeGlyph(const GlyphSpec& spec) {
    const GlyphSize size = glyphSize(spec.height16, spec.look);
    TileSpec        tile;
    tile.w    = size.w;
    tile.h    = size.h;
    tile.look = spec.look;
    tile.tone = spec.tone;
    if (size.w <= 0 || size.h <= 0)
        return bakeTile(tile);

    const double h = spec.height16 / 16.0, w = h * kGlyphW / kGlyphH;
    // Mirrored: the grid's left edge is the glyph's right.
    const auto at = [&](const GlyphPoint& p) {
        const double x = size.w / 2.0 + (0.5 - double(p.x) / kGlyphW) * w, y = size.h / 2.0 + (double(p.y) / kGlyphH - 0.5) * h;
        return std::pair{round16(x), round16(y)};
    };
    for (const GlyphStroke& stroke : glyphStrokes(spec.glyph)) {
        if (stroke.empty())
            continue;
        // A lone point is a dot: a segment that does not go anywhere.
        const size_t count = std::max<size_t>(stroke.size() - 1, 1);
        for (size_t i = 0; i < count; ++i) {
            const auto a = at(stroke[i]), b = at(stroke[std::min(i + 1, stroke.size() - 1)]);
            tile.segs.push_back({a.first, a.second, b.first, b.second, 255, 255});
        }
    }

    Image img = bakeTile(tile);
    if (spec.knock > 0)
        plateUnder(img, w / 2 + kPlatePadX * h, h / 2 + kPlatePadY * h, kPlateFeather * h, kPlateRound * h, spec.knock / 255.0);
    return img;
}

std::string keyOf(const GlyphSpec& spec) {
    return Key().add(4).add(glyphIndex(spec.glyph)).add(spec.height16).add(spec.look).add(spec.tone).add(spec.knock).str();
}

} // namespace smplhud
