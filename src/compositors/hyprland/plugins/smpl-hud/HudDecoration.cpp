#include "HudDecoration.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>

#include "HudPassElement.hpp"
#include "globals.hpp"
#include "stamp.hpp"

namespace {
    using smplhud::Paint;

    SHudSettings                 g_settings;
    smplhud::Extents             g_extents;
    std::vector<CHudDecoration*> g_decorations;
    PHLWINDOWREF                 g_focus; // the window last given focus

    // One timer moves the travelling glow of the focused window and fades out
    // the one that lost focus. It runs only while the theme asks for one, and
    // while nothing could be seen moving it backs off to a slow poll.
    SP<CEventLoopTimer>                 g_sweepTimer;
    Time::steady_tp                     g_sweepLast;
    bool                                g_sweepIdle = true; // on the slow poll
    constexpr std::chrono::milliseconds kSweepIdlePoll{250};
    constexpr std::chrono::milliseconds kSweepFadeStep{33}; // a fade shows steps the glow's own pace hides
    constexpr double                    kSweepMaxStep   = 0.25; // s; a long stall must not make the glow jump
    constexpr double                    kSweepFade      = 0.5;  // s for the glow to fade in or out
    constexpr int                       kSweepWaitSteps = 8;    // how long a focused glow waits for its first draw

    // The speed, in logical px a second, of the fastest thing the HUD moves:
    // the glow, or the dots of a panel, which run ahead of it. It sets how
    // often the timer steps.
    double motionSpeed() {
        const double dots = g_settings.style ? g_settings.style->runnerSpeed : 1.0;
        return g_settings.theme.sweepSpeed * std::max(1.0, dots);
    }

