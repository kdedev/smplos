#include "../scroll_math.hpp"
#include "check.hpp"

using namespace smplscroll;

namespace {

constexpr double kViewport = 2560.0;
constexpr double kHeight   = 1000.0;

void testMaxColumnWidth() {
    CHECK_NEAR(maxColumnWidth(2560.0), 3.2, 1e-9);     // the pixel cap binds
    CHECK_NEAR(maxColumnWidth(1920.0), 4.0, 1e-9);     // the viewport cap binds
    CHECK_NEAR(maxColumnWidth(3840.0), 8192.0 / 3840.0, 1e-9);
    CHECK_NEAR(maxColumnWidth(10000.0), 1.0, 1e-9);    // never below Hyprland's own limit
    CHECK_NEAR(maxColumnWidth(0.0), 1.0, 1e-9);
    CHECK_NEAR(maxColumnWidth(-5.0), 1.0, 1e-9);
}

void testGrowsPastTheViewport() {
    const auto grown = resizeColumnWidth(1.0F, 256.0, kViewport, false);
    CHECK_NEAR(grown.width, 1.1, 1e-6);
    CHECK_NEAR(grown.cameraShift, 0.0, 1e-9);

    // Within Hyprland's own range the result equals stock behaviour.
    CHECK_NEAR(resizeColumnWidth(0.5F, 256.0, kViewport, false).width, 0.6, 1e-6);
    CHECK_NEAR(resizeColumnWidth(0.5F, -256.0, kViewport, false).width, 0.4, 1e-6);
}

void testCeilingAndFloor() {
    CHECK_NEAR(resizeColumnWidth(3.1F, 2560.0, kViewport, false).width, 3.2, 1e-6);
    CHECK_NEAR(resizeColumnWidth(3.9F, 1920.0, 1920.0, false).width, 4.0, 1e-6);
    CHECK_NEAR(resizeColumnWidth(0.06F, -512.0, kViewport, false).width, 0.05, 1e-6);

    // At the ceiling a further grow changes nothing.
    CHECK(resizeColumnWidth(3.2F, 100.0, kViewport, false).width == 3.2F);
}

void testGrowThenShrinkIsReversible() {
    // 200 key presses of 20 px each take the column well past one viewport and back.
    float width = 0.5F;
    for (int i = 0; i < 200; ++i)
        width = resizeColumnWidth(width, 20.0, kViewport, false).width;
    CHECK_NEAR(width, 0.5 + 200 * 20.0 / kViewport, 1e-4);
    CHECK(width > 1.0F);

    for (int i = 0; i < 200; ++i)
        width = resizeColumnWidth(width, -20.0, kViewport, false).width;
    CHECK_NEAR(width, 0.5, 1e-4);
}

void testStartEdgeShiftsTheCamera() {
    // The pointer moves toward the start: the column grows and the camera follows.
    const auto grown = resizeColumnWidth(0.5F, -256.0, kViewport, true);
    CHECK_NEAR(grown.width, 0.6, 1e-6);
    CHECK_NEAR(grown.cameraShift, 256.0, 1e-3);

    const auto shrunk = resizeColumnWidth(0.5F, 256.0, kViewport, true);
    CHECK_NEAR(shrunk.width, 0.4, 1e-6);
    CHECK_NEAR(shrunk.cameraShift, -256.0, 1e-3);

    // The shift is the width actually applied, not the width requested.
    const auto capped = resizeColumnWidth(3.1F, -2560.0, kViewport, true);
    CHECK_NEAR(capped.width, 3.2, 1e-6);
    CHECK_NEAR(capped.cameraShift, 0.1 * kViewport, 1e-3);
}

void testNoChangeCases() {
    for (const bool fromStart : {false, true}) {
        // A zero delta never moves a width, even one beyond a limit.
        CHECK(resizeColumnWidth(5.0F, 0.0, kViewport, fromStart).width == 5.0F);
        CHECK(resizeColumnWidth(0.01F, 0.0, kViewport, fromStart).width == 0.01F);
        CHECK(resizeColumnWidth(0.5F, 0.0, kViewport, fromStart).cameraShift == 0.0);

        // Invalid input is ignored.
        CHECK(resizeColumnWidth(0.5F, 100.0, 0.0, fromStart).width == 0.5F);
        CHECK(resizeColumnWidth(0.5F, 100.0, -3.0, fromStart).width == 0.5F);
        CHECK(resizeColumnWidth(0.5F, NAN, kViewport, fromStart).width == 0.5F);
        CHECK(resizeColumnWidth(0.5F, INFINITY, kViewport, fromStart).width == 0.5F);
    }
}

void testWidthBeyondALimitIsKeptNotSnapped() {
    // Above the ceiling: growing leaves it alone, shrinking works.
    CHECK(resizeColumnWidth(5.0F, 256.0, kViewport, false).width == 5.0F);
    CHECK_NEAR(resizeColumnWidth(5.0F, -256.0, kViewport, false).width, 4.9, 1e-6);

    // Below the floor: shrinking leaves it alone, growing works.
    CHECK(resizeColumnWidth(0.03F, -100.0, kViewport, false).width == 0.03F);
    CHECK_NEAR(resizeColumnWidth(0.03F, 256.0, kViewport, false).width, 0.13, 1e-6);
}

void testRowsBottom() {
    auto r = resizeRowsBottom(0.5F, 0.5F, 100.0, kHeight);
    CHECK(r.changed);
    CHECK_NEAR(r.neighbour, 0.4, 1e-6);
    CHECK_NEAR(r.current, 0.6, 1e-6);

    // The neighbour is already at its minimum: it cannot shrink further.
    r = resizeRowsBottom(0.1F, 0.5F, 100.0, kHeight);
    CHECK(!r.changed);
    CHECK(r.neighbour == 0.1F && r.current == 0.5F);

    // ...but it can grow back.
    r = resizeRowsBottom(0.1F, 0.5F, -100.0, kHeight);
    CHECK(r.changed);
    CHECK_NEAR(r.neighbour, 0.2, 1e-6);
    CHECK_NEAR(r.current, 0.4, 1e-6);

    // A huge delta stops at the neighbour's minimum, and the two rows keep their total.
    r = resizeRowsBottom(0.3F, 0.5F, 900.0, kHeight);
    CHECK_NEAR(r.neighbour, kMinRowHeight, 1e-6);
    CHECK_NEAR(r.neighbour + r.current, 0.8, 1e-6);

    // Rows already below the minimum must not trip std::clamp's precondition.
    r = resizeRowsBottom(0.05F, 0.05F, -10.0, kHeight);
    CHECK(r.neighbour >= kMinRowHeight && r.neighbour <= kMaxRowHeight);
    CHECK(r.current >= kMinRowHeight && r.current <= kMaxRowHeight);

    CHECK(!resizeRowsBottom(0.5F, 0.5F, 100.0, 0.0).changed);
}

void testRowsTop() {
    auto r = resizeRowsTop(0.5F, 0.5F, 100.0, 100.0, kHeight);
    CHECK(r.changed);
    CHECK_NEAR(r.neighbour, 0.6, 1e-6);
    CHECK_NEAR(r.current, 0.4, 1e-6);

    // A keyboard resize flips the sign: asking for +20 moves the border up by 20.
    r = resizeRowsTop(0.5F, 0.5F, -20.0, 20.0, kHeight);
    CHECK(r.changed);
    CHECK_NEAR(r.neighbour, 0.48, 1e-6);
    CHECK_NEAR(r.current, 0.52, 1e-6);

    // Neighbour at its minimum cannot shrink; focused row at its minimum cannot shrink.
    CHECK(!resizeRowsTop(0.1F, 0.5F, -100.0, 100.0, kHeight).changed);
    CHECK(!resizeRowsTop(0.5F, 0.1F, 100.0, 100.0, kHeight).changed);

    r = resizeRowsTop(0.5F, 0.3F, 900.0, 900.0, kHeight);
    CHECK_NEAR(r.current, kMinRowHeight, 1e-6);
    CHECK_NEAR(r.neighbour + r.current, 0.8, 1e-6);

    CHECK(!resizeRowsTop(0.5F, 0.5F, 100.0, 100.0, 0.0).changed);
}

void testWideColumnFit() {
    // A 4000 px column starting at 1000 on a 1600 px view: the view may rest anywhere in 1000..3400.
    CHECK_NEAR(*fitWideColumnOffset(1800.0, 1000.0, 4000.0, 1600.0), 1800.0, 1e-9); // inside: untouched
    CHECK_NEAR(*fitWideColumnOffset(1000.0, 1000.0, 4000.0, 1600.0), 1000.0, 1e-9); // flush with the start
    CHECK_NEAR(*fitWideColumnOffset(3400.0, 1000.0, 4000.0, 1600.0), 3400.0, 1e-9); // flush with the end

    // Coming from the left neighbour shows the start; from the right neighbour, the end.
    CHECK_NEAR(*fitWideColumnOffset(-300.0, 1000.0, 4000.0, 1600.0), 1000.0, 1e-9);
    CHECK_NEAR(*fitWideColumnOffset(5200.0, 1000.0, 4000.0, 1600.0), 3400.0, 1e-9);

    // A column that fits is left to Hyprland, including one exactly as long as the view.
    CHECK(!fitWideColumnOffset(0.0, 1000.0, 800.0, 1600.0));
    CHECK(!fitWideColumnOffset(0.0, 1000.0, 1600.0, 1600.0));
    CHECK(fitWideColumnOffset(0.0, 1000.0, 1601.0, 1600.0));

    // Invalid input is left to Hyprland too.
    CHECK(!fitWideColumnOffset(0.0, 1000.0, 4000.0, 0.0));
    CHECK(!fitWideColumnOffset(0.0, 1000.0, 4000.0, -5.0));
    CHECK(!fitWideColumnOffset(NAN, 1000.0, 4000.0, 1600.0));
    CHECK(!fitWideColumnOffset(0.0, INFINITY, 4000.0, 1600.0));
}

void testVerifiedVersion() {
    CHECK(isVerifiedHyprland("v0.56.2", "0.56"));
    CHECK(isVerifiedHyprland("0.56.0", "0.56"));
    CHECK(isVerifiedHyprland("v0.56", "0.56"));
    CHECK(!isVerifiedHyprland("v0.57.0", "0.56"));
    CHECK(!isVerifiedHyprland("v0.560.1", "0.56"));
    CHECK(!isVerifiedHyprland("v0.5", "0.56"));
    CHECK(!isVerifiedHyprland("v0.56-rc1", "0.56"));
    CHECK(!isVerifiedHyprland("", "0.56"));
    CHECK(!isVerifiedHyprland("v0.56.2", ""));
}

} // namespace

int main() {
    testMaxColumnWidth();
    testGrowsPastTheViewport();
    testCeilingAndFloor();
    testGrowThenShrinkIsReversible();
    testStartEdgeShiftsTheCamera();
    testNoChangeCases();
    testWidthBeyondALimitIsKeptNotSnapped();
    testRowsBottom();
    testRowsTop();
    testWideColumnFit();
    testVerifiedVersion();
    return finish("test_scroll_math");
}
