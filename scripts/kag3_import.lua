-- kag3_import.lua — KAG3 script importer (KAG Neo-Genesis migration tool)
-- Usage:
--   lua scripts/kag3_import.lua <scene.ks> [more.ks ...]      # check mode
--   lua scripts/kag3_import.lua -o <outdir> <scene.ks> ...    # convert mode
--   lua scripts/kag3_import.lua --strict <scene.ks>           # unsupported -> exit 2
--
-- Imports legacy KiriKiri/KAG3 .ks scripts into the KAG Neo-Genesis
-- format: converts &var text embeds to %var%, statically translates
-- TJS-style expressions in exp=/expr=/cond= params to Lua semantics,
-- rewrites commands with a Neo-Genesis equivalent, and REPORTS every
-- command the engine does not support (the scheduler would otherwise
-- silently render it as dialogue text) plus [iscript] TJS blocks that
-- need manual porting. Convert mode rebuilds the file from tokenizer
-- byte offsets, preserving comments/whitespace verbatim.
--
-- Exit codes: 0 = clean (or only warnings), 1 = file/parse error,
-- 2 = --strict and at least one unsupported command or iscript block.

-- Resolve scripts/ from this file's location (works from any CWD).
local BS = string.char(92)  -- backslash
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
package.path = here .. "?.lua;" .. here .. "kag/?.lua;" .. package.path

local tokenizer = require("tokenizer")
local schema = require("kag.schema")
local expr = require("kag.expr")

-- Register every command module so all contracts load (same pattern as
-- ks_check.lua: the contract registry is the known-command source).
pcall(require, "kag.commands.text")
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.audio")
pcall(require, "kag.commands.transition")
pcall(require, "kag.commands.layer")
pcall(require, "kag.commands.vfx")
pcall(require, "kag.commands.video")
pcall(require, "kag.commands.save")
-- Round-71 KAG3-compat modules. Pre-registered explicitly so their schema
-- contracts load into the known set even if the heavy require("kag") fails
-- (headless lint contexts); otherwise [csp]/[csd]/[csl]/[add]/[sub]/[mul]/
-- [div]/[mod]/[dec]/[preload] would fall through to UNSUPPORTED.
pcall(require, "kag.commands.math")
pcall(require, "kag.commands.character")
pcall(require, "kag.commands.resource")
local kag_ok = pcall(require, "kag")

-- ---------------------------------------------------------------------------
-- Known-command set: schema contracts + kag handler table + scheduler
-- flow commands (handled inline by the scheduler, no schema/handler).
-- ---------------------------------------------------------------------------

local FLOW_COMMANDS = {
    "if", "elseif", "else", "endif", "while", "endwhile", "for", "endfor",
    "break", "continue", "jump", "call", "return", "label", "macro",
    "endmacro", "erasemacro", "eval", "emb", "iscript", "endscript",
    "select", "sel", "endselect", "link", "end", "stop", "button",
    "endbutton", "switch", "endswitch", "case", "endcase", "default",
}

local known = {}
for cmd in pairs(schema.dumpContracts()) do known[cmd] = true end
for _, c in ipairs(FLOW_COMMANDS) do known[c] = true end
if kag_ok and type(package.loaded["kag"]) == "table" then
    local kag_tbl = package.loaded["kag"]
    for k in pairs(kag_tbl) do
        if type(k) == "string" then known[k] = true end
    end
end

-- ---------------------------------------------------------------------------
-- Command renames: KAG3 command -> Neo-Genesis equivalent (same semantics).
-- ---------------------------------------------------------------------------
local RENAMES = {
    waitse   = "waitsound",   -- KAG3: wait for SE to finish; engine: waitsound
    ["goto"] = "jump",        -- KAG3: [goto *label] is a strict alias of [jump]
}

