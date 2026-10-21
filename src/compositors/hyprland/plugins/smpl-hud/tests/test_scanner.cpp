#include "../scanner.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <vector>

using namespace smplhud;

namespace {

constexpr double kPi = 3.14159265358979323846;

ScannerPalette amber() {
    return {{1.0f, 0.70f, 0.10f, 1.f}, {1.0f, 0.85f, 0.45f, 1.f}, {1.0f, 0.55f, 0.05f, 1.f}};
}

ScannerPalette green() {
    return {{0.10f, 0.90f, 0.20f, 1.f}, {0.60f, 1.00f, 0.70f, 1.f}, {0.05f, 0.70f, 0.10f, 1.f}};
}

ScannerSpec specOf(double width, double scale, double clock, uint32_t seed, const ScannerPalette& palette = amber()) {
    return scannerSpecOf(scannerSize(width, scale), clock, seed, palette);
}

int channel(const Image& img, int x, int y, int k) {
    return img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4 + size_t(k)];
}

// Premultiplied: no colour is above the alpha.
bool premultiplied(const Image& img) {
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            for (int k = 0; k < 3; ++k)
                if (channel(img, x, y, k) > channel(img, x, y, 3))
                    return false;
    return true;
}

// The outermost px all round are clear, so a hard edge of the image never shows.
bool edgeClear(const Image& img) {
    for (int x = 0; x < img.w; ++x)
        if (channel(img, x, 0, 3) || channel(img, x, img.h - 1, 3))
            return false;
    for (int y = 0; y < img.h; ++y)
        if (channel(img, 0, y, 3) || channel(img, img.w - 1, y, 3))
            return false;
    return true;
}

void testSizes() {
    CHECK(scannerWidthOf(0) == 0);
    CHECK(scannerWidthOf(-5) == 0);
    CHECK(scannerWidthOf(std::nan("")) == 0);
    CHECK(scannerWidthOf(1) == kScannerMinWidth);
    CHECK(scannerWidthOf(23.9) == kScannerMinWidth);
    CHECK(scannerWidthOf(48) == 48);
    CHECK(scannerWidthOf(48.4) == 48);
    CHECK(scannerWidthOf(160) == 160);
    CHECK(scannerWidthOf(1000) == kScannerMaxWidth);
    CHECK(scannerWidthOf(std::numeric_limits<double>::infinity()) == kScannerMaxWidth);

    CHECK((scannerSize(48, 1) == ScannerSize{48, 36}));
    CHECK((scannerSize(48, 2) == ScannerSize{96, 72}));
    CHECK((scannerSize(48, 1.25) == ScannerSize{60, 45}));
    CHECK((scannerSize(48, 1.5) == ScannerSize{72, 54}));
    CHECK((scannerSize(0, 1) == ScannerSize{}));
    CHECK((scannerSize(48, 0) == ScannerSize{}));
    CHECK((scannerSize(48, -1) == ScannerSize{}));
    CHECK((scannerSize(std::nan(""), 1) == ScannerSize{}));
    CHECK((scannerSize(48, std::nan("")) == ScannerSize{}));
    CHECK((scannerSize(160, 100) == ScannerSize{2048, 1536}));
    CHECK((scannerSize(0.01, 1) == ScannerSize{1, 1}));
}

void testLayout() {
    CHECK(!scannerLayout({}).valid());
    CHECK(!scannerLayout({10, 0}).valid());

    const ScannerLayout l = scannerLayout(scannerSize(48, 1));
    CHECK(l.valid());
    CHECK_NEAR(l.u, 1, 1e-12);
    CHECK_NEAR(l.centre.x, 24, 1e-12);
    CHECK_NEAR(l.rx, 20.5, 1e-12);
    CHECK_NEAR(l.ry / l.rx, 0.42, 1e-12);

    // the disc has room round it for its halo, and above it for the markers
    CHECK(l.centre.x - l.rx >= 3);
    CHECK(l.centre.y + l.ry <= l.h - 3);
    CHECK(l.centre.y - l.ry > l.h / 3.0);

    // everything is in units of the width
    const ScannerLayout l2 = scannerLayout(scannerSize(48, 2));
    CHECK_NEAR(l2.u, 2 * l.u, 1e-12);
    CHECK_NEAR(l2.rx, 2 * l.rx, 1e-9);
    CHECK_NEAR(l2.ry, 2 * l.ry, 1e-9);
    CHECK_NEAR(l2.centre.x, 2 * l.centre.x, 1e-9);
    CHECK_NEAR(l2.centre.y, 2 * l.centre.y, 1e-9);
}

void testSpec() {
    const ScannerSize size = scannerSize(48, 1);

    // The colours are in 31 steps and the time in frames, which is how the picture is kept as long as it can be.
    const ScannerSpec a = scannerSpecOf(size, 1.001, 3, amber());
    CHECK(a.line[0] == 31);
    CHECK(a.line[1] == 22);
    CHECK(a.line[2] == 3);
    CHECK(a.frame == 30);
    CHECK(a.seed == 3);
    CHECK(a == scannerSpecOf(size, 1.02, 3, amber()));
    CHECK(!(a == scannerSpecOf(size, 1.04, 3, amber())));
    CHECK(!(a == scannerSpecOf(size, 1.001, 4, amber())));
    CHECK(!(a == scannerSpecOf(scannerSize(48, 2), 1.001, 3, amber())));

    ScannerPalette p = amber();
    p.line.g         = 0.702f;
    CHECK(a == scannerSpecOf(size, 1.001, 3, p)); // inside the same step: nothing to bake again
    p.line.g = 0.76f;
    CHECK(!(a == scannerSpecOf(size, 1.001, 3, p)));

    // anything may come in
    p = amber();
    p.line   = {2.f, -1.f, std::nanf(""), 1.f};
    p.bright = {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.5f, 1.f};
    const ScannerSpec odd = scannerSpecOf(size, 1.0, 0, p);
    CHECK(odd.line[0] == 31 && odd.line[1] == 0 && odd.line[2] == 0);
    CHECK(odd.bright[0] == 31 && odd.bright[1] == 0 && odd.bright[2] == 16);

    CHECK(scannerSpecOf(size, -5, 0, amber()).frame == 0);
    CHECK(scannerSpecOf(size, std::nan(""), 0, amber()).frame == 0);
    CHECK(scannerSpecOf(size, 1.0, 0, amber()).frame == 30);
    CHECK(scannerSpecOf(size, std::numeric_limits<double>::infinity(), 0, amber()).frame > 0);
    CHECK(!renderScanner(scannerSpecOf(size, std::numeric_limits<double>::infinity(), 0, amber())).empty());
}

