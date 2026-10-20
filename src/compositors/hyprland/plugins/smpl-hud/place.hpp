#pragma once

#include "geometry.hpp"

namespace smplhud {

// A box in the compositor's logical px, in global layout coordinates.
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool   operator==(const Rect&) const = default;
};

// Where a floating window has to be for the whole frame the HUD paints round it
// to lie inside `area`. The window is moved in from each edge by that edge's
// extent and, when it cannot fit even then, made smaller. A box that already
// fits comes back as it was, and so does any box when `area` has no room left
// for a frame and a window.
Rect fitWithFrame(const Rect& box, const Extents& frame, const Rect& area);

} // namespace smplhud
