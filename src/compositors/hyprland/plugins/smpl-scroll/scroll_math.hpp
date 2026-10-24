#pragma once

// Compositor-free arithmetic behind smpl-scroll: how a scrolling-layout column's
// width and the row heights inside it change on a resize, and where the camera rests
// on a column wider than the viewport. It mirrors
// Layout::Tiled::CScrollingAlgorithm::resizeTarget of Hyprland 0.56, except that a
// column may grow past the viewport. Sizes are fractions of the viewport.

#include <optional>
#include <string_view>

namespace smplscroll {

    // Hyprland's own limits, kept for everything the plugin does not lift.
    inline constexpr double kMinColumnWidth = 0.05;
    inline constexpr float  kMinRowHeight   = 0.1F;
    inline constexpr float  kMaxRowHeight   = 1.F;

    // The plugin's ceiling on a column's width: a few viewports, and never more than
    // kMaxColumnPx logical pixels so the client's buffer stays inside GPU texture
    // limits. The ceiling is never below one viewport, Hyprland's own limit.
    inline constexpr double kMaxColumnViewports = 4.0;
    inline constexpr double kMaxColumnPx        = 8192.0;

    // Largest column width, as a fraction of the viewport, for a viewport of this many pixels.
    double maxColumnWidth(double viewportPx);

    struct WidthChange {
        float  width       = 0.F; // new column width
        double cameraShift = 0.0; // pixels to add to the camera offset
    };

    // deltaPx is the change along the tape (positive = the pointer moves toward the
    // end). fromStart resizes the column's start edge: the end edge stays where it is on
    // screen, so the camera shifts by the width change. Otherwise the start edge stays.
    // A resize never leaves the width on the far side of where it started: a width
    // already beyond a limit is kept, not snapped back, and a zero delta changes nothing.
    WidthChange resizeColumnWidth(float oldWidth, double deltaPx, double viewportPx, bool fromStart);

    struct RowChange {
        bool  changed   = false;
        float neighbour = 0.F; // new height of the adjacent row
        float current   = 0.F; // new height of the focused row
    };

    // The border between the focused row and the row below it moves by modDeltaY pixels.
    RowChange resizeRowsBottom(float nextSize, float currSize, double modDeltaY, double viewportH);

    // The border between the row above and the focused row moves by modDeltaY pixels.
    // rawDeltaY is the delta as requested, before the sign flip a keyboard resize applies.
    RowChange resizeRowsTop(float prevSize, float currSize, double modDeltaY, double rawDeltaY, double viewportH);

    // Camera offset that keeps a column wider than the viewport filling it: the current
    // offset while the view is inside the column, otherwise the nearer end of the column.
    // Hyprland's own fit centres such a column, which undoes every scroll within it.
    // Empty when the column fits the viewport, where Hyprland's own fit applies.
    // start and size locate the column on the tape; viewport is the visible length.
    std::optional<double> fitWideColumnOffset(double offset, double start, double size, double viewport);

    // Whether a Hyprland version tag ("v0.56.2") belongs to the verified "major.minor" series.
    bool isVerifiedHyprland(std::string_view tag, std::string_view verified);

} // namespace smplscroll
