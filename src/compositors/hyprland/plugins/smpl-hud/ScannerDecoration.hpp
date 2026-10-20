#pragma once

#define WLR_USE_UNSTABLE

#include <cstdint>
#include <memory>
#include <string>

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>

#include "HudPassElement.hpp"

inline constexpr const char* kScannerDisplayName = "smpl-hud-scanner";

// The scanner of a window (the theme's `hud_scanner`): a small animated radar
// in its bottom right corner, drawn over the window's own picture and lit with
// the colours of the frame. Only the window that has focus has it running; the
// others show it where it stopped, dimmed. It takes no input and no room, and
// it steps aside while the pointer is near it, so it never hides a button or a
// line of text that is being reached for.
class CHudScanner : public IHyprWindowDecoration {
  public:
    explicit CHudScanner(PHLWINDOW window);
    ~CHudScanner() override;

    SDecorationPositioningInfo getPositioningInfo() override;
    void                       onPositioningReply(const SDecorationPositioningReply&) override;
    void                       draw(PHLMONITOR monitor, const float& a) override;
    eDecorationType            getDecorationType() override;
    void                       updateWindow(PHLWINDOW) override;
    void                       damageEntire() override;
    uint64_t                   getDecorationFlags() override;
    eDecorationLayer           getDecorationLayer() override;
    std::string                getDisplayName() override;

    // Tells it whether its window is the focused one (`focused` is the
    // compositor's focused window, null if none); cheap to repeat.
    void syncFocus(const PHLWINDOW& focused);

    // Runs it on by `seconds` and damages its box when the picture changes.
    // `pointer` is where the pointer is, in the layout's px. True while it is
    // moving, so the timer must go on stepping often.
    bool advance(double seconds, const Vector2D& pointer);

    // Its colours follow the window's gradients, so it is repainted while they
    // fade or turn. The compositor's own decorations only damage themselves.
    void damageIfAnimated();

  private:
    void damage() const;

    PHLWINDOWREF                   m_window;
    bool                           m_focused = false;
    bool                           m_seen    = false; // it was in sight at the last step
    double                         m_level   = 0;     // how far it has come up from its rest, [0, 1]
    double                         m_aside   = 0;     // how far it has stepped aside for the pointer, [0, 1]
    double                         m_clock   = 0;     // seconds it has been running: stops with the focus, once it has faded
    long                           m_frame   = -1;    // the frame of the clock it was last damaged for
    uint32_t                       m_seed    = 0;     // puts it out of step with the scanners of other windows
    std::shared_ptr<SScannerCache> m_cache;
};

// The scanners follow the theme and the compositor's focus the way the frames
// do. These are the hooks of HudDecoration.cpp for them.

// Hands the focus to the scanners.
void syncScannerFocus(const PHLWINDOW& focused);

// The theme has changed: the timer that runs the scanners is there only while
// the theme has one.
void syncHudScanners();

void damageAnimatedScanners();
void damageAllScanners();

// Stops the timer; the plugin is unloading.
void stopHudScanners();
