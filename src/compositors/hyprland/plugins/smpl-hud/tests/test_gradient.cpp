#include "../gradient.hpp"
#include "check.hpp"

#include <cmath>
#include <vector>

using namespace smplhud;

namespace {

// Standard published sRGB(gamma 2.2) -> OkLab conversion, only used to build stops.
std::vector<float> stop(float r, float g, float b, float a) {
    const auto dec = [](float c) { return std::pow(c, 2.2f); };
    const float lr = dec(r), lg = dec(g), lb = dec(b);

    const float l = std::cbrt(0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb);
    const float m = std::cbrt(0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb);
    const float s = std::cbrt(0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb);

    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
            a};
}

std::vector<float> join(std::initializer_list<std::vector<float>> parts) {
    std::vector<float> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

constexpr float DEG = 3.14159265358979f / 180.f;

void expectColour(const Rgba& c, float r, float g, float b, float a, float eps = 0.01f) {
    CHECK_NEAR(c.r, r, eps);
    CHECK_NEAR(c.g, g, eps);
    CHECK_NEAR(c.b, b, eps);
    CHECK_NEAR(c.a, a, eps);
}

void testRoundTrip() {
    for (const auto& rgb : std::vector<std::vector<float>>{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.2f, 0.5f, 0.8f}, {1, 1, 1}, {0, 0, 0}}) {
        const auto s = stop(rgb[0], rgb[1], rgb[2], 0.5f);
        expectColour(toRgba({s[0], s[1], s[2], s[3]}), rgb[0], rgb[1], rgb[2], 0.5f);
    }
}

void testEmptyAndSingleStop() {
    CHECK(sampleGradient({}, 0, 0.5f, 0.5f).alpha == 0.f);

    const auto one = stop(0, 1, 1, 0.7f);
    const OkLabA c = sampleGradient(one, 45 * DEG, 0.9f, 0.1f);
    CHECK_NEAR(c.alpha, 0.7f, 1e-6);
    CHECK_NEAR(c.l, one[0], 1e-6);
}

void testEndpointsAt45Degrees() {
    const auto g = join({stop(1, 0, 0, 1), stop(0, 0, 1, 1)});

    // at 45 degrees the progress runs from the top left to the bottom right
    expectColour(toRgba(sampleGradient(g, 45 * DEG, 0.f, 0.f)), 1, 0, 0, 1);
    expectColour(toRgba(sampleGradient(g, 45 * DEG, 1.f, 1.f)), 0, 0, 1, 1);
}

void testHorizontalAndVertical() {
    const auto g = join({stop(1, 0, 0, 1), stop(0, 0, 1, 1)});

    // 0 degrees: left to right, independent of y
    const OkLabA l0 = sampleGradient(g, 0, 0.f, 0.3f);
    const OkLabA l1 = sampleGradient(g, 0, 0.f, 0.9f);
    CHECK_NEAR(l0.l, l1.l, 1e-5);
    expectColour(toRgba(sampleGradient(g, 0, 1.f, 0.5f)), 0, 0, 1, 1);

    // 90 degrees: top to bottom, independent of x
    const OkLabA v0 = sampleGradient(g, 90 * DEG, 0.1f, 0.5f);
    const OkLabA v1 = sampleGradient(g, 90 * DEG, 0.8f, 0.5f);
    CHECK_NEAR(v0.l, v1.l, 1e-5);
    expectColour(toRgba(sampleGradient(g, 90 * DEG, 0.5f, 0.f)), 1, 0, 0, 1, 0.02f);
}

void testOppositeAnglesMirror() {
    const auto g = join({stop(1, 0, 0, 1), stop(0, 0, 1, 1)});

    // 225 degrees runs bottom right to top left
    expectColour(toRgba(sampleGradient(g, 225 * DEG, 1.f, 1.f)), 1, 0, 0, 1);
    expectColour(toRgba(sampleGradient(g, 225 * DEG, 0.f, 0.f)), 0, 0, 1, 1);
}

void testMultipleStopsAndAlpha() {
    const auto g = join({stop(1, 0, 0, 0.f), stop(0, 1, 0, 1.f), stop(0, 0, 1, 0.5f)});

    // exact stop positions
    CHECK_NEAR(sampleGradient(g, 0, 0.f, 0.f).alpha, 0.f, 1e-5);
    CHECK_NEAR(sampleGradient(g, 0, 0.5f, 0.f).alpha, 1.f, 1e-5);
    CHECK_NEAR(sampleGradient(g, 0, 1.f, 0.f).alpha, 0.5f, 1e-5);
    // halfway between stops 0 and 1
    CHECK_NEAR(sampleGradient(g, 0, 0.25f, 0.f).alpha, 0.5f, 1e-5);
    // beyond the last stop clamps to it
    CHECK_NEAR(sampleGradient(g, 0, 2.f, 0.f).alpha, 0.5f, 1e-5);
}

void testAngleQuantisedToDegrees() {
    const auto g = join({stop(1, 0, 0, 1), stop(0, 0, 1, 1)});
    const OkLabA a = sampleGradient(g, 45.0f * DEG, 0.2f, 0.7f);
    const OkLabA b = sampleGradient(g, 45.4f * DEG, 0.2f, 0.7f);
    CHECK_NEAR(a.l, b.l, 1e-6);
}

void testMix() {
    const OkLabA a{0.2f, 0.1f, -0.1f, 0.f};
    const OkLabA b{0.8f, -0.1f, 0.3f, 1.f};
    const OkLabA m = mixOkLabA(a, b, 0.25f);
    CHECK_NEAR(m.l, 0.35f, 1e-6);
    CHECK_NEAR(m.a, 0.05f, 1e-6);
    CHECK_NEAR(m.b, 0.0f, 1e-6);
    CHECK_NEAR(m.alpha, 0.25f, 1e-6);
}

void testOutOfGamutIsClamped() {
    const Rgba c = toRgba({1.5f, 0.4f, 0.4f, 2.f});
    CHECK(c.r <= 1.f && c.g <= 1.f && c.b <= 1.f && c.a <= 1.f);
    CHECK(c.r >= 0.f && c.g >= 0.f && c.b >= 0.f);
}

} // namespace

int main() {
    testRoundTrip();
    testEmptyAndSingleStop();
    testEndpointsAt45Degrees();
    testHorizontalAndVertical();
    testOppositeAnglesMirror();
    testMultipleStopsAndAlpha();
    testAngleQuantisedToDegrees();
    testMix();
    testOutOfGamutIsClamped();
    return finish("test_gradient");
}
