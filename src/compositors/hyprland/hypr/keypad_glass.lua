-- Hyprland has one shared blur kernel: borrow it only while the sheet is mapped.
local namespace = "eww-pad-cheatsheet"
local saved, applied
local subscriptions = {}

if type(hl.get_config) ~= "function" or type(hl.config) ~= "function"
    or type(hl.get_layers) ~= "function" or type(hl.on) ~= "function" then
    print("[smplOS] Keypad glass blur unavailable: requires Lua config and layer lifecycle APIs.")
    return
end

local function current_blur()
    local blur = {}
    for _, key in ipairs({"enabled", "size", "passes"}) do
        local value, reason = hl.get_config("decoration.blur." .. key)
        assert(value ~= nil, reason)
        blur[key] = value
    end
    return blur
end

local function sheet_open(excluding_address)
    for _, layer in ipairs(hl.get_layers()) do
        if layer.namespace == namespace and layer.mapped and layer.address ~= excluding_address then
            return true
        end
    end
    return false
end

local function sync(excluding_address)
    if sheet_open(excluding_address) then
        if not saved then
            saved = current_blur()
            applied = {enabled = true, size = math.max(saved.size, 8), passes = math.max(saved.passes, 2)}
            hl.config({decoration = {blur = applied}})
        end
    elseif saved then
        local current, restore = current_blur(), {}
        for key, value in pairs(saved) do
            -- Respect a user's live changes made while the overlay was open.
            if current[key] == applied[key] then
                restore[key] = value
            end
        end
        hl.config({decoration = {blur = restore}})
        saved, applied = nil, nil
    end
end

subscriptions.opened = hl.on("layer.opened", function(layer)
    if layer.namespace == namespace then
        sync()
    end
end)
subscriptions.closed = hl.on("layer.closed", function(layer)
    if layer.namespace == namespace then
        -- Closed fires before mapped is cleared, including on a client crash.
        sync(layer.address)
    end
end)
subscriptions.reloaded = hl.on("config.reloaded", function()
    -- Reload rebuilt the theme config; never restore a previous theme's values.
    saved, applied = nil, nil
    sync()
end)

return subscriptions
