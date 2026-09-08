-- =============================================================================
--  Caesura (AmeKAG) — kag/expr.lua
--  KAG Neo-Genesis expression language.
--
--  Legacy KAG3 scripts use TJS-style operators in [if]/[while]/[eval]:
--      &&  ||  !   !=   cond ? a : b
--  Lua uses:      and or not ~= (a and b or c)
--  This module translates TJS expressions into Lua before compilation, so
--  old scenes run as-is instead of silently failing (previously a TJS
--  expression failed to compile, eval_expr swallowed the error and the
--  [if] silently took the else branch).
--
--  Translation is string-literal aware: operators inside '...' / "..." are
--  left untouched. Ternary is translated to `(cond and (a) or (b))`, which
--  is exact for numbers/strings and for booleans where the then-branch is
--  truthy. Nested ternaries are supported.
--
--  Error visibility: compile/runtime failures now print a scene:line
--  diagnostic instead of returning false silently.
-- =============================================================================

local expr = {}

-- Expression chunk cache, bucketed by translated source + ctx.f. A bucket
-- matches only while all five bound namespace identities still match.
-- Bounded like the scheduler's own cache.
local cache = {}
local CACHE_MAX = 128

-- AOT bytecode cache (Battle 1c): translated source -> string.dump chunk.
-- Loading a dumped chunk with `load(bc, name, "b", env)` skips Lua's
-- lexer+parser (measured ~6x faster than source load). The compiler
-- pre-generates dumps for compiled streams (_compiled.exprDumps); this
-- table back-fills lazily for hand-built/deserialized streams, so the
-- second env identity for the same source is already AOT.
local dump_cache = {}
local DUMP_CACHE_MAX = 128

-- ---------------------------------------------------------------------------
-- Character-level scanner helpers (string-literal aware)
-- ---------------------------------------------------------------------------

