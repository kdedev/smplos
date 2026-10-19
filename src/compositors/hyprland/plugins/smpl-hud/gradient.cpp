#include "gradient.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace smplhud {

namespace {

float quantizedAngle(float angle) {
    constexpr double DEG = std::numbers::pi / 180.0;
    return float(double(int(double(angle) / DEG) % 360) * DEG);
}

float encodeGamma22(float v) {
    return std::clamp(std::pow(std::max(v, 0.f), 1.f / 2.2f), 0.f, 1.f);
}

} // namespace

OkLabA sampleGradient(const std::vector<float>& stops, float angle, float nx, float ny) {
    const int n = int(stops.size() / 4);
    if (n <= 0)
        return {};

    const auto at = [&](int i) { return OkLabA{stops[4 * i], stops[4 * i + 1], stops[4 * i + 2], stops[4 * i + 3]}; };
    if (n < 2)
        return at(0);

    const float a  = quantizedAngle(angle);
    float       x  = nx;
    float       y  = ny;
    float       fin = a;

    if (a > 4.71f) {
        y   = 1.f - y;
        fin = 6.28f - a;
    } else if (a > 3.14f) {
        x   = 1.f - x;
        y   = 1.f - y;
        fin = a - 3.14f;
    } else if (a > 1.57f) {
        x   = 1.f - x;
        fin = 3.14f - a;
    }

    const float sine     = std::sin(fin);
    const float progress = std::clamp((y * sine + x * (1.f - sine)) * float(n - 1), 0.f, float(n - 1));
    if (progress >= float(n - 1))
        return at(n - 1);

    const int bottom = int(std::floor(progress));
    return mixOkLabA(at(bottom), at(bottom + 1), progress - float(bottom));
}

OkLabA mixOkLabA(const OkLabA& from, const OkLabA& to, float t) {
    return {from.l + (to.l - from.l) * t, from.a + (to.a - from.a) * t, from.b + (to.b - from.b) * t, from.alpha + (to.alpha - from.alpha) * t};
}

Rgba toRgba(const OkLabA& c) {
    const float l_ = c.l + c.a * 0.3963377774f + c.b * 0.2158037573f;
    const float m_ = c.l - c.a * 0.1055613458f - c.b * 0.0638541728f;
    const float s_ = c.l - c.a * 0.0894841775f - c.b * 1.2914855480f;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    return {
        encodeGamma22(l * 4.0767416621f - m * 3.3077115913f + s * 0.2309699292f),
        encodeGamma22(-l * 1.2684380046f + m * 2.6097574011f - s * 0.3413193965f),
        encodeGamma22(-l * 0.0041960863f - m * 0.7034186147f + s * 1.7076147010f),
        std::clamp(c.alpha, 0.f, 1.f),
    };
}

} // namespace smplhud