void testRenderIsPure() {
    CHECK(renderScanner(ScannerSpec{}).empty());
    CHECK(renderScanner(scannerSpecOf({}, 1, 1, amber())).empty());

    const ScannerSpec a   = specOf(48, 1, 3.3, 7);
    const Image       one = renderScanner(a);
    CHECK(!one.empty());
    CHECK(one.w == 48 && one.h == 36);
    CHECK(one.rgba.size() == 48u * 36u * 4u);
    CHECK(one.rgba == renderScanner(a).rgba);
    CHECK(one.rgba == renderScanner(specOf(48, 1, 3.3 + 0.01, 7)).rgba); // the same frame

    CHECK(one.rgba != renderScanner(specOf(48, 1, 3.3 + 0.5, 7)).rgba); // time passes
    CHECK(one.rgba != renderScanner(specOf(48, 1, 3.3, 8)).rgba);       // windows are out of step with each other
    CHECK(one.rgba != renderScanner(specOf(48, 1, 3.3, 7, green())).rgba);
}

void testImagesAreSound() {
    int images = 0;
    for (double width : {24.0, 48.0, 96.0})
        for (double scale : {1.0, 1.5, 2.0})
            for (uint32_t seed : {0u, 1u, 0xdeadbeefu})
                for (double clock : {0.0, 0.5, 1.9, 3.3, 7.0, 11.5, 29.7, 123.4}) {
                    const Image img = renderScanner(specOf(width, scale, clock, seed));
                    ++images;
                    CHECK(!img.empty());
                    CHECK(premultiplied(img));
                    if (!edgeClear(img)) {
                        std::fprintf(stderr, "edge not clear: width %g scale %g seed %u clock %g\n", width, scale, seed, clock);
                        ++g_failures;
                    }
                }
    CHECK(images == 216);
}

void testOddSizes() {
    for (int w : {1, 2, 3, 5, 8, 13, 23, 24, 25, 200, 640}) {
        const ScannerSpec spec = scannerSpecOf({w, std::max(1, int(std::lround(w * 0.75)))}, 1, 5, amber());
        const Image       img  = renderScanner(spec);
        CHECK(img.w == w);
        CHECK(img.h == std::max(1, int(std::lround(w * 0.75))));
        CHECK(img.rgba.size() == size_t(img.w) * size_t(img.h) * 4);
        CHECK(premultiplied(img));
    }

    // an image that is not three quarters as high as it is wide
    const Image flat = renderScanner(scannerSpecOf({48, 10}, 2, 5, amber()));
    CHECK(flat.w == 48 && flat.h == 10 && premultiplied(flat));

    // the ends of the colour range
    const Rgba     white{1.f, 1.f, 1.f, 1.f}, black{0.f, 0.f, 0.f, 1.f};
    const Image    light = renderScanner(scannerSpecOf(scannerSize(48, 1), 2.5, 1, {white, white, white}));
    const Image    dark  = renderScanner(scannerSpecOf(scannerSize(48, 1), 2.5, 1, {black, black, black}));
    CHECK(premultiplied(light) && premultiplied(dark) && edgeClear(light) && edgeClear(dark));
}

