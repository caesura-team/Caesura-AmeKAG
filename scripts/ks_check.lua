-- ks_check.lua — static .ks contract checker (KAG Neo-Genesis tool)
-- Usage: lua scripts/ks_check.lua <scene.ks> [more.ks ...]
-- Tokenizes each scene, runs every migrated command's contract over its
-- params, and reports contract violations with scene:line locations.
-- Exit code 0 = clean, 1 = violations found (CI gate).
--
-- Static-validation counterpart of the runtime schema: developers catch
-- bad params before the game ever runs them.

-- Resolve scripts/ from this file's location (works from any CWD).
local BS = string.char(92)  -- backslash
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
package.path = here .. "?.lua;" .. package.path

local tokenizer = require("tokenizer")
local schema = require("kag.schema")

-- register every command module so all contracts load
pcall(require, "kag.commands.text")
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.audio")
pcall(require, "kag.commands.transition")
pcall(require, "kag.commands.layer")
pcall(require, "kag.commands.vfx")
pcall(require, "kag.commands.video")
pcall(require, "kag.commands.save")
pcall(require, "kag.commands.tween")
pcall(require, "kag")

-- The kag command table (handlers) -- used for the unknown-command audit.
local kag_cmd_table = package.loaded["kag"]

-- Flow commands handled by the scheduler itself (no kag handler, no schema).
-- Must mirror scheduler.lua's flow_commands table: [end]/[stop] terminate
-- the coroutine, [label] is a jump target, etc. (audit: [end]/[stop] were
-- missing and ks_check flagged valid scenes that end with [stop]).
local KNOWN_NONHANDLER = {}
for _, c in ipairs({
    "if", "elseif", "else", "endif", "while", "endwhile", "for", "endfor",
    "until", "break", "continue", "jump", "goto", "call", "return", "macro", "endmacro",
    "switch", "endswitch", "case", "endcase", "default", "label", "eval",
    "emb", "iscript", "wait", "delay", "ch", "text", "link", "end", "stop",
}) do
    KNOWN_NONHANDLER[c] = true
end

local issues = 0
-- Structural warnings are informational: they print with a [WARN]
-- marker but do NOT increment `issues`, so valid scenes keep exit code 0
-- (warnings are a lint layer, not a CI gate -- see structuralWarnings).
local warnings = 0

local function report(scene, line, msg)
    issues = issues + 1
    print(string.format("%s:%d: %s", scene, line, msg))
end

local function warn_scene(scene, line, msg)
    warnings = warnings + 1
    print(string.format("[WARN] %s:%d: %s", scene, line, msg))
end

