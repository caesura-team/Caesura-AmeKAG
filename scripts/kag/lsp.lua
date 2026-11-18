-- =============================================================================
--  Caesura (AmeKAG) — kag/lsp.lua
--  KAG Neo-Genesis language service (Battle 2): completion / hover /
--  diagnostics for the IDE. Pure Lua, driven by the declarative command
--  contracts (schema.dumpContracts is the single source of truth).
--
--  The IDE calls this through the engine's /api/eval endpoint (a Lua
--  expression returning a JSON string); the renderer parses it once.
--  Three entry points:
--    lsp.completion(line_text)      -> [{label, kind, detail, insertText}]
--    lsp.hover(cmd, param?)         -> {title, text}  (contract + desc)
--    lsp.diagnostics(text)          -> [{line, col, message, severity}]
--
--  All functions are pure (no ctx, no I/O) except diagnostics which
--  tokenizes the given source text.
-- =============================================================================

local lsp = {}

local schemaModule = require("kag.schema")
local tokenizer = require("tokenizer")

-- Load every command module so their contracts register (same pattern
-- as ks_check.lua; the contract registry is the completion/hover
-- source). No-op when already loaded.
pcall(require, "kag.commands.text")
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.audio")
pcall(require, "kag.commands.transition")
pcall(require, "kag.commands.layer")
pcall(require, "kag.commands.vfx")
pcall(require, "kag.commands.video")
pcall(require, "kag.commands.save")
pcall(require, "kag.commands.resource")
pcall(require, "kag.commands.character")
pcall(require, "kag.commands.math")

-- Completion item kinds (Monaco LanguageIdentifier kinds; numeric values
-- match monaco.languages.CompletionItemKind).
local KIND = {
    Function = 3,  -- command tag
    Field = 5,     -- parameter
    Keyword = 14,  -- flow control
}

-- Flow-control commands handled by the scheduler (completions too).
local FLOW_CMDS = {
    "if", "elseif", "else", "endif", "while", "endwhile", "for", "endfor",
    "until", "break", "continue", "jump", "call", "return", "link", "label",
    "macro", "endmacro", "erasemacro", "goto", "switch", "case", "endcase",
    "default", "endswitch", "eval", "emb", "iscript", "endscript",
    "select", "sel", "endselect", "end", "stop",
}

-- KAG3 parameter-name aliases (mirror of kag3_import.lua PARAM_ALIASES):
-- KAG3 tag param -> Neo-Genesis engine param, keyed by command. Pure
-- advisory — these rows never change validation itself, only let the
-- editor hint the canonical name when an author writes the KAG3 spelling.
--   add/sub/mul/div/mod/dec  var=  ->  name=
--   csp/csl                   left= ->  x=,  top= ->  y=
local KAG3_PARAM_ALIASES = {
    add = { var = "name" },
    sub = { var = "name" },
    mul = { var = "name" },
    div = { var = "name" },
    mod = { var = "name" },
    dec = { var = "name" },
    csp = { left = "x", top = "y" },
    csl = { left = "x", top = "y" },
}

-- Find the canonical param a KAG3 alias maps to, or nil when the command/
-- param pair has no alias. Only the *unknown* param lookup path consults
-- this, so a legitimately-declared param is never touched.
local function kag3_alias_target(cmd, pname)
    local map = KAG3_PARAM_ALIASES[cmd]
    if not map then return nil end
    local target = map[pname]
    if not target then return nil end
    return target
end

local function parameter_contract(specs, pname)
    if specs[pname] then return specs[pname], pname end
    for name, spec in pairs(specs) do
        for _, alias in ipairs(spec.aliases or {}) do
            if alias == pname then return spec, name end
        end
    end
end

