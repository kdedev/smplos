#pragma once

// CPU port of Hyprland's gradient.glsl so flat shapes can take the colour the
// border shader would paint at a point. Keep it in step with the shader.

#include <vector>

namespace smplhud {

struct OkLabA {
    float l = 0, a = 0, b = 0, alpha = 0;
};

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;
};

// `stops` is a flat list of OkLab+alpha quadruples, the layout of
// CGradientValueData::m_colorsOkLabA. `angle` is the gradient angle in radians
// (quantised to whole degrees like the shader does); nx/ny is the position
// normalised over the gradient's box, y pointing down.
OkLabA sampleGradient(const std::vector<float>& stops, float angle, float nx, float ny);

OkLabA mixOkLabA(const OkLabA& from, const OkLabA& to, float t);

// Straight (not premultiplied) sRGB, as the shader outputs before premultiplying.
Rgba toRgba(const OkLabA& lab);

} // namespace smplhud