void testPicture() {
    const ScannerSize   size = scannerSize(96, 1);
    const ScannerLayout l    = scannerLayout(size);
    const int           cx = int(l.centre.x), cy = int(l.centre.y);
    const int           frontY = int(std::floor(l.centre.y + l.ry)), backY = int(std::floor(l.centre.y - l.ry));

    int dimmerBack = 0;
    for (double clock : {0.0, 1.3, 5.1, 9.9, 20.2}) {
        const Image img = renderScanner(scannerSpecOf(size, clock, 2, amber()));

        // the ship is solid in the middle and in the colour of the theme
        CHECK(channel(img, cx, cy, 3) == 255);
        CHECK(channel(img, cx, cy, 0) >= channel(img, cx, cy, 1) && channel(img, cx, cy, 1) >= channel(img, cx, cy, 2));
        CHECK(channel(img, cx, cy, 0) > 200);

        // the near side of the rim is brighter than the far side, unless the ship's pulse is on it just then
        CHECK(channel(img, cx, frontY, 3) >= 235);
        CHECK(channel(img, cx, frontY, 3) > channel(img, cx, backY, 3));
        dimmerBack += channel(img, cx, frontY, 3) > channel(img, cx, backY, 3) + 20;

        // and it takes the hue of the line: red over green over blue
        CHECK(channel(img, cx, frontY, 0) > channel(img, cx, frontY, 1) && channel(img, cx, frontY, 1) > channel(img, cx, frontY, 2));

        // the corner of the image holds nothing
        CHECK(channel(img, 0, 0, 3) == 0);
        CHECK(channel(img, size.w - 1, 0, 3) == 0);
        CHECK(channel(img, 0, size.h - 1, 3) == 0);
        CHECK(channel(img, size.w - 1, size.h - 1, 3) == 0);
    }
    CHECK(dimmerBack >= 4);

    // another theme, another hue
    const Image img = renderScanner(scannerSpecOf(size, 2.0, 2, green()));
    CHECK(channel(img, cx, frontY, 1) > channel(img, cx, frontY, 0));
    CHECK(channel(img, cx, cy, 1) > channel(img, cx, cy, 0));

    // it is a flat disc: its width is much more than its height
    int top = size.h, bottom = -1, left = size.w, right = -1;
    const Image flat = renderScanner(scannerSpecOf(size, 0.0, 2, amber()));
    for (int y = 0; y < size.h; ++y)
        for (int x = 0; x < size.w; ++x)
            if (channel(flat, x, y, 3) >= 200) {
                left   = std::min(left, x);
                right  = std::max(right, x);
                top    = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    CHECK(right - left > 0.8 * size.w);
    CHECK(bottom - top < 0.6 * size.h);
}

// The same picture at another size, give or take the finer lines.
void testScalesWithSize() {
    for (double clock : {0.0, 4.0, 9.0}) {
        const Image small = renderScanner(specOf(48, 1, clock, 3));
        const Image big   = renderScanner(specOf(48, 2, clock, 3));
        CHECK(big.w == 2 * small.w && big.h == 2 * small.h);

        double diff = 0;
        for (int y = 0; y < small.h; ++y)
            for (int x = 0; x < small.w; ++x)
                for (int k = 0; k < 4; ++k) {
                    double mean = 0;
                    for (int j = 0; j < 4; ++j)
                        mean += channel(big, 2 * x + j % 2, 2 * y + j / 2, k) / 4.0;
                    diff += std::fabs(mean - channel(small, x, y, k));
                }
        diff /= small.w * small.h * 4.0;
        if (!(diff < 12)) {
            std::fprintf(stderr, "scales apart: mean difference %g at clock %g\n", diff, clock);
            ++g_failures;
        }
    }
}

void testContacts() {
    CHECK(scannerContacts(scannerLayout({}), 3, 1).empty());

    for (double width : {24.0, 48.0, 160.0}) {
        const ScannerLayout l    = scannerLayout(scannerSize(width, 1));
        const double        room = 3.5 * l.u + 1.5;
        for (uint32_t seed : {0u, 1u, 99u, 0xdeadbeefu})
            for (double clock = 0; clock < 60; clock += 0.37) {
                const auto contacts = scannerContacts(l, clock, seed);
                CHECK(int(contacts.size()) <= kScannerSlots);

                std::set<int> slots;
                double        last = -1e9;
                for (const ScannerContact& c : contacts) {
                    CHECK(c.slot >= 0 && c.slot < kScannerSlots);
                    CHECK(slots.insert(c.slot).second);
                    CHECK(c.alpha > 0 && c.alpha <= 1);
                    CHECK(c.flash >= 0 && c.flash <= 1);
                    CHECK(c.size >= 0.85 - 1e-9 && c.size <= 1.15 + 1e-9);
                    CHECK(c.shape >= 0 && c.shape <= 3);
                    CHECK(c.foot.y >= last); // those at the back come first
                    last = c.foot.y;

                    // on the disc, and with room for the marker above or below it
                    CHECK(std::hypot((c.foot.x - l.centre.x) / l.rx, (c.foot.y - l.centre.y) / l.ry) < 0.93);
                    CHECK_NEAR(c.marker.x, c.foot.x, 1e-12);
                    CHECK(c.marker.y >= room - 1e-9 && c.marker.y <= l.h - room + 1e-9);
                }
            }
    }
}

void testContactsMoveSmoothly() {
    const ScannerLayout l = scannerLayout(scannerSize(96, 1));
    for (uint32_t seed : {0u, 5u, 123456u}) {
        auto before = scannerContacts(l, 0, seed);
        for (int f = 1; f < 30 * 40; ++f) {
            const auto now = scannerContacts(l, f / 30.0, seed);
            for (const ScannerContact& a : now)
                for (const ScannerContact& b : before) {
                    if (a.slot != b.slot || a.alpha < 0.05 || b.alpha < 0.05)
                        continue;
                    CHECK(std::hypot(a.foot.x - b.foot.x, a.foot.y - b.foot.y) < 0.02 * l.rx);
                    CHECK(std::fabs(a.alpha - b.alpha) < 0.15);
                    CHECK(a.shape == b.shape);
                }
            before = now;
        }
    }
}

void testContactsAreLitByTheArm() {
    const ScannerLayout l = scannerLayout(scannerSize(96, 1));
    int                 lit = 0, dark = 0;
    for (uint32_t seed : {0u, 7u})
        for (double clock = 0; clock < 90; clock += 0.21) {
            const double arm = scannerArm(clock, seed);
            for (const ScannerContact& c : scannerContacts(l, clock, seed)) {
                const double angle = std::atan2((c.foot.y - l.centre.y) / l.ry, (c.foot.x - l.centre.x) / l.rx);
                double       lag   = std::fmod(arm - angle, 2 * kPi);
                if (lag < 0)
                    lag += 2 * kPi;
                CHECK_NEAR(c.flash, std::exp(-lag / 0.55), 1e-9);
                lit += c.flash > 0.8;
                dark += c.flash < 0.05;
            }
        }
    CHECK(lit > 20);
    CHECK(dark > 20);
}

void testContactsVary() {
    const ScannerLayout l = scannerLayout(scannerSize(48, 1));
    std::set<int>       shapes;
    bool                above = false, below = false;
    double              total = 0;
    int                 samples = 0;
    for (double clock = 0; clock < 240; clock += 0.5) {
        const auto contacts = scannerContacts(l, clock, 11);
        total += double(contacts.size());
        ++samples;
        for (const ScannerContact& c : contacts) {
            shapes.insert(c.shape);
            above = above || c.marker.y < c.foot.y;
            below = below || c.marker.y > c.foot.y;
        }
    }
    CHECK(shapes.size() == 4);
    CHECK(above);
    CHECK(below);
    CHECK(total / samples > 1.5);
    CHECK(total / samples < double(kScannerSlots));
}

void testArm() {
    for (uint32_t seed : {0u, 3u}) {
        double last = scannerArm(0, seed);
        for (int i = 1; i <= 400; ++i) {
            const double now = scannerArm(i * 0.1, seed);
            CHECK(now >= 0 && now < 2 * kPi);
            double step = now - last;
            if (step < 0)
                step += 2 * kPi;
            CHECK_NEAR(step, 2 * kPi * 0.1 / 4.4, 1e-9); // a turn in 4.4 s, the way a clock's hands go
            last = now;
        }
    }
    CHECK(scannerArm(0, 1) != scannerArm(0, 2));
}

int alphaAt(const Image& img, int x, int y) {
    return channel(img, x, y, 3);
}

// A real layout of the panel style with a pod, which is where a plate comes from.
Layout podLayout(int width, int height, float scale, double lineWidth = 2) {
    Params p;
    p.width          = width;
    p.height         = height;
    p.scale          = scale;
    p.cut            = int(std::lround(21 * scale));
    p.roundingPower  = 1.f;
    p.gap            = 15;
    p.lineWidth      = lineWidth;
    p.chamfer        = 10;
    p.floatGlowRange = 14;

    const ScannerSize size = scannerSize(72, scale);
    p.scannerW             = size.w;
    p.scannerH             = size.h;

    const Style* style = styleFor("panel");
    CHECK(style);
    return style ? buildLayout(*style, p) : Layout{};
}

// A pod by hand, in px from the window's top left corner: a plate with the bezel along its left and top side and
// the cut corner between them.
Pod handPod() {
    Pod pod;
    pod.on        = true;
    pod.plate     = {{205.5, 367.25}, {205.5, 314.25}, {214.5, 305.25}, {287.5, 305.25}, {295.5, 313.25}, {295.5, 361.25}, {281.5, 375.25}, {213.5, 375.25}};
    pod.bezel     = 3;
    pod.halfWidth = 1;
    return pod;
}

// The same plate as an image, so that what is drawn can be told by where its px are: 90 x 70 px with the bezel along
// the left and the top side and the cut corner between them, the outline round the rest, and clear px all round.
constexpr int    kHandPad = 8;
constexpr int    kHandW = 90 + 2 * kHandPad, kHandH = 70 + 2 * kHandPad;
constexpr double kHandCorners[8][2] = {{0, 62}, {0, 9}, {9, 0}, {82, 0}, {90, 8}, {90, 56}, {76, 70}, {8, 70}};

uint8_t stepOf(float v) {
    return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 31));
}