--- Build the command index once (contracts + flow commands + kag table).
local commands = nil
local function ensure_index()
    if commands then return commands end
    commands = {}
    for cmd in pairs(schemaModule.dumpContracts()) do
        commands[cmd] = true
    end
    for _, c in ipairs(FLOW_CMDS) do commands[c] = true end
    -- kag table aliases (registered handlers without contracts)
    local ok, kag = pcall(require, "kag")
    if ok and type(kag) == "table" then
        for k in pairs(kag) do
            if type(k) == "string" and not commands[k] then commands[k] = true end
        end
    end
    return commands
end

--- lsp.completion(lineText, cursorCol) → array of completion items.
--  In-tag completion: after `[` suggests command names; inside a tag with
--  a command present suggests that command's params. `cursorCol` (1-based,
--  byte) is optional; when present it lets the provider also suggest
--  expression-language variable table prefixes (f./sf./tf./mp./lf.) inside
--  an exp= value or a ${...} interpolation span. Default (nil) keeps the
--  original command/param-only behaviour.
function lsp.completion(lineText, cursorCol)
    local items = {}
    -- command-name context: "[ch " -> params; "[c" -> commands
    local openBracket = lineText:find("[", 1, true)
    if not openBracket then return items end
    local afterBracket = lineText:sub(openBracket + 1)
    local cmdName = afterBracket:match("^%s*([%w_]+)")
    -- A command name only counts as "complete" when followed by a space
    -- or the closing bracket: "[ch" is still a name prefix, "[ch " is
    -- param completion.
    local afterName = afterBracket:match("^%s*[%w_]+(.*)$")
    local nameComplete = afterName ~= nil
        and (afterName:sub(1, 1) == " " or afterName:sub(1, 1) == "]")
    if not cmdName or not nameComplete then
        -- prefix of a command name: "[ch" or "[pla"
        local prefix = afterBracket:match("^%s*([%w_]*)")
        for cmd in pairs(ensure_index()) do
            if prefix == "" or cmd:find(prefix, 1, true) == 1 then
                local meta = schemaModule.meta(cmd)
                items[#items + 1] = {
                    label = cmd,
                    kind = KIND.Function,
                    detail = meta and meta.desc or "",
                    insertText = cmd .. " ",
                }
            end
        end
        return items
    end
    -- param context: "[ch " -> params of ch
    local specs = schemaModule.dumpContracts()[cmdName]
    -- KAG3 alias commands share a handler but register no contract of
    -- their own: [sel] IS TextCommands.button (params text/target/cond/
    -- caption/x, round-74 x= result capture) -- complete its params from
    -- the aliased command's contract so the editor offers them.
    local ALIAS_PARAM_CMDS = { sel = "button" }
    if not specs and ALIAS_PARAM_CMDS[cmdName] then
        specs = schemaModule.dumpContracts()[ALIAS_PARAM_CMDS[cmdName]]
    end
    -- Expression flow commands ([if]/[while]) have no contract entry but
    -- still take an exp= value; fall through to the variable-table prefix
    -- suggestions below instead of returning empty.
    local EXPR_CMDS = { ["if"] = true, ["while"] = true, eval = true, emb = true }
    if not specs and not EXPR_CMDS[cmdName] then return items end
    -- prefix after the command name (leading space consumed)
    local prefix = (afterName or ""):match("^%s*([%w_]*)")
    if specs then
        local function add_param(pname, spec, alias_of)
            if prefix ~= "" and pname:find(prefix, 1, true) ~= 1 then return end
            local detail = (spec.type or "string")
                .. (spec.required and " (required)" or "")
                .. (spec.default ~= nil and (" = " .. tostring(spec.default)) or "")
                .. (alias_of and (" (alias of " .. alias_of .. ")") or "")
            items[#items + 1] = {label=pname, kind=KIND.Field, detail=detail, insertText=pname .. "="}
        end
        for pname, spec in pairs(specs) do
            if pname ~= "_meta" then
                add_param(pname, spec)
                for _, alias in ipairs(spec.aliases or {}) do
                    if not specs[alias] then add_param(alias, spec, pname) end
                end
            end
        end
    end
    -- Inside an expression-language context (exp= value or ${...} span) also
    -- suggest the variable table prefixes (f./sf./tf./mp./lf.), filtered by
    -- the current word prefix. Cursor column optional; without it the scan
    -- falls back to the whole line so a CLI caller sees the same hints.
    local upToCursor = lineText
    if type(cursorCol) == "number" then
        upToCursor = lineText:sub(1, math.max(0, cursorCol - 1))
    end
    local inExpr = upToCursor:find("${", 1, true) ~= nil
        or upToCursor:match("%f[%w]exp%s*=") ~= nil
    if inExpr then
        -- filter by the word being typed AT the cursor (not the param-name
        -- prefix): e.g. [if exp="f -> "f", [ch text="hp ${t -> "t".
        local wordPrefix = upToCursor:match("([%w_]*)$") or ""
        for _, tbl in ipairs({ "f", "sf", "tf", "mp", "lf" }) do
            if wordPrefix == "" or tbl:find(wordPrefix, 1, true) == 1 then
                items[#items + 1] = {
                    label = tbl .. ".",
                    kind = KIND.Field,
                    detail = "variable table (expression language)",
                    insertText = tbl .. ".",
                }
            end
        end
    end
    return items
end

--- lsp.hover(cmd, param) → {title, text} describing the command contract
--  or the specific parameter (from _meta.desc + param spec).
function lsp.hover(cmd, param)
    if not cmd or cmd == "" then return nil end
    local contracts = schemaModule.dumpContracts()
    local specs = contracts[cmd]
    local meta = schemaModule.meta(cmd)
    -- Expression commands ([if]/[while]/[eval]/[emb]) and the exp param
    -- surface the expression-language cheat-sheet (below), even when the
    -- command is a scheduler flow command without a contract entry.
    local EXPR_CMDS = { ["if"] = true, ["while"] = true, eval = true, emb = true }
    local isExpr = param == "exp" or EXPR_CMDS[cmd]
    if not specs and not meta then
        if not (isExpr and ensure_index()[cmd]) then return nil end
    end
    local title = "[" .. cmd .. "]"
    local lines = {}
    if meta and meta.desc and meta.desc ~= "" then
        lines[#lines + 1] = meta.desc
    end
    local spec, canonical
    if param and specs then spec, canonical = parameter_contract(specs, param) end
    if spec then
        local parts = { "param `" .. param .. "`" }
        if canonical ~= param then parts[#parts + 1] = "alias of `" .. canonical .. "`" end
        if spec.type then parts[#parts + 1] = "type=" .. tostring(spec.type) end
        if spec.required then parts[#parts + 1] = "required" end
        if spec.default ~= nil then parts[#parts + 1] = "default=" .. tostring(spec.default) end
        if spec.min ~= nil then parts[#parts + 1] = "min=" .. tostring(spec.min) end
        if spec.max ~= nil then parts[#parts + 1] = "max=" .. tostring(spec.max) end
        if spec.choices then parts[#parts + 1] = "choices={" .. table.concat(spec.choices, "|") .. "}" end
        lines[#lines + 1] = table.concat(parts, "  ")
    elseif specs then
        local params = {}
        for pname in pairs(specs) do
            if pname ~= "_meta" then params[#params + 1] = pname end
        end
        table.sort(params)
        lines[#lines + 1] = "params: " .. table.concat(params, ", ")
    end
    if meta and meta.category then
        lines[#lines + 1] = "category: " .. tostring(meta.category)
            .. (meta.blocking and " (blocking)" or "")
    end
    -- Expression-language cheat-sheet: show for the exp param or any
    -- expression command ([if]/[while]/[eval]/[emb]). Static reference so
    -- authors see the operator mapping + variable tables + interpolation
    -- forms without leaving the editor.
    if isExpr then
        lines[#lines + 1] = ""
        lines[#lines + 1] = "expression language:"
        lines[#lines + 1] = "  operators  && || ! != ? :  ->  and or not ~= (t ? a : b)"
        lines[#lines + 1] = "  variables  f. sf. tf. mp. lf.  (scene/char/macro tables)"
        lines[#lines + 1] = "  interp     $tbl.key | %var% | ${expr}"
        lines[#lines + 1] = "  docs       kag-expression-language.md"
    end
    return { title = title, text = table.concat(lines, "\n") }
end

--- lsp.diagnostics(text) → [{line, col, message, severity}] using the
--  tokenizer + contract validation (lightweight ks_check).
--  severity: 1 = error, 2 = warning (Monaco MarkerSeverity).
function lsp.diagnostics(text)
    local issues = {}
    if not text or text == "" then return issues end
    local ok, tokens = pcall(tokenizer.parse_with_offsets, text)
    if not ok or not tokens then return issues end

    -- line index for offset -> line mapping
    local lineStarts = { 1 }
    for i = 1, #text do
        if text:byte(i) == 10 then lineStarts[#lineStarts + 1] = i + 1 end
    end
    local function lineOf(offset)
        local lo, hi = 1, #lineStarts
        while lo < hi do
            local mid = (lo + hi + 1) // 2
            if lineStarts[mid] <= offset then lo = mid else hi = mid - 1 end
        end
        return lo
    end

    local contracts = schemaModule.dumpContracts()
    local function addIssue(line, col, message, severity)
        issues[#issues + 1] = {
            line = line, col = col or 1,
            message = message, severity = severity or 1,
        }
    end

    -- Truncation detection (ks_check parity): the offset stream can stop
    -- mid-scene on malformed input while earlier tokens parsed — flag it
    -- instead of reporting clean. Comments are consumed by the grammar's
    -- skip, so a scene ending in "; done" does not false-positive.
    local consumed = tokens[#tokens] and tokens[#tokens].end_offset or 0
    if consumed > 0 then
        local tail = text:sub(consumed + 1)
        while true do
            local stripped = tail:gsub("^%s*;[^\r\n]*", "")
            if stripped == tail then break end
            tail = stripped
        end
        local first = tail:find("%S")
        if first then
            addIssue(lineOf(consumed + first), 1,
                "parse stream stopped before end of input", 1)
        end
    end

    for _, tok in ipairs(tokens) do
        if tok.type == "command" then
            local cmd = tok.cmd
            local specs = contracts[cmd]
            -- Expression compile pre-check (ks_check parity): exp fields
            -- run through the KAG expression translator at runtime —
            -- validate they COMPILE so typos fail in the editor. [eval]/
            -- [emb] are STATEMENTS (assignments), so they get a plain
            -- chunk check (accept both return-wrapped and bare forms).
            do
                local exp = nil
                for _, pair in ipairs(tok.params or {}) do
                    if type(pair) == "table" and pair[1] == "exp" then
                        exp = pair[2]
                    end
                end
                if type(exp) == "string" and exp ~= "" then
                    local exprLang = require("kag.expr")
                    local fn = nil
                    if cmd == "eval" or cmd == "emb" then
                        fn = load("return " .. exp, "=ks_expr_check", "t", {})
                        if not fn then
                            fn = load(exp, "=ks_expr_check", "t", {})
                        end
                        if not fn and cmd == "eval" then
                            -- round 68: ternary assignments translate via
                            -- translateAssignment (full pipeline on the RHS)
                            fn = load(exprLang.translateAssignment(exp),
                                "=ks_expr_check", "t", {})
                        end
                    else
                        fn = load("return " .. exprLang.translate(exp),
                            "=ks_expr_check", "t", {})
                    end
                    if not fn then
                        addIssue(lineOf(tok.offset), 1,
                            "expression in [" .. cmd .. "] does not compile: " .. exp, 1)
                    end
                end
            end
            -- ${expr} interpolation pre-check (editor): any interpolatable
            -- text param (spec.interpolate, e.g. [ch text=...]) may embed
            -- ${...} spans evaluated through the SAME schema compile path as
            -- runtime (Schema.checkInterp). Flag each failed span so a
            -- bad interpolation fails in the editor instead of silently
            -- rendering the verbatim ${...} at run time. Reported at the
            -- command's offset/col 1, matching the exp= check above.
            if specs then
                local supplied = {}
                for _, pair in ipairs(tok.params or {}) do
                    if type(pair) == "table" and type(pair[1]) == "string" then
                        supplied[pair[1]] = true
                    end
                end
                for _, pair in ipairs(tok.params or {}) do
                    if type(pair) == "table" and type(pair[1]) == "string" then
                        local pname = pair[1]
                        local pval = pair[2]
                        local pspec, canonical = parameter_contract(specs, pname)
                        local selected = canonical
                        -- The runtime takes the canonical spelling first, then the first supplied alias.
                        if pspec and pspec.interpolate and not supplied[canonical] then
                            for _, alias in ipairs(pspec.aliases or {}) do
                                if supplied[alias] then
                                    selected = alias
                                    break
                                end
                            end
                        end
                        if selected == pname and pspec.interpolate
                            and type(pval) == "string"
                            and pval:find("${", 1, true) then
                            local problems = schemaModule.checkInterp(pval)
                            if problems then
                                for _, pr in ipairs(problems) do
                                    addIssue(lineOf(tok.offset), 1,
                                        "[" .. cmd .. "] " .. pname .. " " .. pr.error, pr.severity or 1)
                                end
                            end
                        end
                    end
                end
            end
            if specs then
                local present = {}
                for _, pair in ipairs(tok.params or {}) do
                    if type(pair) == "table" and type(pair[1]) == "string" then
                        local _, canonical = parameter_contract(specs, pair[1])
                        present[canonical or pair[1]] = true
                    end
                end
                -- required params
                for pname, spec in pairs(specs) do
                    if pname ~= "_meta" and spec.required and not present[pname] then
                        addIssue(lineOf(tok.offset), 1,
                            "[" .. cmd .. "] missing required param '" .. pname .. "'", 1)
                    end
                end
                -- any-of requirement (e.g. playbgm needs file OR storage)
                if specs._require_any then
                    local found = false
                    for _, name in ipairs(specs._require_any) do
                        if present[name] then found = true break end
                    end
                    if not found then
                        addIssue(lineOf(tok.offset), 1,
                            "[" .. cmd .. "] requires one of {"
                            .. table.concat(specs._require_any, ",") .. "}", 1)
                    end
                end
                -- unknown-param check (round-73, additive): every NAMED
                -- param must be declared in the command's contract, else a
                -- typo fails in the editor instead of being silently
                -- dropped/ignored at run time. Excluded (never flagged):
                --   - positional slots   (numeric keys, e.g. [set f.hp 30];
                --     the contract's positional_index handles those)
                --   - variable paths     (dotted keys, e.g. f.name spread
                --     into positional slots by the compiler)
                --   - underscore keys    (_require_any/_meta contract
                --     metadata are not params)
                --   - flow commands      (no contract entry -> specs nil,
                --     so they route to the unknown-command branch above)
                -- Warning (severity 2), line = command's own line, col 1,
                -- matching the exp/interp checks. Sorted for determinism.
                local unknown = {}
                for _, pair in ipairs(tok.params or {}) do
                    if type(pair) == "table" and type(pair[1]) == "string" then
                        local pname = pair[1]
                        if not pname:match("^%d+$")
                            and not pname:find(".", 1, true)
                            and pname:sub(1, 1) ~= "_"
                            and not parameter_contract(specs, pname) then
                            unknown[#unknown + 1] = pname
                        end
                    end
                end
                if #unknown > 0 then
                    table.sort(unknown)
                    for _, pname in ipairs(unknown) do
                        -- KAG3 alias hint (pure advisory): when this command+
                        -- param has a KAG3-compat spelling that the importer
                        -- rewrites to the canonical engine param, tell the
                        -- author which spelling to use instead of leaving a
                        -- bare unknown-param warning. Does not change the
                        -- warning itself or the contract check.
                        local hint = ""
                        local target = kag3_alias_target(cmd, pname)
                        if target then
                            hint = "（KAG3 别名：建议用 " .. target .. "= 代替 " .. pname .. "=）"
                        end
                        addIssue(lineOf(tok.offset), 1,
                            "unknown param '" .. pname .. "' for [" .. cmd .. "]" .. hint, 2)
                    end
                end
            else
                -- unknown command (not flow, not alias): warn like ks_check
                local known = ensure_index()[cmd]
                if not known then
                    addIssue(lineOf(tok.offset), 1,
                        "unknown KAG command '" .. cmd .. "' (will render as text)", 2)
                end
            end
        end
    end
    return issues
end

--- Navigation commands whose target param points at a *label.
local NAV_CMDS = { jump = true, call = true, link = true }

-- Rename navigation set: the base NAV_CMDS plus the scheduler/label alias
-- forms that carry a nav target in real scenes -- [goto] (flow-control
-- branch) and the [sel]/[select] button-style commands, whose target may be
-- a named target param (quoted or bare) or a bare *name positional arg.
-- Used by lsp.rename only; definition/references keep the original scope.
local RENAME_NAV_CMDS = {
    jump = true, call = true, link = true,
    ["goto"] = true, sel = true, select = true,
}

--- Extract the *label target of a navigation command token (nil otherwise).
--  navSet (optional) narrows which commands count as nav; defaults to the
--  base NAV_CMDS so existing callers are unchanged.
local function token_target(tok, navSet)
    if tok.type ~= "command" then return nil end
    navSet = navSet or NAV_CMDS
    if not navSet[tok.cmd] then return nil end
    for _, pair in ipairs(tok.params or {}) do
        if type(pair) == "table" then
            local v = pair[2]
            if type(v) == "string" and v:sub(1, 1) == "*" then
                return v:sub(2)
            end
        end
    end
    return nil
end

--- Build the offset -> (line, col) index for a scene text.
local function build_index(text)
    local lineStarts = { 1 }
    for i = 1, #text do
        if text:byte(i) == 10 then lineStarts[#lineStarts + 1] = i + 1 end
    end
    local function lineOf(offset)
        local lo, hi = 1, #lineStarts
        while lo < hi do
            local mid = (lo + hi + 1) // 2
            if lineStarts[mid] <= offset then lo = mid else hi = mid - 1 end
        end
        return lo
    end
    return {
        lineStarts = lineStarts,
        lineOf = lineOf,
        colOf = function(offset) return offset - lineStarts[lineOf(offset)] + 1 end,
    }
end

--- Parse a scene defensively (empty/malformed -> nil).
local function parse_scene(text)
    if not text or text == "" then return nil end
    local ok, tokens = pcall(tokenizer.parse_with_offsets, text)
    if not ok or not tokens then return nil end
    return tokens
end

--- Collect every *label definition in the scene.
local function collect_labels(tokens, idx)
    local labels = {}
    for _, tok in ipairs(tokens) do
        if tok.type == "label" and tok.name then
            labels[tok.name] = {
                name = tok.name,
                line = idx.lineOf(tok.offset),
                col = idx.colOf(tok.offset),
            }
        end
    end
    return labels
end

--- Locate the source byte offset of the NAME (after the leading '*') for
--  a *name occurrence inside a token's source span. The tokenizer only
--  records the command/label token span, not the param-value byte
--  position, so the exact nav-target offset is recovered by a literal scan
--  of the token's slice. Returns nil when not found.
local function locate_label_byte(text, tok, name)
    local slice = text:sub(tok.offset, tok.end_offset)
    local starRel = slice:find("*" .. name, 1, true)
    if not starRel then return nil end
    return tok.offset + starRel
end

--- lsp.definition(text, line, char, navSet?) → {name, line, col} for the
--  *label under the cursor: on a [jump]/[call]/[link] target resolves to
--  the label definition line; on a *label itself returns its own location.
--  Returns {name, line = nil} when the target label is not in THIS scene
--  (labels are scene-scoped; cross-scene jumps land in another file).
--  navSet (optional) narrows which commands count as nav; defaults to
--  NAV_CMDS so the navigation semantics of callers are unchanged.
function lsp.definition(text, line, char, navSet)
    local tokens = parse_scene(text)
    if not tokens or not line then return nil end
    local idx = build_index(text)
    local start = idx.lineStarts[line] or 1
    local pos = start + math.max(0, (char or 1) - 1)

    local hit
    for _, tok in ipairs(tokens) do
        if pos >= tok.offset and pos <= tok.end_offset then
            hit = tok
            break
        end
    end
    if not hit then return nil end

    if hit.type == "label" and hit.name then
        return {
            name = hit.name,
            line = idx.lineOf(hit.offset),
            col = idx.colOf(hit.offset),
        }
    end
    local target = token_target(hit, navSet)
    if not target then return nil end
    local labels = collect_labels(tokens, idx)
    local def = labels[target]
    if def then return def end
    return { name = target, line = nil }
end

--- lsp.references(text, labelName) → [{kind = "definition"|"reference",
--  line, col}] for every jump/call/link targeting labelName plus the
--  label definition itself (Monaco highlights all of them).
function lsp.references(text, labelName)
    local tokens = parse_scene(text)
    if not tokens or not labelName or labelName == "" then return {} end
    local idx = build_index(text)
    local refs = {}
    for _, tok in ipairs(tokens) do
        if tok.type == "label" and tok.name == labelName then
            refs[#refs + 1] = {
                kind = "definition",
                line = idx.lineOf(tok.offset),
                col = idx.colOf(tok.offset),
            }
        elseif tok.type == "command" then
            local target = token_target(tok)
            if target == labelName then
                refs[#refs + 1] = {
                    kind = "reference",
                    line = idx.lineOf(tok.offset),
                    col = idx.colOf(tok.offset),
                }
            end
        end
    end
    return refs
end

--- lsp.rename(text, line, char, newName) → list of edits, each
--  {kind, line, col, length, newText}, covering the *label definition plus
--  every nav reference ([jump]/[call]/[link]/[goto]/[sel]/[select], via a
--  named target="*name" or bare *name). length is the byte length of the
--  old name and newText the replacement; col/line are the position of the
--  name AFTER its leading '*', so an editor applies the edit in place.
--  Returns nil when newName is invalid (rejected), and {} when no label
--  resolves under the cursor (unknown label / not defined in this scene).
function lsp.rename(text, line, char, newName)
    -- Validate the new label name: strip a leading '*' if the IDE sent it,
    -- then require the same charset the tokenizer uses for idents (start
    -- letter/underscore, then letters/digits/underscore, no spaces).
    if type(newName) ~= "string" then return nil end
    newName = newName:gsub("^%*", "")
    if newName == "" or not newName:match("^[A-Za-z_][A-Za-z0-9_]*$") then
        return nil
    end

    local tokens = parse_scene(text)
    if not tokens or not line then return {} end
    local idx = build_index(text)

    -- Resolve the label under the cursor. Unlike lsp.definition (which
    -- keeps the navigation semantics of NAV_CMDS), rename must accept the
    -- SAME nav set that defines its edit-set scope — RENAME_NAV_CMDS — so
    -- a rename triggered on a [goto]/[sel]/[select] reference (which are
    -- not in NAV_CMDS) resolves its origin label and produces edits instead
    -- of silently returning {}. When the target is not defined in THIS
    -- scene (def.line == nil) there is nothing to rename → no edits.
    local def = lsp.definition(text, line, char, RENAME_NAV_CMDS)
    if not def or not def.name or not def.line then return {} end
    local oldName = def.name
    if oldName == newName then return {} end

    local edits = {}
    local function pushEdit(kind, bytePos)
        edits[#edits + 1] = {
            kind = kind,
            line = idx.lineOf(bytePos),
            col = idx.colOf(bytePos),
            length = #oldName,
            newText = newName,
        }
    end

    for _, tok in ipairs(tokens) do
        if tok.type == "label" and tok.name == oldName then
            local b = locate_label_byte(text, tok, oldName)
            if b then pushEdit("definition", b) end
        elseif tok.type == "command" then
            local target = token_target(tok, RENAME_NAV_CMDS)
            if target == oldName then
                local b = locate_label_byte(text, tok, oldName)
                if b then pushEdit("reference", b) end
            end
        end
    end
    return edits
end

--- JSON string escape (shared by the json encoder).
local function json_str(s)
    s = tostring(s)
    return '"' .. s:gsub("\\", "\\\\"):gsub('"', '\\"')
        :gsub("\n", "\\n"):gsub("\r", "\\r") .. '"'
end

--- lsp.json(method, ...) → JSON string for the IDE (via /api/eval).
--  method: "completion" | "hover" | "diagnostics" | "definition" | "references"
function lsp.json(method, ...)
    local result = nil
    if method == "completion" then
        result = lsp.completion(...)
    elseif method == "hover" then
        result = lsp.hover(...)
    elseif method == "diagnostics" then
        result = lsp.diagnostics(...)
    elseif method == "definition" then
        result = lsp.definition(...)
    elseif method == "references" then
        result = lsp.references(...)
    elseif method == "rename" then
        -- lsp.rename returns a list of edits (nil = rejected for a bad
        -- newName, {} = no-op on an unknown/absent label). Wrap it in the
        -- {renamed, edits} response shape the IDE expects.
        local edits = lsp.rename(...)
        if edits == nil then
            result = { renamed = false }
        else
            result = { renamed = true, edits = edits }
        end
    end
    if result == nil then result = {} end
    -- minimal JSON encoder for the result shapes (arrays of flat maps)
    local parts = {}
    if type(result) == "table" and result.title then
        -- hover: {title, text}
        parts[#parts + 1] = "{\"title\":" .. json_str(result.title)
            .. ",\"text\":" .. json_str(result.text) .. "}"
    elseif type(result) == "table" and result.name then
        -- definition: single object {name, line, col}
        local fields = {}
        for k, v in pairs(result) do
            if type(v) == "string" then
                fields[#fields + 1] = json_str(k) .. ":" .. json_str(v)
            elseif type(v) == "number" then
                fields[#fields + 1] = json_str(k) .. ":" .. tostring(v)
            end
        end
        parts[#parts + 1] = "{" .. table.concat(fields, ",") .. "}"
    elseif method == "rename" and type(result) == "table" and result.renamed ~= nil then
        -- rename: {renamed:true, edits:[{line,col,length,newText}]} or
        -- {renamed:false} when the rename was rejected (bad newName).
        if result.renamed and type(result.edits) == "table" then
            local editParts = {}
            for _, e in ipairs(result.edits) do
                local fields = {}
                for k, v in pairs(e) do
                    if type(v) == "string" then
                        fields[#fields + 1] = json_str(k) .. ":" .. json_str(v)
                    elseif type(v) == "number" then
                        fields[#fields + 1] = json_str(k) .. ":" .. tostring(v)
                    end
                end
                editParts[#editParts + 1] = "{" .. table.concat(fields, ",") .. "}"
            end
            parts[#parts + 1] = '{"renamed":true,"edits":['
                .. table.concat(editParts, ",") .. "]}"
        else
            parts[#parts + 1] = '{"renamed":false,"edits":[]}'
        end
    else
        for _, item in ipairs(result) do
            local fields = {}
            for k, v in pairs(item) do
                if type(v) == "string" then
                    fields[#fields + 1] = json_str(k) .. ":" .. json_str(v)
                elseif type(v) == "number" then
                    fields[#fields + 1] = json_str(k) .. ":" .. tostring(v)
                end
            end
            parts[#parts + 1] = "{" .. table.concat(fields, ",") .. "}"
        end
    end
    return "[" .. table.concat(parts, ",") .. "]"
end

return lsp
