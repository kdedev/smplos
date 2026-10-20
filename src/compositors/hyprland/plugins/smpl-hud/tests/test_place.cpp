#include "../place.hpp"
#include "check.hpp"

#include <cmath>
#include <cstdint>

using namespace smplhud;

namespace {

constexpr Extents kFrame{33, 33, 33, 33};

bool sameRect(const Rect& a, const Rect& b) {
    return a == b;
}

void testFittingBoxIsUntouched() {
    const Rect area{0, 0, 1920, 1080};
    const Rect box{100, 100, 800, 600};
    CHECK(sameRect(fitWithFrame(box, kFrame, area), box));

    // right on the line: the frame touches the edge and no more
    const Rect flush{33, 33, 1854, 1014};
    CHECK(sameRect(fitWithFrame(flush, kFrame, area), flush));
}

void testPushedInFromEachEdge() {
    const Rect area{0, 0, 1920, 1080};

    const Rect left = fitWithFrame({2, 200, 480, 720}, kFrame, area);
    CHECK_NEAR(left.x, 33, 1e-9);
    CHECK_NEAR(left.y, 200, 1e-9);
    CHECK_NEAR(left.w, 480, 1e-9);
    CHECK_NEAR(left.h, 720, 1e-9);

    const Rect right = fitWithFrame({1920 - 480 - 2, 200, 480, 600}, kFrame, area);
    CHECK_NEAR(right.x, 1920 - 33 - 480, 1e-9);
    CHECK_NEAR(right.y, 200, 1e-9);

    const Rect top = fitWithFrame({600, -50, 480, 300}, kFrame, area);
    CHECK_NEAR(top.x, 600, 1e-9);
    CHECK_NEAR(top.y, 33, 1e-9);

    const Rect bottom = fitWithFrame({600, 1080 - 300 - 2, 480, 300}, kFrame, area);
    CHECK_NEAR(bottom.x, 600, 1e-9);
    CHECK_NEAR(bottom.y, 1080 - 33 - 300, 1e-9);
}

void testCornerPushesBothWays() {
    const Rect area{0, 0, 2560, 1408};
    const Rect box = fitWithFrame({2560 - 482, 1440 - 754, 480, 720}, kFrame, area);
    CHECK_NEAR(box.x, 2560 - 33 - 480, 1e-9);
    CHECK_NEAR(box.y, 1408 - 33 - 720, 1e-9);
}

void testOversizedBoxShrinksThenSits() {
    // A box the size of the toggled window of a smaller monitor on the right
    const Rect area{2560, 0, 1920, 1080};
    const Rect box = fitWithFrame({-480, -220, 2400, 1300}, kFrame, area);
    CHECK_NEAR(box.w, 1920 - 66, 1e-9);
    CHECK_NEAR(box.h, 1080 - 66, 1e-9);
    CHECK_NEAR(box.x, 2560 + 33, 1e-9);
    CHECK_NEAR(box.y, 33, 1e-9);
}

void testOnlyTheLargeSideShrinks() {
    const Rect area{0, 0, 1920, 1080};
    const Rect box = fitWithFrame({500, 10, 600, 1300}, kFrame, area);
    CHECK_NEAR(box.w, 600, 1e-9);
    CHECK_NEAR(box.h, 1080 - 66, 1e-9);
    CHECK_NEAR(box.x, 500, 1e-9);
    CHECK_NEAR(box.y, 33, 1e-9);
}

void testAsymmetricExtents() {
    const Extents frame{10, 20, 30, 40};
    const Rect    area{100, 200, 1000, 800};

    const Rect topLeft = fitWithFrame({0, 0, 300, 200}, frame, area);
    CHECK_NEAR(topLeft.x, 110, 1e-9);
    CHECK_NEAR(topLeft.y, 220, 1e-9);

    const Rect bottomRight = fitWithFrame({2000, 2000, 300, 200}, frame, area);
    CHECK_NEAR(bottomRight.x, 100 + 1000 - 30 - 300, 1e-9);
    CHECK_NEAR(bottomRight.y, 200 + 800 - 40 - 200, 1e-9);

    const Rect big = fitWithFrame({0, 0, 5000, 5000}, frame, area);
    CHECK_NEAR(big.w, 1000 - 10 - 30, 1e-9);
    CHECK_NEAR(big.h, 800 - 20 - 40, 1e-9);
    CHECK_NEAR(big.x, 110, 1e-9);
    CHECK_NEAR(big.y, 220, 1e-9);
}

void testNoFrameStillKeepsTheWindowOnScreen() {
    const Rect area{0, 0, 1920, 1080};
    const Rect off = fitWithFrame({-480, -220, 2400, 1300}, Extents{}, area);
    CHECK_NEAR(off.x, 0, 1e-9);
    CHECK_NEAR(off.y, 0, 1e-9);
    CHECK_NEAR(off.w, 1920, 1e-9);
    CHECK_NEAR(off.h, 1080, 1e-9);
}

void testNoRoomLeavesTheBoxAlone() {
    const Rect box{-10, -10, 500, 500};
    CHECK(sameRect(fitWithFrame(box, kFrame, {0, 0, 100, 100}), box));
    CHECK(sameRect(fitWithFrame(box, kFrame, {0, 0, 0, 0}), box));
    CHECK(sameRect(fitWithFrame(box, kFrame, {0, 0, 1920, 66 + kMinWindow - 1}), box));
    CHECK(!sameRect(fitWithFrame(box, kFrame, {0, 0, 1920, 66 + kMinWindow}), box));
}

void testDegenerateBoxIsUntouched() {
    const Rect area{0, 0, 1920, 1080};
    for (const Rect& box : {Rect{-50, -50, 0, 0}, Rect{-50, -50, -5, 300}, Rect{-50, -50, 300, 0}, Rect{-50, -50, std::nan(""), 300}}) {
        const Rect fitted = fitWithFrame(box, kFrame, area);
        CHECK(std::isnan(box.w) ? std::isnan(fitted.w) : sameRect(fitted, box));
    }
}

// Whatever the box, the frame ends inside the area, the box is never made
// bigger, and fitting again changes nothing.
void testPropertiesOverManyBoxes() {
    uint64_t seed          = 12345;
    int      alreadyInside = 0;
    auto     next          = [&](double lo, double hi) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return lo + (hi - lo) * double(seed >> 11) / double(1ULL << 53);
    };