PlateSpec handSpec(const ScannerPalette& palette = amber(), int glass = 217, int halfWidth16 = 16) {
    PlateSpec s;
    s.w           = kHandW;
    s.h           = kHandH;
    s.bezel       = 3;
    s.halfWidth16 = halfWidth16;
    s.reach       = 5;
    s.glass       = uint8_t(glass);
    for (const auto& c : kHandCorners)
        s.pts.push_back({kHandPad + c[0], kHandPad + c[1]});

    const Rgba* from[2] = {&palette.line, &palette.glow};
    uint8_t*    to[2]   = {s.line, s.glow};
    for (int i = 0; i < 2; ++i) {
        to[i][0] = stepOf(from[i]->r);
        to[i][1] = stepOf(from[i]->g);
        to[i][2] = stepOf(from[i]->b);
    }
    return s;
}

// How far a point is outside a convex shape, by the side it is furthest beyond; negative inside.
double outsideBy(const std::vector<Vec2>& v, double x, double y) {
    Vec2 mid;
    for (const Vec2& p : v) {
        mid.x += p.x / double(v.size());
        mid.y += p.y / double(v.size());
    }
    double worst = -1e30;
    for (size_t i = 0; i < v.size(); ++i) {
        const Vec2   a = v[i], b = v[(i + 1) % v.size()];
        double       nx = b.y - a.y, ny = a.x - b.x;
        const double len = std::hypot(nx, ny);
        nx /= len;
        ny /= len;
        if (nx * (mid.x - a.x) + ny * (mid.y - a.y) > 0) {
            nx = -nx;
            ny = -ny;
        }
        worst = std::max(worst, nx * (x - a.x) + ny * (y - a.y));
    }
    return worst;
}

