-- =============================================================================
--  Caesura (AmeKAG) — kag/compiler.lua
--  KAG Neo-Genesis compile-time front-end (Phase A of the core rewrite).
--
--  Turns a token stream (tokenizer.parse output, either record format
--  {type=,cmd=,params=} or array format {cmd, params}) into a COMPILED
--  instruction stream: the same array plus a `_compiled` side table with
--  everything the scheduler would otherwise recompute per token at
--  runtime:
--
--    _compiled.flow     [i] = { kind, jump|target, ... }  -- O(1) jumps,
--                          no runtime skip_to scans
--    _compiled.exprs    [i] = translated Lua source        -- TJS->Lua done
--                          once at compile time (load still happens at
--                          runtime with the ctx env, cached there)
--    _compiled.params   [i] = normalized param table       -- {{k,v}} pairs
--                          resolved to {k=v} once
--    _compiled.handlers [i] = handler function reference   -- kag[cmd] lookup
--                          resolved once
--    _compiled.labels   name -> token index                -- label index
--
--  The scheduler runs compiled streams via the fast path (checks
--  tokens._compiled); hand-built token arrays (tests, macro splices)
--  still run the legacy path unchanged. `compile` is a pure function of
--  the token stream -- no ctx, no I/O, deterministic.
-- =============================================================================

local compiler = {}

-- Forward declarations for the .ksc Lua-literal codec (defined below;
-- the cache functions reference these locals, assigned before any
-- writeCache/readCache call runs).
local encode_lua_literal
local capture_compatibility
local CACHE_FORMAT = 2
local COMPILER_SEMANTICS = 'caesura-kag-2'

local schemaModule = require("kag.schema")
local exprLang = require("kag.expr")

-- Flow commands the scheduler handles inline (mirror of scheduler.lua's
-- flow_commands table -- kept here so the compiler can classify tokens).
local FLOW = {
    ["if"] = true, ["else"] = true, ["elseif"] = true, ["endif"] = true,
    ["switch"] = true, ["case"] = true, ["default"] = true,
    ["endswitch"] = true,
    ["jump"] = true, ["call"] = true, ["return"] = true,
    ["link"] = true, ["end"] = true,
    ["label"] = true,
    ["macro"] = true, ["endmacro"] = true, ["erasemacro"] = true,
    ["eval"] = true, ["emb"] = true,
    ["iscript"] = true,
    ["while"] = true, ["endwhile"] = true,
    ["for"] = true, ["endfor"] = true,
    ["until"] = true,
    ["break"] = true, ["continue"] = true,
    ["stop"] = true,
}

-- Tokens whose `exp`/`expr`/`cond`/`start`/`end`/`step` params carry
-- expressions that get compiled once (TJS->Lua translation).
local EXPR_TOKENS = {
    ["if"] = "exp", ["elseif"] = "exp", ["while"] = "exp",
    ["until"] = "exp",  -- declarative conditional wait (Neo-Genesis)
    ["button"] = "cond",  -- conditional choices: AOT at compile time
    ["for"] = nil,  -- handled specially below (three numeric exprs)
    ["switch"] = "exp",  -- optional [switch exp="..."] selector (round 55);
                        -- the KAG3 positional form stays a bare variable name
}

--- Normalize one token to array format {cmd, params} (params = {} when
--  absent). Accepts both record and array input formats.
local function to_array_tok(tok)
    if type(tok) ~= "table" then return nil end
    if tok.type then
        local cmd = tok.cmd or tok.type
        if tok.type == "label" then
            -- Semantic AST labels retain the source '*' for tooling; the
            -- scheduler resolves unprefixed names, as emitted by tokenizer.
            local name = tok.name
            if type(name) == "string" then name = name:gsub("^%*", "") end
            return { "label", { name = name } }
        elseif tok.type == "text" or tok.type == "blocktext" then
            return { "ch", { text = tok.text or tok.content or "" } }
        elseif tok.type == "iscript" then
            return { "iscript", { body = tok.body or "" } }
        end
        return { cmd, tok.params or {} }
    end
    return { tok[1], tok[2] or {} }
end

