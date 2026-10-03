-- schema_doc.lua — generate API docs from command contracts (KAG Neo-Genesis)
-- Usage: lua scripts/schema_doc.lua > docs/api/command-contracts.md
-- The schema registry is the single source of truth: types, defaults,
-- ranges, choices and required flags are documented from the contracts
-- themselves, so the docs can never drift from the implementation.

local BS = string.char(92)  -- backslash
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
package.path = here .. "?.lua;" .. package.path

local schema = require("kag.schema")

-- load every command module so all contracts register
require("kag.commands.text")
require("kag.commands.system")
require("kag.commands.audio")
require("kag.commands.transition")
require("kag.commands.layer")
require("kag.commands.vfx")
require("kag.commands.video")
require("kag.commands.save")
require("kag.commands.resource")
require("kag.commands.character")
require("kag.commands.math")
require("kag")
require("kag.quickmenu")

local contracts = schema.dumpContracts()
local cmds = {}
for cmd in pairs(contracts) do
    if cmd:sub(1, 1) ~= "_" then cmds[#cmds + 1] = cmd end
end
table.sort(cmds)

local out = {}
out[#out + 1] = "# KAG Neo-Genesis Command Contracts (auto-generated)"
out[#out + 1] = ""
out[#out + 1] = "> Generated from the declarative schema registry (`kag/schema.lua`) — do not edit."
out[#out + 1] = "> Regenerate: `lua scripts/schema_doc.lua > docs/api/command-contracts.md`"
out[#out + 1] = ""
out[#out + 1] = "> Explicit canonical parameters take precedence over aliases; aliases resolve before defaults and use the same type/range validation."
out[#out + 1] = ""
out[#out + 1] = "## Commands (" .. #cmds .. ")"
out[#out + 1] = ""
for _, cmd in ipairs(cmds) do
    local specs = contracts[cmd]
    out[#out + 1] = "### `[" .. cmd .. "]`"
    out[#out + 1] = ""
    if specs._meta then
        local m = specs._meta
        out[#out + 1] = string.format(
            "_Category: %s · Blocking: %s · %s_",
            m.category or "-",
            m.blocking and "yes (waits for completion)" or "no (fire-and-forget)",
            m.desc or "")
        out[#out + 1] = ""
    end
    out[#out + 1] = "| Param | Type | Default | Range / Choices | Required | Aliases |"
    out[#out + 1] = "|---|---|---|---|---|---|"
    if specs._require_any then
        out[#out + 1] = string.format(
            "| **requires one of** | — | — | %s | yes | — |",
            table.concat(specs._require_any, ", "))
    end
    local params = {}
    for name in pairs(specs) do
        if name:sub(1, 1) ~= "_" then params[#params + 1] = name end
    end
    table.sort(params)
    for _, name in ipairs(params) do
        local spec = specs[name]
        local range = ""
        if spec.min or spec.max then
            range = (spec.min and tostring(spec.min) or "-") .. ".." ..
                    (spec.max and tostring(spec.max) or "-")
        end
        if spec.choices then
            local cs = {}
            for c in pairs(spec.choices) do cs[#cs + 1] = c end
            table.sort(cs)
            range = table.concat(cs, ",")
        elseif spec.values then
            range = table.concat(spec.values, ", ")
        end
        out[#out + 1] = string.format("| `%s` | %s | %s | %s | %s | %s |",
            name, spec.type or "string",
            spec.default ~= nil and tostring(spec.default) or "-",
            range ~= "" and range or "-",
            spec.required and "yes" or "-",
            type(spec.aliases)=="table" and #spec.aliases>0 and table.concat(spec.aliases, ", ") or "-")
    end
    out[#out + 1] = ""
end

print(table.concat(out, string.char(10)))
