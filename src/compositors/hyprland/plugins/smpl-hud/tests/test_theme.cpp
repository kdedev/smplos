#include "../theme.hpp"
#include "check.hpp"

using namespace smplhud;

namespace {

void testDefaults() {
    const Theme t = parseTheme("");
    CHECK(!t.frame);
    CHECK_NEAR(t.gap, 5, 1e-9);
    CHECK_NEAR(t.lineWidth, 3, 1e-9);
    CHECK_NEAR(t.chamfer, 10, 1e-9);
    CHECK_NEAR(t.floatGlowRange, 0, 1e-9);
    CHECK(t.style == "scifi");
    CHECK(!t.sweep);
    CHECK_NEAR(t.sweepSpeed, 80, 1e-9);
    CHECK_NEAR(t.sweepLength, 180, 1e-9);
    CHECK(!t.hideBorder);
    CHECK(!t.glyphColour);
    CHECK(t == Theme{});
}

void testNeonBlock() {
    const Theme t = parseTheme("$themeRounding = 42\n"
                               "$themeHudFrame = 1\n"
                               "$themeHudGap = 6\n"
                               "$themeHudChamfer = 10\n"
                               "$themeHudLineWidth = 3\n"
                               "$themeHudFloatGlowRange = 14\n"
                               "$themeHudStyle = scifi\n"
                               "$themeHudSweep = 1\n"
                               "$themeHudSweepSpeed = 70\n"
                               "$themeHudSweepLength = 200\n"
                               "$themeHudHideBorder = 1\n"
                               "$themeHudGlyphColor = rgb(46ff3c)\n");
    CHECK(t.frame);
    CHECK_NEAR(t.gap, 6, 1e-9);
    CHECK_NEAR(t.lineWidth, 3, 1e-9);
    CHECK_NEAR(t.chamfer, 10, 1e-9);
    CHECK_NEAR(t.floatGlowRange, 14, 1e-9);
    CHECK(t.style == "scifi");
    CHECK(t.sweep);
    CHECK_NEAR(t.sweepSpeed, 70, 1e-9);
    CHECK_NEAR(t.sweepLength, 200, 1e-9);
    CHECK(t.hideBorder);
    const Rgb8 green{0x46, 0xff, 0x3c};
    CHECK(t.glyphColour == green);
}

void testLaterLinesWinAndWhitespace() {
    const Theme t = parseTheme("$themeHudGap=4\n   $themeHudGap   =   9   \n$themeHudFrame = true\n$themeHudFrame = off\n");
    CHECK_NEAR(t.gap, 9, 1e-9);
    CHECK(!t.frame);
}

void testComments() {
    const Theme t = parseTheme("# $themeHudFrame = 1\n"
                               "$themeHudGap = 8 # eight\n"
                               "general {\n  $themeHudChamfer = 12\n}\n");
    CHECK(!t.frame);
    CHECK_NEAR(t.gap, 8, 1e-9);
    CHECK_NEAR(t.chamfer, 12, 1e-9);
}

void testBooleans() {
    for (const char* on : {"1", "true", "TRUE", "yes", "On"})
        CHECK(parseTheme(std::string("$themeHudFrame = ") + on).frame);
    for (const char* off : {"0", "false", "no", "OFF"})
        CHECK(!parseTheme(std::string("$themeHudFrame = 1\n$themeHudFrame = ") + off).frame);
    // garbage leaves the previous value alone
    CHECK(parseTheme("$themeHudFrame = 1\n$themeHudFrame = maybe").frame);

    for (const char* on : {"1", "true", "yes", "on"})
        CHECK(parseTheme(std::string("$themeHudSweep = ") + on).sweep);
    CHECK(!parseTheme("$themeHudSweep = 1\n$themeHudSweep = off").sweep);
    CHECK(parseTheme("$themeHudSweep = 1\n$themeHudSweep = maybe").sweep);
    CHECK(!parseTheme("$themeHudSweep = maybe").sweep);

    // The hidden-border flag parses like the others and is not the same as the frame flag.
    for (const char* on : {"1", "true", "yes", "on"})
        CHECK(parseTheme(std::string("$themeHudHideBorder = ") + on).hideBorder);
    CHECK(!parseTheme("$themeHudHideBorder = 1\n$themeHudHideBorder = off").hideBorder);
    CHECK(parseTheme("$themeHudHideBorder = 1\n$themeHudHideBorder = maybe").hideBorder);
    CHECK(!parseTheme("$themeHudHideBorder = maybe").hideBorder);
    CHECK(!parseTheme("$themeHudHideBorder = 1").frame);
    CHECK(!parseTheme("$themeHudFrame = 1").hideBorder);
}

void testClamping() {
    const Theme hi = parseTheme("$themeHudGap = 500\n$themeHudLineWidth = 99\n$themeHudChamfer = 1000\n$themeHudFloatGlowRange = 900\n"
                                "$themeHudSweepSpeed = 5000\n$themeHudSweepLength = 9000\n");
    CHECK_NEAR(hi.gap, 64, 1e-9);
    CHECK_NEAR(hi.lineWidth, 16, 1e-9);
    CHECK_NEAR(hi.chamfer, 96, 1e-9);
    CHECK_NEAR(hi.floatGlowRange, 64, 1e-9);
    CHECK_NEAR(hi.sweepSpeed, 400, 1e-9);
    CHECK_NEAR(hi.sweepLength, 600, 1e-9);

    const Theme lo = parseTheme("$themeHudGap = -3\n$themeHudLineWidth = 0\n$themeHudChamfer = -1\n$themeHudFloatGlowRange = -5\n"
                                "$themeHudSweepSpeed = 0\n$themeHudSweepLength = -20\n");
    CHECK_NEAR(lo.gap, 0, 1e-9);
    CHECK_NEAR(lo.lineWidth, 1, 1e-9);
    CHECK_NEAR(lo.chamfer, 0, 1e-9);
    CHECK_NEAR(lo.floatGlowRange, 0, 1e-9);
    CHECK_NEAR(lo.sweepSpeed, 10, 1e-9);
    CHECK_NEAR(lo.sweepLength, 40, 1e-9);
}

void testInvalidValuesKeepDefaults() {
    const Theme t = parseTheme("$themeHudGap = abc\n$themeHudLineWidth = 3px\n$themeHudChamfer =\n$themeHudFloatGlowRange = nan\n$themeHudStyle = bad style!\n"
                               "$themeHudSweepSpeed = fast\n$themeHudSweepLength = inf\n");
    CHECK_NEAR(t.gap, 5, 1e-9);
    CHECK_NEAR(t.lineWidth, 3, 1e-9);
    CHECK_NEAR(t.chamfer, 10, 1e-9);
    CHECK_NEAR(t.floatGlowRange, 0, 1e-9);
    CHECK(t.style == "scifi");
    CHECK_NEAR(t.sweepSpeed, 80, 1e-9);
    CHECK_NEAR(t.sweepLength, 180, 1e-9);
}

void testFractionalAndStyle() {
    const Theme t = parseTheme("$themeHudGap = 2.5\n$themeHudStyle = minimal\n");
    CHECK_NEAR(t.gap, 2.5, 1e-9);
    CHECK(t.style == "minimal");
}

void testIgnoresUnrelatedLines() {
    const Theme t = parseTheme("windowrule = float\n$themeHudFrameX = 1\nmisc:foo = 1\n$noequals\n$themeHudHideBorderX = 1\n$themeHudGlyphColorX = rgb(ffffff)\n");
    CHECK(!t.frame);
    CHECK(!t.hideBorder);
    CHECK(!t.glyphColour);
}

void testGlyphColour() {
    const Rgb8 black{0, 0, 0}, mixed{0xff, 0xa0, 0x01}, matrix{0x1f, 0xe8, 0x6a};
    CHECK(parseTheme("$themeHudGlyphColor = rgb(000000)").glyphColour == black);
    CHECK(parseTheme("$themeHudGlyphColor = rgb(FfA001)").glyphColour == mixed);
    CHECK(parseTheme("  $themeHudGlyphColor=rgb(1fe86a)   # matrix green").glyphColour == matrix);

    // Anything but a colour leaves what was there; `none` takes the colour away.
    const Theme set = parseTheme("$themeHudGlyphColor = rgb(1fe86a)");
    for (const char* bad : {"", "green", "#1fe86a", "1fe86a", "rgb(1fe86a", "rgb(1fe86)", "rgb(1fe86aa)", "rgb(1fe86g)", "rgba(1fe86aff)", "rgb( 1fe86a)", "NONE"})
        CHECK(parseTheme(std::string("$themeHudGlyphColor = rgb(1fe86a)\n$themeHudGlyphColor = ") + bad).glyphColour == set.glyphColour);
    for (const char* bad : {"", "green", "rgb(1fe86g)", "rgba(1fe86aff)"})
        CHECK(!parseTheme(std::string("$themeHudGlyphColor = ") + bad).glyphColour);
    CHECK(!parseTheme("$themeHudGlyphColor = rgb(1fe86a)\n$themeHudGlyphColor = none").glyphColour);
    CHECK(!parseTheme("$themeHudGlyphColor = none").glyphColour);

    // It is a setting of its own: setting it turns nothing else on.
    const Theme only = parseTheme("$themeHudGlyphColor = rgb(1fe86a)");
    CHECK(only.glyphColour && !only.frame && !only.sweep && !only.hideBorder);
}

} // namespace

int main() {
    testDefaults();
    testNeonBlock();
    testLaterLinesWinAndWhitespace();
    testComments();
    testBooleans();
    testClamping();
    testInvalidValuesKeepDefaults();
    testFractionalAndStyle();
    testIgnoresUnrelatedLines();
    testGlyphColour();
    return finish("test_theme");
}
