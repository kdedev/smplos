#pragma once

#define WLR_USE_UNSTABLE

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>

#include "geometry.hpp"
#include "panel.hpp"
#include "sweep.hpp"
#include "theme.hpp"

struct SHudSweepCache;

inline constexpr const char* kHudDisplayName = "smpl-hud";

// The HUD the active theme asks for: its knobs and the style they resolve to.
struct SHudSettings {
    smplhud::Theme        theme;
    const smplhud::Style* style    = nullptr;
    uint64_t              revision = 0; // bumped on every change so cached layouts are rebuilt

    bool active() const {
        return theme.frame && style;
    }

    // Whether the glow that travels along the line is on.
    bool sweeping() const {
        return active() && theme.sweep && style->sweepAlpha > 0;
    }
};

const SHudSettings& hudSettings();

// Switches every window to a new theme: damages the old look, re-registers
// the new extents with the compositor, damages the new look.
void setHudTheme(const smplhud::Theme& theme);

// Damages the HUD of windows whose border or glow gradient is animating. The
// compositor's own border/glow decorations only damage themselves.
void damageAnimatedHuds();
void damageAllHuds();

// Stops the timer that moves the travelling glow; the plugin is unloading.
void stopHudSweep();

// The travelling glow, and the turning of the border and glow gradients the
// theme can ask for, belong to the window that has focus. This follows the
// compositor's focus: it does nothing if focus has not moved since the last
// call, unless `force`.
void syncHudFocus(bool force = false);

// Hands the turning gradients of every window back to the compositor, as if
// the plugin had never been loaded. No code of this plugin may be left in a
// window when it unloads.
void restoreHudLoops();

// A window decoration that paints the HUD in the compositor's own render pass,
// with the window's live animated geometry, so it can never lag behind it and
// sits in the right place in the window stack. It takes no input.
class CHudDecoration : public IHyprWindowDecoration {
  public:
    explicit CHudDecoration(PHLWINDOW window);
    ~CHudDecoration() override;

    SDecorationPositioningInfo getPositioningInfo() override;
    void                       onPositioningReply(const SDecorationPositioningReply&) override;
    void                       draw(PHLMONITOR monitor, const float& a) override;
    eDecorationType            getDecorationType() override;
    void                       updateWindow(PHLWINDOW) override;
    void                       damageEntire() override;
    uint64_t                   getDecorationFlags() override;
    eDecorationLayer           getDecorationLayer() override;
    std::string                getDisplayName() override;

    void reposition();
    void damageIfAnimated();

    // Tells the window whether it is the focused one (`focused` is the
    // compositor's focused window, null if none). Only the focused window has
    // a travelling glow and turning gradients; it is cheap to repeat.
    void syncFocus(const PHLWINDOW& focused, bool force);

    // What one step of the travelling glow did.
    struct SSweepStep {
        bool moved  = false; // something visible changed, so keep stepping often
        bool fading = false; // the glow is fading in or out
    };

    // Moves the glow on by `seconds`, fades it in or out, and damages the two
    // small boxes it leaves and enters. Nothing moves while the window is not
    // on screen.
    SSweepStep advanceSweep(double seconds);
    void       dropSweep();

  private:
    struct SLayoutKey {
        int      width = 0, height = 0, cut = 0;
        float    scale = 0, roundingPower = 0;
        bool     floating = false;
        uint64_t revision = 0;
        bool     operator==(const SLayoutKey&) const = default;
    };

    // The travelling glow. The path is built from the layout, so it goes when
    // the layout does; the phase is a fraction of the path and survives that.
    struct SSweep {
        std::shared_ptr<const smplhud::Path> path; // null while off, or before the first draw
        smplhud::SweepShape                  shape;
        smplhud::IBox                        limit; // window-relative physical px
        int                                  texel = 1;
        double                               scale = 1; // of the monitor the path was built for
        double                               phase = 0; // [0, 1) round the path
        std::shared_ptr<SHudSweepCache>      cache;
    };

    void buildSweep(float scale);
    void setFocused(bool focused, bool force);

    PHLWINDOWREF                           m_window;
    smplhud::Extents                       m_extents; // what the compositor was last told
    std::shared_ptr<const smplhud::Layout> m_layout;
    SLayoutKey                             m_layoutKey;
    SSweep                                 m_sweep;
    bool                                   m_focused    = false;
    double                                 m_sweepLevel = 0; // how far the glow has faded in, [0, 1]
    double                                 m_dotPos     = 0; // how far the runner dots have travelled at speed 1, physical px; it outlives the glow so the dots go on where they were
    double                                 m_clock      = 0; // seconds the characters of a glyph style have been running, which stops with the glow
    unsigned                               m_seed       = 0; // gives this window's characters a rhythm of their own
    std::vector<smplhud::GlyphDraw>        m_glyphs;         // the characters as last drawn or stepped to: what the next step is compared with
    int                                    m_waiting    = 0; // steps spent waiting for the first draw to build the path
};
