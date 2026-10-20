#include "place.hpp"

#include <algorithm>

namespace smplhud {

Rect fitWithFrame(const Rect& box, const Extents& frame, const Rect& area) {
    const double room_w = area.w - frame.left - frame.right;
    const double room_h = area.h - frame.top - frame.bottom;
    if (!(box.w > 0 && box.h > 0) || !(room_w >= kMinWindow && room_h >= kMinWindow))
        return box;

    Rect fitted = box;
    fitted.w    = std::min(box.w, room_w);
    fitted.h    = std::min(box.h, room_h);
    fitted.x    = std::clamp(box.x, area.x + frame.left, area.x + area.w - frame.right - fitted.w);
    fitted.y    = std::clamp(box.y, area.y + frame.top, area.y + area.h - frame.bottom - fitted.h);
    return fitted;
}

} // namespace smplhud