void testPlateBox() {
    const Pod pod = handPod();

    // the plate's own bounds in whole px, with room all round for the halo of the bezel
    const struct {
        double scale;
        int    reach;
    } scales[] = {{1, 5}, {1.25, 6}, {1.5, 8}, {2, 10}, {0.5, 3}, {0.1, 1}, {0.05, 1}, {0.001, 1}};
    for (const auto& s : scales) {
        const IBox box = plateBox(pod, s.scale);
        CHECK(box.x == 205 - s.reach);
        CHECK(box.y == 305 - s.reach);
        CHECK(box.x + box.w == 296 + s.reach);
        CHECK(box.y + box.h == 376 + s.reach);
    }

    // which sides are the bezel does not change where the image goes
    for (int bezel = 1; bezel < 8; ++bezel) {
        Pod p   = pod;
        p.bezel = bezel;
        CHECK((plateBox(p, 1) == IBox{200, 300, 101, 81}));
    }

    // without a pod, a plate that can be drawn or a scale there is no box and no image
    const auto nothing = [](const Pod& p, double scale) {
        const Plate plate = plateOf(p, scale, amber(), 0.85);
        return plateBox(p, scale) == IBox{} && plate.box == IBox{} && !plate.valid();
    };
    CHECK(!nothing(pod, 1));
    CHECK(nothing(Pod{}, 1));

    Pod bad = pod;
    bad.on  = false;
    CHECK(nothing(bad, 1));
    for (int bezel : {-1, 0, 8, 9}) {
        bad       = pod;
        bad.bezel = bezel;
        CHECK(nothing(bad, 1));
    }
    for (size_t n : {0u, 1u, 3u, 9u, 12u}) {
        bad = pod;
        bad.plate.resize(n);
        CHECK(nothing(bad, 1));
    }
    for (double scale : {0.0, -1.0, std::nan("")})
        CHECK(nothing(pod, scale));

    // from 4 to 8 corners, and a bezel of at least a side and fewer than all of them
    for (size_t n = 4; n <= 8; ++n)
        for (int bezel = 1; bezel < int(n); ++bezel) {
            Pod p = pod;
            p.plate.resize(n);
            p.bezel = bezel;
            CHECK(!nothing(p, 1));
        }

    // whole px: the box holds the plate and the halo, and is no bigger than that and a px
    const IBox box = plateBox(pod, 1);
    CHECK(box.x <= 205.5 - 5 && box.x + box.w >= 295.5 + 5);
    CHECK(box.y <= 305.25 - 5 && box.y + box.h >= 375.25 + 5);
    CHECK(box.w < (295.5 - 205.5) + 2 * 5 + 2);
    CHECK(box.h < (375.25 - 305.25) + 2 * 5 + 2);
}

void testPlateOf() {
    const Pod   pod   = handPod();
    const Plate plate = plateOf(pod, 1, amber(), kPlateGlassFlat);
    CHECK(plate.valid());
    CHECK((plate.box == IBox{200, 300, 101, 81}));

    // the image is the box, and the corners are in it, as many as the pod's
    const PlateSpec& s = plate.spec;
    CHECK(s.w == 101 && s.h == 81);
    CHECK(s.pts.size() == pod.plate.size());
    for (size_t i = 0; i < s.pts.size() && i < pod.plate.size(); ++i) {
        CHECK_NEAR(s.pts[i].x, pod.plate[i].x - 200, 1e-12);
        CHECK_NEAR(s.pts[i].y, pod.plate[i].y - 300, 1e-12);
    }
    CHECK(s.bezel == 3 && s.halfWidth16 == 16 && s.reach == 5 && s.glass == 217);
    CHECK(s.line[0] == 31 && s.line[1] == 22 && s.line[2] == 3);
    CHECK(s.glow[0] == 31 && s.glow[1] == 17 && s.glow[2] == 2);

    // a bigger scale has more room for the halo
    const Plate big = plateOf(pod, 2, amber(), kPlateGlassFlat);
    CHECK((big.box == IBox{195, 295, 111, 91}));
    CHECK(big.spec.reach == 10 && big.spec.w == 111 && big.spec.h == 91);
    CHECK_NEAR(big.spec.pts[0].x, 205.5 - 195, 1e-12);
    CHECK_NEAR(big.spec.pts[0].y, 367.25 - 295, 1e-12);

    // how opaque the glass is, as a byte
    const struct {
        double glass;
        int    byte;
    } glasses[] = {{0, 0}, {1, 255}, {0.5, 128}, {kPlateGlassFlat, 217}, {kPlateGlassBlurred, 158}, {-1, 0}, {2, 255}, {std::nan(""), 0}};
    for (const auto& g : glasses)
        CHECK(plateOf(pod, 1, amber(), g.glass).spec.glass == g.byte);

    // the thickness of the line, in sixteenths of a px, never less than one
    const struct {
        double half;
        int    sixteenths;
    } lines[] = {{1, 16}, {1.5, 24}, {2, 32}, {0.5, 8}, {0, 1}, {-3, 1}, {0.01, 1}};
    for (const auto& l : lines) {
        Pod p       = pod;
        p.halfWidth = l.half;
        CHECK(plateOf(p, 1, amber(), kPlateGlassFlat).spec.halfWidth16 == l.sixteenths);
    }

    // the colours are in 31 steps, like the scanner's, and only the line and the glow are the plate's
    ScannerPalette p = amber();
    p.line.g         = 0.702f;
    CHECK(plateOf(pod, 1, p, kPlateGlassFlat).spec == plate.spec);
    p.bright = {0.f, 0.f, 1.f, 1.f};
    CHECK(plateOf(pod, 1, p, kPlateGlassFlat).spec == plate.spec);
    p.line.g = 0.76f;
    CHECK(!(plateOf(pod, 1, p, kPlateGlassFlat).spec == plate.spec));
    p = amber();
    p.glow.r = 0.5f;
    CHECK(!(plateOf(pod, 1, p, kPlateGlassFlat).spec == plate.spec));
    CHECK(!(plateOf(pod, 1, green(), kPlateGlassFlat).spec == plate.spec));
    CHECK(!(plateOf(pod, 1, amber(), kPlateGlassBlurred).spec == plate.spec));

    // anything may come in
    p      = amber();
    p.line = {2.f, -1.f, std::nanf(""), 1.f};
    p.glow = {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.5f, 1.f};
    const PlateSpec odd = plateOf(pod, 1, p, kPlateGlassFlat).spec;
    CHECK(odd.line[0] == 31 && odd.line[1] == 0 && odd.line[2] == 0);
    CHECK(odd.glow[0] == 31 && odd.glow[1] == 0 && odd.glow[2] == 16);
}