-- ---------------------------------------------------------------------------
-- KAG3 commands with no Neo-Genesis equivalent: reported with suggestions.
-- Common KAG3 tag set the engine deliberately does not carry.
-- ---------------------------------------------------------------------------
local SUGGESTIONS = {
    chara        = "use [fg storage=...] / [ch] (character layer); sprite family: [sprite_fade]/[sprite_move]/[sprite_scale]/[sprite_swap]",
    chara_show   = "use [fg storage=...] or [sprite_fade] for enter animations",
    chara_hide   = "use [ld layer=fg] or [sprite_fade opacity=0]",
    chara_modify = "use [ch name=...] + [sprite_swap] for expression/dress changes",
    chara_up     = "z-order handled by layer ids; use [position layer=...]",
    chara_down   = "z-order handled by layer ids; use [position layer=...]",
    motion       = "no skeletal motion system; use [sprite_move]/[sprite_scale]",
    waitmotion   = "no skeletal motion system; use [wait] after [sprite_move]",
    btndef       = "use [button text=... target=*label] ... [endbutton]",
    btn          = "use [button text=... target=*label] ... [endbutton]",
    wm           = "message window is always shown; use [textbox] to restyle",
    waits        = "use [voice_wait] / [waitsound] / [waitbgm] explicitly",
    user         = "no TJS user-command bridge; port the logic to [iscript] Lua",
    syscall      = "no TJS syscall bridge; port the logic to [iscript] Lua",
    notif        = "use [notify msg=...] (Neo-Genesis)",
    savegame     = "use [save slot=N] (bare [save N] works too)",
    loadgame     = "use [load slot=N]",
    savenumber   = "save slots are engine-managed; use [listsaves]/[save]",
    savefrom     = "save slots are engine-managed; use [save slot=N]",
    reload       = "use kagReloadScene (hot reload) or [jump] to re-run the scene",
    resettimer   = "no TJS timer; use [wait] with explicit durations",
    textgosub    = "use [call *label] / [return] (same-scene subroutines)",
    setwindow    = "use [textbox] (Neo-Genesis window styling)",
    maxkaisou    = "no KAG3 history-cap limit; [history] backlog is unbounded",
    monocro      = "no monochrome effect; use [palette] with a custom LUT or [flash] tinting",
    gsel         = "use [button]/[endbutton] or [select]/[sel]/[endselect]",
    menu         = "use [button]/[endbutton] (in-game menus are Lua UI)",
    help         = "no help overlay; add one with Lua UI via [emb]/[iscript]",
}

-- Text-ish params: &var embeds are converted inside these values.
local TEXT_PARAMS = {
    text = true, name = true, caption = true, title = true, msg = true,
}
-- Expression params: TJS-style source is statically translated to Lua.
local EXPR_PARAMS = {
    exp = true, expr = true, cond = true,
}

-- Parameter-name aliases: KAG3 tag param -> Neo-Genesis engine param, keyed
-- by the KAG3 command name. Applied in processScene's param rewrite so e.g.
-- KAG3 [add var="f.hp" value="5"] becomes [add name="f.hp" value="5"]
-- (the engine math contract is name= + value/amount=). Also covers the KAG3
-- character-position convention left/top -> engine x/y for [csp]/[csl].
local PARAM_ALIASES = {
    add   = { var = "name" },
    sub   = { var = "name" },
    mul   = { var = "name" },
    div   = { var = "name" },
    mod   = { var = "name" },
    dec   = { var = "name" },
    csp   = { left = "x", top = "y" },
    csl   = { left = "x", top = "y" },
}

-- Commands whose NAME matches an engine command but whose KAG3 semantics
-- differ from the engine's meaning. Reported as a non-blocking conflict
-- note (advisory): the tag passes through as KNOWN (the engine owns the
-- name) so import never hard-fails, but the author is told the meaning
-- changed so a silent semantic mismatch cannot creep in.
local CONFLICT_NOTES = {
    palette = "KAG3 [palette] selects the message-window palette by index; the engine [palette effect=...] is LUT color grading. Pick index= colors -> [color]/[textbox] styling, or rewrite as an explicit [palette effect=...] LUT apply.",
}

