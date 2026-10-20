#include "ScannerDecoration.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "HudDecoration.hpp"
#include "scanner.hpp"
#include "sweep.hpp"

namespace {
    std::vector<CHudScanner*> g_scanners;

    // One timer runs the scanner of the window that has focus, and fades the one
    // that lost it. It runs only while the theme has a scanner, and while
    // nothing moves it backs off to a slow poll, which also watches the pointer.
    SP<CEventLoopTimer>                 g_timer;
    Time::steady_tp                     g_last;
    bool                                g_idle = true; // on the slow poll
    constexpr std::chrono::milliseconds kStep{1000 / smplhud::kScannerFps};
    constexpr std::chrono::milliseconds kIdlePoll{100};
    constexpr double                    kMaxStep = 0.25; // s; a long stall must not make the scanner jump
    constexpr double                    kFade    = 0.5;  // s for it to come up from its rest and back
    constexpr float                     kRest    = 0.4F; // how much of its strength it has in a window without focus
    constexpr double                    kRoom    = 5;    // a window must be this many times as wide, and as high, as the scanner to have one
    constexpr double                    kReach   = 0.75; // the pointer is near it within this many widths of the scanner
    constexpr double                    kAway    = 0.15; // s for it to step aside from the pointer: quick, it is in the way
    constexpr double                    kBack    = 0.6;  // s for it to come back: slow, so a pointer passing by does not make it flicker

    bool wanted() {
        const auto& settings = hudSettings();
        return settings.active() && smplhud::scannerWidthOf(settings.theme.scanner) > 0;
    }

    smplhud::ScannerSize sizeOn(double scale) {
        return smplhud::scannerSize(smplhud::scannerWidthOf(hudSettings().theme.scanner), scale);
    }

    // How far it stands from the corner of the window, in logical px: clear of
    // the rounding of the corner, and of the line of the frame along the edge.
    double insetOf(const PHLWINDOW& window) {
        return std::max(6.0, window->rounding() * 0.3 + 2.0);
    }

    bool fits(const CBox& window, const smplhud::ScannerSize& size, double scale) {
        return size.w > 0 && size.h > 0 && window.width >= kRoom * size.w / scale && window.height >= kRoom * size.h / scale;
    }

    // The box of the scanner in the logical px of the window box `window`.
    CBox boxIn(const CBox& window, double inset, const smplhud::ScannerSize& size, double scale) {
        const double w = size.w / scale, h = size.h / scale;
        return {window.x + window.width - inset - w, window.y + window.height - inset - h, w, h};
    }

    // Whether the pointer, in the global logical px, is close enough to the
    // scanner for it to give way: what lies under it is about to be used.
    bool pointerNear(const PHLWINDOW& window, double scale, const Vector2D& pointer) {
        const auto size = sizeOn(scale);
        const CBox box  = hudWindowBoxGlobal(window);
        return fits(box, size, scale) && boxIn(box, insetOf(window), size, scale).expand(kReach * size.w / scale).containsPoint(pointer);
    }

    // Has the timer step soon: a window has just been given focus.
    void wake() {
        if (!g_timer)
            return;

        if (g_idle) {
            g_last = Time::steadyNow(); // not the whole time it slept
            g_idle = false;
        }
        g_timer->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(std::chrono::milliseconds{1}));
    }

    void tick(SP<CEventLoopTimer> self, void*) {
        const auto   now = Time::steadyNow();
        const double dt  = std::min(std::chrono::duration<double>(now - g_last).count(), kMaxStep);
        g_last           = now;

        bool moving = false;
        if (!g_pSessionLockManager || !g_pSessionLockManager->isSessionLocked()) {
            const auto&    pointers = Pointer::mgr();
            const Vector2D pointer  = pointers ? pointers->position() : Vector2D{-1e9, -1e9};
            for (auto* scanner : g_scanners)
                moving = scanner->advance(dt, pointer) || moving;
        }

        g_idle = !moving;
        self->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(moving ? kStep : kIdlePoll));
    }
}