    void sweepTick(SP<CEventLoopTimer> self, void*) {
        const auto   now = Time::steadyNow();
        const double dt  = std::min(std::chrono::duration<double>(now - g_sweepLast).count(), kSweepMaxStep);
        g_sweepLast      = now;

        syncHudFocus(); // focus can move without anyone being told

        CHudDecoration::SSweepStep all;
        if (!g_pSessionLockManager || !g_pSessionLockManager->isSessionLocked()) {
            for (auto* deco : g_decorations) {
                const auto step = deco->advanceSweep(dt);
                all.moved       = all.moved || step.moved;
                all.fading      = all.fading || step.fading;
            }
        }

        g_sweepIdle = !all.moved;

        std::chrono::duration<double> next = kSweepIdlePoll;
        if (all.moved) {
            next = std::chrono::duration<double>(smplhud::sweepInterval(motionSpeed()));
            if (all.fading)
                next = std::min(next, std::chrono::duration<double>(kSweepFadeStep));
        }
        self->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(next));
    }

    // Has the timer step soon: a window has just been given focus.
    void wakeHudSweep() {
        if (!g_sweepTimer)
            return;

        if (g_sweepIdle) {
            g_sweepLast = Time::steadyNow(); // not the whole time it slept
            g_sweepIdle = false;
        }
        g_sweepTimer->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(std::chrono::milliseconds{1}));
    }

    void syncHudSweep() {
        if (g_settings.sweeping()) {
            if (!g_sweepTimer) {
                g_sweepLast  = Time::steadyNow();
                g_sweepIdle  = true;
                g_sweepTimer = makeShared<CEventLoopTimer>(std::chrono::duration_cast<Time::steady_dur>(kSweepIdlePoll), sweepTick, nullptr);
                g_pEventLoopManager->addTimer(g_sweepTimer);
            }
            return;
        }

        stopHudSweep();
        for (auto* deco : g_decorations)
            deco->dropSweep();
    }

    // The gradients a theme can set turning in a loop. The compositor starts
    // each loop when a window maps and, when a lap ends, calls its own function
    // to start the next; that call is all the focus has to take away or give back.
    using LoopEnd = void (Desktop::View::CWindow::*)(WP<Hyprutils::Animation::CBaseAnimatedVariable>);

    struct SLoop {
        PHLANIMVAR<float> Desktop::View::CWindow::* var;
        LoopEnd                                     end;
    };

    constexpr std::array<SLoop, 3> kLoops = {{
        {&Desktop::View::CWindow::m_borderAngleAnimationProgress, &Desktop::View::CWindow::onBorderAngleAnimEnd},
        {&Desktop::View::CWindow::m_glowAngleAnimationProgress, &Desktop::View::CWindow::onGlowAngleAnimEnd},
        {&Desktop::View::CWindow::m_shadowAngleAnimationProgress, &Desktop::View::CWindow::onShadowAngleAnimEnd},
    }};

    // The windows whose loops this plugin has taken a hook from or given one
    // to. Kept apart from the decorations: a window that is unmapped, or whose
    // decoration is gone, must still be given back when the plugin unloads.
    std::vector<PHLWINDOWREF> g_scoped;

    void remember(const PHLWINDOW& window) {
        std::erase_if(g_scoped, [](const PHLWINDOWREF& ref) { return ref.expired(); });
        if (std::ranges::none_of(g_scoped, [&](const PHLWINDOWREF& ref) { return ref == window; }))
            g_scoped.emplace_back(window);
    }

    bool looping(const PHLANIMVAR<float>& var) {
        return var->enabled() && var->getStyle() == "loop";
    }

    // Fullscreen has no border, glow or frame, so nothing is left to turn. Asked
    // of the controller: the window's cached border size is not yet updated when
    // the fullscreen event arrives.
    bool fullscreen(const PHLWINDOW& window) {
        return Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    }

    // Only the focused window's gradients turn. A loop runs lap after lap, each
    // lap's end starting the next: the focused window keeps that going, the
    // others finish the lap they are on and then rest, which looks the same as
    // a gradient that never turned (a whole lap is one full turn).
    void scopeLoops(const PHLWINDOW& window, bool focused) {
        remember(window);

        for (const auto& loop : kLoops) {
            const auto& var = (*window).*loop.var;
            if (!var)
                continue;

            if (!focused) {
                var->setCallbackOnEnd(nullptr); // first: warping runs the hook
                if (var->isBeingAnimated() && var->value() < 0.02F)
                    var->setValueAndWarp(1.F); // a lap only just begun is nothing to wait for
                continue;
            }

            if (!looping(var))
                continue;

            // At the end of a lap the compositor's own function starts the next
            // and puts its own hook back. On a gradient at rest that is at once.
            var->setCallbackOnEnd(
                [ref = PHLWINDOWREF(window), end = loop.end](WP<Hyprutils::Animation::CBaseAnimatedVariable> self) {
                    if (const auto target = ref.lock())
                        ((*target).*end)(self);
                },
                true);
        }
    }

    // Every window registers the same extents, so a theme change is one
    // re-registration. They only have to cover the widest look (floating).
    smplhud::Extents canonicalExtents(const SHudSettings& settings) {
        if (!settings.active())
            return {};

        smplhud::Params params;
        params.scale          = 1.F;
        params.floating       = true;
        params.gap            = settings.theme.gap;
        params.lineWidth      = settings.theme.lineWidth;
        params.chamfer        = settings.theme.chamfer;
        params.floatGlowRange = settings.theme.floatGlowRange;
        return smplhud::extentsFor(*settings.style, params);
    }

    // The window box in global logical coordinates, moved by whatever
    // animations the compositor is applying right now.
    CBox windowBoxGlobal(const PHLWINDOW& window) {
        CBox       box       = window->getWindowMainSurfaceBox();
        const auto workspace = window->m_workspace;
        if (workspace && workspace->m_renderOffset->isBeingAnimated() && !window->m_pinned)
            box.translate(workspace->m_renderOffset->value());
        box.translate(window->m_floatingOffset);
        return box;
    }

    // The same box in logical coordinates relative to `monitor`, from the live
    // animated geometry the window surface itself is drawn with.
    CBox windowBoxLocal(const PHLWINDOW& window, const PHLMONITOR& monitor) {
        const auto workspace = window->m_workspace;
        const auto offset    = workspace && !window->m_pinned ? workspace->m_renderOffset->value() : Vector2D();
        CBox       box       = {window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
        box.translate(-monitor->m_position + offset + window->m_floatingOffset);
        return box;
    }

    CBox grown(const CBox& box, const smplhud::Extents& e, int slack) {
        return {box.x - e.left - slack, box.y - e.top - slack, box.width + e.left + e.right + 2 * slack, box.height + e.top + e.bottom + 2 * slack};
    }

    bool visible(const Config::CGradientValueData& gradient) {
        return std::ranges::any_of(gradient.m_colors, [](const CHyprColor& c) { return c.a >= 0.01; });
    }

    // Mirrors what the compositor's own border and glow decorations do with a
    // window's gradient, on a copy so the window's state stays untouched.
    template <typename AnimVar>
    SHudPaint animatedPaint(const Config::CGradientValueData& current, Config::CGradientValueData previous, const AnimVar& angle, const AnimVar& fade) {
        SHudPaint paint;
        paint.to = current;

        if (angle->enabled()) {
            paint.to.m_angle += angle->value() * M_PI * 2;
            paint.to.m_angle = normalizeAngleRad(paint.to.m_angle);
            previous.m_angle = paint.to.m_angle; // a rotating gradient should not also fade between two angles
        }

        if (fade->isBeingAnimated()) {
            paint.fading = true;
            paint.lerp   = fade->value();
            paint.from   = std::move(previous);
        }
        return paint;
    }

    SHudPaint whitened(const SHudPaint& source, float amount) {
        const auto whiten = [amount](const Config::CGradientValueData& gradient) {
            std::vector<CHyprColor> colors;
            colors.reserve(gradient.m_colors.size());
            for (const auto& c : gradient.m_colors)
                colors.emplace_back(c.r + (1.0 - c.r) * amount, c.g + (1.0 - c.g) * amount, c.b + (1.0 - c.b) * amount, c.a);
            return Config::CGradientValueData(std::move(colors), gradient.m_angle);
        };

        SHudPaint out = source;
        out.to        = whiten(source.to);
        out.from      = whiten(source.from);
        return out;
    }

    // Adds to `region` the boxes, in global logical px, of the dots of `panel`
    // that are visible once they have travelled `travelled`. `origin` is the
    // window's box.
    void addRunnerBoxes(CRegion& region, const smplhud::Panel& panel, double travelled, const CBox& origin, double scale) {
        const auto poses = smplhud::runnerPoses(panel, travelled);
        for (size_t i = 0; i < poses.size(); ++i) {
            const auto& pose = poses[i];
            if (pose.alpha <= 0.F)
                continue;

            // A dot that glows is drawn with the flare over it.
            int size = smplhud::beadSize(smplhud::lookOf(panel.runners[i].look));
            if (pose.glow > 0.F)
                size = std::max(size, smplhud::beadSize(smplhud::lookOf(panel.flare)));

            const double half = size / 2.0 + 1;
            CBox         box  = {origin.x + (pose.pos.x - half) / scale, origin.y + (pose.pos.y - half) / scale, 2 * half / scale, 2 * half / scale};
            region.add(CRegion(box.expand(2)));
        }
    }
}

