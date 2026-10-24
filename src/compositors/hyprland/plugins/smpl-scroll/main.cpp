#define WLR_USE_UNSTABLE

#include <dlfcn.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>

#include "globals.hpp"
#include "scroll_math.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

namespace {
    using namespace Layout;
    using namespace Layout::Tiled;

    // Layout::Tiled::CScrollingAlgorithm::resizeTarget(Vector2D const&, SP<ITarget>, eRectCorner) and
    // CScrollTapeController::fitStrip(unsigned long, CBox const&, bool). The signature is part of the
    // name, so a Hyprland that changes either has no such symbol and the plugin does not load.
    constexpr const char* kResizeTargetSymbol = "_ZN6Layout5Tiled19CScrollingAlgorithm12resizeTargetERKN9Hyprutils4Math8Vector2DENS2_6Memory14CSharedPointerINS_7ITargetEEENS_11eRectCornerE";
    constexpr const char* kFitStripSymbol     = "_ZN6Layout5Tiled21CScrollTapeController8fitStripEmRKN9Hyprutils4Math4CBoxEb";

    // The Hyprland series whose resizeTarget and fitStrip this plugin was written against. Bump it
    // only after diffing the new release's functions against resizeColumn() and hkFitStrip() below.
    constexpr const char* kVerifiedHyprland = "0.56";

    using ResizeTargetFn = void (*)(CScrollingAlgorithm*, const Vector2D&, SP<ITarget>, eRectCorner);
    using FitStripFn     = void (*)(CScrollTapeController*, size_t, const CBox&, bool);

    CFunctionHook* g_resizeHook = nullptr;
    CFunctionHook* g_fitHook    = nullptr;

    // Hyprland's own handling for a window in a column, minus the width limits that stop a
    // column at the viewport. Returns false to leave the call to Hyprland.
    bool resizeColumn(CScrollingAlgorithm* algo, const Vector2D& delta, const SP<ITarget>& target, eRectCorner corner) {
        if (!target || !Desktop::View::validMapped(target->window()))
            return false;

        const auto DATA = algo->dataFor(target);
        if (!DATA)
            return false;

        const auto COLUMN = DATA->column.lock();
        const auto SCROLL = COLUMN ? COLUMN->scrollingData.lock() : nullptr;
        if (!SCROLL || !SCROLL->controller || !SCROLL->controller->isPrimaryHorizontal())
            return false;

        const auto USABLE = algo->usableArea();
        if (SCROLL->idx(COLUMN) < 0 || USABLE.w <= 0.0 || USABLE.h <= 0.0)
            return false;

        // Hyprland re-applies the offset first, which holds the view still while scrolling is inhibited.
        auto& camera = *SCROLL->controller;
        camera.setOffset(camera.getOffset());

        const float OLD_WIDTH = COLUMN->getColumnWidth();
        const auto  WIDTH     = smplscroll::resizeColumnWidth(OLD_WIDTH, delta.x, USABLE.w, edgeLeft(corner));
        if (WIDTH.width != OLD_WIDTH) {
            COLUMN->setColumnWidth(WIDTH.width);
            camera.adjustOffset(WIDTH.cameraShift);
        }

        if (COLUMN->targetDatas.size() > 1) {
            const auto NEXT = COLUMN->next(DATA);
            const auto PREV = COLUMN->prev(DATA);
            double     modDeltaY = delta.y;

            // A keyboard resize has no corner: Hyprland grows the lowest row downward, any other upward.
            if (corner == CORNER_NONE) {
                if (!PREV)
                    corner = CORNER_BOTTOMRIGHT;
                else {
                    corner = CORNER_TOPRIGHT;
                    modDeltaY *= -1.0;
                }
            }

            if (edgeBottom(corner) && NEXT) {
                const auto ROWS = smplscroll::resizeRowsBottom(COLUMN->getTargetSize(NEXT), COLUMN->getTargetSize(DATA), modDeltaY, USABLE.h);
                if (ROWS.changed) {
                    COLUMN->setTargetSize(NEXT, ROWS.neighbour);
                    COLUMN->setTargetSize(DATA, ROWS.current);
                }
            } else if (edgeTop(corner) && PREV) {
                const auto ROWS = smplscroll::resizeRowsTop(COLUMN->getTargetSize(PREV), COLUMN->getTargetSize(DATA), modDeltaY, delta.y, USABLE.h);
                if (ROWS.changed) {
                    COLUMN->setTargetSize(PREV, ROWS.neighbour);
                    COLUMN->setTargetSize(DATA, ROWS.current);
                }
            }
        }

        SCROLL->recalculate(true);
        return true;
    }

