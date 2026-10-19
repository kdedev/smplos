#include "sweep.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace smplhud {

namespace {

// `d` brought into [-length / 2, length / 2].
double wrapDelta(double d, double length) {
    d = std::fmod(d, length);
    if (d > length / 2)
        d -= length;
    else if (d < -length / 2)
        d += length;
    return d;
}

int floorDiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int ceilDiv(int a, int b) {
    return -floorDiv(-a, b);
}

} // namespace

Path buildPath(const Centreline& c) {
    Path path;
    if (c.w <= 0 || c.h <= 0)
        return path;

    const double x0 = c.x, y0 = c.y, x1 = c.x + c.w, y1 = c.y + c.h;
    const double leg   = std::clamp(c.leg, 0.0, std::min(c.w, c.h) / 2);
    const double power = std::max(1.0, c.power);
    const double e     = 2.0 / power;

    // A chamfer is one straight segment; anything rounder gets enough
    // segments that the polyline stays within a fraction of a px of the curve.
    const int steps = (power < 1.0001 || leg < 1.0) ? 1 : std::clamp(int(std::ceil(leg / 6)), 3, 12);

    // The corners in clockwise order, each from where it leaves the previous
    // edge (centre + leg * u) to where it joins the next (centre + leg * v).
    struct Corner {
        Vec2 centre, u, v;
    };
    const Corner corners[] = {
        {{x1 - leg, y0 + leg}, {0, -1}, {1, 0}},
        {{x1 - leg, y1 - leg}, {1, 0}, {0, 1}},
        {{x0 + leg, y1 - leg}, {0, 1}, {-1, 0}},
        {{x0 + leg, y0 + leg}, {-1, 0}, {0, -1}},
    };

    std::vector<Vec2> points;
    for (const Corner& k : corners) {
        for (int i = 0; i <= steps; ++i) {
            const double phi = (std::numbers::pi / 2) * i / steps;
            const double cu  = std::pow(std::cos(phi), e);
            const double cv  = std::pow(std::sin(phi), e);
            points.push_back({k.centre.x + leg * (k.u.x * cu + k.v.x * cv), k.centre.y + leg * (k.u.y * cu + k.v.y * cv)});
        }
    }

    // The straight edges are the segments between one corner and the next.
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec2   a   = points[i];
        const Vec2   b   = points[(i + 1) % points.size()];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-9)
            continue;
        path.segments.push_back({a, {(b.x - a.x) / len, (b.y - a.y) / len}, len, path.length});
        path.length += len;
    }
    return path;
}

Path buildPath(std::span<const Vertex> pts) {
    Path path;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vertex& a   = pts[i];
        const Vertex& b   = pts[(i + 1) % pts.size()];
        const double  len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-9)
            continue;
        path.segments.push_back({{a.x, a.y}, {(b.x - a.x) / len, (b.y - a.y) / len}, len, path.length, a.alpha, b.alpha});
        path.length += len;
    }
    return path;
}

PathPoint pointAt(const Path& path, double s) {
    if (!path.valid())
        return {};

    s = std::fmod(s, path.length);
    if (s < 0)
        s += path.length;

    const auto it = std::upper_bound(path.segments.begin(), path.segments.end(), s, [](double v, const Path::Segment& seg) { return v < seg.arc; });
    const Path::Segment& seg = *std::prev(it);
    const double         t   = std::min(s - seg.arc, seg.length);
    return {{seg.from.x + seg.dir.x * t, seg.from.y + seg.dir.y * t}, seg.dir};
}

int sweepTexel(double scale) {
    return std::clamp(int(std::lround(2 * scale)), 2, 6);
}

double sweepInterval(double speed) {
    constexpr double kStep = 3.5, kFastest = 0.016, kSlowest = 0.1;
    return speed > 0 ? std::clamp(kStep / speed, kFastest, kSlowest) : kSlowest;
}

double sweepAdvance(double phase, double seconds, double speed, double scale, double trackLength) {
    double next = phase;
    if (trackLength > 0 && seconds > 0 && speed > 0 && scale > 0)
        next += seconds * speed * scale / trackLength;

    next = std::fmod(next, 1.0);
    if (!(next >= 0)) // negative, or not a number
        next = next < 0 ? next + 1.0 : 0.0;
    return std::min(next, std::nextafter(1.0, 0.0));
}

double sweepFade(double level, bool focused, double seconds, double duration) {
    const double target = focused ? 1.0 : 0.0;
    level               = std::isfinite(level) ? std::clamp(level, 0.0, 1.0) : target;

    if (!(duration > 0))
        return target;
    if (!(seconds > 0))
        return level;

    const double step = seconds / duration;
    return focused ? std::min(level + step, 1.0) : std::max(level - step, 0.0);
}

double sweepIntensity(double level) {
    const double t = std::isfinite(level) ? std::clamp(level, 0.0, 1.0) : 0.0;
    return t * t * (3 - 2 * t);
}

