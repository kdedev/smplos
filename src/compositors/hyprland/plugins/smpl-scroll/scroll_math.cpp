#include "scroll_math.hpp"

#include <algorithm>
#include <cmath>

namespace smplscroll {

    namespace {
        // std::clamp is undefined when lo > hi; a row smaller than the minimum can make that so.
        float clampRow(float v, float lo, float hi) {
            return std::min(std::max(v, lo), hi);
        }
    }

    double maxColumnWidth(double viewportPx) {
        if (!(viewportPx > 0.0))
            return 1.0;
        return std::max(1.0, std::min(kMaxColumnViewports, kMaxColumnPx / viewportPx));
    }

    WidthChange resizeColumnWidth(float oldWidth, double deltaPx, double viewportPx, bool fromStart) {
        WidthChange out{oldWidth, 0.0};

        if (!(viewportPx > 0.0) || !std::isfinite(deltaPx) || deltaPx == 0.0)
            return out;

        const double requested = (fromStart ? -deltaPx : deltaPx) / viewportPx;
        const double lower     = std::min<double>(oldWidth, kMinColumnWidth);
        const double upper     = std::max<double>(oldWidth, maxColumnWidth(viewportPx));
        const float  width     = static_cast<float>(std::clamp(oldWidth + requested, lower, upper));

        if (width == oldWidth)
            return out;

        out.width = width;
        if (fromStart)
            out.cameraShift = (static_cast<double>(width) - oldWidth) * viewportPx;
        return out;
    }

    RowChange resizeRowsBottom(float nextSize, float currSize, double modDeltaY, double viewportH) {
        RowChange out{false, nextSize, currSize};

        if (!(viewportH > 0.0) || (nextSize <= kMinRowHeight && modDeltaY >= 0.0))
            return out;

        const float adjust = clampRow(static_cast<float>(modDeltaY / viewportH), -currSize + kMinRowHeight, nextSize - kMinRowHeight);

        out.changed   = true;
        out.neighbour = clampRow(nextSize - adjust, kMinRowHeight, kMaxRowHeight);
        out.current   = clampRow(currSize + adjust, kMinRowHeight, kMaxRowHeight);
        return out;
    }

    RowChange resizeRowsTop(float prevSize, float currSize, double modDeltaY, double rawDeltaY, double viewportH) {
        RowChange out{false, prevSize, currSize};

        if (!(viewportH > 0.0) || (prevSize <= kMinRowHeight && modDeltaY <= 0.0) || (currSize <= kMinRowHeight && rawDeltaY >= 0.0))
            return out;

        const float adjust = clampRow(static_cast<float>(modDeltaY / viewportH), -(prevSize - kMinRowHeight), currSize - kMinRowHeight);

        out.changed   = true;
        out.neighbour = clampRow(prevSize + adjust, kMinRowHeight, kMaxRowHeight);
        out.current   = clampRow(currSize - adjust, kMinRowHeight, kMaxRowHeight);
        return out;
    }

    std::optional<double> fitWideColumnOffset(double offset, double start, double size, double viewport) {
        if (!(viewport > 0.0) || !(size > viewport) || !std::isfinite(offset) || !std::isfinite(start))
            return std::nullopt;

        return std::clamp(offset, start, start + size - viewport);
    }

    bool isVerifiedHyprland(std::string_view tag, std::string_view verified) {
        if (tag.starts_with('v'))
            tag.remove_prefix(1);
        if (verified.empty() || !tag.starts_with(verified))
            return false;

        const auto rest = tag.substr(verified.size());
        return rest.empty() || rest.front() == '.';
    }

} // namespace smplscroll
