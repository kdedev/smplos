#include "../scanner.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    return finish("test_scanner");
}
