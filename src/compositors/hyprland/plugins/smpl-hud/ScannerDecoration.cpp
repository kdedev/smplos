#include "ScannerDecoration.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
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
    // The picture moves on the beat of the frame's glow, when that is running:
    // a step of its own would only make the compositor draw one more picture.
    SP<CEventLoopTimer>                 g_timer;
    Time::steady_tp                     g_last;
    bool                                g_idle = true; // on the slow poll
    constexpr std::chrono::milliseconds kStep{1000 / smplhud::kScannerFps};
    constexpr std::chrono::milliseconds kFadeStep{33}; // while it fades, which is seen at once, whatever beats
    constexpr std::chrono::milliseconds kIdlePoll{100};
    constexpr double                    kMaxStep = 0.25; // s; a long stall must not make the scanner jump
    constexpr double                    kFade    = 0.5;  // s for it to come up from its rest and back
    constexpr float                     kRest    = 0.4F; // how much of its strength it has in a window without focus
    constexpr double                    kRoom    = 5;    // a window must be this many times as wide, and as high, as the scanner to have one
    constexpr double                    kReach   = 0.4;  // the pointer is near it within this many widths of the scanner's plate, or of its picture
    constexpr double                    kAway    = 0.15; // s for it to step aside from the pointer: quick, it is in the way
    constexpr double                    kBack    = 0.6;  // s for it to come back: slow, so a pointer passing by does not make it flicker

    bool wanted() {
        const auto& settings = hudSettings();
        return settings.active() && smplhud::scannerWidthOf(settings.theme.scanner) > 0;
    }

    smplhud::ScannerSize sizeOn(double scale) {
        return smplhud::scannerSize(smplhud::scannerWidthOf(hudSettings().theme.scanner), scale);
    }

    // How far it stands from the corner of a window that has no pod for it, in
    // logical px: clear of the rounding of the corner, and of the line of the
    // frame along the edge.
    double insetOf(const PHLWINDOW& window) {
        return std::max(6.0, window->rounding() * 0.3 + 2.0);
    }

    // Where the scanner is on a window, in physical px from the window's top
    // left corner, as the frame last laid it out.
    struct SPlace {
        smplhud::IBox                          picture;
        smplhud::IBox                          plate;  // empty without a pod
        smplhud::IBox                          area;   // the picture and the plate together
        std::shared_ptr<const smplhud::Layout> layout; // the layout of the pod; none without one
        float                                  scale  = 1;
        int                                    width  = 0; // of the window the layout was made for
        int                                    height = 0;
    };

    smplhud::IBox united(const smplhud::IBox& a, const smplhud::IBox& b) {
        if (b.w <= 0 || b.h <= 0)
            return a;
        if (a.w <= 0 || a.h <= 0)
            return b;

        const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    // None before the frame of the window has been laid out for the theme as it
    // is now, and where the window has no room for it.
    std::optional<SPlace> placeOf(const PHLWINDOW& window) {
        const auto info = hudLayoutOf(window);
        if (!info || !info->layout)
            return std::nullopt;

        const auto size = sizeOn(info->scale);
        if (size.w <= 0 || size.h <= 0)
            return std::nullopt;

        SPlace place;
        place.scale  = info->scale;
        place.width  = info->width;
        place.height = info->height;

        const auto& pod = info->layout->pod;
        if (pod.on) {
            if (pod.picture.w != size.w || pod.picture.h != size.h)
                return std::nullopt; // laid out for another size: the frame is about to be laid out again

            place.picture = pod.picture;
            place.plate   = smplhud::plateBox(pod, info->scale);
            place.layout  = info->layout;
        } else {
            if (info->width < kRoom * size.w || info->height < kRoom * size.h)
                return std::nullopt;

            const int edge = int(std::lround(insetOf(window) * info->scale));
            place.picture  = {info->width - edge - size.w, info->height - edge - size.h, size.w, size.h};
        }

        place.area = united(place.picture, place.plate);
        return place;
    }

    // `box`, in physical px from the corner of a window, in the px of the space the corner (x, y) is in: `perPx` is the size of one physical px there.
    CBox boxOf(const smplhud::IBox& box, double x, double y, double perPx) {
        return {x + box.x * perPx, y + box.y * perPx, box.w * perPx, box.h * perPx};
    }

    // The room it takes in the global logical px of the compositor.
    CBox areaOf(const PHLWINDOW& window, const SPlace& place) {
        const CBox global = hudWindowBoxGlobal(window);
        return boxOf(place.area, global.x, global.y, 1.0 / place.scale).expand(2);
    }

    // Whether the pointer, in the global logical px, is close enough to the
    // scanner for it to give way: what lies under it is about to be used.
    bool pointerNear(const PHLWINDOW& window, const SPlace& place, const Vector2D& pointer) {
        return areaOf(window, place).expand(kReach * place.picture.w / place.scale).containsPoint(pointer);
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

    // Steps every scanner on by the time since the last step. On a `beat` the
    // pictures move on a frame; in between only what fades is repainted.
    CHudScanner::SStep stepAll(bool beat) {
        const auto   now = Time::steadyNow();
        const double dt  = std::min(std::chrono::duration<double>(now - g_last).count(), kMaxStep);
        g_last           = now;

        CHudScanner::SStep all;
        if (!g_pSessionLockManager || !g_pSessionLockManager->isSessionLocked()) {
            const auto&    pointers = Pointer::mgr();
            const Vector2D pointer  = pointers ? pointers->position() : Vector2D{-1e9, -1e9};
            for (auto* scanner : g_scanners) {
                const auto step = scanner->advance(dt, pointer, beat);
                all.animating   = all.animating || step.animating;
                all.fading      = all.fading || step.fading;
            }
        }

        g_idle = !all.animating && !all.fading;
        return all;
    }

    void tick(SP<CEventLoopTimer> self, void*) {
        const bool beaten = hudSweepBeating();
        const auto step   = stepAll(!beaten);

        // Fades are stepped often. A picture that moves is stepped here only
        // when the glow of the frame does not step it: the pointer is watched all the same.
        const auto next = step.fading ? kFadeStep : (step.animating && !beaten ? kStep : kIdlePoll);
        self->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(next));
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

    if (const auto place = placeOf(window))
        g_pHyprRenderer->damageBox(areaOf(window, *place));
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

CHudScanner::SStep CHudScanner::advance(double seconds, const Vector2D& pointer, bool beat) {
    const auto window = m_window.lock();
    if (!wanted() || !validMapped(window) || !hudFramed(window) || hudFullscreen(window)) {
        m_seen = false;
        return {};
    }

    const auto monitor = window->m_monitor.lock();
    if (!monitor || !monitor->m_enabled || !monitor->m_dpmsStatus || !g_pHyprRenderer->shouldRenderWindow(window, monitor)) {
        if (!m_focused)
            m_level = 0; // out of sight: nothing to fade, it is at rest when it is seen again
        m_seen = false;
        return {};
    }

    // Until the frame of the window has been laid out there is nowhere to put it.
    const auto place = placeOf(window);
    if (!place) {
        m_seen = false;
        return {};
    }

    const bool near = pointerNear(window, *place, pointer);
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

    // The picture is made of the frame it shows, of how far it has come up and
    // of how far it has stepped aside; nothing else repaints. It takes its next
    // frame only on a beat, and what it shows is always the frame it has damaged
    // itself for. Nothing at all while it is out of the way.
    if (m_aside < 1 || aside < 1) {
        const long frame = long(std::floor(m_clock * smplhud::kScannerFps));
        const bool next  = beat && frame != m_frame;
        if (next)
            m_frame = frame;
        if (next || m_level != level || m_aside != aside)
            damage();
    }

    SStep step;
    step.animating = running && m_aside < 1;
    step.fading    = m_level != (m_focused ? 1.0 : 0.0) || m_aside != (near ? 1.0 : 0.0);
    return step;
}

void CHudScanner::draw(PHLMONITOR monitor, const float& a) {
    const auto window = m_window.lock();
    if (!wanted() || !monitor || !validMapped(window) || !hudFramed(window) || hudFullscreen(window))
        return;

    const float present = 1.F - float(smplhud::sweepIntensity(m_aside));
    if (present <= 0.F)
        return;

    // The colours of the frame under it, which has been drawn by now, and where
    // it laid the scanner out.
    const auto colours = hudColoursOf(window);
    const auto place   = placeOf(window);
    if (!colours || !place)
        return;

    // The picture goes on whole px of the monitor: its texels are the pixels. The
    // frame may not have been laid out for the window as it is on this monitor yet.
    const double scale    = monitor->m_scale;
    const CBox   local    = hudWindowBoxLocal(window, monitor);
    const CBox   physical = local.copy().scale(scale).round();
    if (place->scale != monitor->m_scale || place->width != int(physical.width) || place->height != int(physical.height))
        return;

    SScannerFrame frame;
    frame.paints    = colours->paints;
    frame.sampleBox = colours->sampleBox;
    frame.window    = physical;
    frame.box       = boxOf(place->picture, physical.x, physical.y, 1.0);
    frame.bounds    = boxOf(place->area, local.x, local.y, 1.0 / scale).expand(2);
    frame.size      = sizeOn(scale);
    frame.frame     = m_frame;
    frame.seed      = m_seed;
    frame.alpha     = hudFadeOf(window, a) * present * (kRest + (1.F - kRest) * float(smplhud::sweepIntensity(m_level)));
    frame.layout    = place->layout;
    frame.scale     = scale;
    frame.blur      = hudSettings().theme.scannerBlur;

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

void stepHudScanners() {
    if (g_timer)
        stepAll(true);
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
    clearScannerFrames(); // nothing shows them any more

    if (!g_timer)
        return;

    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_timer);
    g_timer.reset();
    g_idle = true;
}
