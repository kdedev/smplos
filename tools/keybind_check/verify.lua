-- Config for `Hyprland --verify-config`: checks one bind file or one dispatch expression.
-- Hyprland loads it like any config, reports every error it raises and exits 1.
--
--   KB_MODE=bind        KB_LOADER KB_CONF KB_POLICY
--       Load KB_CONF through bindings_loader.lua (KB_POLICY=1 activates the workspace policy).
--   KB_MODE=expression  KB_EXPR
--       Build the hl.dsp dispatcher that a `hyprctl dispatch "<expression>"` bind would run.
--   KB_MODE=file        KB_FILE
--       Run a Lua bind module (popup_watchers.lua) as it would load.
local mode = os.getenv("KB_MODE")

if mode == "bind" then
    _G.smplos_workspace_policy_active = os.getenv("KB_POLICY") == "1"
    dofile(os.getenv("KB_LOADER")).load(os.getenv("KB_CONF"))
elseif mode == "file" then
    dofile(os.getenv("KB_FILE"))
elseif mode == "expression" then
    -- Verification really runs what it loads, so only the dispatcher constructors are
    -- reachable: a bind file cannot start programs or touch files while it is checked.
    local env = {
        hl = { dsp = hl.dsp }, math = math, string = string, table = table,
        pairs = pairs, ipairs = ipairs, type = type, tostring = tostring, tonumber = tonumber,
    }
    local chunk, err = load("return " .. os.getenv("KB_EXPR"), "=expression", "t", env)
    if not chunk then error(err, 0) end
    chunk()
else
    error("KB_MODE must be 'bind' or 'expression'", 0)
end