// What the plate is made of does not depend on where the window is or how big it is, only on its scale and the theme.
void testPlateOfWindow() {
    const int width[]  = {900, 1280, 1920, 3840};
    const int height[] = {600, 720, 1080, 2160};
    for (const float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        Plate first;
        for (int i = 0; i < 4; ++i) {
            const Layout lay = podLayout(width[i], height[i], scale);
            CHECK(lay.pod.on);
            const Plate plate = plateOf(lay.pod, scale, amber(), kPlateGlassFlat);
            CHECK(plate.valid());
            if (i == 0) {
                first = plate;
                continue;
            }
            CHECK(plate.spec == first.spec);

            // and the image goes as far beyond the corner of the window
            CHECK(plate.box.x + plate.box.w - width[i] == first.box.x + first.box.w - width[0]);
            CHECK(plate.box.y + plate.box.h - height[i] == first.box.y + first.box.h - height[0]);
        }
    }
}

// The plate of the pod a layout makes: where it goes, and that it holds the scanner.
void testPlateOfARealPod() {
    CHECK((scannerSize(72, 1) == ScannerSize{72, 54}));
    CHECK((scannerSize(72, 2) == ScannerSize{144, 108}));

    const int W = 1000, H = 700;
    for (const float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        for (const double lineWidth : {2.0, 3.0}) {
            const Layout lay = podLayout(W, H, scale, lineWidth);
            CHECK(lay.valid && lay.pod.on);
            const Plate plate = plateOf(lay.pod, scale, amber(), kPlateGlassFlat);
            CHECK(plate.valid());
            if (!plate.valid())
                continue;

            // The picture is on the plate and the plate pokes out beyond the corner of the window, where
            // the decoration reaches.
            const IBox        pic = lay.pod.picture;
            const ScannerSize sz  = scannerSize(72, scale);
            CHECK(pic.w == sz.w && pic.h == sz.h);
            CHECK(plate.box.x <= pic.x && plate.box.y <= pic.y);
            CHECK(plate.box.x + plate.box.w >= pic.x + pic.w && plate.box.y + plate.box.h >= pic.y + pic.h);
            CHECK(plate.box.x > 0 && plate.box.y > 0);
            CHECK(plate.box.x + plate.box.w > W && plate.box.y + plate.box.h > H);

            const Extents& e = lay.extents;
            CHECK(plate.box.x >= -int(std::ceil(e.left * scale)));
            CHECK(plate.box.y >= -int(std::ceil(e.top * scale)));
            CHECK(plate.box.x + plate.box.w <= W + int(std::ceil(e.right * scale)));
            CHECK(plate.box.y + plate.box.h <= H + int(std::ceil(e.bottom * scale)));

            // the bezel is as thick as the outline
            const int bs = std::max(1, int(std::lround(lineWidth)));
            const int T  = std::max(1, int(std::round(float(bs) * scale)));
            CHECK(plate.spec.halfWidth16 == 8 * T);

            // the image is sound, and its outermost px are clear but for a trace of the halo
            const Image img = renderPlate(plate.spec);
            CHECK(img.w == plate.box.w && img.h == plate.box.h);
            CHECK(premultiplied(img));
            int loudest = 0;
            for (int x = 0; x < img.w; ++x)
                loudest = std::max({loudest, alphaAt(img, x, 0), alphaAt(img, x, img.h - 1)});
            for (int y = 0; y < img.h; ++y)
                loudest = std::max({loudest, alphaAt(img, 0, y), alphaAt(img, img.w - 1, y)});
            CHECK(loudest <= 2);

            // the glass is under the whole disc of the scanner
            const ScannerLayout sl = scannerLayout(sz);
            const double        ox = pic.x - plate.box.x, oy = pic.y - plate.box.y;
            const Vec2          spots[] = {
                {sl.centre.x - sl.rx, sl.centre.y}, {sl.centre.x + sl.rx, sl.centre.y}, {sl.centre.x, sl.centre.y - sl.ry}, {sl.centre.x, sl.centre.y + sl.ry}, sl.centre};
            for (const Vec2& v : spots) {
                const int x = int(std::floor(ox + v.x)), y = int(std::floor(oy + v.y));
                CHECK(x >= 0 && x < img.w && y >= 0 && y < img.h);
                if (x >= 0 && x < img.w && y >= 0 && y < img.h)
                    CHECK(alphaAt(img, x, y) >= int(plate.spec.glass) - 1);
            }
        }
    }
}

