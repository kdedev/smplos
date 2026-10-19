#pragma once

// The notched, beaded frame of a Kind::Panel style.
//
// A panel is a closed outline of straight and 45 degree runs round the window:
// chamfered corners, a notch and a step on the top edge, a raised and a ghost
// line on the bottom edge and a bigger cut at the bottom left. Three thin
// traces run beyond it and end in beads, and dots travel along the lines
// while the window has focus. Everything is measured in physical px from the
// top-left corner of the window box and snapped so that lines land on whole
// pixels; see stamp.hpp for how the lines become images.
//
// Nothing here touches Hyprland, so it is unit tested with a plain compiler.

#include "geometry.hpp"

namespace smplhud {

// Fills `out.panel`, `out.centre`, `out.sampleBox`, `out.extents` and `out.valid`
// for a style of Kind::Panel. The extents do not depend on the window size.
void buildPanel(const Style& style, const Params& params, Layout& out);

struct RunnerPose {
    Vec2  pos;
    float alpha = 0.f; // 0 where the line under the dot is invisible
};

// Where `runner` is once the dots have travelled `travelled` px at speed 1.
// On a closed stroke it goes round and round; on an open one it runs from
// start to end, fades out there and starts again.
RunnerPose runnerPose(const Panel& panel, const Runner& runner, double travelled);

} // namespace smplhud