    for (int i = 0; i < 4000; ++i) {
        const Extents frame{int(next(0, 60)), int(next(0, 60)), int(next(0, 60)), int(next(0, 60))};
        const Rect    area{next(-3000, 3000), next(-3000, 3000), next(300, 4000), next(300, 3000)};
        const Rect    box{area.x + next(-400, area.w + 200), area.y + next(-400, area.h + 200), next(20, area.w * 1.3), next(20, area.h * 1.3)};

        const Rect fitted = fitWithFrame(box, frame, area);

        CHECK(fitted.x - frame.left >= area.x - 1e-9);
        CHECK(fitted.y - frame.top >= area.y - 1e-9);
        CHECK(fitted.x + fitted.w + frame.right <= area.x + area.w + 1e-9);
        CHECK(fitted.y + fitted.h + frame.bottom <= area.y + area.h + 1e-9);
        CHECK(fitted.w <= box.w && fitted.h <= box.h);
        CHECK(fitted.w > 0 && fitted.h > 0);
        CHECK(sameRect(fitWithFrame(fitted, frame, area), fitted));

        // a box that was already inside is not moved
        if (box.x - frame.left >= area.x && box.y - frame.top >= area.y && box.x + box.w + frame.right <= area.x + area.w && box.y + box.h + frame.bottom <= area.y + area.h) {
            CHECK(sameRect(fitted, box));
            ++alreadyInside;
        }
    }
    CHECK(alreadyInside > 20); // the case above is really exercised
}

void testFractionalMonitor() {
    // 2560x1440 at scale 1.5: the logical area is not a whole number of px
    const Rect area{0, 0, 2560 / 1.5, 1440 / 1.5};
    const Rect box = fitWithFrame({area.w - 100, area.h - 100, 480, 720}, kFrame, area);
    CHECK_NEAR(box.x + box.w + 33, area.w, 1e-9);
    CHECK_NEAR(box.y + box.h + 33, area.h, 1e-9);
}

void testFitRequestTakesTheAddress() {
    const auto address = parseFitRequest("smplhud fit 0x55d0c2a4b3f0");
    CHECK(address.has_value());
    CHECK(address == uintptr_t(0x55d0c2a4b3f0ULL));

    // as the address is printed by `hyprctl clients`, or without its 0x
    CHECK(parseFitRequest("smplhud fit 0X1F") == std::optional<uintptr_t>(0x1f));
    CHECK(parseFitRequest("smplhud fit 1f") == std::optional<uintptr_t>(0x1f));
    CHECK(parseFitRequest("smplhud fit 0xABCdef") == std::optional<uintptr_t>(0xabcdef));

    // spacing, and the newline a request can end with
    CHECK(parseFitRequest("  smplhud \t fit   0x2a \n") == std::optional<uintptr_t>(0x2a));
}

void testFitRequestRefusesAnythingElse() {
    CHECK(!parseFitRequest(""));
    CHECK(!parseFitRequest("smplhud"));
    CHECK(!parseFitRequest("smplhud fit"));
    CHECK(!parseFitRequest("smplhud fit 0x"));
    CHECK(!parseFitRequest("smplhud fit 0"));
    CHECK(!parseFitRequest("smplhud fit 0x0"));
    CHECK(!parseFitRequest("smplhud fit 0xzz"));
    CHECK(!parseFitRequest("smplhud fit 12g4"));
    CHECK(!parseFitRequest("smplhud fit -0x1"));
    CHECK(!parseFitRequest("smplhud fit +0x1"));
    CHECK(!parseFitRequest("smplhud fit address:0x1"));
    CHECK(!parseFitRequest("smplhud fit 0x1 0x2"));
    CHECK(!parseFitRequest("smplhud fit 0x1 extra"));
    CHECK(!parseFitRequest("smplhud fits 0x1"));
    CHECK(!parseFitRequest("smplhud0x1 fit 0x1"));
    CHECK(!parseFitRequest("smplhudx fit 0x1"));
    CHECK(!parseFitRequest("hud fit 0x1"));
    CHECK(!parseFitRequest("fit 0x1"));
    CHECK(!parseFitRequest("smplhud 0x1"));

    // wider than an address
    CHECK(!parseFitRequest("smplhud fit 0x10000000000000000"));
    CHECK(!parseFitRequest("smplhud fit 0xffffffffffffffffff"));
}

} // namespace

int main() {
    testFittingBoxIsUntouched();
    testPushedInFromEachEdge();
    testCornerPushesBothWays();
    testOversizedBoxShrinksThenSits();
    testOnlyTheLargeSideShrinks();
    testAsymmetricExtents();
    testNoFrameStillKeepsTheWindowOnScreen();
    testNoRoomLeavesTheBoxAlone();
    testDegenerateBoxIsUntouched();
    testPropertiesOverManyBoxes();
    testFractionalMonitor();
    testFitRequestTakesTheAddress();
    testFitRequestRefusesAnythingElse();
    return finish("test_place");
}