void testPlatePicture() {
    const PlateSpec spec = handSpec();
    const Image     img  = renderPlate(spec);
    CHECK(img.w == kHandW && img.h == kHandH);
    CHECK(img.rgba.size() == size_t(kHandW) * size_t(kHandH) * 4);
    CHECK(premultiplied(img));
    CHECK(edgeClear(img));
    CHECK(img.rgba == renderPlate(spec).rgba);

    const int left = kHandPad, top = kHandPad, right = kHandPad + 90, bottom = kHandPad + 70;
    const int midX = kHandPad + 45, midY = kHandPad + 35;
    const int glass = spec.glass;

    // dark glass in the middle
    CHECK(std::abs(alphaAt(img, midX, midY) - glass) <= 1);
    CHECK(channel(img, midX, midY, 0) * 8 < alphaAt(img, midX, midY));

    // The bezel is a line of the outline's thickness just inside the edge of the plate, along the left side, the
    // top and the cut corner between them.
    for (int d = 0; d < 2; ++d) {
        CHECK(alphaAt(img, left + d, midY) >= 235 && channel(img, left + d, midY, 0) >= 230);
        CHECK(alphaAt(img, midX, top + d) >= 235 && channel(img, midX, top + d, 0) >= 230);
    }
    CHECK(alphaAt(img, left + 2, midY) < 235);
    CHECK(alphaAt(img, midX, top + 2) < 235);
    CHECK(alphaAt(img, left + 5, top + 5) >= 235);
    CHECK(alphaAt(img, left - 2, top - 2) == 0); // the corner that is cut off

    // its halo dims away outside it, in the colour of the glow: kPlateHaloAlpha of it at the bezel, fading with the
    // square of the distance until `reach` px from it, and not as opaque as the glass
    for (int d = 1; d <= 5; ++d) {
        const double k = 1 - (d - 0.5) / spec.reach;
        const int    a = alphaAt(img, left - d, midY);
        CHECK_NEAR(a, 255 * kPlateHaloAlpha * k * k, 1);
        CHECK(alphaAt(img, midX, top - d) == a);
    }
    CHECK(alphaAt(img, left - 1, midY) <= int(kPlateHaloAlpha * 255) + 1);
    CHECK(alphaAt(img, left - 6, midY) == 0);
    CHECK(channel(img, left - 1, midY, 0) == alphaAt(img, left - 1, midY));
    CHECK_NEAR(double(channel(img, left - 1, midY, 1)) / alphaAt(img, left - 1, midY), 17.0 / 31, 0.06);

    // the outline is the frame's: the glass stops where its line begins and nothing is drawn beyond
    CHECK(alphaAt(img, right - 2, midY) >= glass - 1);
    CHECK(alphaAt(img, right - 1, midY) == 0 && alphaAt(img, right, midY) == 0 && alphaAt(img, right + 1, midY) == 0);
    CHECK(alphaAt(img, midX, bottom - 2) >= glass - 1);
    CHECK(alphaAt(img, midX, bottom - 1) == 0 && alphaAt(img, midX, bottom) == 0 && alphaAt(img, midX, bottom + 1) == 0);

    // A rim lit just inside the bezel, over the glass: 16 % of the line's colour at its brightest, fading with the
    // square of the distance until 7 px from the bezel. Without glass it is all there is, and no halo is under it.
    const int interior = channel(img, midX, midY, 0);
    for (const int g : {0, glass}) {
        const Image  lit = renderPlate(handSpec(amber(), g));
        const double a   = g / 255.0;
        for (int d = 2; d < 10; ++d) {
            const double k = std::max(0.0, 1 - (d + 0.5) / 7), rim = 0.16 * k * k;
            CHECK_NEAR(alphaAt(lit, left + d, midY), 255 * (a + rim * (1 - a)), 1);
            CHECK_NEAR(channel(lit, left + d, midY, 0), 255 * (rim + 0.07 * a * (1 - rim)), 1);
        }
    }
    CHECK(channel(img, left + 2, midY, 0) > interior + 8);
    CHECK(channel(img, left + 7, midY, 0) == interior);

    // the colours are the theme's
    const Image other = renderPlate(handSpec(green()));
    CHECK(channel(img, left, midY, 0) > channel(other, left, midY, 0) + 100);
    CHECK(channel(other, left, midY, 1) > channel(img, left, midY, 1) + 20);
    CHECK(alphaAt(other, midX, midY) == alphaAt(img, midX, midY));
}

void testPlateLine() {
    const int left = kHandPad, right = kHandPad + 90, midX = kHandPad + 45, midY = kHandPad + 35;

    // The bezel is as thick as the outline's line, and the glass stops where the line of the outline begins,
    // which is that much inside its middle.
    const struct {
        int half16, px;
    } lines[] = {{8, 1}, {16, 2}, {24, 3}, {32, 4}};
    for (const auto& l : lines) {
        const Image img = renderPlate(handSpec(amber(), 217, l.half16));
        int         across = 0, down = 0;
        for (int i = 0; i < kHandPad + 8; ++i) {
            across += alphaAt(img, i, midY) >= 235;
            down += alphaAt(img, midX, i) >= 235;
        }
        CHECK(across == l.px);
        CHECK(down == l.px);

        if (l.half16 % 16 == 0) {
            const int half = l.half16 / 16;
            CHECK(alphaAt(img, right - half - 1, midY) >= 217 - 1);
            CHECK(alphaAt(img, right - half, midY) == 0);
        }
        CHECK(alphaAt(img, left - 1, midY) <= int(kPlateHaloAlpha * 255) + 1);
    }
}

void testPlateGlass() {
    const int midX = kHandPad + 45, midY = kHandPad + 35;

    // the glass is as opaque as it is asked, and nothing else changes with it but the rim and the line over it
    const Image reference = renderPlate(handSpec());
    for (int glass : {0, 64, 128, 217, 255}) {
        const Image img = renderPlate(handSpec(amber(), glass));
        CHECK(premultiplied(img));
        CHECK(std::abs(alphaAt(img, midX, midY) - glass) <= 1);
        CHECK(alphaAt(img, kHandPad, midY) >= 235);
        for (int d = 1; d <= 6; ++d)
            CHECK(alphaAt(img, kHandPad - d, midY) == alphaAt(reference, kHandPad - d, midY));
    }
    CHECK(alphaAt(renderPlate(handSpec(amber(), 0)), kHandPad + 2, midY) < 40); // only the rim is left of the glass

    // Where the compositor blurs what lies under the glass it takes a px for glass if it is more opaque than half
    // way between the halo and the glass; so the halo has to stay below that and the glass above it.
    const double    stencil = 0.5 * (kPlateHaloAlpha + kPlateGlassBlurred) * 255;
    const PlateSpec spec    = handSpec(amber(), int(std::lround(kPlateGlassBlurred * 255)));
    const Image     img     = renderPlate(spec);
    int             glassPx = 0, haloPx = 0, loud = 0, dim = 0, stray = 0;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const double out = outsideBy(spec.pts, x + 0.5, y + 0.5);
            const int    a   = alphaAt(img, x, y);
            if (out > spec.reach + 0.5)
                stray += a != 0;
            if (out >= 1) {
                haloPx += a > 0;
                loud += a > stencil;
            }
            if (out <= -3) {
                ++glassPx;
                dim += a <= stencil;
            }
        }
    CHECK(glassPx > 1000);
    CHECK(haloPx > 100);
    CHECK(stray == 0); // no halo further out than its reach
    CHECK(loud == 0);  // the halo is not taken for glass
    CHECK(dim == 0);   // the glass, and what is over it, is
}

