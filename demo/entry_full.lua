-- Caesura (AmeKAG) - Full-Pipeline Demo Entry
-- Loads demo/full_pipeline_demo.ks and drives it through kag_runner.
-- Switch config.entry_script to "../demo/entry_full.lua" to run it.
local kag_runner = require("kag_runner")
local layers = require("layers")

local function file_exists(path)
    local f = io.open(path, "r")
    if f then f:close(); return true end
    return false
end

local demo_path = nil
for _, p in ipairs({"demo/full_pipeline_demo.ks", "full_pipeline_demo.ks",
                    "../demo/full_pipeline_demo.ks"}) do
    if file_exists(p) then demo_path = p; break end
end

if not demo_path then
    print("[FullPipeline Entry] FATAL: Cannot find full_pipeline_demo.ks")
    return
end

print("[FullPipeline Entry] Loading: " .. demo_path)
local started = kag_runner.start(demo_path)
if not started then
    print("[FullPipeline Entry] FATAL: Failed to start demo")
    return
end

function engine_update(dt)
    kag_runner.update(dt or 0.016)
end

function engine_render()
    layers.render()
    kag_runner.render()
end

function _KAG_onClick()
    kag_runner.on_click()
end

print("[FullPipeline Entry] Full-pipeline demo active.")