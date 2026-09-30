-- Always-on click-outside-to-dismiss watcher for smplOS popup apps
-- (start-menu, smpl-calendar). Registers a single non-consuming bind on
-- the left mouse button so every click triggers popup-click-check, which
-- in turn dismisses any open popup whose window the click missed.
--
-- Why this lives separately from bindings.conf:
--   • It's a system mechanism, not a user-visible keybinding.
--   • Keeping it out of bindings.conf means it doesn't show up in the
--     keybind-help overlay or any future DWM C-struct dump.
--
-- Why this replaces toggle-start-menu / toggle-calendar's old runtime
-- bind/unbind dance:
--   Hyprland 0.55's non-legacy parser rejects `hyprctl keyword bindn` and
--   `hyprctl keyword unbind` at runtime ("keyword can't work with
--   non-legacy parsers. Use eval."), so the runtime bind was silently
--   never registered → click-outside never dismissed → only Esc closed.
--   See: src/shared/bin/popup-click-check, toggle-start-menu, toggle-calendar.
--
-- Key format note:
--   hl.bind() expects just the key (no leading hyprlang "MODS," prefix);
--   for a non-modded bind we pass "mouse:272" — passing ", mouse:272"
--   trips "Unknown keysym" in the Lua parser.

smplos_workspace_overview_escape_ready = false

local popup_click_bind = hl.bind("mouse:272", hl.dsp.exec_cmd("popup-click-check"), {
    non_consuming = true,
    description   = "_popup_click_check",  -- _-prefix marks as internal
})

local function escape_unavailable(reason)
    print("[smplOS] Workspace overview Escape unavailable: " .. reason
        .. "; keeping normal on-demand popup focus.")
end

-- Probe the harmless existing bind before creating any consuming Escape bind.
local checked, compatible = pcall(function()
    if type(hl.get_layers) ~= "function" or type(hl.on) ~= "function"
        or type(hl.exec_cmd) ~= "function" or not popup_click_bind
        or type(popup_click_bind.set_enabled) ~= "function" then
        return false
    end
    popup_click_bind:set_enabled(true)
    return type(hl.get_layers()) == "table"
end)
if not checked or not compatible then
    escape_unavailable("requires working hl.get_layers and Keybind:set_enabled APIs")
    return
end

-- EWW 0.6 has no key-event attribute. Scope a consuming Escape bind to the
-- overview's mapped lifetime instead of entering a sticky submap.
local overview_namespace = "eww-workspace-overview"
local overview_escape

local function overview_open(excluding_address)
    for _, layer in ipairs(hl.get_layers()) do
        if layer.namespace == overview_namespace and layer.mapped
            and layer.address ~= excluding_address then
            return true
        end
    end
    return false
end

local function sync_overview_escape(excluding_address)
    if overview_escape then
        overview_escape:set_enabled(overview_open(excluding_address))
    end
end

local registered, reason = pcall(function()
    assert(hl.on("layer.opened", function(layer)
        if layer.namespace == overview_namespace then
            sync_overview_escape()
        end
    end), "layer.opened is unsupported")

    assert(hl.on("layer.closed", function(layer)
        if layer.namespace == overview_namespace then
            -- Hyprland emits closed before clearing mapped, including on crashes.
            sync_overview_escape(layer.address)
        end
    end), "layer.closed is unsupported")

    assert(hl.on("config.reloaded", function()
        sync_overview_escape()
    end), "config.reloaded is unsupported")
end)
if not registered then
    escape_unavailable(tostring(reason))
    return
end

overview_escape = hl.bind("Escape", function()
    if overview_open() then
        hl.exec_cmd('eww --config "$HOME/.config/eww" close workspace-overview')
    end
end, {
    description = "_workspace_overview_escape",
    dont_inhibit = true,
})
overview_escape:set_enabled(false)
sync_overview_escape()
smplos_workspace_overview_escape_ready = true