const SHudSettings& hudSettings() {
    return g_settings;
}

void setHudTheme(const smplhud::Theme& theme) {
    for (auto* deco : g_decorations)
        deco->damageEntire();

    g_settings.theme = theme;
    g_settings.style = smplhud::styleFor(theme.style);
    g_settings.revision++;
    g_extents = canonicalExtents(g_settings);

    if (theme.frame && !g_settings.style)
        HyprlandAPI::addNotification(PHANDLE, "[smpl-hud] unknown hud_style '" + theme.style + "'", CHyprColor{1.0, 0.6, 0.2, 1.0}, 5000);

    for (auto* deco : g_decorations)
        deco->reposition();
    for (auto* deco : g_decorations)
        deco->damageEntire();

    syncHudSweep();

    // Focus-only motion is the HUD's: without a HUD the compositor's loops are left alone.
    if (g_settings.active())
        syncHudFocus(true);
    else
        restoreHudLoops();
}

void stopHudSweep() {
    if (!g_sweepTimer)
        return;

    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_sweepTimer);
    g_sweepTimer.reset();
    g_sweepIdle = true;
}

void syncHudFocus(bool force) {
    const PHLWINDOW focused = Desktop::focusState()->window();
    if (!force && g_focus.lock() == focused)
        return;

    g_focus = focused;
    if (!g_settings.active())
        return;

    for (auto* deco : g_decorations)
        deco->syncFocus(focused, force);
}