--- Normalize raw param pairs {{key,val},...} into {key=val} (bare
--  positional args keep numeric keys 1..N, matching scheduler semantics).
--  Hand-built streams may already use named tables ({text="a"}) -- those
--  have no pair array part and pass through unchanged (identity, so
--  handler mutation semantics match the legacy runtime path exactly).
--  When the command's contract declares positional_index for a param,
--  bare args are ALSO mapped to the declared name (compile-time
--  positional -> named resolution; the numeric key stays for legacy
--  handlers that still read params[1]).
local function normalize_params(cmd, raw)
    if type(raw) ~= "table" then return {} end
    local out = {}
    local has_pairs = false
    local bare = {}
    for _, p in ipairs(raw) do
        if type(p) == "table" and type(p[1]) == "string" then
            has_pairs = true
            local key = tonumber(p[1]) or p[1]
            local is_dotted = type(key) == "string" and key:find("%.")
            if not is_dotted then
                out[key] = p[2]
                if type(key) == "number" then bare[key] = p[2] end
            else
                -- Dotted key pair { "f.name", "Aoi" } (tokenizer's
                -- ident(.ident)* = value branch, KAG3 [set f.x = v]):
                -- the dotted key is a VARIABLE path, not a named param --
                -- expand it into positional slots so the set contract maps
                -- var = "f.name", value = "Aoi" via positional_index.
                -- (round 50 audit: [set f.name = "Aoi"] stored the quoted
                -- literal because the bare "=" fallback kept quotes; this
                -- path also strips them via qval.)
                local idx = #bare + 1
                bare[idx] = key
                bare[idx + 1] = p[2]
            end
        end
    end
    if not has_pairs then return raw end  -- named table: keep identity
    -- KAG3 assignment sugar: [set f.x = 5] tokenizes as bare
    -- {1,"f.x"},{2,"="},{3,"5"}. The standalone "=" separator is NOT a
    -- value (a literal "=" value must be quoted, [set f.s "="], which
    -- parses as qval and never lands here). Drop it and shift the
    -- following positional args down so positional_index mapping sees
    -- var="f.x", value="5" (audit: showcase's [set f.luck = math.random(2)]
    -- had stored the literal "=" and its branch never varied).
    if has_pairs then
        local shifted = {}
        local n = 0
        for i = 1, math.max(1, #bare) do
            local v = bare[i]
            if v == "=" and bare[i + 1] ~= nil then
                -- separator: skip it; the next positional shifts down
            else
                n = n + 1
                shifted[n] = v
            end
        end
        if n > 0 and (n ~= #bare or shifted[1] ~= bare[1]) then
            bare = shifted
        end
    end
    -- Compile-time positional -> named mapping via the contract's
    -- positional_index declarations (e.g. [set f.hp 30] -> var="f.hp").
    local specs = schemaModule.specs(cmd)
    if specs then
        for name, spec in pairs(specs) do
            if spec.positional_index and bare[spec.positional_index] ~= nil
                and out[name] == nil then
                out[name] = bare[spec.positional_index]
            end
        end
    end
    return out
end

-- The semantic frontend uses the same parameter contract as execution,
-- including numeric positions, dotted assignments and named precedence.
compiler.normalize_params = normalize_params

--- Compile-time expression translation (TJS->Lua) for one param value.
--  Returns the translated source string (or the original when translation
--  is not applicable). Runtime still loads it with the ctx env; the
--  scheduler's expr cache keys on translated source so the load is
--  amortized. Compile time removes the per-token translate() cost.
local function compile_expr_param(tok_cmd, params)
    if tok_cmd == "for" then
        for _, pname in ipairs({ "start", "end", "step" }) do
            local v = params[pname]
            if type(v) == "string" then
                params[pname] = exprLang.translate(v)
            end
        end
        return
    end
    local pname = EXPR_TOKENS[tok_cmd]
    if pname and type(params[pname]) == "string" then
        params[pname] = exprLang.translate(params[pname])
    end
end

--- Build the label index (first definition wins, KAG3 semantics).
local function build_labels(tokens)
    local labels = {}
    for i, tok in ipairs(tokens) do
        if tok[1] == "label" and type(tok[2]) == "table"
            and type(tok[2].name) == "string" then
            if not labels[tok[2].name] then labels[tok[2].name] = i end
        end
    end
    return labels
end

-- ---------------------------------------------------------------------------
-- Flow pre-scan: compute O(1) jump targets for every flow token.
-- Semantics must EXACTLY match the scheduler's runtime skip_to scans
-- (see scheduler.lua): depth-aware nesting, closers vs branch markers.
-- ---------------------------------------------------------------------------

-- Skip target: index of the token AFTER the matching closer (i.e. where
-- execution continues). Returns #tokens+1 when the chain is unterminated.
local function scan_skip(tokens, start, targets, opens, closers)
    closers = closers or targets
    local depth = 1
    for i = start + 1, #tokens do
        local cmd = tokens[i][1]
        if targets[cmd] then
            if depth == 1 then return i end
            if closers[cmd] then depth = depth - 1 end
        elseif opens and opens[cmd] then
            depth = depth + 1
        end
    end
    return #tokens + 1
end

local IF_CHAIN = { ["elseif"] = true, ["else"] = true, ["endif"] = true }
local IF_CLOSER = { ["endif"] = true }
local IF_OPENS = { ["if"] = true }
local WHILE_SET = { ["endwhile"] = true }
local WHILE_OPENS = { ["while"] = true }
local FOR_SET = { ["endfor"] = true }
local FOR_OPENS = { ["for"] = true }
local LOOP_SET = { ["endwhile"] = true, ["endfor"] = true }
local LOOP_OPENS = { ["while"] = true, ["for"] = true }
local SWITCH_SET = { ["case"] = true, ["default"] = true, ["endswitch"] = true }
local SWITCH_CLOSER = { ["endswitch"] = true }
local SWITCH_OPENS = { ["switch"] = true }
local ENDSWITCH_SET = { ["endswitch"] = true }
local ENDSWITCH_OPENS = { ["switch"] = true }

--- Compile the flow metadata for a token array. Returns:
--   flow[i] = {
--     kind = "if"|"elseif"|"else"|"endif"|"while"|"endwhile"|"for"|
--            "endfor"|"break"|"continue"|"switch"|"case"|"default"|
--            "endswitch"|"jump"|"call"|"return"|"link"|"label"|"end",
--     ...kind-specific fields (jump targets as token indexes)
--   }
local function compile_flow(tokens)
    local flow = {}
    local n = #tokens

    for i = 1, n do
        local cmd = tokens[i][1]
        if cmd == "if" then
            -- false branch: jump to first elseif/else/endif (not consumed)
            local t = scan_skip(tokens, i, IF_CHAIN, IF_OPENS, IF_CLOSER)
            flow[i] = { kind = "if", jump_false = t }
        elseif cmd == "elseif" or cmd == "else" then
            -- taken branch already handled: skip rest of chain
            local t = scan_skip(tokens, i, IF_CHAIN, IF_OPENS, IF_CLOSER)
            flow[i] = { kind = cmd, jump_chain_end = t }
        elseif cmd == "endif" then
            flow[i] = { kind = "endif" }
        elseif cmd == "while" then
            local t = scan_skip(tokens, i, WHILE_SET, WHILE_OPENS, WHILE_SET)
            flow[i] = { kind = "while", skip_body_to = t }
        elseif cmd == "endwhile" then
            flow[i] = { kind = "endwhile" }
        elseif cmd == "for" then
            local t = scan_skip(tokens, i, FOR_SET, FOR_OPENS, FOR_SET)
            flow[i] = { kind = "for", skip_body_to = t }
        elseif cmd == "endfor" then
            flow[i] = { kind = "endfor" }
        elseif cmd == "break" or cmd == "continue" then
            local t = scan_skip(tokens, i, LOOP_SET, LOOP_OPENS, LOOP_SET)
            flow[i] = { kind = cmd, loop_end = t }
        elseif cmd == "switch" then
            -- Compile-time case table: { [case_value] = body_start_idx }
            -- for depth-1 cases only (a nested switch's cases belong to
            -- IT -- the runtime scan's depth-awareness is compiled away).
            -- default maps to the special key "default".
            local cases, default_idx, end_idx = {}, nil, nil
            local depth = 1
            for j = i + 1, n do
                local scmd = tokens[j][1]
                if scmd == "switch" then
                    depth = depth + 1
                elseif scmd == "endswitch" then
                    if depth == 1 then end_idx = j break end
                    depth = depth - 1
                elseif depth == 1 and scmd == "case" then
                    local caseVal = (tokens[j][2] or {})[1] or ""
                    cases[tostring(caseVal)] = j + 1
                elseif depth == 1 and scmd == "default" then
                    if not default_idx then default_idx = j + 1 end
                end
            end
            flow[i] = {
                kind = "switch",
                cases = cases, default = default_idx,
                endswitch = end_idx or (n + 1),
            }
        elseif cmd == "case" or cmd == "default" then
            -- when a previous case was taken: skip to endswitch
            local t = scan_skip(tokens, i, ENDSWITCH_SET, ENDSWITCH_OPENS,
                                ENDSWITCH_SET)
            flow[i] = { kind = cmd, skip_to_end = t }
        elseif cmd == "endswitch" then
            flow[i] = { kind = "endswitch" }
        elseif cmd == "jump" or cmd == "call" or cmd == "link" then
            -- compile-time label resolution for intra-scene targets
            local params = tokens[i][2] or {}
            local target = params.target or params.label or params.storage
            if type(params[1]) == "string" then target = target or params[1] end
            flow[i] = { kind = cmd, target = target }
        elseif cmd == "return" or cmd == "end" or cmd == "stop" then
            flow[i] = { kind = cmd }
        elseif cmd == "label" then
            flow[i] = { kind = "label" }
        end
    end
    return flow
end

-- ---------------------------------------------------------------------------
--  Static macro inlining (Battle 1d: compile-time expansion, zero runtime
--  splice for statically-safe macros).
--
--  The scheduler still expands macros at runtime (ctx.macros + splice);
--  that path stays for DYNAMIC macros (defined inside flow branches,
--  redefined, erased before the call, or invoked before their definition).
--  Here the compiler additionally inlines macros that are statically
--  safe — definition at flow depth 0, definition before the call site,
--  no [erasemacro] between definition and call, no redefinition — so
--  the runtime never splices for them. Inlined tokens are parameterized
--  (params preserved: %arg% filled from the call site, same semantics as
--  the runtime splice, including numeric %1% -> params[1]).
--
--  Correctness: a static-safe macro is GUARANTEED to be registered with
--  the same body when the runtime reaches the call site (definition
--  executed unconditionally before it, never erased, never redefined), so
--  inlining is behavior-identical to the runtime splice. [macro]/
--  [endmacro] definition blocks stay in the stream (runtime still
--  records them — dynamic calls to the same name keep working).
-- ---------------------------------------------------------------------------

-- Expansion budget (compile-time): mirrors the runtime guard — a
-- self-recursive macro ([macro m][m][endmacro]) cannot be inlined; the
-- budget fails fast instead of growing the token array forever.
local MACRO_INLINE_BUDGET = 1000

local function macro_cmd(tok)
    if type(tok) ~= "table" then return nil end
    if tok.type then return tok.cmd or tok.type end
    return tok[1]
end

-- Collect static macro definitions. Returns:
--   defs:   name -> { idx = defIndex, args = {names...}, body = {tokens...} }
--   erased: name -> true when ANY [erasemacro name] exists (conservative:
--           an erase anywhere makes the macro dynamic — the runtime may
--           or may not have the macro at the call site)
--   redef:  name -> true when the macro is defined more than once (the
--           runtime keeps the LAST body; inlining the first would be a
--           behavior change)
local function collect_macro_defs(tokens)
    local defs, erased, redef = {}, {}, {}
    local depth = 0
    local i = 1
    while i <= #tokens do
        local t = tokens[i]
        local cmd = macro_cmd(t)
        if cmd == "macro" then
            local at = to_array_tok(t)
            local params = at[2] or {}
            local name = params.name
            if type(name) ~= "string" and type(params[1]) == "string" then
                name = params[1]
            end
            local args = {}
            if type(params.args) == "string" then
                for raw_a in params.args:gmatch("[^,]+") do
                    local a = raw_a:match("^%s*(.-)%s*$")
                    if #a > 0 then args[#args + 1] = a end
                end
            end
            -- body: tokens up to the [endmacro] matching this [macro]
            -- (converted to array form). Depth-aware (round 75): a nested
            -- [macro inner]...[endmacro] pair belongs to the OUTER body;
            -- the naive scan stopped at the first [endmacro] and
            -- corrupted the stream. Any nested definition inside the
            -- body also makes the outer macro DYNAMIC (the runtime
            -- executes [macro] tokens and may redefine names at call
            -- time), so it is conservatively excluded from inlining.
            local body = {}
            local j = i + 1
            local bdepth = 1
            while j <= #tokens do
                local bcmd = macro_cmd(tokens[j])
                if bcmd == "macro" then
                    bdepth = bdepth + 1
                elseif bcmd == "endmacro" then
                    bdepth = bdepth - 1
                    if bdepth == 0 then break end
                end
                local bt = to_array_tok(tokens[j])
                if bt then body[#body + 1] = bt end
                j = j + 1
            end
            if name then
                local hasNested = false
                for _, bt in ipairs(body) do
                    if macro_cmd(bt) == "macro" then hasNested = true break end
                end
                if hasNested then
                    -- nested definition inside the body: runtime may
                    -- redefine macros when the body executes -- dynamic
                    redef[name] = true
                elseif defs[name] then
                    -- redefinition: never inline this name (runtime
                    -- semantics = last definition wins)
                    redef[name] = true
                elseif depth == 0 then
                    defs[name] = { idx = i, args = args, body = body }
                end
            end
            i = j
        elseif cmd == "erasemacro" then
            local at = to_array_tok(t)
            local params = at[2] or {}
            local name = params.name
            if type(name) ~= "string" and type(params[1]) == "string" then
                name = params[1]
            end
            if name then erased[name] = true end
            i = i + 1
        else
            -- flow nesting depth (only macro definitions inside flow
            -- branches are excluded from inlining; erases/calls anywhere)
            if cmd == "if" or cmd == "while" or cmd == "for"
                or cmd == "select" then
                depth = depth + 1
            elseif cmd == "endif" or cmd == "endwhile" or cmd == "endfor"
                or cmd == "endselect" then
                if depth > 0 then depth = depth - 1 end
            end
            i = i + 1
        end
    end
    return defs, erased, redef
end

-- Fill %arg% placeholders in a param value (string or nested table).
-- Missing args keep the literal placeholder (runtime-splice parity).
local function fill_param_value(v, params)
    if type(v) == "string" then
        return (v:gsub("%%([%w_]+)%%", function(an)
            local key = tonumber(an) or an
            local val = params[key]
            if type(val) == "string" then return val end
            return "%" .. an .. "%"
        end))
    elseif type(v) == "table" then
        local out = {}
        for k, vv in pairs(v) do out[k] = fill_param_value(vv, params) end
        return out
    end
    return v
end

-- Expand one macro call site into its inlined body tokens. Recurses into
-- nested static-safe macro calls inside the body (deep-copied params).
-- Returns nil when the macro is not statically safe at this call site
-- (caller keeps the runtime splice path) or the budget is exceeded.
local function expand_macro_call(defs, erased, redef, name, callParams, budget)
    if budget <= 0 then return nil end
    budget = budget - 1
    local def = defs[name]
    if not def or erased[name] or redef[name] then return nil end
    local out = {}
    for _, bt in ipairs(def.body) do
        local bcmd = bt[1]
        -- nested static-safe macro: inline recursively (same rules)
        local nested = defs[bcmd] and not erased[bcmd] and not redef[bcmd]
            and expand_macro_call(defs, erased, redef, bcmd, bt[2] or {}, budget)
        if nested then
            -- The nested expansion may still carry THIS macro's
            -- placeholders (e.g. outer's %who% inside inner's call
            -- params) — fill them with the current call params too.
            -- (Runtime parity: the outer splice fills the whole body,
            -- including the nested call's params, before dispatching.)
            for _, nt in ipairs(nested) do
                out[#out + 1] = { nt[1], fill_param_value(nt[2] or {}, callParams) }
            end
        else
            out[#out + 1] = { bcmd, fill_param_value(bt[2] or {}, callParams) }
        end
    end
    return out
end

--- compiler.inlineStaticMacros(tokens) → tokens (same array, in place).
--  Replaces statically-safe macro call sites with their inlined bodies;
--  all other tokens (dynamic macro calls, definitions, flow) pass through
--  unchanged. Idempotent-safe: inlined bodies contain no macro command
--  tokens for the inlined name (nested recursion resolved), so a second
--  pass is a no-op for already-inlined sites.
--  Fast path: scenes without ANY macro command skip the full scan (the
--  common case — measured 0.14ms vs 0.78ms for a 4000-token stream).
function compiler.inlineStaticMacros(tokens)
    if not tokens or #tokens == 0 then return tokens end
    local hasMacro = false
    for i = 1, #tokens do
        local t = tokens[i]
        local cmd = type(t) == "table" and (t.type and (t.cmd or t.type) or t[1])
        if cmd == "macro" or cmd == "endmacro" then
            hasMacro = true
            break
        end
    end
    if not hasMacro then return tokens end
    local defs, erased, redef = collect_macro_defs(tokens)
    if not next(defs) then return tokens end
    local out = {}
    local budget = MACRO_INLINE_BUDGET
    local i = 1
    while i <= #tokens do
        local t = tokens[i]
        local cmd = macro_cmd(t)
        if defs[cmd] then
            local at = to_array_tok(t)
            local callParams = at[2] or {}
            local def = defs[cmd]
            -- static-safe: definition BEFORE this call site; no erase
            -- anywhere; no redefinition (runtime keeps the last body)
            local safe = def.idx < i and not erased[cmd] and not redef[cmd]
            local expanded = safe and expand_macro_call(defs, erased, redef,
                cmd, callParams, budget)
            if expanded then
                budget = budget - 1
                for _, nt in ipairs(expanded) do out[#out + 1] = nt end
                i = i + 1
            else
                out[#out + 1] = t
                i = i + 1
            end
        else
            out[#out + 1] = t
            i = i + 1
        end
    end
    for n = 1, #out do tokens[n] = out[n] end
    for n = #out + 1, #tokens do tokens[n] = nil end
    return tokens
end

--- compiler.compile(tokens) → tokens (same array) with tokens._compiled set.
--  Pure, deterministic, idempotent (recompiling a compiled stream is a no-op
--  when the side table is already present and complete).
function compiler.compile(tokens)
    if not tokens then return tokens end
    -- Support direct compilation from a Unified Semantic AST model
    if type(tokens) == "table" and tokens.nodes ~= nil and type(tokens.nodes) == "table" then
        tokens = tokens.nodes
    end
    if #tokens == 0 then return tokens end
    if tokens._compiled then return tokens end
    -- Compile against the initialized runtime contract even for a stream that
    -- contains only inline flow commands. Loading handlers later must not make
    -- this compiler's own cold output incompatible or change positional rules.
    local kag = require('kag')

    -- 1) Normalize to array format + keyed params FIRST: tokenizer.parse
    -- emits raw pair-array params ({{key,val},...}); macro definition
    -- collection and call-site expansion read keyed params (name/args).
    -- Inlining after normalization guarantees statically-safe macros are
    -- expanded for real .ks scenes, not just hand-built streams (the
    -- stream may grow here; binding below uses final indices).
    local norm = {}
    for i, tok in ipairs(tokens) do
        local at = to_array_tok(tok)
        if not at then
            norm[i] = tok  -- passthrough (unknown shape)
        else
            at[2] = normalize_params(at[1], at[2])
            norm[i] = at
        end
    end

    -- 2) Static macro inlining (Battle 1d) on the normalized stream:
    -- statically-safe macro call sites are expanded at compile time
    -- (zero runtime splice).
    -- A runtime splice owns live instruction/control/macro indices. Rebuild
    -- its metadata without silently inlining newly exposed nested calls:
    -- those must go through the scheduler's index-adjusting splice path.
    if not tokens._runtime_rewritten then compiler.inlineStaticMacros(norm) end

    -- 3) Handler binding + expression precompile on the final stream.
    local handlers = {}
    local params_by_idx = {}
    local exprs = {}
    local exprDumps = {}  -- Battle 1c: AOT bytecode per expression token
    for i, at in ipairs(norm) do
        if type(at) == "table" and at[1] and not at.type then
            local cmd = at[1]
            local p = at[2] or {}
            params_by_idx[i] = p
            if FLOW[cmd] or EXPR_TOKENS[cmd] then
                -- flow tokens + non-flow expression commands (e.g.
                -- [button cond]): params already normalized in pass 1
                compile_expr_param(cmd, p)
                if cmd == "if" or cmd == "elseif" or cmd == "while"
                    or cmd == "for" or cmd == "until" or cmd == "switch" then
                    -- keep the translated source for the scheduler (it
                    -- loads with the ctx env at runtime, cached there)
                    if cmd == "for" then
                        -- [for] carries THREE expressions (start/end/step);
                        -- each gets its own AOT dump — sharing one would
                        -- evaluate the wrong bytecode for the other two.
                        local dumps = {}
                        for _, pn in ipairs({ "start", "end", "step" }) do
                            local v = p[pn]
                            if type(v) == "string" then
                                exprs[i] = exprs[i] or p.start
                                local okC, chunk = pcall(load,
                                    "return " .. v, "=kag_expr", "t", {})
                                if okC and chunk then
                                    local okD, dumped =
                                        pcall(string.dump, chunk, true)
                                    if okD and dumped then
                                        dumps[pn] = dumped
                                    end
                                end
                            end
                        end
                        if next(dumps) then exprDumps[i] = dumps end
                    else
                        local pname = EXPR_TOKENS[cmd]
                        if pname and type(p[pname]) == "string" then
                            exprs[i] = p[pname]
                            -- Battle 1c: precompile the expression to
                            -- bytecode (string.dump). Runtime
                            -- evaluateTranslated loads it with mode "b" —
                            -- skips Lua's lexer+parser (~6x faster than
                            -- source load, measured). Any failure keeps
                            -- the source fallback (no dump).
                            local okC, chunk = pcall(load,
                                "return " .. p[pname], "=kag_expr", "t", {})
                            if okC and chunk then
                                local okD, dumped =
                                    pcall(string.dump, chunk, true)
                                if okD and dumped then
                                    exprDumps[i] = dumped
                                end
                            end
                        end
                    end
                end
            else
                -- regular command: bind the handler once
                local handler = kag[cmd]
                if handler then handlers[i] = handler end
            end
        end
    end
    for i = 1, #norm do tokens[i] = norm[i] end

    -- 2) Flow pre-scan.
    local flow = compile_flow(tokens)

    -- 3) Label index (compile-time; scheduler no longer rescans).
    local labels = build_labels(tokens)

    tokens._compiled = {
        flow = flow,
        exprs = exprs,
        exprDumps = exprDumps,  -- Battle 1c: AOT bytecode (session-only)
        params = params_by_idx,
        handlers = handlers,
        labels = labels,
        compatibility = capture_compatibility(tokens),
    }
    return tokens
end

--- compiler.isCompiled(tokens) → boolean
function compiler.isCompiled(tokens)
    return type(tokens) == "table" and tokens._compiled ~= nil
end

--- compiler.invalidate(tokens) — drop the side table (after a macro
--  splice mutates the stream; the stream must be recompiled).
function compiler.invalidate(tokens)
    if type(tokens) == "table" then tokens._compiled = nil end
end

-- ---------------------------------------------------------------------------
--  Bytecode persistence (Battle 1b: compile once, reuse many).
--  The _compiled side table splits into serializable data (flow jump
--  table, pre-translated expressions, normalized params, label index)
--  and runtime bindings (handler function refs). Only the data part is
--  serialized to a .ksc cache file; handlers rebind on load.
-- ---------------------------------------------------------------------------

-- Values that survive serialization: nil/boolean/number/string plus
-- plain tables (arrays or string/number-keyed maps). Functions, threads
-- and userdata are dropped with a warning. `skip_key` names a field to
-- ignore (the token array's `_compiled` side table is the serialization
-- SOURCE, not part of the payload).
local function is_serializable(v, seen, skip_key)
    local t = type(v)
    if t=='number' then return v==v and v~=math.huge and v~=-math.huge end
    if t == "nil" or t == "boolean" or t == "string" then
        return true
    end
    if t ~= "table" then return false end
    if seen[v] then return false end  -- cycle guard
    seen[v] = true
    for k, val in pairs(v) do
        if k == skip_key then
            -- skip (e.g. tokens._compiled)
        else
            local kt = type(k)
            if kt ~= "string" and kt ~= "number" then return false end
            if not is_serializable(val, seen, skip_key) then return false end
        end
    end
    return true
end

-- Deep copy the token array WITHOUT the `_compiled` side table: the
-- cache payload must not carry function refs (handlers) or the compiled
-- tables that are stored separately (flow/exprs/params/labels). Shared
-- params tables (tokens[i][2] === c.params[i]) are copied once per token.
-- Compare canonical contract strings, so correctness does not depend on a
-- short fingerprint collision. Unrelated commands and _meta are excluded.
local function contract_identity(command)
    local specs=schemaModule.specs(command)
    if specs==nil then return 'absent' end
    if not is_serializable(specs,{}) then return nil end
    return encode_lua_literal(specs)
end

capture_compatibility=function(tokens)
    local commands={}
    for _,token in ipairs(tokens) do
        local command=type(token)=='table' and token[1]
        if type(command)~='string' then return nil end
        if commands[command]==nil then
            commands[command]=contract_identity(command)
            if commands[command]==nil then return nil end
        end
    end
    return {format=CACHE_FORMAT,semantics=COMPILER_SEMANTICS,commands=commands}
end

local function compatible(tokens,identity)
    if type(identity)~='table' or identity.format~=CACHE_FORMAT then return false,'cache-format-mismatch' end
    if identity.semantics~=COMPILER_SEMANTICS then return false,'compiler-semantics-mismatch' end
    if type(identity.commands)~='table' then return false,'command-contracts-missing' end
    require('kag') -- deserialize/readCache are also public cold-start entries
    local current=capture_compatibility(tokens)
    if not current then return false,'command-contracts-unserializable' end
    for command,value in pairs(current.commands) do
        if identity.commands[command]~=value then return false,'command-contract-mismatch:'..command end
    end
    for command in pairs(identity.commands) do
        if current.commands[command]==nil then return false,'command-contract-extra:'..tostring(command) end
    end
    return true
end

function compiler.isCompatible(tokens)
    if type(tokens)~='table' or type(tokens._compiled)~='table' then return false,'uncompiled-stream' end
    return compatible(tokens,tokens._compiled.compatibility)
end

function compiler.validateSerialized(data)
    if type(data)~='table' or data.version~=CACHE_FORMAT then return false,'cache-format-mismatch' end
    if type(data.tokens)~='table' or type(data.flow)~='table' or type(data.labels)~='table'
        or type(data.params)~='table' or type(data.exprs)~='table' then return false,'compiled-stream-shape' end
    return compatible(data.tokens,data.compatibility)
end

-- Validate all bundled scenes before replacing a live provider/session.
-- Compatibility is separate from publisher authenticity.
function compiler.validateBundle(bundle,expected_scenes)
    if type(bundle)~='table' or bundle.version~=1 or type(bundle.scenes)~='table'
        or next(bundle.scenes)==nil then return false,'bundle-format-mismatch' end
    for name,data in pairs(bundle.scenes) do
        if type(name)~='string' or name=='' then return false,'bundle-scene-key' end
        local ok,reason=compiler.validateSerialized(data)
        if not ok then return false,'incompatible-scene:'..name..':'..reason end
    end
    if expected_scenes~=nil then
        if type(expected_scenes)~='table' then return false,'bundle-required-scenes' end
        local expected={}
        for _,name in ipairs(expected_scenes) do
            if expected[name] then return false,'duplicate-bundle-scene:'..tostring(name) end
            if bundle.scenes[name]==nil then return false,'missing-bundle-scene:'..tostring(name) end
            expected[name]=true
        end
        for name in pairs(bundle.scenes) do
            if not expected[name] then return false,'unexpected-bundle-scene:'..tostring(name) end
        end
    end
    return true
end

local function strip_compiled(tokens)
    local out = {}
    for i, tok in ipairs(tokens) do
        if type(tok) == "table" then
            local copy = { tok[1] }
            local p = tok[2]
            if type(p) == "table" then
                local pc = {}
                for k, v in pairs(p) do pc[k] = v end
                copy[2] = pc
            else
                copy[2] = p
            end
            out[i] = copy
        else
            out[i] = tok
        end
    end
    return out
end

--- compiler.serialize(tokens) → data table (serializable part of the
--  compiled stream) or nil when the stream is not compiled.
--  The returned table contains: version, tokens (normalized array),
--  flow, exprs, params, labels. handlers are NOT included (rebound on
--  load). Exprs hold pre-translated source strings (loaded+cached at
--  runtime), so they survive serialization.
function compiler.serialize(tokens)
    if not tokens or not tokens._compiled then return nil end
    local current,reason=compiler.isCompatible(tokens)
    if not current then return nil,reason end
    local c = tokens._compiled
    -- independent seen tables per sub-check: the same params table is
    -- referenced both from tokens[i][2] and c.params[i], so a shared
    -- cycle guard would falsely flag it as a cycle
    local function ok_table(v)
        return is_serializable(v, {}, "_compiled")
    end
    if not ok_table(c.flow) or not ok_table(c.exprs)
        or not ok_table(c.params) or not ok_table(c.labels)
        or not ok_table(tokens) then
        print("[compiler] serialize: non-serializable value in compiled stream")
        return nil
    end
    return {
        version = CACHE_FORMAT,
        compatibility = c.compatibility,
        tokens = strip_compiled(tokens),
        flow = c.flow,
        exprs = c.exprs,
        params = c.params,
        labels = c.labels,
        _srcHash = tokens._srcHash or c._srcHash,
    }
end

--- compiler.deserialize(data) → token array with a restored _compiled
--  side table (handlers rebound lazily on first scheduler use). Returns
--  nil when the data version mismatches or the shape is invalid.
--  JSON round-trips object keys as strings; the decoder converts
--  numeric index keys back directly (flow jump table, params/exprs), so
--  compiled.flow[i] lookups stay O(1) with no post-decode deep pass.
local function copy_serialized(value, copies)
    if type(value) ~= "table" then return value end
    if copies[value] then return copies[value] end
    local result = {}
    copies[value] = result
    for key, child in pairs(value) do result[key] = copy_serialized(child, copies) end
    return result
end

function compiler.deserialize(data)
    local current,reason=compiler.validateSerialized(data)
    if not current then return nil,reason end
    -- A bundle is trusted input, never the live scheduler's mutable storage.
    -- Preserve sharing within this decoded graph without aliasing another run.
    data = copy_serialized(data, {})
    local tokens = data.tokens
    -- handlers: not serialized — leave nil; the scheduler falls back to
    -- the kag[cmd] lookup (compiled.handlers[i] or kag[cmd]).
    -- The JSON decoder already converted numeric index keys (flow jump
    -- table, params/exprs) so compiled.flow[i] lookups stay O(1).
    -- Restore the params sharing invariant: the scheduler reads tok[2]
    -- directly, and the compiled params table is the authoritative
    -- normalized copy (compile() sets at[2] = normalized params).
    local params = data.params or {}
    for i, tok in ipairs(tokens) do
        if type(tok) == "table" and params[i] ~= nil then
            tok[2] = params[i]
        end
    end
    tokens._compiled = {
        flow = data.flow or {},
        exprs = data.exprs or {},
        params = params,
        handlers = {},
        labels = data.labels or {},
        _srcHash = data._srcHash,
        compatibility = data.compatibility,
    }
    return tokens
end

--- compiler.hashFile(path) → FNV-1a 32-bit content hash (hex string) or
--  nil on read failure. Used for .ksc freshness checks. 32-bit is exact
--  in Lua doubles. This is a non-cryptographic freshness marker, not trust
--  evidence; format and compiler/contract identity are checked separately.
--- FNV-1a 32-bit content hash. Recalculated on every call: the scenes
--  are small (<100KB) and correctness of the .ksc freshness check beats
--  the ~1ms saving a cache would buy (a stale-cache bug silently loads
--  old bytecode — not acceptable). readCache compares full encoded content
--  before reusing parsed data, and never shares live token arrays.
function compiler.hashFile(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local hash = 2166136261  -- FNV offset basis (32-bit)
    while true do
        local chunk = f:read(8192)
        if not chunk then break end
        for i = 1, #chunk do
            hash = (hash ~ chunk:byte(i)) * 16777619
            hash = hash % 4294967296
        end
    end
    f:close()
    return string.format("%08x", hash)
end

-- Cache immutable parsed data using exact text. External rebakes can preserve
-- size and prefix. Every read returns a detached runtime graph.
--  Declared BEFORE writeCache: writeCache invalidates the entry after a
--  rewrite, and the local must already be assigned at call time (a
--  later `local` would shadow with nil until this line executes).
local read_cache = {}
local READ_CACHE_MAX = 64

--- compiler.writeCache(tokens, cachePath) — persist the compiled stream
--  to a .ksc file. Returns true on success, false on failure (cache is
--  an optimization; failure must never break scene loading).
--  The cache format is a Lua chunk (`return { ... }`) — Lua's own parser
--  loads table literals far faster than a hand-rolled JSON decoder.
function compiler.writeCache(tokens, cachePath)
    local data = compiler.serialize(tokens)
    if not data then return false end
    local ok, text = pcall(encode_lua_literal, data)
    if not ok or type(text) ~= "string" then return false end
    text = "return " .. text
    -- ensure the cache directory exists (best-effort). Windows cmd's
    -- mkdir has NO -p option -- `mkdir -p "<dir>"` happily creates a
    -- literal "-p" DIRECTORY in the CWD on every compile (observed residue
    -- build/tests/Debug/-p; dev runs would litter the repo root too). Same
    -- cross-platform pattern as scripts/system.lua:504 and
    -- music_room.lua:129-134: check existence first and never pass -p on
    -- Windows (cmd's mkdir already creates intermediate components); keep
    -- `mkdir -p` on POSIX. Sandboxed envs may disable os.execute —
    -- tolerate that (the dir may already exist).
    local dir = cachePath:match("^(.*)[/\\][^/\\]+$")
    if dir then
        local sep = package.config:sub(1, 1)
        if sep == "\\" then
            local d = dir:gsub("[/\\]+$", "")
            pcall(os.execute, 'if not exist "' .. d .. '" mkdir "' .. d .. '"')
        else
            local q = "'"
            pcall(os.execute, "mkdir -p " .. q .. dir .. q .. " 2>/dev/null")
        end
    end
    local ok3, f = pcall(io.open, cachePath, "w")
    if not ok3 or not f then return false end
    local wrote,result=pcall(f.write,f,text)
    local closed,close_result=pcall(f.close,f)
    -- A failed/partial write must not preserve the old parsed cache entry.
    read_cache[cachePath] = nil
    return wrote and result~=nil and closed and close_result~=nil
end

function compiler.readCache(cachePath)
    local opened,f = pcall(io.open,cachePath, "rb")
    if not opened then return nil end
    if not f then return nil end
    local read,text=pcall(f.read,f,"*a")
    pcall(f.close,f)
    if not read or type(text)~='string' or #text==0 then return nil end
    local cached = read_cache[cachePath]
    if cached and cached.text==text then
        return compiler.deserialize(cached.data)
    end
    local chunk, err = load(text, "=ksc", "t", {})
    if not chunk then return nil end
    local ok2, data = pcall(chunk)
    if not ok2 or type(data) ~= "table" then return nil end
    local tokens = compiler.deserialize(data)
    if not tokens then return nil end
    local n = 0
    for _ in pairs(read_cache) do n = n + 1 end
    if n >= READ_CACHE_MAX then
        local keys = {}
        for k in pairs(read_cache) do keys[#keys + 1] = k end
        for j = 1, math.floor(#keys / 2) do read_cache[keys[j]] = nil end
    end
    read_cache[cachePath] = { text = text, data = data }
    return tokens
end

-- ---------------------------------------------------------------------------
--  Lua-literal encoder for the .ksc cache. Emits a `return { ... }`
--  chunk; numeric keys stay numeric (Lua table literals), so no key
--  conversion is needed on load. Values are nil/boolean/number/string/
--  table (enforced by is_serializable).
-- ---------------------------------------------------------------------------

local function lua_escape(s)
    s = tostring(s)
    return '"' .. s:gsub("\\", "\\\\"):gsub('"', '\\"')
        :gsub("\n", "\\n"):gsub("\r", "\\r")
        :gsub("\t", "\\t"):gsub("\b", "\\b"):gsub("\f", "\\f") .. '"'
end

local function encode_literal_value(v)
    local t = type(v)
    if v == nil then return "nil" end
    if t == "boolean" then return v and "true" or "false" end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "nil" end
        if math.type and math.type(v)=='integer' then return tostring(v) end
        return string.format("%.17g", v)
    end
    if t == "string" then return lua_escape(v) end
    if t == "table" then return encode_lua_literal(v) end
    return "nil"
end

encode_lua_literal = function(t)
    local n = #t
    local is_array = true
    for k in pairs(t) do
        if type(k) ~= "number" or k < 1 or k > n or math.floor(k) ~= k then
            is_array = false
            break
        end
    end
    if is_array then
        local parts = {}
        for i = 1, n do parts[i] = encode_literal_value(t[i]) end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    local parts = {}
    -- t28: deterministic key order. pairs() follows the hash table's
    -- internal layout, which varies per Lua run -- three bakes of the same
    -- demo tree produced three different (semantically identical) web
    -- bundles, and every .ksc cache baked from the same source differed
    -- too. Emit explicit [key]= pairs in a stable order: numeric keys
    -- ascending, then string keys byte-wise. Non-(number|string) keys do
    -- not occur in serialized data; leave their original order untouched.
    local keys = {}
    for k in pairs(t) do keys[#keys + 1] = k end
    local sortable = true
    for _, k in ipairs(keys) do
        local tk = type(k)
        if tk ~= "number" and tk ~= "string" then sortable = false; break end
    end
    if sortable then
        table.sort(keys, function(x, y)
            local tx, ty = type(x), type(y)
            if tx ~= ty then return tx < ty end
            return x < y
        end)
    end
    for _, k in ipairs(keys) do
        if type(k) == "number" then
            parts[#parts + 1] = "[" .. string.format("%.17g", k) .. "]="
                .. encode_literal_value(t[k])
        else
            parts[#parts + 1] = "[" .. lua_escape(tostring(k)) .. "]="
                .. encode_literal_value(t[k])
        end
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

-- Web player bundle encoding (round 35): reuse the cache literal writer
-- so ks_bake --web can emit story bundles without a JSON dependency.
compiler.encode_lua_literal = encode_lua_literal

--- compiler.compile_from_ast(ast_model) → compiled instruction stream
function compiler.compile_from_ast(ast_model)
    return compiler.compile(ast_model)
end

--- compiler.compile_from_source(ks_text, filename) → compiled instruction stream
function compiler.compile_from_source(ks_text, filename)
    local ok, sem = pcall(require, "kag.semantic")
    if ok and sem then
        local ast = sem.parse(ks_text, filename)
        return compiler.compile(ast)
    end
    local tok = require("tokenizer")
    local tokens = tok.parse_with_offsets(ks_text)
    return compiler.compile(tokens)
end

return compiler