local M = {}

local AMP_NS = { f = true, tf = true, sf = true, mp = true, lf = true }

--- M.convertAmpVars(s) -> string. KAG3 text embed &f.x / &tf.x / &sf.x /
--  &mp.x / &lf.x become %f.x% (Neo-Genesis interpolation). Logical &&
--  is protected (placeholder byte, restored after); unknown namespaces
--  (&kag.status) are left untouched.
function M.convertAmpVars(s)
    if type(s) ~= "string" or not s:find("&", 1, true) then return s end
    s = s:gsub("&&", "\1")  -- protect logical and
    s = s:gsub("&(%a+)%.([%w_]+)", function(ns, key)
        if AMP_NS[ns] then
            return "%" .. ns .. "." .. key .. "%"
        end
        return "&" .. ns .. "." .. key
    end)
    s = s:gsub("\1", "&&")
    return s
end

--- M.convertMacroArgs(s) -> string. KAG3 macro-body argument references:
--  positional &N and bare &name become the Neo-Genesis %N%/ %name%
--  placeholders the scheduler's macro expansion fills from the invocation's
--  params (bare positional -> params[1], args="..."-declared -> %arg%).
--  Namespace-qualified system vars (&f.x, &kag.status) are shielded so they
--  are never mistaken for a macro arg (convertAmpVars already handled the
--  f/tf/sf/mp/lf cases). Logical && is protected.
function M.convertMacroArgs(s)
    if type(s) ~= "string" or not s:find("&", 1, true) then return s end
    s = s:gsub("&&", "\1")       -- protect logical and
    local shielded = {}
    s = s:gsub("&%a+%.%w+", function(m)  -- shield &ns.key namespaced refs
        shielded[#shielded + 1] = m
        return "\2"
    end)
    s = s:gsub("&([%w_]+)", function(name)
        return "%" .. name .. "%"
    end)
    s = s:gsub("\2", function() return table.remove(shielded, 1) end)
    s = s:gsub("\1", "&&")
    return s
end

--- M.translateExpr(src) -> string. Static TJS->Lua translation for
--  expression params (reuses the runtime translator as a pure function).
function M.translateExpr(src)
    if type(src) ~= "string" then return src end
    local ok, out = pcall(expr.translate, src)
    if ok then return out end
    return src
end

--- M.convertCommand(cmd, params) -> new_cmd, notes
--  Applies the rename table and collects unsupported-command notes.
function M.convertCommand(cmd, params)
    local notes = {}
    local new_cmd = RENAMES[cmd]
    if new_cmd then
        notes[#notes + 1] = "renamed [" .. cmd .. "] -> [" .. new_cmd .. "]"
        return new_cmd, notes
    end
    if known[cmd] then
        -- Known command. A name that also carries a KAG3 semantic conflict
        -- is surfaced as a non-blocking advisory note (see CONFLICT_NOTES).
        local conflict = CONFLICT_NOTES[cmd]
        if conflict then
            notes[#notes + 1] = "NOTE [" .. cmd .. "]: " .. conflict
        end
        return cmd, notes
    end
    local sug = SUGGESTIONS[cmd]
    notes[#notes + 1] = "UNSUPPORTED [" .. cmd .. "]"
        .. (sug and (": " .. sug) or ": no Neo-Genesis equivalent")
    return cmd, notes
end

local function processSceneText(path, raw)
    -- BOM: tokenizer.parse_with_offsets strips a leading UTF-8 BOM before
    -- computing byte offsets; rebuild must use the same stripped text.
    local bom = ""
    if raw:sub(1, 3) == "\239\187\191" then
        bom = raw:sub(1, 3)
        raw = raw:sub(4)
    end

    local tokens = tokenizer.parse_with_offsets(raw)
    if not tokens then
        return nil, "tokenize failed: " .. tostring(path)
    end

    local report = {
        path = path, tokens = #tokens, texts = 0, labels = 0, iscripts = 0,
        converted_embeds = 0, converted_exprs = 0, renames = 0,
        unsupported = {}, conflicts = {}, iscript_blocks = {},
    }

    -- Line index for source-accurate line numbers (same as ks_check).
    local lineStarts = { 1 }
    for i = 1, #raw do
        if raw:byte(i) == 10 then lineStarts[#lineStarts + 1] = i + 1 end
    end
    local function lineOf(offset)
        local lo, hi = 1, #lineStarts
        while lo < hi do
            local mid = (lo + hi + 1) // 2
            if lineStarts[mid] <= offset then lo = mid else hi = mid - 1 end
        end
        return lo
    end

    local pieces = {}      -- rebuilt output chunks
    local pos = 1          -- scan position in stripped raw text
    local inMacro = false  -- inside [macro]..[endmacro]: &N/&name -> %N%/%name%

    for _, tok in ipairs(tokens) do
        local st, en = tok.offset, tok.end_offset
        if st > pos then
            pieces[#pieces + 1] = raw:sub(pos, st - 1)  -- comments/whitespace verbatim
        end
        local raw_tok = raw:sub(st, en)
        local line = lineOf(st)

        if tok.type == "text" then
            report.texts = report.texts + 1
            local conv = M.convertAmpVars(raw_tok)
            if inMacro then  -- KAG3 macro-arg references (&N, &name)
                local mconv = M.convertMacroArgs(conv)
                if mconv ~= conv then
                    report.converted_embeds = report.converted_embeds + 1
                    conv = mconv
                end
            end
            if conv ~= raw_tok then report.converted_embeds = report.converted_embeds + 1 end
            pieces[#pieces + 1] = conv
        elseif tok.type == "label" then
            report.labels = report.labels + 1
            pieces[#pieces + 1] = raw_tok
        elseif tok.type == "iscript" then
            report.iscripts = report.iscripts + 1
            report.iscript_blocks[#report.iscript_blocks + 1] = line
            pieces[#pieces + 1] = raw_tok  -- TJS body kept verbatim for manual porting
        elseif tok.type == "command" then
            local new_cmd, notes = M.convertCommand(tok.cmd, tok.params)
            local out = raw_tok
            if new_cmd ~= tok.cmd then
                out = out:gsub("^%[%s*[%w_]+", "[" .. new_cmd, 1)
                report.renames = report.renames + 1
            end
            -- Param-name aliases (KAG3 name -> engine name), e.g.
            -- [add var=...] -> [add name=...] and [csp left/top=...] -> x/y.
            local aliases = PARAM_ALIASES[tok.cmd]
            -- Param-value rewrites: text-ish params get &var conversion,
            -- expression params get static TJS translation. Operates on
            -- the raw token slice, keyed by the (possibly aliased) param name.
            -- Helper shared by the quoted and bare (unquoted) param-value
            -- rewrite passes: rename the param via PARAM_ALIASES, and for
            -- text/expression params translate the VALUE. `q` is the quote
            -- character (a quote or "") and is preserved so rebuild parity
            -- holds (a bare value stays bare).
            local function rewrite_param(pname, pval, q)
                local eparam = (aliases and aliases[pname]) or pname
                if TEXT_PARAMS[eparam] then
                    local conv = M.convertAmpVars(pval)
                    if conv ~= pval then report.converted_embeds = report.converted_embeds + 1 end
                    return eparam .. "=" .. q .. conv .. (q or "")
                elseif EXPR_PARAMS[eparam] then
                    local conv = M.translateExpr(pval)
                    if conv ~= pval then report.converted_exprs = report.converted_exprs + 1 end
                    return eparam .. "=" .. q .. conv .. (q or "")
                end
                return eparam .. "=" .. q .. pval .. (q or "")
            end
            -- Quoted values: name="value" (the common KAG3 form).
            out = out:gsub("([%w_]+)%s*=%s*\"([^\"]*)\"", function(pname, pval)
                return rewrite_param(pname, pval, '"')
            end)
            -- Bare (unquoted) values: name=var.path / name=5 / name=hero. KAG3
            -- allows unquoted simple values, and round-72's param aliases
            -- ([add var=...] -> name=...) must rename the KEY even when the
            -- value is bare, or the converted scene silently no-ops at runtime
            -- (the engine math contract reads `name`). The bare charset
            -- explicitly excludes quotes/spaces/operators so the quoted pass
            -- above and expression params stay untouched.
            out = out:gsub("([%w_]+)%s*=%s*([%w_%.%-+]+)", function(pname, pval)
                return rewrite_param(pname, pval, "")
            end)
            for _, note in ipairs(notes) do
                if note:find("^UNSUPPORTED") then
                    report.unsupported[#report.unsupported + 1] = {
                        line = line, cmd = tok.cmd, note = note,
                    }
                elseif note:find("^NOTE") then
                    report.conflicts[#report.conflicts + 1] = {
                        line = line, cmd = tok.cmd, note = note,
                    }
                end
            end
            -- Toggle macro-body tracking so text tokens between [macro] and
            -- [endmacro] get the KAG3 positional/arg reference conversion.
            if new_cmd == "macro" then inMacro = true
            elseif new_cmd == "endmacro" then inMacro = false end
            pieces[#pieces + 1] = out
        else
            pieces[#pieces + 1] = raw_tok
        end
        pos = en + 1
    end
    if pos <= #raw then
        pieces[#pieces + 1] = raw:sub(pos)
    end

    report.output = bom .. table.concat(pieces)
    return report
end

--- M.processScene(path, opts) -> report table
function M.processScene(path, _opts)
    local f <close> = io.open(path, "r")
    if not f then
        return nil, "cannot open file: " .. tostring(path)
    end
    local raw, err = f:read("*a")
    if not raw then return nil, err end
    return processSceneText(path, raw)
end

--- M.printReport(report, out) -> void
function M.printReport(report, out)
    out = out or io.stdout
    out:write(string.format("=== %s ===\n", report.path))
    out:write(string.format("tokens: %d (%d text, %d labels, %d iscript)\n",
        report.tokens, report.texts, report.labels, report.iscripts))
    local converted = report.converted_embeds + report.converted_exprs
        + report.renames
    if converted > 0 then
        out:write(string.format("converted: %d (&var embeds %d, expressions %d, renames %d)\n",
            converted, report.converted_embeds, report.converted_exprs,
            report.renames))
    end
    if #report.unsupported > 0 then
        out:write(string.format("unsupported: %d\n", #report.unsupported))
        for _, u in ipairs(report.unsupported) do
            out:write(string.format("  line %d: %s\n", u.line, u.note))
        end
    end
    if report.conflicts and #report.conflicts > 0 then
        out:write(string.format("conflicts (advisory, pass-through KNOWN): %d\n",
            #report.conflicts))
        for _, c in ipairs(report.conflicts) do
            out:write(string.format("  line %d: %s\n", c.line, c.note))
        end
    end
    if #report.iscript_blocks > 0 then
        out:write(string.format("iscript blocks: %d -- TJS code needs manual porting to Lua\n",
            #report.iscript_blocks))
        for _, ln in ipairs(report.iscript_blocks) do
            out:write(string.format("  line %d: [iscript] ... [endscript]\n", ln))
        end
    end
end

--- M.hasBlocking(report) -> bool. --strict blocking items.
function M.hasBlocking(report)
    return #report.unsupported > 0 or #report.iscript_blocks > 0
end

-- ---------------------------------------------------------------------------
-- CLI
-- ---------------------------------------------------------------------------
-- Exact basename guard: "test_kag3_import.lua" embeds kag3_import.lua as
-- a suffix and must NOT trigger the CLI (it would os.exit the test run).
local function base_name(p)
    return p:match("([^/\\]+)$")
end
local is_script = arg and arg[0] and base_name(arg[0]) == "kag3_import.lua"

local function readCarcScene(archive, member)
    local sep = package.config:sub(1, 1)
    local suffix = sep == BS and ".exe" or ""
    local exe
    for _, directory in ipairs({"bin/Debug/", "bin/Release/", "bin/"}) do
        local path = directory .. "carc_pack" .. suffix
        local file <close> = io.open(path, "rb")
        if file then
            exe = path:gsub("/", sep)
            break
        end
    end
    if not exe then return nil, "carc_pack executable not found" end

    local function hex(value)
        return (value:gsub(".", function(byte) return ("%02x"):format(byte:byte()) end))
    end
    local pipe, openError = io.popen(exe .. " read-member-hex " .. hex(archive) .. " " .. hex(member), "r")
    if not pipe then return nil, openError end
    local raw, readError = pipe:read("*a")
    local ok = pipe:close()
    if not ok then return nil, "carc_pack could not read the archive member" end
    return raw, readError
end

if is_script then
    local outdir, strict, inputs = nil, false, {}
    local carcPath, carcScene = nil, nil
    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "-o" then
            i = i + 1
            outdir = arg[i]
        elseif a == "--strict" then
            strict = true
        elseif a == "--carc" then
            i = i + 1
            carcPath = arg[i]
        elseif a == "--path" then
            i = i + 1
            carcScene = arg[i]
        elseif a == "-h" or a == "--help" then
            print("Usage: lua scripts/kag3_import.lua [-o <outdir>] [--strict] <scene.ks> [more.ks ...]")
            print("       lua scripts/kag3_import.lua --carc <archive.carc> --path <rel.ks> [-o <outdir>] [--strict]")
            print("  check mode:  reports conversions and unsupported commands (exit 0/1/2)")
            print("  convert mode (-o): writes imported .ks files into <outdir>")
            print("  --strict: exit code 2 if any unsupported command or iscript block exists")
            print("  --carc + --path: extract one scene from a CARC archive, then import it")
            os.exit(0)
        else
            inputs[#inputs + 1] = a
        end
        i = i + 1
    end

    local carcText
    if carcPath and carcScene then
        local err
        if carcPath == "" or carcScene == "" then
            err = "--carc and --path require nonempty arguments"
        else
            carcText, err = readCarcScene(carcPath, carcScene)
        end
        if not carcText then
            print("error: " .. tostring(err))
            os.exit(1)
        end
        inputs = { carcScene }
    elseif carcPath or carcScene then
        print("error: --carc and --path must be used together")
        os.exit(1)
    end

    if #inputs == 0 then
        print("error: no input files")
        print("Usage: lua scripts/kag3_import.lua [-o <outdir>] [--strict] <scene.ks> [more.ks ...]")
        os.exit(1)
    end

    local errs, blocking = 0, false
    for _, path in ipairs(inputs) do
        local report, err
        if carcText then
            report, err = processSceneText(path, carcText)
        else
            report, err = M.processScene(path)
        end
        if not report then
            print("error: " .. tostring(err))
            errs = errs + 1
        else
            M.printReport(report)
            if strict and M.hasBlocking(report) then blocking = true end
            if outdir then
                local base = path:gsub(".*[/" .. BS .. "]", "")
                local outpath = outdir .. "/" .. base:gsub("%.ks$", "") .. ".imported.ks"
                local out = io.open(outpath, "w")
                if out then
                    out:write(report.output)
                    out:close()
                    print("  wrote " .. outpath)
                else
                    print("  error: cannot write " .. outpath)
                    errs = errs + 1
                end
            end
        end
    end
    if errs > 0 then os.exit(1) end
    if blocking then os.exit(2) end
    os.exit(0)
end

return M
