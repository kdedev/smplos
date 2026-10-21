#pragma once

// The scanner of a window: a small flat disc seen at a slant, like the radar of
// a space game, in the corner of the window. A bright arrow, the ship, sits in
// the middle of three rings. An arm sweeps round them and lights the contacts it
// passes, small markers that float above or below the disc on a thin stalk and
// drift across it, the way things go by when the ship flies.
//
// It is a picture made on the CPU one frame at a time from nothing but its
// `ScannerSpec`, so that a frame is always the same, can be tested and can be
// kept for as long as its spec does not change. Nothing here touches Hyprland.

#include "geometry.hpp"
#include "gradient.hpp"
#include "stamp.hpp"

#include <cstdint>
#include <vector>

namespace smplhud {

// How wide the scanner may be, in logical px (`hud_scanner`); a theme with 0 has none.
constexpr int kScannerMinWidth = 24;
constexpr int kScannerMaxWidth = 160;

// How many frames a second a moving scanner is drawn at.
constexpr int kScannerFps = 30;

// How many contacts there are at a time, at most.
constexpr int kScannerSlots = 4;

// The width a theme asks for, as it is used: nothing stays nothing, anything else is held between the least and the most.
int scannerWidthOf(double themeValue);

// The image in physical px: three quarters as high as it is wide. Empty for no width.
struct ScannerSize {
    int w = 0, h = 0;

    bool operator==(const ScannerSize&) const = default;
};

ScannerSize scannerSize(double width, double scale);

// Where things are in an image. Every length of the picture is a number of `u`,
// 1/48 of the width, so the scanner looks the same at any size.
struct ScannerLayout {
    int    w = 0, h = 0;
    double u = 0;
    Vec2   centre; // of the disc, in px from the top left corner of the image
    double rx = 0, ry = 0;

    bool valid() const { return w > 0 && h > 0; }
};

ScannerLayout scannerLayout(const ScannerSize& size);

// The colours of the scanner, as the theme's frame has them where the scanner is.
struct ScannerPalette {
    Rgba line;   // the rim, the rings and the stalks
    Rgba bright; // the ship, the arm and the markers
    Rgba glow;   // the halo
};

// Everything a frame depends on. The colours are rounded to 5 bits a channel
// and the time to a frame, so a slowly changing gradient bakes a new image only
// now and then, and equal specs make equal images.
struct ScannerSpec {
    ScannerSize size;
    long        frame = 0;
    uint32_t    seed  = 0; // windows with another seed are out of step with each other
    uint8_t     line[3]{}, bright[3]{}, glow[3]{};

    bool operator==(const ScannerSpec&) const = default;
};

ScannerSpec scannerSpecOf(const ScannerSize& size, double clock, uint32_t seed, const ScannerPalette& palette);

// The same for a frame number, for a caller that keeps the frame it shows apart from the clock.
ScannerSpec scannerSpecOfFrame(const ScannerSize& size, long frame, uint32_t seed, const ScannerPalette& palette);

// Premultiplied RGBA like the other images; empty if the size is.
Image renderScanner(const ScannerSpec& spec);

// The plate under the scanner's picture where a panel style has made a pod for it (`Pod` in
// geometry.hpp): dark glass over the window, held by the outline where it meets the frame and edged
// by a bezel, a line like the outline's, along the sides that are on the window. It depends on
// nothing but the pod's shape, the scale and the colours, not on where the window is, so the image
// of one window serves every other one.
// How opaque the glass is, 0..1. Where the compositor blurs what lies under the glass the blur does
// part of the hiding, so it can be lighter than where it is flat.
constexpr double kPlateGlassFlat    = 0.85;
constexpr double kPlateGlassBlurred = 0.62;

// How opaque the halo of the bezel is at its brightest. It stays under the glass, so the alpha tells
// the two apart when only the glass is to be blurred.
constexpr double kPlateHaloAlpha = 0.3;

struct PlateSpec {
    int               w = 0, h = 0;     // of the image, px
    std::vector<Vec2> pts;              // the corners clockwise, in px from the image's top left corner
    int               bezel       = 3;  // how many of the first sides are the bezel
    int               halfWidth16 = 16; // half the thickness of the outline's line, in 1/16 px
    int               reach       = 5;  // how far the bezel's halo reaches out onto the window, px
    uint8_t           line[3]{}, glow[3]{};
    uint8_t           glass = 0;        // how opaque the glass is, 0..255

    bool operator==(const PlateSpec&) const = default;
};

// A plate and where its image goes, in px from the window's top left corner.
struct Plate {
    PlateSpec spec;
    IBox      box;

    bool valid() const { return spec.w > 0 && spec.h > 0; }
};

// Where the plate's image goes, in px from the window's top left corner: the plate's own bounds with room
// for the bezel's halo. An empty box where `plateOf` is not valid.
IBox  plateBox(const Pod& pod, double scale);

// The plate of `pod` for the scale and colours; `glass` is how opaque the glass is, 0..1. Not valid without a pod.
Plate plateOf(const Pod& pod, double scale, const ScannerPalette& palette, double glass);

// Premultiplied RGBA like the other images; empty if the plate is not valid.
Image renderPlate(const PlateSpec& spec);

// A thing the scanner has found: where it is on the disc, where its marker is
// (above or below that place, on a stalk) and how it looks.
struct ScannerContact {
    int    slot  = 0;
    Vec2   foot;               // in px from the top left corner of the image
    Vec2   marker;
    double alpha = 0;          // 0..1: fades in and out at the edge of the disc and at the ends of its way
    double flash = 0;          // 0..1: lit up as the arm passes, and dying away after
    double size  = 1;          // 0.85 at the back of the disc to 1.15 at the front, as things look nearer there
    int    shape = 0;          // 0 square, 1 arrow up, 2 arrow down, 3 diamond
};

// The contacts that can be seen at a time, those at the back first.
std::vector<ScannerContact> scannerContacts(const ScannerLayout& layout, double clock, uint32_t seed);

// Where the arm points, in radians 0..2pi counted from the disc's right, round the way the hands of a clock go.
double scannerArm(double clock, uint32_t seed);

} // namespace smplhud
