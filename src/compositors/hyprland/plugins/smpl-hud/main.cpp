#define WLR_USE_UNSTABLE

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "HudDecoration.hpp"
#include "HudPassElement.hpp"
#include "ScannerDecoration.hpp"
#include "globals.hpp"
#include "theme.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

namespace {
    std::vector<CHyprSignalListener> g_listeners;
    SP<SHyprCtlCommand>              g_command;

    // The frame, and the scanner that goes with it, are one decoration each.
    void decorate(const PHLWINDOW& window) {
        if (!window || !window->m_isMapped)
            return;

        const auto has = [&](const char* name) {
            return std::ranges::any_of(window->m_windowDecorations, [name](const auto& deco) { return deco->getDisplayName() == name; });
        };

        if (!has(kHudDisplayName))
            HyprlandAPI::addWindowDecoration(PHANDLE, window, makeUnique<CHudDecoration>(window));
        if (!has(kScannerDisplayName))
            HyprlandAPI::addWindowDecoration(PHANDLE, window, makeUnique<CHudScanner>(window));
    }

    void reloadTheme() {
        const auto theme = smplhud::loadTheme();
        if (!(theme == hudSettings().theme))
            setHudTheme(theme);
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[smpl-hud] Version mismatch: this plugin was built for a different Hyprland and was not loaded", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[smpl-hud] Version mismatch");
    }

    setHudTheme(smplhud::loadTheme());

    auto& events = Event::bus()->m_events;
    g_listeners.emplace_back(events.window.open.listen([](PHLWINDOW window) {
        decorate(window);
        placeHudWindow(window); // the box it was given can put the frame off the screen
        syncHudFocus(true);     // a window that maps again starts its loops afresh
    }));
    g_listeners.emplace_back(events.window.floating.listen([](PHLWINDOW window) { placeHudWindow(window); }));
    g_listeners.emplace_back(events.window.moveToWorkspace.listen([](PHLWINDOW window, PHLWORKSPACE) { placeHudWindowLater(window); }));
    g_listeners.emplace_back(events.window.active.listen([](PHLWINDOW, Desktop::eFocusReason) { syncHudFocus(); }));
    g_listeners.emplace_back(events.window.fullscreen.listen([](PHLWINDOW) { syncHudFocus(true); })); // a fullscreen window stops turning, and starts again
    g_listeners.emplace_back(events.config.reloaded.listen([] {
        reloadTheme();
        syncHudFocus(true);
    }));
    g_listeners.emplace_back(events.tick.listen([] {
        syncHudFocus(); // focus can be dropped without an event
        damageAnimatedHuds();
    }));

    for (const auto& window : Desktop::windowState()->windows())
        decorate(window);
    syncHudFocus(true);

    SHyprCtlCommand command;
    command.name  = "smplhud";
    command.exact = false; // the verb and its argument follow the name
    command.fn    = [](eHyprCtlOutputFormat, std::string request) { return runHudCommand(request); };
    g_command     = HyprlandAPI::registerHyprCtlCommand(PHANDLE, command);

    return {"smpl-hud", "HUD frames around windows, drawn in the compositor from the active theme", "smplOS", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_command)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, std::exchange(g_command, nullptr));
    g_listeners.clear();
    stopHudSweep();
    stopHudScanners();
    stopHudPlacement();
    restoreHudLoops();
    damageAllHuds();
    g_pHyprRenderer->m_renderPass.removeAllOfType(CHudPassElement::kPassName);
    g_pHyprRenderer->m_renderPass.removeAllOfType(CScannerPassElement::kPassName);
    clearHudStamps();
}