-- Cross-scene [call]/[jump]/[link] resolution: the scheduler loads a
-- non-* target as assets/script/<target>.ks, so any scene a scene can reach
-- lives as a *.ks sibling in the same directory. Build a scene-basename
-- registry by listing that directory (lfs > popen dir/ls), and resolve
-- targets against it. Listing is environment-dependent, so this degrades to
-- an empty list on failure -- callers gate on "saw the current scene" to
-- stay false-positive-free (see scene_discovery).
local function list_ks_files(dir)
    local files = {}
    local lfsok, lfs = pcall(require, "lfs")
    if lfsok and lfs and lfs.dir then
        local ok, iter = pcall(lfs.dir, dir)
        if ok and iter then
            for fname in iter do
                if fname:find("%.ks$") then files[#files + 1] = fname end
            end
            if #files > 0 then return files end
        end
    end
    local isWin = package.config:sub(1, 1) == "\\"
    local cmd = isWin and ('dir /b "' .. dir .. '" 2>nul')
        or ('ls "' .. dir .. '" 2>/dev/null')
    local ok, result = pcall(function()
        local f = io.popen(cmd)
        if not f then return nil end
        local out = f:read("*a")
        f:close()
        return out
    end)
    if ok and result then
        for fname in result:gmatch("[^\r\n]+") do
            local trimmed = fname:match("^%s*(.-)%s*$")
            if trimmed and trimmed:find("%.ks$") then
                files[#files + 1] = trimmed
            end
        end
        return files
    end
    return files
end

-- Returns (registry, ready). ready is true only when the scan provably
-- listed the current scene itself (proving the directory was read); if it
-- cannot see the current scene (unreadable dir / no listing tool), ready is
-- false so the cross-scene check stays silent -- conservative, never a false
-- positive when resolution is genuinely unknowable.
local function scene_discovery(path)
    local base = path and path:match("([^/\\]+)$")
    if base == nil then return {}, false end
    local dir = (path:match("^(.*)[/\\][^/\\]*$") or "."):gsub("\\", "/")
    local files = list_ks_files(dir)
    local reg = {}
    local saw_self = false
    for _, f in ipairs(files) do
        reg[f] = true
        if f == base then saw_self = true end
    end
    return reg, saw_self
end
local function strip_tail(text, consumed)
    local tail = text:sub(consumed + 1)
    while true do
        local stripped = tail:gsub("^%s*;[^\r\n]*", "")
        if stripped == tail then break end
        tail = stripped
    end
    return tail
end
-- Forward declaration: checkScene calls structuralWarnings, defined below
-- (informational lint). Lua resolves the binding at compile time, so the
-- local must be declared before its first use.
local structuralWarnings

local function rawParams(tok)
    local params = {}
    for _, pair in ipairs(tok.params or {}) do
        if type(pair) == "table" and pair[1] then
            local key = tonumber(pair[1]) or pair[1]
            if type(key) == "string" and key:find("%.") then
                local n = 0
                for index = 1, 20 do if params[index] then n = index end end
                params[n + 1], params[n + 2] = key, pair[2]
            else
                params[key] = pair[2]
            end
        end
    end
    return params
end

local function checkScene(path, capabilitySession)
    local f = io.open(path, "r")
    if not f then
        report(path, 0, "cannot open file")
        return
    end
    local text = f:read("*a")
    f:close()
    -- BOM alignment: tokenizer.parse_with_offsets strips a leading UTF-8
    -- BOM before computing byte offsets; the tail-consumption check below
    -- slices the raw text with those offsets, so strip the BOM here too
    -- (a 3-byte mismatch left a trailing "]" and false-positived
    -- "parse stream stopped before end of input" on BOM'd scenes).
    if text:sub(1, 3) == "\239\187\191" then
        text = text:sub(4)
    end
    -- Neo-Genesis: tokenizer.parse_with_offsets yields exact byte offsets
    -- (pure-Lua LPeg Cp capture), so line numbers are source-accurate --
    -- no find-hack, no sequential scanning.
    local tokens = tokenizer.parse_with_offsets(text)
    if not tokens then
        report(path, 0, "tokenize failed")
        return
    end
    local LF = string.char(10)
    -- Line index: one scan builds line-start offsets; lineOf becomes a
    -- binary search (O(log n)) instead of O(n) per call -- editors run
    -- ks_check on every keystroke, so quadratic behavior mattered.
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
    -- The offset stream can stop mid-scene on malformed input while
    -- earlier tokens parsed -- fail loudly instead of reporting OK for
    -- a truncated scene. end_offset is the last consumed byte.
    -- Trailing COMMENT lines are consumed by the grammar's skip but
    -- never become tokens -- strip them from the tail so a scene
    -- ending in "; done" does not false-positive (review should-fix).
    local consumed = tokens[#tokens] and tokens[#tokens].end_offset or 0
    if consumed > 0 then
        local tail = strip_tail(text, consumed)
        local first = tail:find("%S")
        if first then
            report(path, lineOf(consumed + first), "parse stream stopped before end of input")
            return
        end
    end
    -- Local macro definitions: a [macro name=...] defines a tag that the
    -- scheduler expands at runtime — its invocations are NOT unknown
    -- commands. Scan once before the audit loop (audit: [shout] calls
    -- after [macro shout ...] were falsely flagged).
    local local_macros = {}
    for _, mtok in ipairs(tokens) do
        if mtok.type == "command" and mtok.cmd == "macro" then
            local mname = nil
            for _, pair in ipairs(mtok.params or {}) do
                if type(pair) == "table" and pair[1] == "name" then
                    mname = pair[2]
                end
            end
            if mname == nil and mtok.params and mtok.params[1] then
                mname = mtok.params[1][2]  -- bare [macro shout]
            end
            if type(mname) == "string" and mname ~= "" then
                local_macros[mname] = true
            end
        end
    end
    if capabilitySession then
        for _, tok in ipairs(tokens) do
            local command = tok.type == "iscript" and "iscript" or tok.cmd
            if tok.type == "command" or tok.type == "iscript" then
                local features, unproven
                if local_macros[command] and not KNOWN_NONHANDLER[command] then
                    features, unproven = {}, {"macro_expansion"}
                else
                    features, unproven = capabilitySession.module.command_features(command, rawParams(tok), true)
                end
                for _, feature in ipairs(features) do
                    capabilitySession.calls[#capabilitySession.calls + 1] = {
                        feature=feature, scene=path, line=lineOf(tok.offset or 1), command=command,
                    }
                end
                for _, reason in ipairs(unproven) do
                    capabilitySession.unproven[#capabilitySession.unproven + 1] = {
                        scene=path, line=lineOf(tok.offset or 1), command=command, reason=reason,
                    }
                end
            end
        end
        if capabilitySession.capabilities_only then return end
    end
    for _, tok in ipairs(tokens) do
        if tok.type == "command" then
            local cmd = tok.cmd
            -- Unknown-command audit: a tag that is neither a migrated
            -- contract nor a known KAG handler will be rendered as text at
            -- runtime (with a [WARN]); flag it statically so typos (e.g.
            -- [elsif] pre-alias, [wait] vs [wiat]) fail the check.
            if type(cmd) == "string" then
                local knownHandler = kag_cmd_table and kag_cmd_table[cmd]
                if not knownHandler and not schema.isMigrated(cmd)
                    and not KNOWN_NONHANDLER[cmd]
                    and not local_macros[cmd] then
                    report(path, lineOf(tok.offset or 1),
                        "unknown KAG command '" .. cmd
                            .. "' (will render as text at runtime)")
                end
            end
            -- Expression pre-check (covers BOTH migrated commands and flow
            -- commands like [if]/[while]): exp fields run through the KAG
            -- expression translator at runtime -- validate they COMPILE so
            -- typos fail statically. [eval] is a STATEMENT (assignments
            -- like ctx.tf.x = 1), so it gets a plain chunk check.
            if type(cmd) == "string" then
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
                        -- STATEMENT semantic (assignments); the runtime
                        -- wraps plain expressions in `return`, so accept
                        -- either form here (no false positives on
                        -- `[eval exp="ctx.f.score + 50"]`).
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
                        report(path, lineOf(tok.offset or 1),
                            "expression in [" .. cmd .. "] does not compile: "
                                .. exp)
                    end
                end
            end
            if type(cmd) == "string" and schema.isMigrated(cmd) then
                local params = rawParams(tok)
                local line = lineOf(tok.offset or 1)
                local ok, err2 = pcall(function()
                    -- Check types and interpolation syntax without evaluating
                    -- author expressions against a fabricated empty context.
                    schema.validate_static(cmd, params, { current_scene = path, token_index = line })
                end)
                if not ok then
                    report(path, line, tostring(err2))
                end
            end
        end
    end
    -- Structural warnings (informational lint): run after contract checks.
    -- They print with a [WARN] marker and never increment `issues`, so valid
    -- scenes keep exit code 0 regardless of how many lint hints surface.
    structuralWarnings(path, tokens, lineOf)
end

-- ── Structural warnings (informational lint, NOT a CI gate) ──────────
local NAV_CMDS = { jump = true, call = true, link = true }
-- Flow commands the scheduler dispatches BEFORE the macro registry (round-74
-- asymmetric semantics: runtime macros override only NON-special commands,
-- i.e. any name that reaches the scheduler's regular kag dispatch). A macro
-- that shadows one of these special names can never be invoked as a macro,
-- and a macro that shadows a migrated/handler command silently overrides it
-- at runtime -- both are smells worth a [WARN].
local FLOW_SPECIAL = {}
for _, c in ipairs({
    "if", "elseif", "else", "endif", "while", "endwhile", "for", "endfor",
    "until", "break", "continue", "jump", "goto", "call", "return", "switch",
    "endswitch", "case", "endcase", "default", "label", "macro", "endmacro",
    "erasemacro", "eval", "iscript", "emb",
}) do
    FLOW_SPECIAL[c] = true
end
-- Blocks whose opener/closer must balance (conservative nesting parity).
local BLOCK_OPEN  = { ["if"] = true, ["while"] = true, ["for"] = true, switch = true }
local BLOCK_CLOSE = { endif = "if", endwhile = "while", endfor = "for", endswitch = "switch" }
local BLOCK_CLOSE_TOK = { endif = true, endwhile = true, endfor = true, endswitch = true }
-- (c) BUILT-IN *flow* command names: the scheduler dispatches these BEFORE
-- macro lookup (they never reach the kag/macro dispatch), so a [macro] named
-- any of them is dead -- it can never be invoked as a macro (round-76 lint).
-- [goto] added round-77: goto is flow, so [macro goto] is a dead macro too.
local FLOW_MACRO_SLOT = {
    ["if"] = true, ["while"] = true, ["for"] = true, ["jump"] = true,
    ["call"] = true, ["link"] = true, ["label"] = true, ["goto"] = true,
}
-- (g) condition commands that MUST carry a non-empty exp= to run their body.
local COND_EXP = { ["if"] = true, ["while"] = true, ["until"] = true }
-- (j) commands whose target may reference a *label in the current scene.
--     Includes [sel]/[select] (choice navigation jumps to a target label).
local NAV_LABEL_REF = {}
for _, c in ipairs({ "jump", "call", "link", "goto", "sel", "select", "button" }) do
    NAV_LABEL_REF[c] = true
end

local function macroNameOf(tok)
    if tok.type ~= "command" or tok.cmd ~= "macro" then return nil end
    local mname = nil
    for _, pair in ipairs(tok.params or {}) do
        if type(pair) == "table" and pair[1] == "name" then
            mname = pair[2]
        end
    end
    if mname == nil and tok.params and tok.params[1] then
        mname = tok.params[1][2]  -- bare [macro shout]
    end
    if type(mname) ~= "string" then return nil end
    return mname
end

structuralWarnings = function(path, tokens, lineOf, sceneRegistry)
    -- (a) cross-scene [call]/[jump]/[link] resolution registry. An optional
    --     caller-supplied registry overrides directory discovery (tests pass a
    --     synthetic one so the check is driveable without real scene dirs);
    --     otherwise scan the current scene's directory. ready=false => the scan
    --     could not prove it read the directory -> skip cross-scene checks.
    local registry, registry_ready = nil, false
    if type(sceneRegistry) == "table" then
        registry = sceneRegistry
        registry_ready = true
    else
        registry, registry_ready = scene_discovery(path)
    end
    -- (a) collect every *label defined in THIS scene; a name seen more than
    --     once is a duplicate definition (runtime: first definition wins).
    local labels = {}        -- name -> first-definition line
    local seen_dup = {}
    for _, tok in ipairs(tokens) do
        if tok.type == "label" and type(tok.name) == "string" then
            local name = tok.name
            local ln = lineOf(tok.offset or 1)
            if labels[name] == nil then
                labels[name] = ln
            elseif not seen_dup[name] then
                seen_dup[name] = true
                warn_scene(path, ln,
                    "[label *" .. name .. "] defined more than once in this "
                        .. "scene (first definition at line " .. labels[name]
                        .. " wins)")
            end
        end
    end

    -- (j) [label] defined but never targeted by any navigation in THIS scene
    --     ([jump]/[call]/[link]/[goto]/[sel]/[select]). Labels are how the
    --     scheduler re-enters a scene, so a label can also be a cross-scene
    --     entry point, an entry fallback, or a fall-through section anchor --
    --     all legitimate uses an in-scene-absence would wrongly flag. To stay
    --     conservative and false-positive-free we only warn when:
    --       * the scene-directory registry scan succeeded (registry_ready,
    --         mirroring the round-77 cross-scene gate), so we are in a known
    --         project layout;
    --       * the label is not the scene's FIRST *label (default entry);
    --       * no in-scene nav targets it;
    --       * the immediately-preceding top-level structural token is a
    --         [jump]/[goto]/[return] -- i.e. a label perched right after a
    --         one-way transfer is genuinely orphaned (fall-through anchors,
    --         sub-entries and labels parked after [end]/[stop]/content are
    --         all exempt -- [end]/[stop] labels are deliberate dead-end
    --         scaffolding, also surfaced by the unreachable-after-jump check).
    if registry_ready then
        local first_label = nil
        for _, tok in ipairs(tokens) do
            if tok.type == "label" and type(tok.name) == "string" then
                first_label = tok.name
                break
            end
        end
        local labeled = {}   -- name -> first token index
        for i, tok in ipairs(tokens) do
            if tok.type == "label" and type(tok.name) == "string"
                and labeled[tok.name] == nil then
                labeled[tok.name] = i
            end
        end
        -- in-scene navigation target set (label references)
        local reached = {}
        for _, tok in ipairs(tokens) do
            if tok.type == "command" and type(tok.cmd) == "string"
                and NAV_LABEL_REF[tok.cmd] then
                local tgt = nil
                for _, pair in ipairs(tok.params or {}) do
                    if type(pair) == "table" and (pair[1] == "target"
                        or pair[1] == "label" or pair[1] == "storage"
                        or pair[1] == "x") then
                        tgt = pair[2]
                    end
                end
                if tgt == nil and tok.params and tok.params[1] then
                    tgt = tok.params[1][2]
                end
                if type(tgt) == "string" and tgt:sub(1, 1) == "*" then
                    local nm = tgt:gsub("^*", "")
                    if nm ~= "" then reached[nm] = true end
                end
            end
        end
        -- for each label, find the preceding top-level structural command
        local function preceding_outer(start_idx)
            local depth = 0
            local outer = nil
            for j = start_idx - 1, 1, -1 do
                local t = tokens[j]
                if t and t.type == "label" then
                    if outer == nil then outer = "label" end
                    break
                end
                if t and t.type == "command" and type(t.cmd) == "string" then
                    local c = t.cmd
                    if BLOCK_CLOSE_TOK[c] then
                        depth = depth + 1
                    elseif BLOCK_OPEN[c] then
                        if depth > 0 then depth = depth - 1
                        elseif outer == nil then outer = c end
                    else
                        if outer == nil then outer = c end
                    end
                end
            end
            return outer
        end
        for name, idx in pairs(labeled) do
            if name ~= first_label and not reached[name] then
                local outer = preceding_outer(idx)
                if outer == "jump" or outer == "goto" or outer == "return" then
                    warn_scene(path, labels[name] or lineOf(tokens[idx].offset or 1),
                        "[label *" .. name .. "] defined but never referenced "
                            .. "by any [jump]/[call]/[link]/[goto]/[sel] in this "
                            .. "scene, and unreachable by fall-through")
                end
            end
        end
    end

    -- (b) collect [macro] definitions (name -> 0 = never invoked)
    local macro_calls = {}
    for _, tok in ipairs(tokens) do
        local mname = macroNameOf(tok)
        if type(mname) == "string" and mname ~= "" then
            macro_calls[mname] = 0
        end
    end

    -- switch nesting: each frame holds already-seen tostring(case-value) set
    local switch_stack = {}

    -- nesting parity for unreachable/closer checks
    local flow_depth = 0
    local unreachable = nil       -- line of the top-level [jump] after which code is dead
    local unreachable_report = false

    local function paramVal(tok, key)
        for _, pair in ipairs(tok.params or {}) do
            if type(pair) == "table" and pair[1] == key then
                return pair[2]
            end
        end
        return nil
    end

    for _, tok in ipairs(tokens) do
        -- Labels terminate an unreachable region at the top level.
        if tok.type == "label" and type(tok.name) == "string" then
            if unreachable and flow_depth == 0 then
                unreachable = nil
                unreachable_report = false
            end
            goto nexttok
        end
        if tok.type ~= "command" or type(tok.cmd) ~= "string" then
            goto nexttok
        end
        local cmd = tok.cmd

        -- (f) [endfor]/[endwhile]/[endif]/[endswitch] without a matching
        --     opener (orphaned closer). Conservative shared-depth count: an
        --     opener anywhere balances a closer, so only genuinely orphaned
        --     closers fire (no false positives on valid scenes).
        if flow_depth <= 0 and BLOCK_CLOSE_TOK[cmd] then
            warn_scene(path, lineOf(tok.offset or 1),
                "[" .. cmd .. "] without matching opener (unbalanced flow nesting)")
        end

        -- (g) [if]/[while]/[until] with a MISSING or empty exp= -- the
        --     expression compiles to a constant that never enables the branch
        --     (a missing exp is false, an empty one is falsey), so the body
        --     never runs -- almost always a bug. Conservative: only fires on
        --     those three condition commands (for/switch use other params).
        if COND_EXP[cmd] then
            local eexp = paramVal(tok, "exp")
            if eexp == nil or (type(eexp) == "string"
                and eexp:gsub("%s", "") == "") then
                warn_scene(path, lineOf(tok.offset or 1),
                    "[" .. cmd .. "] missing or empty exp= (condition always "
                        .. "false -- body never runs)")
            end
        end

        -- (h) duplicate named parameters in ONE tag ([ch text="a" text="b"]):
        --     the LAST value wins at runtime, so the earlier one is silently
        --     dropped -- a copy/paste smell. Skips positional/numeric slots.
        if tok.params then
            local seen_param = {}
            local warned_param = {}
            for _, p in ipairs(tok.params) do
                if type(p) == "table" and type(p[1]) == "string"
                    and not p[1]:match("^%d+$") then
                    if seen_param[p[1]] then
                        if not warned_param[p[1]] then
                            warned_param[p[1]] = true
                            warn_scene(path, lineOf(tok.offset or 1),
                                "[" .. cmd .. "] duplicate parameter '"
                                    .. p[1] .. "' (last value wins at runtime)")
                        end
                    else
                        seen_param[p[1]] = true
                    end
                end
            end
        end

        -- unreachable: first command/text token after a top-level [jump]
        if unreachable and flow_depth == 0 and not unreachable_report then
            warn_scene(path, lineOf(tok.offset or 1),
                "token(s) unreachable after [jump] (line " .. unreachable
                    .. ") until the next *label")
            unreachable_report = true
        end

        -- (a) navigation target label existence (same-scene only)
        if NAV_CMDS[cmd] then
            local target = nil
            for _, pair in ipairs(tok.params or {}) do
                if type(pair) == "table" and (pair[1] == "target"
                    or pair[1] == "label" or pair[1] == "storage") then
                    target = pair[2]
                end
            end
            if target == nil and tok.params and tok.params[1] then
                target = tok.params[1][2]  -- bare [jump *lab]
            end
            if type(target) == "string" and target:sub(1, 1) == "*" then
                local name = target:gsub("^*", "")
                if name ~= "" and not labels[name] then
                    warn_scene(path, lineOf(tok.offset or 1),
                        "[" .. cmd .. "] target '*" .. name
                            .. "' not defined in this scene (label missing?)")
                end
            elseif type(target) == "string" then
                -- (b) empty / whitespace-only navigation target: the scheduler
                --     has no destination to resolve (it would try to load a
                --     blank scene and fail). Warn unconditionally.
                if target:gsub("%s", "") == "" then
                    warn_scene(path, lineOf(tok.offset or 1),
                        "[" .. cmd .. "] empty target (missing destination scene/label)")
                else
                    -- (a) cross-scene *.ks reference: resolve the basename against
                    --     the scene directory registry. Only fires in a "known
                    --     good" scene dir and only for .ks targets -- a staged /
                    --     expression-free non-.ks target is left alone.
                    local tbase = target:match("([^/\\\\]+)$")
                    if tbase and tbase:find("%.ks$") and registry_ready
                        and not registry[tbase] then
                        warn_scene(path, lineOf(tok.offset or 1),
                            "[" .. cmd .. "] cross-scene target scene '" .. tbase
                                .. "' not found in scene directory")
                    end
                end
            end
            -- (e) an unconditional top-level [jump] makes same-level code below
            --     it unreachable until the next *label. [call] is deliberately
            --     NOT treated as terminating: it returns to the caller, so
            --     code after it is reachable again (no false positives).
            if cmd == "jump" and flow_depth == 0 and not unreachable then
                unreachable = lineOf(tok.offset or 1)
                unreachable_report = false
            end

        -- (for) loop that can never run: start>end with positive step, or
        -- start<end with negative step. Only fires on compile-time numeric
        -- literals (expressions are skipped to stay conservative).
        elseif cmd == "for" then
            local s = tonumber(paramVal(tok, "start"))
            local ev = tonumber(paramVal(tok, "end"))
            local step = tonumber(paramVal(tok, "step")) or 1
            if s ~= nil and ev ~= nil and step ~= 0
                and ((step > 0 and s > ev) or (step < 0 and s < ev)) then
                warn_scene(path, lineOf(tok.offset or 1),
                    "[for] loop body never runs (start=" .. s
                        .. " end=" .. ev .. " step=" .. step .. ")")
            end

        -- (c) case values within the current [switch] block
        elseif cmd == "case" and #switch_stack > 0 then
            local val = nil
            for _, pair in ipairs(tok.params or {}) do
                if type(pair) == "table" and (pair[1] == "value"
                    or pair[1] == "exp" or pair[1] == "1") then
                    val = pair[2]
                end
            end
            if val == nil and tok.params and tok.params[1] then
                val = tok.params[1][2]
            end
            if val ~= nil then
                local key = tostring(val)
                local frame = switch_stack[#switch_stack]
                if frame[key] then
                    warn_scene(path, lineOf(tok.offset or 1),
                        "[case] duplicate value '" .. val
                            .. "' in [switch] block")
                else
                    frame[key] = true
                end
            end

        -- (c) switch block open/close
        elseif cmd == "switch" then
            switch_stack[#switch_stack + 1] = {}
        elseif cmd == "endswitch" then
            if #switch_stack > 0 then table.remove(switch_stack) end
        end

        -- (c) a [macro] whose name is a BUILT-IN *flow* command name
        --     (if/while/for/jump/call/link/label): the scheduler dispatches these
        --     BEFORE macro lookup -- the flow branch always wins, so the macro is
        --     dead (it can never be invoked as a macro). Round-76 lint.
        if cmd == "macro" then
            local mname = macroNameOf(tok)
            if mname and mname ~= "" then
                if FLOW_MACRO_SLOT[mname] then
                    warn_scene(path, lineOf(tok.offset or 1),
                        "[macro " .. mname .. "] shadows BUILT-IN flow command '"
                            .. mname
                            .. "' (dead macro: scheduler dispatches flow before macro lookup)")
                -- (d) a [macro] whose name a runtime macro would override: a migrated
                --     contract command or a registered kag handler reached through
                --     the regular dispatch (round-74 asymmetric semantics). Excludes
                --     flow-special names (those can't be macro-dispatched, and are
                --     already reported above as dead flow-shadow macros).
                elseif not FLOW_SPECIAL[mname] then
                    local shadow = schema.isMigrated(mname)
                        or (kag_cmd_table and kag_cmd_table[mname] ~= nil)
                    if shadow then
                        warn_scene(path, lineOf(tok.offset or 1),
                            "[macro " .. mname .. "] shadows built-in command '"
                                .. mname .. "' (runtime macro overrides it)")
                    end
                end
            end
        end

        -- (b) macro invocation count (a defined macro used as [name])
        if macro_calls[cmd] ~= nil then
            macro_calls[cmd] = macro_calls[cmd] + 1
        end
        -- erasemacro removes the macro before future calls: exempt
        if cmd == "erasemacro" then
            local e = paramVal(tok, "name")
            if e == nil and tok.params and tok.params[1] then
                e = tok.params[1][2]
            end
            if type(e) == "string" and macro_calls[e] ~= nil then
                macro_calls[e] = -1
            end
        end

        -- maintain flow nesting parity for unreachable/closer checks
        if BLOCK_OPEN[cmd] then
            flow_depth = flow_depth + 1
        elseif BLOCK_CLOSE_TOK[cmd] then
            flow_depth = math.max(0, flow_depth - 1)
        end

        ::nexttok::
    end

    -- (b) report macros that were defined but never invoked
    for name, count in pairs(macro_calls) do
        if count == 0 then
            local dline = 0
            for _, tok in ipairs(tokens) do
                if macroNameOf(tok) == name then
                    dline = lineOf(tok.offset or 1)
                    break
                end
            end
            warn_scene(path, dline,
                "[macro " .. name .. "] defined but never invoked in this scene")
        end
    end
end

-- ── Default-shadowing audit (Neo-Genesis regression guard) ────────────
-- Contract defaults fill absent params during coerce; if the handler
-- falls back to params[1] (positional form), the default shadows it.
-- This bit us three times (ending/gallery/image/set*volume). The audit
-- lists every migrated command whose handler mentions params[N] while
-- the contract declares a non-nil default for a named param.
local function auditDefaults()
    local contracts = schema.dumpContracts()
    local suspects = {}
    local function scanFile(path, cmds)
        local f = io.open(path, "r")
        if not f then return end
        local src = f:read("*a")
        f:close()
        if not src:find("params%[%d%]", 1) then return end  -- no positional use
        for cmd in pairs(cmds) do
            local specs = contracts[cmd]
            if specs then
                for name, spec in pairs(specs) do
                    if spec.default ~= nil then
                        suspects[#suspects + 1] = string.format(
                            "%s: %s.default=%s (handler uses params[N])",
                            cmd, name, tostring(spec.default))
                    end
                end
            end
        end
    end
    -- command-module files with positional fallbacks
    scanFile(here .. "kag/commands/audio.lua", { ["setbgmvolume"] = true, ["setsevolume"] = true, ["setvoicevolume"] = true })
    scanFile(here .. "kag.lua", { ["ld"] = true, ["play"] = true, ["se"] = true, ["voice"] = true, ["bgm"] = true })
    if #suspects == 0 then
        print("AUDIT: no default-shadowing suspects")
    else
        print("AUDIT: " .. #suspects .. " default-shadowing suspect(s):")
        for _, s in ipairs(suspects) do print("  " .. s) end
    end
    return #suspects
end

-- Module guard: tests require this file for its functions; only run
-- the CLI main when invoked as a script (audit: requiring it called
-- os.exit and killed the test process).

-- exact basename: "test_ks_check.lua" must NOT count (it embeds
-- ks_check.lua as a suffix) -- audit: the test process hit usage/exit
local function base_name(p)
    return p:match("([^/\\]+)$")
end
local is_script = arg and arg[0] and base_name(arg[0]) == "ks_check.lua"
if is_script and arg[1] == "--audit-defaults" then
    os.exit(auditDefaults() > 0 and 1 or 0)  -- CI gate: nonzero on suspects
end

if is_script and #arg == 0 then
    print("usage: lua scripts/ks_check.lua <scene.ks> [more ...]")
    print("       lua scripts/ks_check.lua --audit-defaults")
    print("       lua scripts/ks_check.lua --target web|native --profile facts.json --project project.json [--json-output report.json] [--capabilities-only] <scenes ...>")
    os.exit(2)
end
if is_script then
    local options, paths = {}, {}
    local option_names = {
        ["--target"]="target", ["--profile"]="profile",
        ["--project"]="project", ["--json-output"]="output",
    }
    local index = 1
    while index <= #arg do
        local value = arg[index]
        if value == "--" then
            for rest=index+1,#arg do paths[#paths+1] = arg[rest] end
            break
        elseif option_names[value] then
            local key = option_names[value]
            if options[key] or not arg[index+1] or arg[index+1] == "" then
                io.stderr:write("ks_check: missing or duplicate option value\n"); os.exit(2)
            end
            options[key] = arg[index+1]
            index = index + 1
        elseif value == "--capabilities-only" then
            options.capabilities_only = true
        elseif value:sub(1,2) == "--" then
            io.stderr:write("ks_check: unknown option\n"); os.exit(2)
        else paths[#paths+1] = value end
        index = index + 1
    end
    if #paths == 0 then io.stderr:write("ks_check: at least one scene is required\n"); os.exit(2) end
    if not options.target then
        if options.profile or options.project or options.output or options.capabilities_only then
            io.stderr:write("ks_check: capability options require --target\n"); os.exit(2)
        end
        for _, path in ipairs(paths) do checkScene(path) end
        if issues > 0 then
            print(string.format("%d contract violation(s) found", issues))
            os.exit(1)
        end
        print("OK: all scenes pass contract checks")
        os.exit(0)
    end
    if (options.target ~= "native" and options.target ~= "web")
        or not options.profile or not options.project then
        io.stderr:write("ks_check: target checks require web|native, --profile and --project inputs\n"); os.exit(2)
    end
    local json = require("capability_json")
    local capabilities = require("target_capabilities")
    local function read_json(path)
        local file = io.open(path, "rb")
        if not file then return nil, "cannot_open_capability_input" end
        local content = file:read(json.MAX_BYTES + 1)
        file:close()
        return json.decode(content)
    end
    local function failure_report(reason, path)
        return {passed=false, target=options.target, catalog_sha256=capabilities.catalog_sha256,
            analysis_scope="declared_and_catalogued_static_calls",
            requirements=json.array(), warnings=json.array(), not_proven=json.array(),
            errors=json.array({{reason=reason, location={scene=path}}})}
    end
    local project, project_error = read_json(options.project)
    local profile, profile_error = read_json(options.profile)
    local policy, policy_error
    if project ~= nil then policy, policy_error = capabilities.validate_project(project) end
    local profile_ok, profile_reason
    if profile ~= nil then profile_ok, profile_reason = capabilities.validate_profile(profile) end
    local result
    if project == nil then result = failure_report(project_error, options.project)
    elseif not policy then result = failure_report(policy_error, options.project)
    elseif profile == nil then result = failure_report(profile_error, options.profile)
    elseif not profile_ok then result = failure_report(profile_reason, options.profile)
    elseif profile.target ~= options.target then result = failure_report("target_profile_mismatch", options.profile)
    else
        local session = {module=capabilities, calls={}, unproven={}, capabilities_only=options.capabilities_only}
        for _, path in ipairs(paths) do checkScene(path, session) end
        result = capabilities.check(policy, profile, session.calls, session.unproven)
        if issues > 0 then
            result.passed = false
            result.errors[#result.errors+1] = {reason="scene_contract_violations", count=issues}
        end
    end
    if options.output then
        local encoded, reason = json.encode(result)
        if not encoded then io.stderr:write(reason .. "\n"); os.exit(1) end
        local file = io.open(options.output, "wb")
        if not file then io.stderr:write("ks_check: cannot write capability report\n"); os.exit(1) end
        local written = file:write(encoded, "\n")
        local closed = file:close()
        if not written or not closed then io.stderr:write("ks_check: capability report write failed\n"); os.exit(1) end
    end
    for _, group in ipairs({result.errors, result.warnings}) do
        for _, item in ipairs(group) do
            local query = item.capability or {}
            local location = item.location or {}
            print(string.format("[capabilities] target=%s feature=%s decision=%s reason=%s at %s:%s [%s]",
                options.target, item.feature or query.feature or "?", item.decision or "deny",
                item.reason or query.reason or "capability_check_failed", location.scene or "project",
                tostring(location.line or 0), location.command or "declaration"))
        end
    end
    print(string.format("Target capabilities: %s; %d unresolved dynamic span(s)",
        result.passed and "PASS" or "FAIL", #result.not_proven))
    os.exit(result.passed and 0 or 1)
end

return { strip_tail = strip_tail, checkScene = checkScene,
    structuralWarnings = structuralWarnings }
