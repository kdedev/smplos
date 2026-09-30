-- Workspace homes. The marker preserves the user's enabled/disabled choice.
_G.smplos_workspace_policy_loaded = true
_G.smplos_workspace_policy_active = false
local home = os.getenv("HOME") or ""
local marker = io.open(home .. "/.config/smplos/workspace-policy.enabled", "r")
if marker then
    marker:close()
    local ok, err = pcall(dofile, home .. "/.config/smplos/workspace-rules.lua")
    if ok then
        _G.smplos_workspace_policy_active = true
    else
        print("smplOS workspace policy: " .. tostring(err))
    end
end
