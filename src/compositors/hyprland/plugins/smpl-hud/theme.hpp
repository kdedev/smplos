#pragma once

// The HUD settings a theme provides. They live in the user's active theme
// file (~/.config/hypr/theme.conf) as `$themeHud...` variables, so the plugin
// reads them straight from there and needs no compositor config hooks.

#include <cstdint>
#include <optional>
#include <string>

namespace smplhud {

struct Rgb8 {
    uint8_t r = 0, g = 0, b = 0;

    bool operator==(const Rgb8&) const = default;
};

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
    double      sideOpacity    = 1;     // $themeHudSideOpacity, 0..1: how visible the outline of a panel style is down its left and right sides; 1 draws them like the rest
    std::optional<Rgb8> glyphColour;    // $themeHudGlyphColor, `rgb(rrggbb)`: the colour of the characters of a glyph style; `none` or absent leaves them the colours of the line

    bool operator==(const Theme&) const = default;
};

// Later lines win; out-of-range numbers are clamped, unparsable ones ignored.
Theme       parseTheme(const std::string& text);

// $XDG_CONFIG_HOME/hypr/theme.conf, falling back to ~/.config/hypr/theme.conf.
std::string themePath();

// Reads themePath(); a missing file gives the defaults (HUD off).
Theme       loadTheme();

} // namespace smplhud