CHudScanner::CHudScanner(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {
    static unsigned s_scanners = 0;
    m_seed                     = s_scanners++ * 2654435761U;

    g_scanners.push_back(this);
}

CHudScanner::~CHudScanner() {
    std::erase(g_scanners, this);
}

SDecorationPositioningInfo CHudScanner::getPositioningInfo() {
    // It paints inside the window and tells the compositor of no room around it.
    SDecorationPositioningInfo info;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.edges          = DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT;
    info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CHudScanner::onPositioningReply(const SDecorationPositioningReply&) {
    ;
}

void CHudScanner::updateWindow(PHLWINDOW) {
    ;
}

eDecorationType CHudScanner::getDecorationType() {
    return DECORATION_CUSTOM;
}

uint64_t CHudScanner::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

// Over the window: it is in the window's corner, where the window's own picture
// would hide it. The windows above this one and its popups still cover it.
eDecorationLayer CHudScanner::getDecorationLayer() {
    return DECORATION_LAYER_OVER;
}

std::string CHudScanner::getDisplayName() {
    return kScannerDisplayName;
}

void CHudScanner::damageEntire() {
    damage();
}

void CHudScanner::damage() const {
    const auto window = m_window.lock();
    if (!wanted() || !validMapped(window))
        return;

    const auto monitor = window->m_monitor.lock();
    if (!monitor)
        return;

    const double scale = monitor->m_scale;
    const auto   size  = sizeOn(scale);
    const CBox   box   = hudWindowBoxGlobal(window);
    if (fits(box, size, scale))
        g_pHyprRenderer->damageBox(boxIn(box, insetOf(window), size, scale).expand(2));
}

void CHudScanner::damageIfAnimated() {
    const auto window = m_window.lock();
    if (!wanted() || !validMapped(window))
        return;

    const bool animated = window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_borderFadeAnimationProgress->isBeingAnimated() ||
        window->m_glowAngleAnimationProgress->isBeingAnimated() || window->m_glowFadeAnimationProgress->isBeingAnimated();
    if (animated && g_pHyprRenderer->shouldRenderWindow(window))
        damage();
}

void CHudScanner::syncFocus(const PHLWINDOW& focused) {
    const bool now = focused && m_window.lock() == focused;
    if (now == m_focused)
        return;

    m_focused = now;
    wake();
}

bool CHudScanner::advance(double seconds, const Vector2D& pointer) {
    const auto window = m_window.lock();
    if (!wanted() || !validMapped(window) || !hudFramed(window) || hudFullscreen(window)) {
        m_seen = false;
        return false;
    }

    const auto monitor = window->m_monitor.lock();
    if (!monitor || !monitor->m_enabled || !monitor->m_dpmsStatus || !g_pHyprRenderer->shouldRenderWindow(window, monitor)) {
        if (!m_focused)
            m_level = 0; // out of sight: nothing to fade, it is at rest when it is seen again
        m_seen = false;
        return false;
    }

    const bool near = pointerNear(window, monitor->m_scale, pointer);
    if (!m_seen) {
        m_aside = near ? 1 : 0; // it comes into sight as it will stay, not stepping aside as it appears
        m_seen  = true;
    }

    const bool   running = m_focused || m_level > 0;
    const double level   = m_level;
    const double aside   = m_aside;
    if (running) {
        m_level = smplhud::sweepFade(m_level, m_focused, seconds, kFade);
        m_clock += seconds;
    }
    m_aside = smplhud::sweepFade(m_aside, near, seconds, near ? kAway : kBack);

    // The picture is made of the frames of the clock, of how far it has come up
    // and of how far it has stepped aside; nothing else repaints. Nothing at all
    // while it is out of the way.
    const long frame = long(std::floor(m_clock * smplhud::kScannerFps));
    if ((m_aside < 1 || aside < 1) && (frame != m_frame || m_level != level || m_aside != aside))
        damage();
    m_frame = frame;

    return (running && m_aside < 1) || m_aside != (near ? 1.0 : 0.0);
}

void CHudScanner::draw(PHLMONITOR monitor, const float& a) {
    const auto window = m_window.lock();
    if (!wanted() || !monitor || !validMapped(window) || !hudFramed(window) || hudFullscreen(window))
        return;

    const float present = 1.F - float(smplhud::sweepIntensity(m_aside));
    if (present <= 0.F)
        return;

    // The colours of the frame under it, which has been drawn by now.
    const auto colours = hudColoursOf(window);
    if (!colours)
        return;

    const double scale = monitor->m_scale;
    const auto   size  = sizeOn(scale);
    const CBox   local = hudWindowBoxLocal(window, monitor);
    if (!fits(local, size, scale))
        return;

    // The picture goes on whole px of the monitor: its texels are the pixels.
    const double inset    = insetOf(window);
    const CBox   physical = local.copy().scale(scale).round();
    const double edge     = std::round(inset * scale);

    SScannerFrame frame;
    frame.paints    = colours->paints;
    frame.sampleBox = colours->sampleBox;
    frame.window    = physical;
    frame.box       = {physical.x + physical.width - edge - size.w, physical.y + physical.height - edge - size.h, double(size.w), double(size.h)};
    frame.bounds    = boxIn(local, inset, size, scale).expand(2);
    frame.size      = size;
    frame.clock     = m_clock;
    frame.seed      = m_seed;
    frame.alpha     = hudFadeOf(window, a) * present * (kRest + (1.F - kRest) * float(smplhud::sweepIntensity(m_level)));

    if (!m_cache)
        m_cache = std::make_shared<SScannerCache>();
    frame.cache = m_cache;

    g_pHyprRenderer->addPassElement(makeUnique<CScannerPassElement>(std::move(frame)));
}

void syncScannerFocus(const PHLWINDOW& focused) {
    for (auto* scanner : g_scanners)
        scanner->syncFocus(focused);
}

void syncHudScanners() {
    if (wanted()) {
        if (!g_timer) {
            g_last  = Time::steadyNow();
            g_idle  = true;
            g_timer = makeShared<CEventLoopTimer>(std::chrono::duration_cast<Time::steady_dur>(kIdlePoll), tick, nullptr);
            g_pEventLoopManager->addTimer(g_timer);
        }
        wake(); // the focused window may be waiting for it
        return;
    }

    stopHudScanners();
}

void damageAnimatedScanners() {
    for (auto* scanner : g_scanners)
        scanner->damageIfAnimated();
}

void damageAllScanners() {
    for (auto* scanner : g_scanners)
        scanner->damageEntire();
}

void stopHudScanners() {
    if (!g_timer)
        return;

    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_timer);
    g_timer.reset();
    g_idle = true;
}