// Whatever the shape, with the bezel on as many of its sides as asked for.
void testPlateShapes() {
    for (int n = 4; n <= 8; ++n) {
        int before = 0;
        for (int bezel = 1; bezel < n; ++bezel) {
            PlateSpec s = handSpec();
            s.w         = 100;
            s.h         = 80;
            s.pts.clear();
            for (int i = 0; i < n; ++i) {
                const double a = -kPi / 2 + 2 * kPi * i / n;
                s.pts.push_back({50 + 30 * std::cos(a), 40 + 30 * std::sin(a)});
            }
            s.bezel = bezel;

            const Image img = renderPlate(s);
            CHECK(img.w == 100 && img.h == 80);
            CHECK(premultiplied(img) && edgeClear(img));
            CHECK(alphaAt(img, 50, 40) >= int(s.glass) - 1);
            CHECK(alphaAt(img, 0, 0) == 0 && alphaAt(img, 99, 79) == 0);

            // each side that is the bezel has a line of its own
            int bright = 0;
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x)
                    bright += alphaAt(img, x, y) >= 235;
            CHECK(bright > before);
            before = bright;
        }
    }
}

void testPlateInvalid() {
    CHECK(!renderPlate(handSpec()).empty());
    CHECK(renderPlate(PlateSpec{}).empty());

    PlateSpec s = handSpec();
    s.w         = 0;
    CHECK(renderPlate(s).empty());
    s   = handSpec();
    s.h = 0;
    CHECK(renderPlate(s).empty());
    s   = handSpec();
    s.w = -4;
    CHECK(renderPlate(s).empty());
    for (size_t n : {0u, 3u, 9u}) {
        s = handSpec();
        s.pts.resize(n);
        CHECK(renderPlate(s).empty());
    }
    for (int bezel : {-1, 0, 8, 12}) {
        s       = handSpec();
        s.bezel = bezel;
        CHECK(renderPlate(s).empty());
    }
    for (int bezel = 1; bezel < 8; ++bezel) {
        s       = handSpec();
        s.bezel = bezel;
        const Image img = renderPlate(s);
        CHECK(!img.empty() && premultiplied(img));
    }

    // a reach or a line that is nothing, or too much, still makes an image
    for (int reach : {0, -3, 1000}) {
        s       = handSpec();
        s.reach = reach;
        const Image img = renderPlate(s);
        CHECK(!img.empty() && premultiplied(img));
    }
    for (int half16 : {0, -16, 1000}) {
        s             = handSpec();
        s.halfWidth16 = half16;
        const Image img = renderPlate(s);
        CHECK(!img.empty() && premultiplied(img));
    }
}

void testSpecOfFrame() {
    const ScannerSize size = scannerSize(72, 1);

    // a frame is the clock in frames, rounded down
    CHECK(scannerSpecOfFrame(size, 30, 3, amber()) == scannerSpecOf(size, 1.001, 3, amber()));
    CHECK(scannerSpecOfFrame(size, 0, 3, amber()) == scannerSpecOf(size, 0, 3, amber()));
    CHECK(scannerSpecOfFrame(size, 5, 3, amber()).frame == 5);
    CHECK(scannerSpecOfFrame(size, 5, 7, amber()).seed == 7);
    CHECK(scannerSpecOfFrame(size, 5, 7, amber()).size == size);
    CHECK(!(scannerSpecOfFrame(size, 5, 3, amber()) == scannerSpecOfFrame(size, 6, 3, amber())));

    // and stays between the first and the last
    const long last = long(1e9 * kScannerFps);
    CHECK(scannerSpecOfFrame(size, -1, 0, amber()).frame == 0);
    CHECK(scannerSpecOfFrame(size, std::numeric_limits<long>::min(), 0, amber()).frame == 0);
    CHECK(scannerSpecOfFrame(size, last, 0, amber()).frame == last);
    CHECK(scannerSpecOfFrame(size, last + 1, 0, amber()).frame == last);
    CHECK(scannerSpecOfFrame(size, std::numeric_limits<long>::max(), 0, amber()).frame == last);
    CHECK(scannerSpecOf(size, 1e12, 0, amber()) == scannerSpecOfFrame(size, last, 0, amber()));

    // the picture of a frame is the one the clock makes
    const Image five = renderScanner(scannerSpecOfFrame(size, 5, 1, amber()));
    CHECK(!five.empty());
    CHECK(five.rgba == renderScanner(scannerSpecOf(size, 5.5 / kScannerFps, 1, amber())).rgba);
    CHECK(five.rgba != renderScanner(scannerSpecOfFrame(size, 6, 1, amber())).rgba);
}

void testPlateConstants() {
    CHECK(kPlateGlassBlurred > 0 && kPlateGlassBlurred < kPlateGlassFlat && kPlateGlassFlat <= 1);

    // the halo is under the glass, so the glass can be told from it by how opaque it is
    CHECK(kPlateHaloAlpha > 0 && kPlateHaloAlpha < kPlateGlassBlurred);
}

} // namespace

int main() {
    testSizes();
    testLayout();
    testSpec();
    testRenderIsPure();
    testImagesAreSound();
    testOddSizes();
    testPicture();
    testScalesWithSize();
    testContacts();
    testContactsMoveSmoothly();
    testContactsAreLitByTheArm();
    testContactsVary();
    testArm();
    testPlateBox();
    testPlateOf();
    testPlateOfWindow();
    testPlateOfARealPod();
    testPlatePicture();
    testPlateLine();
    testPlateGlass();
    testPlateShapes();
    testPlateInvalid();
    testSpecOfFrame();
    testPlateConstants();
    return finish("test_scanner");
}
