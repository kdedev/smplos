#pragma once

#define WLR_USE_UNSTABLE

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include "geometry.hpp"
#include "panel.hpp"
#include "sweep.hpp"

// The colours one shape kind (smplhud::Paint) is drawn with, mid-fade or not.
struct SHudPaint {
    Config::CGradientValueData to;
    Config::CGradientValueData from; // only meaningful while fading
    bool                       fading = false;
    float                      lerp   = 1.F;
};

// The glow image uploaded to the GPU, kept from frame to frame: it changes
// only when the glow moves or its colour visibly changes, not every time the
// window repaints.
struct SHudSweepCache {
    std::shared_ptr<const smplhud::Path> path; // what the texture was drawn for
    double                               at    = -1;
    int                                  texel = 0;
    smplhud::SweepShape                  shape;
    smplhud::IBox                        limit;
    std::array<uint8_t, 4>               colour{}; // quantised r, g, b, strength
    smplhud::IBox                        box;      // where the texture goes, window-relative physical px
    SP<Render::ITexture>                 texture;
};

// One travelling glow of one window at one moment.
struct SHudSweep {
    std::shared_ptr<const smplhud::Path> path;
    smplhud::SweepShape                  shape;
    smplhud::IBox                        limit; // window-relative physical px the glow must stay inside
    int                                  texel = 1;
    double                               at    = 0; // distance along the path, physical px
    std::shared_ptr<SHudSweepCache>      cache;   // the glow's own: two glows never share one
    float                                intensity = 1.F; // fades the glow in and out, and a lamp's pulse; never part of the cached image
};

// Everything one frame of one window's HUD needs. It is copied out of the
// window when the pass element is queued, so drawing never touches the window
// or its decoration again.
struct SHudFrame {
    std::shared_ptr<const smplhud::Layout> layout;
    std::array<SHudPaint, 3>               paints; // indexed by smplhud::Paint
    std::vector<SHudSweep>                 sweeps; // one glow going round the line, or the lamps of a lamp style
    std::vector<smplhud::RunnerPose>       runners; // one per layout->panel.runners; their opacity already includes the fade with the focus, their glow says how much of the flare goes over them
    std::vector<smplhud::GlyphDraw>        glyphs;  // the falling characters of a glyph style
    float                                  alpha         = 1.F;
    float                                  roundingPower = 1.F;
    CBox                                   window;  // window box, monitor-local physical px
    CBox                                   bounds;  // everything the HUD can touch, monitor-local logical px
};

// Drops the baked line, dot and character images a panel style keeps on the GPU,
// shared by all windows. They are baked again when next needed.
void clearHudStamps();

class CHudPassElement : public IPassElement {
  public:
    explicit CHudPassElement(SHudFrame frame);
    ~CHudPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    bool                          needsLiveBlur() override;
    bool                          needsPrecomputeBlur() override;
    std::optional<CBox>           boundingBox() override;

    const char* passName() override {
        return kPassName;
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

    static constexpr const char* kPassName = "CSmplHudPassElement";

  private:
    SHudFrame m_frame;
};
