#include "../glyph.hpp"
#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace smplhud;

namespace {

std::string describe(int index) {
    std::string out;
    for (const GlyphStroke& stroke : glyphStrokes(index)) {
        for (const GlyphPoint& p : stroke)
            out += std::to_string(p.x) + std::to_string(p.y) + ' ';
        out += '/';
    }
    return out;
}

void testEveryGlyphIsDrawable() {
    CHECK(glyphCount() >= 30);
    for (int i = 0; i < glyphCount(); ++i) {
        const auto strokes = glyphStrokes(i);
        CHECK(!strokes.empty());
        for (const GlyphStroke& stroke : strokes) {
            CHECK(stroke.size() >= 2); // a dot is two equal points
            for (const GlyphPoint& p : stroke) {
                CHECK(p.x >= 0 && p.x <= kGlyphW);
                CHECK(p.y >= 0 && p.y <= kGlyphH);
            }
        }
    }
}

void testGlyphsAreDistinct() {
    std::set<std::string> seen;
    for (int i = 0; i < glyphCount(); ++i)
        CHECK(seen.insert(describe(i)).second);
}

void testGlyphsAreNotSpecks() {
    // Each glyph is a real shape, so that all of them show up on screen: tall,
    // or wide, or both.
    for (int i = 0; i < glyphCount(); ++i) {
        int minX = kGlyphW, maxX = 0, minY = kGlyphH, maxY = 0;
        for (const GlyphStroke& stroke : glyphStrokes(i)) {
            for (const GlyphPoint& p : stroke) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
        }
        CHECK(maxY - minY >= 3 || maxX - minX >= kGlyphW);
        CHECK((maxX - minX) + (maxY - minY) >= 5);
    }
}

void testIndexWrapsAround() {
    const int n = glyphCount();
    CHECK(glyphIndex(0) == 0);
    CHECK(glyphIndex(n) == 0);
    CHECK(glyphIndex(n + 3) == 3);
    CHECK(glyphIndex(-1) == n - 1);
    CHECK(glyphIndex(-n) == 0);
    CHECK(glyphIndex(1'000'000'007LL * n + 5) == 5);
    for (long long v : {-7LL, 0LL, 1LL, 1234567890123LL, -1234567890123LL}) {
        CHECK(glyphIndex(v) >= 0 && glyphIndex(v) < n);
        CHECK(glyphStrokes(int(v % n)).data() == glyphStrokes(glyphIndex(v % n)).data());
    }
    CHECK(glyphStrokes(n + 2).data() == glyphStrokes(2).data());
    CHECK(glyphStrokes(-1).data() == glyphStrokes(n - 1).data());
}

} // namespace

int main() {
    testEveryGlyphIsDrawable();
    testGlyphsAreDistinct();
    testGlyphsAreNotSpecks();
    testIndexWrapsAround();
    return finish("test_glyph");
}
