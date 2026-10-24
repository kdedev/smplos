#include "place.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace smplhud {

namespace {
    bool isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }
}

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

std::optional<uintptr_t> parseFitRequest(std::string_view request) {
    std::array<std::string_view, 3> words;
    size_t                          count = 0;

    for (size_t at = 0; at < request.size();) {
        if (isSpace(request[at])) {
            ++at;
            continue;
        }

        size_t end = at;
        while (end < request.size() && !isSpace(request[end]))
            ++end;

        if (count == words.size())
            return std::nullopt;
        words[count++] = request.substr(at, end - at);
        at             = end;
    }

    if (count != words.size() || words[0] != "smplhud" || words[1] != "fit")
        return std::nullopt;

    auto digits = words[2];
    if (digits.starts_with("0x") || digits.starts_with("0X"))
        digits.remove_prefix(2);

    uintptr_t   address = 0;
    const char* last    = digits.data() + digits.size();
    const auto  parsed  = std::from_chars(digits.data(), last, address, 16);
    if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != last || address == 0)
        return std::nullopt;
    return address;
}

} // namespace smplhud