IBox sweepBounds(const Path& path, double s, const SweepShape& shape, int texel, const IBox& limit) {
    const double reach = std::max(shape.reachOut, shape.reachIn);
    if (!path.valid() || shape.halfLength <= 0 || reach <= 0 || texel < 1)
        return {};

    const double half = std::min(shape.halfLength, path.length / 2);

    double minX = std::numeric_limits<double>::infinity(), minY = minX;
    double maxX = -minX, maxY = -minX;
    const auto include = [&](Vec2 p) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    };

    // The box of a polyline is the box of its corners, so the part of the
    // track under the glow is its two ends and the vertices between them.
    include(pointAt(path, s - half).pos);
    include(pointAt(path, s + half).pos);
    for (const Path::Segment& seg : path.segments) {
        if (std::abs(wrapDelta(seg.arc - s, path.length)) <= half)
            include(seg.from);
    }

    // The outermost texel centres sit half a texel inside the box edge; this
    // padding puts them beyond the reach, where the glow is zero.
    const double pad = reach + texel / 2.0 + 0.5;
    int          x0  = floorDiv(int(std::floor(minX - pad)), texel) * texel;
    int          y0  = floorDiv(int(std::floor(minY - pad)), texel) * texel;
    int          x1  = ceilDiv(int(std::ceil(maxX + pad)), texel) * texel;
    int          y1  = ceilDiv(int(std::ceil(maxY + pad)), texel) * texel;

    x0 = std::max(x0, ceilDiv(limit.x, texel) * texel);
    y0 = std::max(y0, ceilDiv(limit.y, texel) * texel);
    x1 = std::min(x1, floorDiv(limit.x + limit.w, texel) * texel);
    y1 = std::min(y1, floorDiv(limit.y + limit.h, texel) * texel);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

Sprite renderSweep(const Path& path, double s, const SweepShape& shape, const SweepColour& colour, int texel, const IBox& limit) {
    Sprite sprite;
    sprite.texel = std::max(1, texel);
    sprite.box   = sweepBounds(path, s, shape, sprite.texel, limit);
    if (sprite.box.w <= 0 || sprite.box.h <= 0)
        return sprite;

    const int    q      = sprite.texel;
    const double half   = std::min(shape.halfLength, path.length / 2);
    const double reach  = std::max(shape.reachOut, shape.reachIn);
    sprite.width        = sprite.box.w / q;
    sprite.height       = sprite.box.h / q;
    const size_t count  = size_t(sprite.width) * sprite.height;
    sprite.rgba.assign(count * 4, 0);

    // For every texel, the nearest point of the track: its squared distance,
    // where along the track it is, which side of the track the texel is on, and
    // how visible the line is there.
    struct Nearest {
        float  d2      = std::numeric_limits<float>::infinity();
        float  arc     = 0;
        float  fade    = 1;
        bool   outside = true;
    };
    std::vector<Nearest> nearest(count);

    // Segment by segment, only over the texels near enough to matter: a texel
    // farther than `reach` from every segment is dark whatever its nearest point.
    for (const Path::Segment& seg : path.segments) {
        const Vec2 to{seg.from.x + seg.dir.x * seg.length, seg.from.y + seg.dir.y * seg.length};

        const auto first = [&](double v, int origin) { return int(std::ceil((v - origin) / q - 0.5)); };
        const auto last  = [&](double v, int origin) { return int(std::floor((v - origin) / q - 0.5)); };
        const int  i0    = std::max(0, first(std::min(seg.from.x, to.x) - reach, sprite.box.x));
        const int  i1    = std::min(sprite.width - 1, last(std::max(seg.from.x, to.x) + reach, sprite.box.x));
        const int  j0    = std::max(0, first(std::min(seg.from.y, to.y) - reach, sprite.box.y));
        const int  j1    = std::min(sprite.height - 1, last(std::max(seg.from.y, to.y) + reach, sprite.box.y));

        for (int j = j0; j <= j1; ++j) {
            const double ry = sprite.box.y + (j + 0.5) * q - seg.from.y;
            for (int i = i0; i <= i1; ++i) {
                const double rx = sprite.box.x + (i + 0.5) * q - seg.from.x;
                const double t  = std::clamp(rx * seg.dir.x + ry * seg.dir.y, 0.0, seg.length);
                const double dx = rx - seg.dir.x * t, dy = ry - seg.dir.y * t;
                const float  d2 = float(dx * dx + dy * dy);

                Nearest& n = nearest[size_t(j) * sprite.width + i];
                if (d2 < n.d2) {
                    n.d2      = d2;
                    n.arc     = float(seg.arc + t);
                    n.fade    = float(seg.alphaFrom + (seg.alphaTo - seg.alphaFrom) * (t / seg.length));
                    n.outside = dx * seg.dir.y - dy * seg.dir.x >= 0; // the outward normal is (dir.y, -dir.x)
                }
            }
        }
    }

    const auto byte = [](double v) { return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    for (size_t k = 0; k < count; ++k) {
        const Nearest& n = nearest[k];
        const double   r = n.outside ? shape.reachOut : shape.reachIn;
        if (!(n.d2 < std::numeric_limits<float>::infinity()) || r <= 0)
            continue;

        const double across = std::sqrt(double(n.d2)) / r;
        const double along  = wrapDelta(double(n.arc) - s, path.length) / half;
        const double rho2   = across * across + along * along;
        if (rho2 >= 1)
            continue;

        // smooth falloff: flat at the centre and no visible edge
        const double intensity = (1 - rho2) * (1 - rho2);
        const double alpha     = std::clamp(double(shape.alpha) * colour.strength * intensity * std::clamp(double(n.fade), 0.0, 1.0), 0.0, 1.0);
        const double white     = std::clamp(double(shape.whiten) * intensity * intensity, 0.0, 1.0);

        uint8_t* px = &sprite.rgba[k * 4];
        px[0]       = byte((colour.r + (1 - colour.r) * white) * alpha);
        px[1]       = byte((colour.g + (1 - colour.g) * white) * alpha);
        px[2]       = byte((colour.b + (1 - colour.b) * white) * alpha);
        px[3]       = byte(alpha);
    }
    return sprite;
}

} // namespace smplhud
