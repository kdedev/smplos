#pragma once

// The HUD settings a theme provides. They live in the user's active theme
// file (~/.config/hypr/theme.conf) as `$themeHud...` variables, so the plugin
// reads them straight from there and needs no compositor config hooks.

#include <string>

namespace smplhud {

struct Theme {
    bool        frame          = false; // $themeHudFrame
    double      gap            = 5;     // $themeHudGap, logical px between window and line
    double      lineWidth      = 3;     // $themeHudLineWidth
    double      chamfer        = 10;    // $themeHudChamfer, minimum corner leg of the line
    double      floatGlowRange = 0;     // $themeHudFloatGlowRange
    std::string style          = "scifi"; // $themeHudStyle
    bool        sweep          = false; // $themeHudSweep, a glow travelling slowly along the line
    double      sweepSpeed     = 80;    // $themeHudSweepSpeed, logical px per second
    double      sweepLength    = 180;   // $themeHudSweepLength, logical px along the line
    bool        hideBorder     = false; // $themeHudHideBorder, Hyprland's own border is off so the frame is the only line

    bool operator==(const Theme&) const = default;
};

// Later lines win; out-of-range numbers are clamped, unparsable ones ignored.
Theme       parseTheme(const std::string& text);

// $XDG_CONFIG_HOME/hypr/theme.conf, falling back to ~/.config/hypr/theme.conf.
std::string themePath();

// Reads themePath(); a missing file gives the defaults (HUD off).
Theme       loadTheme();

} // namespace smplhud
