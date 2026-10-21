#include "scanner.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace smplhud {

namespace {

constexpr double kPi = std::numbers::pi;

constexpr double kImageAspect = 0.75;
constexpr double kDiscAspect  = 0.42;
constexpr double kPad         = 3.5; // room round the disc for its glow, in u
constexpr int    kMaxWidth    = 2048;

// The plate of a pod, at scale 1, in px.
constexpr double kPlateReach = 5; // how far the halo of the bezel reaches out onto the window
constexpr double kPlateRim   = 7; // how far the lit rim just inside the bezel reaches in

constexpr double kLongestClock = 1e9; // seconds; a clock past that, 31 years, stays there

constexpr double kArmPeriod  = 4.4; // seconds a turn
constexpr double kPingPeriod = 5.8;
constexpr double kPingLength = 1.9;

// Also for NaN, which becomes 0.
double clamp01(double v) {
    return v > 0 ? (v < 1 ? v : 1) : 0;
}

// How far the bezel's halo reaches out onto the window at this scale, whole px.
int plateReachOf(double scale) {
    return std::max(1, int(std::lround(kPlateReach * scale)));
}

double mixd(double a, double b, double t) {
    return a + (b - a) * t;
}

// 0 below `e0`, 1 above `e1` and a smooth step between; falls if `e0` is the larger.
double smooth(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

uint32_t scramble(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// A number from 0 up to but not including 1 that depends on nothing but its arguments.
double chance(uint32_t seed, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    uint32_t h = scramble(seed ^ 0x9e3779b9U);
    h          = scramble(h + a * 0x85ebca6bU);
    h          = scramble(h + b * 0xc2b2ae35U);
    h          = scramble(h + c * 0x27d4eb2fU);
    return h / 4294967296.0;
}

double wrapTurn(double angle) {
    angle = std::fmod(angle, 2 * kPi);
    return angle < 0 ? angle + 2 * kPi : angle;
}

Rgba mixColour(const Rgba& a, const Rgba& b, double t) {
    return {float(mixd(a.r, b.r, t)), float(mixd(a.g, b.g, t)), float(mixd(a.b, b.b, t)), 1.f};
}

Rgba scaled(const Rgba& c, double k) {
    return {float(c.r * k), float(c.g * k), float(c.b * k), 1.f};
}

uint8_t levelOf(float v) {
    return uint8_t(std::lround(clamp01(v) * 31));
}

Rgba colourOf(const uint8_t (&level)[3]) {
    return {level[0] / 31.f, level[1] / 31.f, level[2] / 31.f, 1.f};
}

// A pixel being painted: premultiplied.
struct Px {
    double r = 0, g = 0, b = 0, a = 0;

    void over(const Rgba& c, double alpha) {
        alpha = clamp01(alpha);
        if (alpha <= 0)
            return;
        const double keep = 1 - alpha;
        r                 = c.r * alpha + r * keep;
        g                 = c.g * alpha + g * keep;
        b                 = c.b * alpha + b * keep;
        a                 = alpha + a * keep;
    }

    // Premultiplied bytes at `d`.
    void store(uint8_t* d) const {
        const int alpha = int(std::lround(clamp01(a) * 255));
        d[3]            = uint8_t(alpha);
        d[0]            = uint8_t(std::min(alpha, int(std::lround(clamp01(r) * 255))));
        d[1]            = uint8_t(std::min(alpha, int(std::lround(clamp01(g) * 255))));
        d[2]            = uint8_t(std::min(alpha, int(std::lround(clamp01(b) * 255))));
    }
};

// How much of a px is covered by a stroke `w` px wide whose middle is `d` px from the px's middle.
double strokeCover(double d, double w) {
    const double a = std::fabs(d);
    return clamp01(std::min(a + 0.5, w / 2) - std::max(a - 0.5, -w / 2));
}

// How much of a px is inside a shape whose edge is `d` px from the px's middle, negative inside.
double fillCover(double d) {
    return clamp01(0.5 - d);
}

// The distance to an ellipse with the radii a and b, negative inside. Exact on
// the axes and close elsewhere, which is as much as a px needs.
double ellipseDist(double dx, double dy, double a, double b) {
    const double f  = std::hypot(dx / a, dy / b);
    const double gx = dx / (a * a), gy = dy / (b * b);
    const double g  = std::hypot(gx, gy);
    if (g < 1e-12)
        return -std::min(a, b);
    return (f - 1) * f / g;
}

double segmentDist(double x, double y, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    const double t    = len2 > 0 ? clamp01(((x - ax) * dx + (y - ay) * dy) / len2) : 0;
    return std::hypot(x - (ax + t * dx), y - (ay + t * dy));
}

// A convex shape of up to 8 corners, kept as the lines of its sides.
constexpr int kMaxCorners = 8;

struct Poly {
    int    n = 0;
    double nx[kMaxCorners]{}, ny[kMaxCorners]{}, c[kMaxCorners]{};
};

Poly polyOf(const Vec2* v, int n) {
    Poly p;
    n   = std::clamp(n, 0, kMaxCorners);
    p.n = n;
    Vec2 mid{};
    for (int i = 0; i < n; ++i) {
        mid.x += v[i].x / n;
        mid.y += v[i].y / n;
    }
    for (int i = 0; i < n; ++i) {
        const Vec2   a = v[i], b = v[(i + 1) % n];
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double len = std::hypot(ex, ey);
        double       nx = len > 0 ? ey / len : 0, ny = len > 0 ? -ex / len : 0;
        if (nx * (mid.x - a.x) + ny * (mid.y - a.y) > 0) {
            nx = -nx;
            ny = -ny;
        }
        p.nx[i] = nx;
        p.ny[i] = ny;
        p.c[i]  = nx * a.x + ny * a.y;
    }
    return p;
}

// The distance to the sides of the shape, negative inside; the distance to its nearest side or just a bit under it outside.
double polyDist(const Poly& p, double x, double y) {
    double d = -1e30;
    for (int i = 0; i < p.n; ++i)
        d = std::max(d, p.nx[i] * x + p.ny[i] * y - p.c[i]);
    return d;
}

Poly markerShape(int shape, double mx, double my, double u) {
    switch (shape) {
        case 1: {
            const Vec2 v[3] = {{mx, my - 2.0 * u}, {mx - 2.0 * u, my + 1.6 * u}, {mx + 2.0 * u, my + 1.6 * u}};
            return polyOf(v, 3);
        }
        case 2: {
            const Vec2 v[3] = {{mx, my + 2.0 * u}, {mx - 2.0 * u, my - 1.6 * u}, {mx + 2.0 * u, my - 1.6 * u}};
            return polyOf(v, 3);
        }
        case 3: {
            const Vec2 v[4] = {{mx, my - 2.5 * u}, {mx + 2.5 * u, my}, {mx, my + 2.5 * u}, {mx - 2.5 * u, my}};
            return polyOf(v, 4);
        }
        default: {
            const Vec2 v[4] = {{mx - 1.5 * u, my - 1.5 * u}, {mx + 1.5 * u, my - 1.5 * u}, {mx + 1.5 * u, my + 1.5 * u}, {mx - 1.5 * u, my + 1.5 * u}};
            return polyOf(v, 4);
        }
    }
}

} // namespace

int scannerWidthOf(double themeValue) {
    if (!(themeValue > 0))
        return 0;
    return int(std::lround(std::clamp(themeValue, double(kScannerMinWidth), double(kScannerMaxWidth))));
}

ScannerSize scannerSize(double width, double scale) {
    if (!(width > 0) || !(scale > 0))
        return {};
    const int w = std::clamp(int(std::lround(width * scale)), 1, kMaxWidth);
    return {w, std::max(1, int(std::lround(w * kImageAspect)))};
}

ScannerLayout scannerLayout(const ScannerSize& size) {
    ScannerLayout l;
    if (size.w <= 0 || size.h <= 0)
        return l;

    l.w      = size.w;
    l.h      = size.h;
    l.u      = size.w / 48.0;
    l.rx     = (24.0 - kPad) * l.u;
    l.ry     = kDiscAspect * l.rx;
    l.centre = {size.w / 2.0, size.h - kPad * l.u - l.ry};
    return l;
}

ScannerSpec scannerSpecOf(const ScannerSize& size, double clock, uint32_t seed, const ScannerPalette& palette) {
    return scannerSpecOfFrame(size, clock > 0 ? long(std::floor(std::min(clock, kLongestClock) * kScannerFps)) : 0, seed, palette);
}

ScannerSpec scannerSpecOfFrame(const ScannerSize& size, long frame, uint32_t seed, const ScannerPalette& palette) {
    ScannerSpec s;
    s.size  = size;
    s.frame = std::clamp(frame, 0L, long(kLongestClock * kScannerFps));
    s.seed  = seed;

    const Rgba* from[3] = {&palette.line, &palette.bright, &palette.glow};
    uint8_t*    to[3]   = {s.line, s.bright, s.glow};
    for (int i = 0; i < 3; ++i) {
        to[i][0] = levelOf(from[i]->r);
        to[i][1] = levelOf(from[i]->g);
        to[i][2] = levelOf(from[i]->b);
    }
    return s;
}

double scannerArm(double clock, uint32_t seed) {
    const double turns = clock / kArmPeriod + chance(seed, 0x51);
    return 2 * kPi * (turns - std::floor(turns));
}

std::vector<ScannerContact> scannerContacts(const ScannerLayout& l, double clock, uint32_t seed) {
    std::vector<ScannerContact> out;
    if (!l.valid())
        return out;

    const double arm = scannerArm(clock, seed);

    for (int s = 0; s < kScannerSlots; ++s) {
        const double period = 8.0 + 4.0 * chance(seed, 0x100 + uint32_t(s));
        const double t      = clock / period + chance(seed, 0x200 + uint32_t(s));
        const double whole  = std::floor(t);
        const double p      = t - whole;
        const auto   cycle  = uint32_t(int64_t(whole));
        const auto   r      = [&](uint32_t k) { return chance(seed, 0x1000 + uint32_t(s), k, cycle); };

        // Most things come at the ship from far ahead, as they do when it flies; some go by across. Each slot has a
        // lane of its own, so that the contacts spread over the disc instead of clumping.
        const double lane = (s + 0.5) / kScannerSlots * 2 - 1; // -0.75 .. 0.75 for 4 slots
        double       ax, ay, bx, by;
        if (r(0) < 0.7) {
            const double drift = lane * 0.9 + (r(1) - 0.5) * 0.3, spread = (r(2) - 0.5) * 0.3;
            ax = drift - spread / 2;
            bx = drift + spread / 2;
            ay = -0.95;
            by = 0.95;
        } else {
            const double dir = r(1) < 0.5 ? 1 : -1, level = lane * 0.6 + (r(2) - 0.5) * 0.2, tilt = (r(3) - 0.5) * 0.3;
            ax = -0.95 * dir;
            bx = 0.95 * dir;
            ay = level - tilt / 2;
            by = level + tilt / 2;
        }

        const double px = mixd(ax, bx, p), py = mixd(ay, by, p);
        const double rho = std::hypot(px, py);

        const double life  = smooth(0, 0.06, p) * (1 - smooth(0.94, 1, p));
        const double alpha = life * (1 - smooth(0.74, 0.92, rho));
        if (alpha < 0.002)
            continue;

        ScannerContact c;
        c.slot  = s;
        c.foot  = {l.centre.x + px * l.rx, l.centre.y + py * l.ry};
        c.alpha = alpha;
        c.size  = 1 + 0.15 * std::clamp(py, -1.0, 1.0);

        // Near the front rim a marker below the disc would hang under it like a leg, so it goes above there.
        const double up = (py > 0.4 || r(5) < 0.6) ? 1.0 : -1.0, height = (2.6 + 5.4 * r(6)) * (up > 0 ? 1.0 : 0.7);
        const double room = 3.5 * l.u + 1.5; // the marker and its halo stay inside the image
        c.marker = {c.foot.x, std::clamp(c.foot.y - up * height * l.u, room, std::max(room, l.h - room))};

        const double v = r(7);
        c.shape        = v < 0.36 ? 0 : (v < 0.64 ? 1 : (v < 0.82 ? 2 : 3));

        const double lag = wrapTurn(arm - std::atan2(py, px));
        c.flash          = std::exp(-lag / 0.55);

        out.push_back(c);
    }

    std::sort(out.begin(), out.end(), [](const ScannerContact& a, const ScannerContact& b) { return a.foot.y < b.foot.y; });
    return out;
}

Image renderScanner(const ScannerSpec& spec) {
    Image               img;
    const ScannerLayout l = scannerLayout(spec.size);
    if (!l.valid())
        return img;

    img.w = l.w;
    img.h = l.h;
    img.rgba.assign(size_t(l.w) * size_t(l.h) * 4, 0);

    const double u = l.u, cx = l.centre.x, cy = l.centre.y, rx = l.rx, ry = l.ry;
    const double clock = double(spec.frame) / kScannerFps;

    const Rgba line = colourOf(spec.line), bright = colourOf(spec.bright), glow = colourOf(spec.glow);
    const Rgba plateLow = scaled(line, 0.12), plateRim = scaled(line, 0.42), lipColour = scaled(line, 0.28);
    const Rgba white = mixColour(bright, {1.f, 1.f, 1.f, 1.f}, 0.75);

    const double arm  = scannerArm(clock, spec.seed);
    const double armX = cx + rx * std::cos(arm), armY = cy + ry * std::sin(arm);

    const std::vector<ScannerContact> contacts = scannerContacts(l, clock, spec.seed);

    // The pieces of each contact, made once for the frame.
    struct Marker {
        ScannerContact c;
        Poly           shape;
        double         x0, y0, x1, y1; // where it can be seen, in px
    };
    std::vector<Marker> markers;
    markers.reserve(contacts.size());
    for (const ScannerContact& c : contacts) {
        Marker m;
        m.c     = c;
        m.shape = markerShape(c.shape, c.marker.x, c.marker.y, u * c.size);
        m.x0    = std::min(c.foot.x, c.marker.x) - 3.6 * u - 1;
        m.x1    = std::max(c.foot.x, c.marker.x) + 3.6 * u + 1;
        m.y0    = std::min(c.foot.y, c.marker.y) - 3.6 * u - 1;
        m.y1    = std::max(c.foot.y, c.marker.y) + 3.6 * u + 1;
        markers.push_back(m);
    }

    // The ship: an arrowhead that sways a little as it flies, with a flame that flickers.
    const double roll  = 0.2 * std::sin(2 * kPi * 0.17 * clock) + 0.07 * std::sin(2 * kPi * 0.61 * clock + 1.0);
    const double sway  = 1.1 * u * std::sin(2 * kPi * 0.11 * clock + 0.5);
    const double bob   = 0.5 * u * std::sin(2 * kPi * 0.23 * clock + 1.3);
    const double flick = 0.5 + 0.5 * std::sin(2 * kPi * 3.1 * clock) * std::sin(2 * kPi * 1.7 * clock + 0.6);
    const auto   shipAt = [&](double x, double y) -> Vec2 {
        const double c = std::cos(roll), s = std::sin(roll);
        return {cx + sway + (x * c - y * s) * u, cy + bob + (x * s + y * c) * u};
    };
    const Vec2 nose = shipAt(0, -3.7), left = shipAt(-2.6, 2.1), right = shipAt(2.6, 2.1), notch = shipAt(0, 0.7);
    const Poly wingL = [&] { const Vec2 v[3] = {nose, left, notch}; return polyOf(v, 3); }();
    const Poly wingR = [&] { const Vec2 v[3] = {nose, right, notch}; return polyOf(v, 3); }();
    const double flame = (1.4 + 1.6 * flick) * u;
    const Poly   tail  = [&] { const Vec2 v[3] = {shipAt(-0.9, 1.5), shipAt(0.9, 1.5), {shipAt(0, 1.5).x, shipAt(0, 1.5).y + flame}}; return polyOf(v, 3); }();

    // An expanding ring now and then, as if the ship sent out a pulse.
    const double pingAt   = std::fmod(clock + kPingPeriod * chance(spec.seed, 0x77), kPingPeriod);
    const double pingX    = pingAt / kPingLength;
    const bool   pinging  = pingAt < kPingLength;
    const double pingRho  = pinging ? 1 - (1 - pingX) * (1 - pingX) : 0;
    const double pingFade = pinging ? 0.6 * std::pow(1 - pingX, 1.3) : 0;

    // The halo fades out a whole px before the edge of the image, so that the image has no hard edge to show.
    const double glowReach = std::min((kPad - 0.5) * u, kPad * u - 1.0);
    const double lipShift  = 1.5 * u;

    const auto drawMarker = [&](Px& out, const Marker& m, double x, double y) {
        if (x < m.x0 || x > m.x1 || y < m.y0 || y > m.y1)
            return;

        const double alpha = m.c.alpha, flash = m.c.flash;

        // the place on the disc, the stalk up to the marker and the marker
        out.over(line, fillCover(ellipseDist(x - m.c.foot.x, y - m.c.foot.y, 1.7 * u, 0.8 * u)) * 0.75 * alpha);
        out.over(line, strokeCover(segmentDist(x, y, m.c.foot.x, m.c.foot.y, m.c.marker.x, m.c.marker.y), 0.7 * u) * (0.55 + 0.25 * flash) * alpha);

        if (flash > 0.04) {
            const double d = std::hypot(x - m.c.marker.x, y - m.c.marker.y) / (3.0 * u * m.c.size);
            if (d < 1)
                out.over(bright, 0.35 * flash * alpha * (1 - d) * (1 - d));
        }

        const Rgba&  body = m.c.shape == 2 ? white : (m.c.shape == 3 ? mixColour(line, bright, 0.5) : mixColour(line, bright, 0.8));
        const double d    = polyDist(m.shape, x, y);
        const double cov  = m.c.shape == 3 ? strokeCover(d, 0.9 * u) : fillCover(d);
        out.over(body, cov * (0.72 + 0.28 * flash) * alpha);
    };

    for (int py = 0; py < l.h; ++py) {
        for (int pxi = 0; pxi < l.w; ++pxi) {
            const double x = pxi + 0.5, y = py + 0.5;
            const double dx = x - cx, dy = y - cy;
            Px           out;

            const double dDisc  = ellipseDist(dx, dy, rx, ry);
            const double inside = fillCover(dDisc);
            const double rho    = std::hypot(dx / rx, dy / ry);
            const double front  = clamp01(0.5 + 0.5 * (dy / ry) / std::max(rho, 1e-6));
            const double lit    = mixd(0.62, 1.0, front); // the near side of a ring is brighter than the far one

            if (glowReach > 0 && dDisc > -1 && dDisc < glowReach) {
                const double t = std::max(dDisc, 0.0) / glowReach;
                out.over(glow, 0.55 * std::pow(1 - t, 2.2));
            }

            out.over(lipColour, fillCover(ellipseDist(dx, dy - lipShift, rx, ry)) * 0.85);

            if (inside > 0) {
                out.over(mixColour(plateLow, plateRim, smooth(0.55, 1.0, rho)), inside * mixd(0.5, 0.7, rho * rho));

                const double lag = wrapTurn(arm - std::atan2(dy / ry, dx / rx));
                if (lag < 2.4) {
                    const double k = 1 - lag / 2.4;
                    out.over(mixColour(line, bright, std::exp(-lag * 5)), inside * 0.36 * k * k * (0.35 + 0.65 * std::min(rho, 1.0)));
                }
            }

            out.over(line, strokeCover(ellipseDist(dx, dy, rx * 0.33, ry * 0.33), 0.7 * u) * 0.45 * lit);
            out.over(line, strokeCover(ellipseDist(dx, dy, rx * 0.66, ry * 0.66), 0.8 * u) * 0.58 * lit);

            if (pinging && pingRho > 0.02)
                out.over(bright, strokeCover(ellipseDist(dx, dy, rx * pingRho, ry * pingRho), 0.9 * u) * pingFade * (rho < 1 ? 1 : 0));

            out.over(line, strokeCover(dDisc, 1.2 * u) * 0.95 * lit);

            if (inside > 0) {
                const double along = std::min(rho, 1.0);
                out.over(bright, strokeCover(segmentDist(x, y, cx, cy, armX, armY), 0.9 * u) * 0.9 * (0.4 + 0.6 * along) * inside);
            }

            for (const Marker& m : markers)
                if (m.c.foot.y <= cy)
                    drawMarker(out, m, x, y);

            const double shipR = std::hypot(dx - sway, dy - bob) / (3.4 * u);
            if (shipR < 1)
                out.over(glow, 0.4 * (1 - shipR) * (1 - shipR));
            out.over(bright, fillCover(polyDist(tail, x, y)) * 0.8 * (0.55 + 0.45 * flick));
            // the wings share the middle edge, so their covers add up to the whole there
            out.over(bright, fillCover(polyDist(wingL, x, y)) + fillCover(polyDist(wingR, x, y)));

            for (const Marker& m : markers)
                if (m.c.foot.y > cy)
                    drawMarker(out, m, x, y);

            uint8_t* d = &img.rgba[(size_t(py) * size_t(l.w) + size_t(pxi)) * 4];
            out.store(d);
        }
    }

    return img;
}

IBox plateBox(const Pod& pod, double scale) {
    const size_t n = pod.plate.size();
    if (!pod.on || n < 4 || n > size_t(kMaxCorners) || pod.bezel < 1 || size_t(pod.bezel) >= n || !(scale > 0))
        return {};

    const int reach = plateReachOf(scale);

    double minX = pod.plate[0].x, maxX = minX, minY = pod.plate[0].y, maxY = minY;
    for (const Vec2& v : pod.plate) {
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y);
        maxY = std::max(maxY, v.y);
    }
    // Whole px, with room for the halo, so that where the image goes does not depend on where the window is.
    const int x0 = int(std::floor(minX)) - reach, y0 = int(std::floor(minY)) - reach;
    const int x1 = int(std::ceil(maxX)) + reach, y1 = int(std::ceil(maxY)) + reach;
    return {x0, y0, x1 - x0, y1 - y0};
}

Plate plateOf(const Pod& pod, double scale, const ScannerPalette& palette, double glass) {
    Plate      out;
    const IBox box = plateBox(pod, scale);
    if (box.w <= 0 || box.h <= 0)
        return out;

    PlateSpec& s = out.spec;
    s.w          = box.w;
    s.h          = box.h;
    for (const Vec2& v : pod.plate)
        s.pts.push_back({v.x - box.x, v.y - box.y});
    s.bezel       = pod.bezel;
    s.halfWidth16 = std::max(1, int(std::lround(pod.halfWidth * 16)));
    s.reach       = plateReachOf(scale);
    s.glass       = uint8_t(std::lround(clamp01(glass) * 255));

    const Rgba* from[2] = {&palette.line, &palette.glow};
    uint8_t*    to[2]   = {s.line, s.glow};
    for (int i = 0; i < 2; ++i) {
        to[i][0] = levelOf(from[i]->r);
        to[i][1] = levelOf(from[i]->g);
        to[i][2] = levelOf(from[i]->b);
    }

    out.box = box;
    return out;
}

Image renderPlate(const PlateSpec& s) {
    Image        img;
    const size_t n = s.pts.size();
    if (s.w <= 0 || s.h <= 0 || n < 4 || n > size_t(kMaxCorners) || s.bezel < 1 || size_t(s.bezel) >= n)
        return img;

    img.w = s.w;
    img.h = s.h;
    img.rgba.assign(size_t(s.w) * size_t(s.h) * 4, 0);

    const Poly   poly  = polyOf(s.pts.data(), int(n));
    const double hw    = s.halfWidth16 / 16.0;
    const double reach = std::max(1, s.reach), rim = reach * (kPlateRim / kPlateReach);
    const double glass = s.glass / 255.0;
    const Rgba   line = colourOf(s.line), glow = colourOf(s.glow), tint = scaled(line, 0.07);

    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            const double px = x + 0.5, py = y + 0.5;

            // How far the px is outside each side. The sides on the outline are moved in by the thickness of its
            // line, which the frame draws; the sides of the bezel are not, as the bezel is drawn here.
            double best = -1e30, plain = -1e30;
            for (int i = 0; i < poly.n; ++i) {
                const double d = poly.nx[i] * px + poly.ny[i] * py - poly.c[i];
                plain          = std::max(plain, d);
                best           = std::max(best, i < s.bezel ? d : d + hw);
            }

            // how far the px is from the bezel itself, the sides it has and no more
            double near = 1e30;
            for (int i = 0; i < s.bezel; ++i)
                near = std::min(near, segmentDist(px, py, s.pts[size_t(i)].x, s.pts[size_t(i)].y, s.pts[size_t(i) + 1].x, s.pts[size_t(i) + 1].y));

            Px out;

            // the halo of the bezel, out on the window
            if (plain > 0 && near < reach) {
                const double k = 1 - near / reach;
                out.over(glow, kPlateHaloAlpha * k * k);
            }

            const double fill = fillCover(best);
            out.over(tint, fill * glass);

            // the rim lit just inside the bezel
            if (near < rim) {
                const double k = 1 - near / rim;
                out.over(line, fill * 0.16 * k * k);
            }

            // the line of the bezel, as thick as the outline's and just inside the edge of the plate
            double cover = 0;
            for (int i = 0; i < s.bezel; ++i) {
                const double d = poly.nx[i] * px + poly.ny[i] * py - poly.c[i];
                cover          = std::max(cover, std::min(strokeCover(d + hw, 2 * hw), fillCover(best)));
            }
            out.over(line, cover * 0.95);

            out.store(&img.rgba[(size_t(y) * size_t(s.w) + size_t(x)) * 4]);
        }
    }

    return img;
}

} // namespace smplhud