-- Skip a Lua long bracket [=*[ ... ]=*] starting at j (src:sub(j,j) == '[').
-- Returns the index AFTER the closer, or nil when not a long string or
-- unterminated. Round 70 review C2: bracket/paren scanners must not let
-- a ']' inside a long string close the group early.
local function skip_long_string(src, j)
    local eqs = src:match("^=*", j + 1) or ""
    if src:sub(j + 1 + #eqs, j + 1 + #eqs) ~= "[" then return nil end
    local closer = "]" .. eqs .. "]"
    local k = src:find(closer, j + 2 + #eqs, true)
    if not k then return nil end
    return k + #closer
end

-- Find the first occurrence of ch at paren-depth 0, outside string literals.
-- Returns byte index or nil.
local function find_top(src, ch, from)
    local depth = 0
    local i = from or 1
    local n = #src
    local quote = nil
    while i <= n do
        local c = src:sub(i, i)
        if quote then
            if c == "\\" then
                i = i + 2
            elseif c == quote then
                quote = nil
                i = i + 1
            else
                i = i + 1
            end
        else
            if c == '"' or c == "'" then
                quote = c
                i = i + 1
            elseif c == "[" then
                local ls = skip_long_string(src, i)
                if ls then
                    i = ls  -- next iteration starts right after the literal
                else
                    depth = depth + 1
                    i = i + 1
                end
            elseif c == "(" then
                depth = depth + 1
                i = i + 1
            elseif c == ")" or c == "]" then
                depth = depth - 1
                i = i + 1
            elseif c == ch and depth == 0 then
                return i
            else
                i = i + 1
            end
        end
    end
    return nil
end

-- Find the top-level ':' that pairs with a '?' at position q_pos, tolerating
-- nested ternaries (a ? b ? c : d : e -> the LAST top-level ':').
local function match_colon(src, q_pos)
    local depth = 0
    local nested = 0
    local i = q_pos + 1
    local n = #src
    local quote = nil
    while i <= n do
        local c = src:sub(i, i)
        if quote then
            if c == "\\" then
                i = i + 2
            elseif c == quote then
                quote = nil
                i = i + 1
            else
                i = i + 1
            end
        else
            if c == '"' or c == "'" then
                quote = c
                i = i + 1
            elseif c == "[" then
                local ls = skip_long_string(src, i)
                if ls then
                    i = ls  -- next iteration starts right after the literal
                else
                    depth = depth + 1
                    i = i + 1
                end
            elseif c == "(" then
                depth = depth + 1
                i = i + 1
            elseif c == ")" or c == "]" then
                depth = depth - 1
                i = i + 1
            elseif c == "?" and depth == 0 then
                nested = nested + 1
                i = i + 1
            elseif c == ":" and depth == 0 then
                if nested == 0 then
                    return i
                end
                nested = nested - 1
                i = i + 1
            else
                i = i + 1
            end
        end
    end
    return nil
end

-- ---------------------------------------------------------------------------
-- Operator translation (TJS -> Lua), string-literal aware
-- ---------------------------------------------------------------------------

local function translate_operators(src)
    local out = {}
    local i = 1
    local n = #src
    local quote = nil

    -- Trim trailing whitespace of the last emitted chunk, then append the
    -- operator with single surrounding spaces, skipping whitespace that
    -- follows the operator in the source. "a && b" / "a&&b" both -> "a and b".
    local function emit_op(op)
        -- Trim trailing whitespace of the last emitted chunk (and a leading
        -- space when nothing was emitted yet, e.g. "!a"), then append the
        -- operator with single surrounding spaces, skipping whitespace that
        -- follows the operator in the source.
        if #out > 0 and out[#out]:match("%s$") then
            out[#out] = out[#out]:gsub("%s+$", "")
        elseif #out == 0 and op:match("^%s") then
            op = op:gsub("^%s+", "")
        end
        out[#out + 1] = op
        while i <= n and src:sub(i, i):match("%s") do
            i = i + 1
        end
    end

    while i <= n do
        local c = src:sub(i, i)
        if quote then
            out[#out + 1] = c
            if c == "\\" then
                out[#out + 1] = src:sub(i + 1, i + 1)
                i = i + 2
            else
                if c == quote then quote = nil end
                i = i + 1
            end
        else
            if c == '"' or c == "'" then
                quote = c
                out[#out + 1] = c
                i = i + 1
            elseif c == "[" then
                local ls = skip_long_string(src, i)
                if ls then
                    out[#out + 1] = src:sub(i, ls - 1)  -- verbatim literal
                    i = ls
                else
                    out[#out + 1] = c
                    i = i + 1
                end
            elseif c == "&" and src:sub(i + 1, i + 1) == "&" then
                i = i + 2
                emit_op(" and ")
            elseif c == "|" and src:sub(i + 1, i + 1) == "|" then
                i = i + 2
                emit_op(" or ")
            elseif c == "!" and src:sub(i + 1, i + 1) == "=" then
                i = i + 2
                emit_op(" ~= ")
            elseif c == "!" then
                i = i + 1
                emit_op(" not ")
            elseif c == "?" and src:sub(i + 1, i + 1) == "?" then
                -- TJS null-coalescing: a ?? b keeps a when it is not
                -- nil/false, else b. Lua 'or' is exactly this value
                -- fallback (0 and "" are truthy, so numbers and empty
                -- strings survive; only nil/false fall through).
                -- (round 53: the doc mentioned ?? but it never translated)
                i = i + 2
                emit_op(" or ")
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
    end
    return table.concat(out)
end

-- ---------------------------------------------------------------------------
-- Full translation: ternary first (recursive), then operators
-- ---------------------------------------------------------------------------

local translate

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- Translate ternary expressions INSIDE [...] index brackets before the
-- outer ternary pass (round 61): find_top/match_colon treat [ ] as depth,
-- so `f.arr[flag ? 1 : 2]` was never translated and the raw '?' broke
-- Lua load(). Flatten bracket contents innermost-out; the outer pass then
-- sees already-valid Lua inside the brackets.
-- Forward declaration: translate_ternary is defined later and assigned
-- here so the bracket/paren passes can call back into the full pipeline.
local translate

local function translate_brackets(src, depth)
    local out = {}
    local i, n = 1, #src
    local quote = nil
    while i <= n do
        local c = src:sub(i, i)
        if quote then
            out[#out + 1] = c
            if c == "\\" then
                out[#out + 1] = src:sub(i + 1, i + 1)
                i = i + 2
            else
                if c == quote then quote = nil end
                i = i + 1
            end
        else
            if c == '"' or c == "'" then
                quote = c
                out[#out + 1] = c
                i = i + 1
            elseif c == "[" then
                local ls = skip_long_string(src, i)
                if ls then
                    out[#out + 1] = src:sub(i, ls - 1)  -- verbatim literal
                    i = ls
                else
                -- find the matching ] (bracket depth, quote-aware; a
                -- long string inside does not count)
                local j, depth, q2 = i + 1, 1, nil
                while j <= n do
                    local d = src:sub(j, j)
                    if q2 then
                        if d == "\\" then j = j + 1
                        elseif d == q2 then q2 = nil end
                    elseif d == '"' or d == "'" then
                        q2 = d
                    elseif d == "[" then
                        local ls2 = skip_long_string(src, j)
                        if ls2 then
                            j = ls2 - 1  -- loop advance lands after it
                        else
                            depth = depth + 1
                        end
                    elseif d == "]" then
                        depth = depth - 1
                        if depth == 0 then break end
                    end
                    j = j + 1
                end
                if depth == 0 then
                    out[#out + 1] = "[" .. translate(src:sub(i + 1, j - 1), depth) .. "]"
                    i = j + 1
                else
                    out[#out + 1] = c
                    i = i + 1
                end
                end  -- if ls (long-string literal emitted verbatim)
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
    end
    return table.concat(out)
end

-- Same flattening for (...) parenthesised groups (round 68): a ternary
-- inside parens — f.arr[1] + (f.flag ? f.arr[2] : f.arr[3]) — was never
-- translated because find_top only matches '?' at paren-depth 0.
local function translate_parens(src, depth)
    local out = {}
    local i, n = 1, #src
    local quote = nil
    while i <= n do
        local c = src:sub(i, i)
        if quote then
            out[#out + 1] = c
            if c == "\\" then
                out[#out + 1] = src:sub(i + 1, i + 1)
                i = i + 2
            else
                if c == quote then quote = nil end
                i = i + 1
            end
        else
            if c == '"' or c == "'" then
                quote = c
                out[#out + 1] = c
                i = i + 1
            elseif c == "(" then
                local j, depth, q2 = i + 1, 1, nil
                while j <= n do
                    local d = src:sub(j, j)
                    if q2 then
                        if d == "\\" then j = j + 1
                        elseif d == q2 then q2 = nil end
                    elseif d == '"' or d == "'" then
                        q2 = d
                    elseif d == "(" then
                        depth = depth + 1
                        if depth > 48 then
                            -- [round 97] nesting budget: leave this whole
                            -- group untranslated (runtime parser reports it)
                            out[#out + 1] = src:sub(i, j - 1)
                            i = j
                            break
                        end
                    elseif d == ")" then
                        depth = depth - 1
                        if depth == 0 then break end
                    elseif d == "[" then
                        local ls2 = skip_long_string(src, j)
                        if ls2 then
                            j = ls2 - 1  -- a ')' inside a long string
                        end               -- must not close the group
                    end
                    j = j + 1
                end
                if depth == 0 then
                    -- [round 84] A parenthesized function call carries a
                    -- comma-separated argument list: translating the whole
                    -- inner text as ONE expression lets a ternary swallow the
                    -- commas (f.calc(f.flag ? 1 : 2, 3) -> (2, 3) folded
                    -- into the else branch). Split top-level commas and
                    -- translate each argument independently, then rejoin.
                    local inner = src:sub(i + 1, j - 1)
                    local segs = {}
                    local cur = 1
                    local comma = find_top(inner, ",", 1)
                    if comma then
                        while comma do
                            segs[#segs + 1] = inner:sub(cur, comma - 1)
                            cur = comma + 1
                            comma = find_top(inner, ",", cur)
                        end
                        segs[#segs + 1] = inner:sub(cur)
                        for k = 1, #segs do segs[k] = translate(segs[k], depth) end
                        -- trim each segment: a trailing space before the
                        -- comma (f.calc(a, b ? 1 : 2, 3)) would otherwise
                        -- survive into the rejoined argument list
                        for k = 1, #segs do segs[k] = segs[k]:gsub("^%s+", ""):gsub("%s+$", "") end
                        out[#out + 1] = "(" .. table.concat(segs, ", ") .. ")"
                    else
                        out[#out + 1] = "(" .. translate(inner, depth) .. ")"
                    end
                    i = j + 1
                else
                    out[#out + 1] = c
                    i = i + 1
                end
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
    end
    return table.concat(out)
end

local function translate_ternary(src, depth)
    -- [round 97] Complexity budget: deep nested ternaries make the
    -- recursive scan O(n^3) (each level re-scans its whole substring),
    -- and ~100 nested levels already take seconds. Beyond the budget the
    -- expression is left untranslated (the runtime Lua parser reports the
    -- syntax), so a hostile/accidental deep nest can never hang the
    -- translator. Normal scripts stay well below 30 levels.
    src = translate_parens(src, depth)
    src = translate_brackets(src, depth)
    local q = find_top(src, "?", 1)
    if not q then
        return translate_operators(src)
    end
    local colon = match_colon(src, q)
    if not colon then
        -- Unbalanced '?' -- leave as-is; the compile error below will be
        -- reported visibly by expr.evaluate.
        return translate_operators(src)
    end
    local cond = trim(src:sub(1, q - 1))
    local then_part = trim(src:sub(q + 1, colon - 1))
    local else_part = trim(src:sub(colon + 1))
    return "((" .. translate(cond, depth) .. ") and (" .. translate(then_part, depth)
        .. ") or (" .. translate(else_part, depth) .. "))"
end

translate = function(input, depth)
    if type(input) ~= "string" then return tostring(input) end
    return translate_ternary(input, depth)
end

-- translate now defined above with the depth budget

-- ---------------------------------------------------------------------------
-- Public API
-- ---------------------------------------------------------------------------

--- expr.translate(source) -> Lua source. Exported for tests/tooling.
function expr.translate(source)
    if type(source) ~= "string" then return tostring(source) end
    return translate(source)
end

--- expr.translateOperators(source) — TJS operator translation ONLY
--  (&& || ! != ??), no ternary wrap. Used by [eval] where assignment
--  statements make the (a and b or c) ternary wrap invalid (round 61).
function expr.translateOperators(source)
    if type(source) ~= "string" then return tostring(source) end
    return translate_operators(source)
end

--- expr.translateAssignment(source) — translate an [eval] statement.
--  Splits at the FIRST top-level '=' and runs the FULL pipeline (ternary
--  included) on the RHS — x = cond ? a : b becomes
--  x = ((cond) and (a) or (b)), which is valid as an assignment RHS
--  (round 68: eval assignments previously never got ternary support).
--  With no assignment, falls back to operators-only translation.
function expr.translateAssignment(source)
    if type(source) ~= "string" then return tostring(source) end
    local depth, quote = 0, nil
    local i = 1
    local n = #source
    while i <= n do
        local c = source:sub(i, i)
        if quote then
            if c == "\\" then
                -- skip escaped char
            elseif c == quote then
                quote = nil
            end
        elseif c == '"' or c == "'" then
            quote = c
        elseif c == "[" then
            local ls = skip_long_string(source, i)
            if ls then
                i = ls - 1  -- skip [[...]] literals: '=' inside is content
            end
        elseif c == "(" then
            depth = depth + 1
        elseif c == ")" then
            depth = math.max(0, depth - 1)
        elseif c == "=" then
            if source:sub(i + 1, i + 1) == "=" then
                i = i + 1  -- skip == comparisons entirely
            elseif depth == 0 then
                local prev = source:sub(i - 1, i - 1)
                if prev == ">" or prev == "<" or prev == "!"
                        or prev == "~" then
                    -- part of >= <= != ~= — not an assignment (round 70
                    -- review C1: f.x = f.lv >= 5 ? 1 : 0 broke here)
                else
                    -- first top-level single '=': translate the RHS fully
                    return source:sub(1, i - 1):gsub("%s+$", "")
                        .. " = " .. translate(source:sub(i + 1):gsub("^%s+", ""))
                end
            end
        end
        i = i + 1
    end
    return translate_operators(source)
end

--- expr.evaluate(ctx, source) -> ok, value
--  Compiles the translated expression in the ctx variable environment
--  (ctx.f / ctx.sf / ctx.tf / ctx.mp / ctx.lf). On compile or runtime error
--  prints a scene:line diagnostic and returns false, nil -- the [if] then
--  takes the else branch, but the author sees the error.
function expr.evaluate(ctx, source)
    if type(source) ~= "string" then return true, source end
    local translated = translate(source)
    return expr.evaluateTranslated(ctx, translated, source)
end

--- expr.evaluateTranslated(ctx, translated, original) → ok, value
--  Same as evaluate() but SKIPS the TJS->Lua translation: the compile-time
--  front-end (kag/compiler.lua) already translated the source once, so the
--  runtime hot path avoids re-scanning it on every [if]/[while] evaluation
--  (measured: 400-if scene ~30% faster on the eval path). `original` is
--  used only for error diagnostics (source shown to the author).
--  Optional `dump` (Battle 1c): precompiled string.dump bytecode from the
--  compiler (_compiled.exprDumps). Loading bytecode skips Lua's lexer+
--  parser (~6x faster than source load, measured). The dump is bound to
--  the caller's env at load time (mode "b" + env), so the same dump
--  serves every env identity — a session-wide AOT win, not per-env.
function expr.evaluateTranslated(ctx, translated, original, dump)
    if type(translated) ~= "string" then return true, translated end
    -- Compare the actual namespaces before allocating fallback tables: an
    -- absent namespace is stable too. rawequal excludes user __eq methods.
    local f, sf, tf = ctx and ctx.f or nil, ctx and ctx.sf or nil, ctx and ctx.tf or nil
    local mp, lf = ctx and ctx.mp or nil, ctx and ctx.lf or nil
    local key = translated .. "\0" .. tostring(f)
    local cached = cache[key]
    local fn = cached and rawequal(cached.f, f) and rawequal(cached.sf, sf)
        and rawequal(cached.tf, tf) and rawequal(cached.mp, mp)
        and rawequal(cached.lf, lf) and cached.fn or nil
    if not fn then
        -- Bind a fresh function instead of replacing a live function's _ENV:
        -- an expression may re-enter evaluation with a different call frame.
        -- Bare identifiers resolve through f; qualified names use the five
        -- explicit namespace tables, with independent empty defaults.
        local env = { f = f or {}, sf = sf or {}, tf = tf or {}, mp = mp or {}, lf = lf or {} }
        setmetatable(env, { __index = env.f })
        -- AOT path: use the precompiled bytecode when available (either
        -- supplied by the compiler or back-filled in dump_cache). Fall
        -- back to source load on any dump failure (e.g. a dump produced
        -- by a different Lua build) — correctness beats speed.
        local bc = dump or dump_cache[translated]
        local okChunk, chunk
        if bc then
            okChunk, chunk = pcall(load, bc, "=kag_expr", "b", env)
            if okChunk and chunk and not dump_cache[translated] then
                -- Share the compiler-supplied dump session-wide: a
                -- deserialized (.ksc) stream evaluating the same
                -- expression gets the AOT path too.
                dump_cache[translated] = bc
                local n = 0
                for _ in pairs(dump_cache) do n = n + 1 end
                if n > DUMP_CACHE_MAX then
                    local keys = {}
                    for k in pairs(dump_cache) do keys[#keys + 1] = k end
                    for j = 1, math.floor(#keys / 2) do
                        dump_cache[keys[j]] = nil
                    end
                end
            end
        end
        if not bc or not okChunk or not chunk then
            okChunk, chunk = pcall(load, "return " .. translated,
                                   "=kag_expr", "t", env)
            if okChunk and chunk and not dump_cache[translated] then
                -- Back-fill: a hand-built/deserialized stream has no
                -- compiler-supplied dump; make the NEXT env identity AOT.
                local okDump, dumped = pcall(string.dump, chunk, true)
                if okDump and dumped then
                    dump_cache[translated] = dumped
                    local n = 0
                    for _ in pairs(dump_cache) do n = n + 1 end
                    if n > DUMP_CACHE_MAX then
                        local keys = {}
                        for k in pairs(dump_cache) do keys[#keys + 1] = k end
                        for j = 1, math.floor(#keys / 2) do
                            dump_cache[keys[j]] = nil
                        end
                    end
                end
            end
        end
        if okChunk and chunk then
            fn = chunk
            cache[key] = { fn = fn, f = f, sf = sf, tf = tf, mp = mp, lf = lf }
            local n = 0
            for _ in pairs(cache) do n = n + 1 end
            if n > CACHE_MAX then
                local keys = {}
                for k in pairs(cache) do keys[#keys + 1] = k end
                for j = 1, math.floor(#keys / 2) do
                    cache[keys[j]] = nil
                end
            end
        else
            local where = "?"
            if ctx then
                where = (ctx.current_scene or ctx.currentScene or "?")
                    .. ":" .. tostring(ctx.token_index or ctx.tokenIndex or "?")
            end
            print(string.format(
                "[KAG] expression error in %s: %s (source: %s)",
                where, tostring(chunk), tostring(original or translated)))
            return false, nil
        end
    end
    local ok, value = pcall(fn)
    if not ok then
        local where = "?"
        if ctx then
            where = (ctx.current_scene or ctx.currentScene or "?")
                .. ":" .. tostring(ctx.token_index or ctx.tokenIndex or "?")
        end
        print(string.format(
            "[KAG] expression runtime error in %s: %s (source: %s)",
            where, tostring(value), tostring(original or translated)))
        return false, nil
    end
    return true, value
end

--- expr.reset_cache() — test/tooling hook.
function expr.reset_cache()
    cache = {}
    dump_cache = {}
end

return expr