    void hkResizeTarget(CScrollingAlgorithm* self, const Vector2D& delta, SP<ITarget> target, eRectCorner corner) {
        if (!resizeColumn(self, delta, target, corner))
            reinterpret_cast<ResizeTargetFn>(g_resizeHook->m_original)(self, delta, target, corner);
    }

    // Hyprland centres a column wider than the viewport whenever it is fitted into view (on focus,
    // click, "move +col"). Here the view stays where it is while it is inside the column, so the
    // column can be scrolled and clicked without jumping.
    void hkFitStrip(CScrollTapeController* self, size_t index, const CBox& usable, bool fullscreenOnOne) {
        const double VIEWPORT = self->isPrimaryHorizontal() ? usable.w : usable.h;
        const auto   OFFSET   = smplscroll::fitWideColumnOffset(self->getOffset(), self->calculateStripStart(index, usable, fullscreenOnOne),
                                                                self->calculateStripSize(index, usable, fullscreenOnOne), VIEWPORT);

        if (OFFSET)
            self->setOffset(*OFFSET);
        else
            reinterpret_cast<FitStripFn>(g_fitHook->m_original)(self, index, usable, fullscreenOnOne);
    }

    [[noreturn]] void refuse(const std::string& why) {
        const auto text = "[smpl-scroll] " + why;
        HyprlandAPI::addNotification(PHANDLE, text, CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error(text);
    }

    void unhook() {
        for (CFunctionHook** hook : {&g_resizeHook, &g_fitHook}) {
            if (!*hook)
                continue;

            HyprlandAPI::removeFunctionHook(PHANDLE, *hook);
            *hook = nullptr;
        }
    }

    // Hooks one Hyprland function, or drops every hook of this plugin and refuses to load: the two
    // hooks only make sense together.
    CFunctionHook* hookFunction(const char* symbol, void* replacement, const std::string& name) {
        void* const SOURCE = dlsym(RTLD_DEFAULT, symbol);
        if (!SOURCE) {
            unhook();
            refuse("this Hyprland has no " + name + " with the expected signature; columns keep Hyprland's width limit");
        }

        CFunctionHook* const HOOK = HyprlandAPI::createFunctionHook(PHANDLE, SOURCE, replacement);
        if (!HOOK) {
            unhook();
            refuse("could not create the hook for " + name + "; columns keep Hyprland's width limit");
        }

        if (!HOOK->hook()) {
            HyprlandAPI::removeFunctionHook(PHANDLE, HOOK);
            unhook();
            refuse("could not hook " + name + "; columns keep Hyprland's width limit");
        }

        return HOOK;
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        refuse("built for a different Hyprland and was not loaded");

    const auto VERSION = HyprlandAPI::getHyprlandVersion(PHANDLE);
    if (!smplscroll::isVerifiedHyprland(VERSION.tag, kVerifiedHyprland))
        refuse(std::string("only verified with Hyprland ") + kVerifiedHyprland + " (running " + (VERSION.tag.empty() ? "an untagged build" : VERSION.tag) +
               "); columns keep Hyprland's width limit");

    g_resizeHook = hookFunction(kResizeTargetSymbol, reinterpret_cast<void*>(&hkResizeTarget), "CScrollingAlgorithm::resizeTarget");
    g_fitHook    = hookFunction(kFitStripSymbol, reinterpret_cast<void*>(&hkFitStrip), "CScrollTapeController::fitStrip");

    return {"smpl-scroll", "Scrolling-layout columns may grow wider than the monitor", "smplOS", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    unhook();
}
