#pragma once

// The notched, beaded frame of a Kind::Panel style.
//
// A panel is a closed outline of straight and 45 degree runs round the window:
// chamfered corners, a notch and a step on the top edge, a raised and a ghost
// line on the bottom edge and a bigger cut at the bottom left. Three thin
// traces run beyond it and end in beads. While the window has focus, dots move
// along the lines: two go round the outline in opposite directions and one
// bounces back and forth between the beads of each trace. A dot going round
// glows brighter when a bouncing dot or the other one comes close. Everything
// is measured in physical px from the top-left corner of the window box and
// snapped so that lines land on whole pixels; see stamp.hpp for how the lines
// become images.
//
// Nothing here touches Hyprland, so it is unit tested with a plain compiler.

#include "geometry.hpp"

#include <vector>

namespace smplhud {

// Fills `out.panel`, `out.centre`, `out.sampleBox`, `out.extents` and `out.valid`
// for a style of Kind::Panel. The extents do not depend on the window size.
void buildPanel(const Style& style, const Params& params, Layout& out);

struct RunnerPose {
    Vec2  pos;
    float alpha = 0.f; // 0 where the line under the dot is invisible
    float glow  = 0.f; // 0..1, how far the dot has swelled into the panel's flare
};

// How much a dot running back and forth slows down towards the ends of its
// line: at the ends it goes (1 - kBounceEase) times as fast as it does on
// average, half way along (1 + kBounceEase) times.
constexpr double kBounceEase = 0.4;

// Where `runner` is once the dots have travelled `travelled` px at speed 1.
// On a closed stroke it goes round and round, the other way if its speed is
// negative. On an open one it goes back and forth between the ends, never
// fading, and takes `length / speed` to get from one end to the other. Its
// glow is not known without the other dots; see runnerPoses.
RunnerPose runnerPose(const Panel& panel, const Runner& runner, double travelled);

// The poses of all of `panel.runners`, in that order, with the glow of the ones
// going round: it rises as they come close to a bouncing dot or to each other
// and is 1 where they touch.
std::vector<RunnerPose> runnerPoses(const Panel& panel, double travelled);

} // namespace smplhud