void restoreHudLoops() {
    // At shutdown the windows are going anyway and nothing may be started.
    const bool shuttingDown = g_pCompositor && g_pCompositor->m_isShuttingDown;

    for (const auto& ref : g_scoped) {
        const auto window = ref.lock();
        if (!window)
            continue;

        for (const auto& loop : kLoops) {
            const auto& var = (*window).*loop.var;
            if (!var)
                continue;

            var->setCallbackOnEnd(nullptr); // whatever the state: none of this plugin's code may stay in a window
            if (!shuttingDown && looping(var))
                ((*window).*loop.end)(WP<Hyprutils::Animation::CBaseAnimatedVariable>(var));
        }
    }

    g_scoped.clear();
    g_focus.reset();
}

void damageAnimatedHuds() {
    for (auto* deco : g_decorations)
        deco->damageIfAnimated();
}

void damageAllHuds() {
    for (auto* deco : g_decorations)
        deco->damageEntire();
}

CHudDecoration::CHudDecoration(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {
    // Windows start at different points of their lines, spread by the golden ratio.
    static unsigned s_decorations = 0;
    m_sweep.phase                 = std::fmod(s_decorations++ * 0.6180339887498949, 1.0);

    g_decorations.push_back(this);
}

CHudDecoration::~CHudDecoration() {
    std::erase(g_decorations, this);
}

SDecorationPositioningInfo CHudDecoration::getPositioningInfo() {
    m_extents = g_extents;

    // Absolute and non-solid: the HUD only tells the compositor how far it
    // paints, so damage and culling cover it, and reserves no space.
    SDecorationPositioningInfo info;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.edges          = DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT;
    info.desiredExtents = {{double(m_extents.left), double(m_extents.top)}, {double(m_extents.right), double(m_extents.bottom)}};
    return info;
}

void CHudDecoration::onPositioningReply(const SDecorationPositioningReply&) {
    ;
}

void CHudDecoration::updateWindow(PHLWINDOW) {
    ;
}

eDecorationType CHudDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

uint64_t CHudDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

eDecorationLayer CHudDecoration::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

std::string CHudDecoration::getDisplayName() {
    return kHudDisplayName;
}

void CHudDecoration::reposition() {
    if (validMapped(m_window.lock()))
        g_pDecorationPositioner->repositionDeco(this);
}

void CHudDecoration::damageEntire() {
    const auto window = m_window.lock();
    if (!validMapped(window))
        return;

    g_pHyprRenderer->damageBox(grown(windowBoxGlobal(window), m_extents, 2));
}

void CHudDecoration::damageIfAnimated() {
    const auto window = m_window.lock();
    if (!g_settings.active() || !validMapped(window))
        return;

    const bool animated = window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_borderFadeAnimationProgress->isBeingAnimated() ||
        window->m_glowAngleAnimationProgress->isBeingAnimated() || window->m_glowFadeAnimationProgress->isBeingAnimated();
    if (!animated || !g_pHyprRenderer->shouldRenderWindow(window))
        return;

    // The margin around the window plus the places where the HUD reaches into
    // the window box: the corner squares of a ring style, the pieces of a
    // panel's lines. Nothing else repaints.
    const CBox box = windowBoxGlobal(window);
    CRegion    region(grown(box, m_extents, 2));
    region.subtract(CRegion(box));

    if (m_layout && !m_layout->panel.empty()) {
        const double scale = m_layoutKey.scale;
        const auto   add   = [&](const smplhud::IBox& piece) {
            region.add(CRegion(CBox{box.x + piece.x / scale - 1, box.y + piece.y / scale - 1, piece.w / scale + 2, piece.h / scale + 2}));
        };
        for (const auto& stroke : m_layout->panel.strokes) {
            for (const auto& strip : stroke.strips)
                add(strip.box);
            for (const auto& tile : stroke.tiles)
                add(tile.box);
        }
    } else {
        const double corner = window->rounding() + 2;
        for (const double cx : {box.x, box.x + box.width - corner}) {
            for (const double cy : {box.y, box.y + box.height - corner})
                region.add(CRegion(CBox{cx, cy, corner, corner}));
        }
    }

    g_pHyprRenderer->damageRegion(region);
}

void CHudDecoration::buildSweep(float scale) {
    if (m_sweep.path || !m_layout)
        return;

    // A panel's glow runs along its outline
    const auto& layout = *m_layout;
    auto        path   = std::make_shared<const smplhud::Path>(layout.panel.empty() ? smplhud::buildPath(layout.centre) : smplhud::buildPath(layout.panel.strokes.front().pts));
    if (!path->valid())
        return;

    const auto& theme  = g_settings.theme;
    const auto& style  = *g_settings.style;
    const auto& e      = m_layout->extents;
    const int   left   = int(std::ceil(e.left * scale));
    const int   top    = int(std::ceil(e.top * scale));
    const int   right  = int(std::ceil(e.right * scale));
    const int   bottom = int(std::ceil(e.bottom * scale));

    m_sweep.limit = {-left, -top, m_layoutKey.width + left + right, m_layoutKey.height + top + bottom};
    m_sweep.shape = {theme.sweepLength * scale / 2, m_layout->centre.reachOut, m_layout->centre.reachIn, style.sweepWhiten, style.sweepAlpha};
    m_sweep.texel = smplhud::sweepTexel(scale);
    m_sweep.scale = scale;
    m_sweep.cache.reset();
    m_sweep.path = std::move(path);
}

void CHudDecoration::dropSweep() {
    m_sweep.path.reset();
    m_sweep.cache.reset();
    m_sweepLevel = 0;
}

void CHudDecoration::syncFocus(const PHLWINDOW& focused, bool force) {
    setFocused(focused && m_window.lock() == focused, force);
}

void CHudDecoration::setFocused(bool focused, bool force) {
    const bool changed = focused != m_focused;
    m_focused          = focused;

    if (changed || force) {
        if (const auto window = m_window.lock())
            scopeLoops(window, focused && !fullscreen(window));
    }

    if (!changed)
        return;

    m_waiting = 0;
    wakeHudSweep();
    if (focused)
        damageEntire(); // the draw it causes builds the glow's path
}

CHudDecoration::SSweepStep CHudDecoration::advanceSweep(double seconds) {
    if (!m_focused && m_sweepLevel <= 0)
        return {};

    const auto window = m_window.lock();
    if (!g_settings.sweeping() || !validMapped(window) || window->getRealBorderSize() < 1)
        return {};

    const auto monitor = window->m_monitor.lock();
    if (!monitor || !monitor->m_enabled || !monitor->m_dpmsStatus || !g_pHyprRenderer->shouldRenderWindow(window, monitor)) {
        if (!m_focused)
            dropSweep(); // out of sight: nothing to fade out
        return {};
    }

    if (!m_sweep.path) // the first draw builds it; stay quick for a little while to start on time
        return {.moved = m_focused && m_waiting++ < kSweepWaitSteps};

    m_sweepLevel = smplhud::sweepFade(m_sweepLevel, m_focused, seconds, kSweepFade);

    const auto&  path   = *m_sweep.path;
    const double before = m_sweep.phase * path.length;
    m_sweep.phase       = smplhud::sweepAdvance(m_sweep.phase, seconds, g_settings.theme.sweepSpeed, m_sweep.scale, path.length);
    const double after  = m_sweep.phase * path.length;

    const double dotsBefore = m_dotPos;
    m_dotPos += seconds * g_settings.theme.sweepSpeed * g_settings.style->runnerSpeed * m_sweep.scale;

    // The glow leaves one box and enters another, and so does each dot; nothing else repaints.
    const CBox origin = windowBoxGlobal(window);
    CRegion    region;
    for (const double at : {before, after}) {
        const auto box = smplhud::sweepBounds(path, at, m_sweep.shape, m_sweep.texel, m_sweep.limit);
        if (box.w <= 0 || box.h <= 0)
            continue;

        CBox logical = {origin.x + box.x / m_sweep.scale, origin.y + box.y / m_sweep.scale, box.w / m_sweep.scale, box.h / m_sweep.scale};
        region.add(CRegion(logical.expand(2)));
    }

    if (m_layout) {
        addRunnerBoxes(region, m_layout->panel, dotsBefore, origin, m_sweep.scale);
        addRunnerBoxes(region, m_layout->panel, m_dotPos, origin, m_sweep.scale);
    }

    SSweepStep step;
    step.fading = m_sweepLevel > 0 && m_sweepLevel < 1;

    if (!m_focused && m_sweepLevel <= 0)
        dropSweep(); // faded out; the damage below repaints without it

    if (region.empty())
        return step;

    g_pHyprRenderer->damageRegion(region);
    step.moved = true;
    return step;
}

void CHudDecoration::draw(PHLMONITOR monitor, const float& a) {
    const auto window = m_window.lock();
    if (!g_settings.active() || !monitor || !validMapped(window) || window->getRealBorderSize() < 1)
        return;

    const float scale    = monitor->m_scale;
    const CBox  local    = windowBoxLocal(window, monitor);
    const CBox  physical = local.copy().scale(scale).round();
    if (physical.width < smplhud::kMinWindow || physical.height < smplhud::kMinWindow) {
        dropSweep();
        return;
    }

    const SLayoutKey key = {
        .width         = int(physical.width),
        .height        = int(physical.height),
        .cut           = int(window->rounding() * scale),
        .scale         = scale,
        .roundingPower = window->roundingPower(),
        .floating      = window->m_isFloating,
        .revision      = g_settings.revision,
    };

    if (!m_layout || !(key == m_layoutKey)) {
        const auto& theme = g_settings.theme;

        smplhud::Params params;
        params.width          = key.width;
        params.height         = key.height;
        params.scale          = key.scale;
        params.cut            = key.cut;
        params.roundingPower  = key.roundingPower;
        params.floating       = key.floating;
        params.gap            = theme.gap;
        params.lineWidth      = theme.lineWidth;
        params.chamfer        = theme.chamfer;
        params.floatGlowRange = theme.floatGlowRange;

        m_layout    = std::make_shared<const smplhud::Layout>(smplhud::buildLayout(*g_settings.style, params));
        m_layoutKey = key;
        m_sweep.path.reset(); // the path follows the layout
    }

    if (!m_layout->valid)
        return;

    if (g_settings.sweeping() && (m_focused || m_sweepLevel > 0))
        buildSweep(scale);
    else
        dropSweep();

    SHudFrame frame;
    frame.layout = m_layout;

    auto& line = frame.paints[size_t(Paint::Line)];
    line       = animatedPaint(window->m_realBorderColor, window->m_realBorderColorPrevious, window->m_borderAngleAnimationProgress, window->m_borderFadeAnimationProgress);
    if (line.to.m_colorsOkLabA.empty())
        return;

    auto& glow = frame.paints[size_t(Paint::Glow)];
    glow       = animatedPaint(window->m_realGlowColor, window->m_realGlowColorPrevious, window->m_glowAngleAnimationProgress, window->m_glowFadeAnimationProgress);
    if (!visible(glow.to))
        glow = line;

    frame.paints[size_t(Paint::Bright)] = whitened(line, g_settings.style->brightWhiten);

    // The window's own opacity is not the HUD's; fades (workspace switches,
    // open/close) still are.
    const float windowOpacity = window->m_ruleApplicator->opaque().valueOrDefault() ? 1.F : window->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
    frame.alpha               = std::clamp(a / std::max(windowOpacity, 0.001F), 0.F, 1.F);
    frame.roundingPower       = key.roundingPower;
    frame.window              = physical;
    frame.bounds              = grown(local, m_extents, 2);

    const float intensity = float(smplhud::sweepIntensity(m_sweepLevel));
    if (m_sweep.path && intensity > 0.002F) {
        if (!m_sweep.cache)
            m_sweep.cache = std::make_shared<SHudSweepCache>();
        frame.sweep = SHudSweep{m_sweep.path, m_sweep.shape, m_sweep.limit, m_sweep.texel, m_sweep.phase * m_sweep.path->length, m_sweep.cache, intensity};

        frame.runners = smplhud::runnerPoses(m_layout->panel, m_dotPos);
        for (auto& pose : frame.runners)
            pose.alpha *= intensity;
    }

    g_pHyprRenderer->addPassElement(makeUnique<CHudPassElement>(std::move(frame)));
}
