-- Runs the real bindings_loader.lua and popup_watchers.lua against a recording
-- mock of Hyprland's `hl` API and prints every hl.bind() call as one JSON line.
--
--   lua driver.lua LOADER POLICY POPUP NAME=PATH...
--     POLICY  "1" activates the workspace policy before the loader is read
--     POPUP   popup_watchers.lua path, or "-" to skip it
--
-- stdout carries only the JSON document; anything the scripts print goes to stderr.
local loader_path, policy, popup_path = arg[1], arg[2], arg[3]

print = function(...)
    local parts = {}
    for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
    io.stderr:write(table.concat(parts, "\t"), "\n")
end

local calls, source, submap = {}, "", ""

-- hl.dsp.a.b(x) returns {name = "a.b", args = {x}} whatever the path is.
local function dsp_proxy(path)
    return setmetatable({}, {
        __index = function(_, key)
            return dsp_proxy(path == "" and key or (path .. "." .. key))
        end,
        __call = function(_, ...) return { name = path, args = { ... } } end,
    })
end

local function bind_object(call)
    return {
        set_enabled = function(_, enabled) call.enabled = enabled and true or false end,
    }
end

hl = {
    dsp = dsp_proxy(""),
    bind = function(keys, action, opts)
        local call = {
            source = source, submap = submap, keys = keys, opts = opts or {}, enabled = true,
            action = type(action) == "table" and action
                or { name = "<" .. type(action) .. ">", args = {} },
        }
        calls[#calls + 1] = call
        return bind_object(call)
    end,
    define_submap = function(name, first, second)
        local previous = submap
        submap = name
        ;(type(first) == "function" and first or second)()
        submap = previous
    end,
    get_layers = function() return {} end,
    get_windows = function() return {} end,
    on = function() return true end,
    exec_cmd = function() end,
    dispatch = function() end,
}

local ESCAPES = { ['"'] = '\\"', ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }

local function encode(value)
    local kind = type(value)
    if kind == "nil" then return "null" end
    if kind == "boolean" then return tostring(value) end
    if kind == "number" then
        if math.type(value) == "integer" then return string.format("%d", value) end
        if value ~= value or value == math.huge or value == -math.huge then return "null" end
        return string.format("%.14g", value)
    end
    if kind == "string" then
        return '"' .. value:gsub('[%c"\\]', function(c)
            return ESCAPES[c] or string.format("\\u%04x", c:byte())
        end) .. '"'
    end
    if kind ~= "table" then return encode(tostring(value)) end

    local count = 0
    for _ in pairs(value) do count = count + 1 end
    local parts = {}
    if count == #value then
        for i = 1, count do parts[i] = encode(value[i]) end
        return "[" .. table.concat(parts, ",") .. "]"
    end
    local keys = {}
    for key in pairs(value) do keys[#keys + 1] = key end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for i, key in ipairs(keys) do
        parts[i] = encode(tostring(key)) .. ":" .. encode(value[key])
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

_G.smplos_workspace_policy_active = policy == "1"
local loader = dofile(loader_path)

for i = 4, #arg do
    local name, path = arg[i]:match("^([^=]+)=(.*)$")
    source = name
    loader.load(path)
end

if popup_path and popup_path ~= "-" then
    source, submap = "popup_watchers", ""
    dofile(popup_path)
end

io.stdout:write(encode({ calls = calls }), "\n")
