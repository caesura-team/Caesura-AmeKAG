-- ═══════════════════════════════════════════════════════════════════════
--  kag/schema.lua — Declarative command contracts (KAG Neo-Genesis rules)
--
--  KAG3 commands read params as raw strings and each handler re-parses
--  (tonumber(params.x) or default) with silent fallbacks on bad input.
--  This module replaces that with a declarative contract:
--
--    Schema.define("pt", {
--        speed = { type = "number", default = 50, min = 8, max = 5000 },
--    })
--
--  The scheduler coerces params BEFORE dispatch: types are converted,
--  ranges clamped, unknown params warned, bad values reported with the
--  command/scene/token location instead of being swallowed. Handlers can
--  then read params.speed as a plain number.
--
--  Incremental: commands migrate one at a time; unmigrated commands pass
--  through unchanged (no behavior change).
-- ═══════════════════════════════════════════════════════════════════════

local Schema = {}

-- cmd -> { paramName = spec } ; spec: {type, default, min, max, choices, required}
local registry = {}
local migrated = {}  -- set of migrated command names
-- Command metadata (category / blocking / description) -- the schema
-- registry stores typed param contracts; _meta carries tooling facts:
--   category : text|audio|layer|transition|vfx|resource|save|system|video
--   blocking : true when the command waits for completion (duration/input)
--   desc     : one-line human summary (editor tooltips / docs)
local registry_meta = {}

-- ${expr} interpolation chunk cache: the expression text is a pure
-- function of the scene source (compiled once), so load() per evaluation
-- is waste -- the chunk is cached by expression string (bounded like the
-- expr module's cache). The env is a SHARED table: Lua 5.4 binds _ENV at
-- load time, so chunks load once against it and the f/sf/tf/mp/lf fields
-- are updated to the current ctx tables before each evaluation (the
-- scheduler is single-threaded -- no concurrent evaluation can observe a
-- torn env).
local interp_cache = {}
local INTERP_CACHE_MAX = 128
local interp_env = { f = {}, sf = {}, tf = {}, mp = {}, lf = {} }

-- Evaluate one ${expr} interpolation span (cache notes above). Returns
-- the formatted value, or the raw "${expr}" span when the expression
-- fails to compile or throws (bad input leaks verbatim, as before).
-- Find the index of the brace matching the '{' at position open.
-- Quote-aware: braces inside '...' / "..." do not count. nil when
-- unterminated. Shared by the ${...} scanner and the constructor
-- parenthesizer (round 54).
local function match_brace(s, open)
    local depth, quote = 1, nil
    local j, n = open + 1, #s
    while j <= n do
        local d = s:sub(j, j)
        if quote then
            if d == "\\" then
                j = j + 1  -- escaped char inside a literal
            elseif d == quote then
                quote = nil
            end
        elseif d == "'" or d == '"' then
            quote = d
        elseif d == "[" then
            -- Lua long bracket [=[ ... ]=] (round 62): braces inside a
            -- long string must not close the ${...} span. Skip to the
            -- matching closer of any level ([[, [=[, [==[ ...).
            local eqs = s:match("^=*", j + 1) or ""
            if s:sub(j + 1 + #eqs, j + 1 + #eqs) == "[" then
                local closer = "]" .. eqs .. "]"
                local k = s:find(closer, j + 2 + #eqs, true)
                if k then
                    j = k + #closer - 1  -- loop advance lands after it
                else
                    j = n + 1  -- unterminated long string: stop scanning
                end
            end
        elseif d == "{" then
            depth = depth + 1
        elseif d == "}" then
            depth = depth - 1
            if depth == 0 then return j end
        end
        j = j + 1
    end
    return nil
end

-- Prepare a ${expr} body for Lua compilation: TJS->Lua translation
-- (&& || ! != ?:) then parenthesize a leading table constructor. Returns
-- the `return (...)` source string, or nil when the body is empty.
-- Shared by eval_interp_expr (runtime) and Schema.checkInterp (editor
-- validation) so both compile through ONE code path.
local function prepare_interp_source(expr)
    if type(expr) ~= "string" or expr == "" then return nil end
    local exprLua = expr
    pcall(function()
        local ex = require("kag.expr")
        if ex and ex.translate then exprLua = ex.translate(expr) end
    end)
    local first = exprLua:match("^%s*(.)")
    if first == "{" then
        local open_pos = #exprLua:match("^%s*") + 1
        local close2 = match_brace(exprLua, open_pos)
        if close2 then
            exprLua = exprLua:sub(1, open_pos - 1)
                .. "(" .. exprLua:sub(open_pos, close2) .. ")"
                .. exprLua:sub(close2 + 1)
        end
    end
    return "return (" .. exprLua .. ")"
end

local function eval_interp_expr(expr, ctx)
    local f2 = interp_cache[expr]
    if not f2 then
        interp_env.f = ctx and ctx.f or {}
        interp_env.sf = ctx and ctx.sf or {}
        interp_env.tf = ctx and ctx.tf or {}
        interp_env.mp = ctx and ctx.mp or {}
        interp_env.lf = ctx and ctx.lf or {}
        -- Compile via the shared preparation path (single source of truth
        -- for ${expr} -> Lua). A nil source means an empty body: render
        -- verbatim, as before.
        local src = prepare_interp_source(expr)
        if not src then return "${" .. expr .. "}" end
        f2 = load(src, "=ks_interp", "t", interp_env)
        if not f2 then return "${" .. expr .. "}" end  -- syntax error
        interp_cache[expr] = f2
        local n = 0
        for _ in pairs(interp_cache) do n = n + 1 end
        if n > INTERP_CACHE_MAX then
            local keys = {}
            for k in pairs(interp_cache) do keys[#keys + 1] = k end
            for j = 1, math.floor(#keys / 2) do
                interp_cache[keys[j]] = nil
            end
        end
    else
        -- update the shared env to the current ctx tables
        interp_env.f = ctx and ctx.f or {}
        interp_env.sf = ctx and ctx.sf or {}
        interp_env.tf = ctx and ctx.tf or {}
        interp_env.mp = ctx and ctx.mp or {}
        interp_env.lf = ctx and ctx.lf or {}
    end
    local ok2, val2 = pcall(f2)
    if ok2 then return tostring(val2) end
    return "${" .. expr .. "}"
end

-- Expand every ${...} span in v with balanced-brace scanning (round 54):
-- the old "%${([^{}]+)}" pattern truncated expressions containing
-- literal braces (${ {a=1,b=2}.a } leaked the raw span into the text).
-- Depth counts nested braces; quoted string literals are skipped so
-- ${ "}" .. f.x } still balances. Unterminated "${" spans are left
-- verbatim, matching the syntax-error fallback.
local function expand_interp_exprs(v, ctx)
    local out = {}
    local i, n = 1, #v
    while i <= n do
        local k = v:find("${", i, true)
        if not k then
            out[#out + 1] = v:sub(i)
            break
        end
        if k > i then out[#out + 1] = v:sub(i, k - 1) end
        local close2 = match_brace(v, k + 1)
        if not close2 then
            -- unterminated "${" — leave the rest verbatim
            out[#out + 1] = v:sub(k)
            break
        end
        out[#out + 1] = eval_interp_expr(v:sub(k + 2, close2 - 1), ctx)
        i = close2 + 1
    end
    return table.concat(out)
end

--- Schema.define(cmd, specs) — register the contract for one command.
--  `specs._meta = { category=..., blocking=..., desc=... }` is optional and
--  stored separately (never treated as a parameter contract).
function Schema.define(cmd, specs)
    if type(cmd) ~= "string" or type(specs) ~= "table" then
        error("schema.define: cmd(string) and specs(table) required", 2)
    end
    if specs._meta ~= nil then
        registry_meta[cmd] = specs._meta
        local clean = {}
        for k, v in pairs(specs) do
            if k ~= "_meta" then clean[k] = v end
        end
        specs = clean
    end
    registry[cmd] = specs
    migrated[cmd] = true
end

--- Schema.meta(cmd) → metadata table or nil
function Schema.meta(cmd)
    return registry_meta[cmd]
end

local interp_namespaces = { f = true, sf = true, tf = true, mp = true, lf = true }

-- Static validation shares the syntax checker but never evaluates its chunks
-- or reads variable tables. Empty ${} bodies retain checkInterp's literal
-- behavior; only expressions and supported variable shorthands are deferred.
local function check_static_interpolation(value, name, whereFn)
    local issue = Schema.checkInterp(value)[1]
    if issue then
        error(string.format("%s: param '%s' interpolation at byte %d: %s",
            whereFn(), name, issue.offset, issue.error), 0)
    end
    local i = 1
    while true do
        local open = value:find("${", i, true)
        if not open then break end
        local close = match_brace(value, open + 1)
        if value:sub(open + 2, close - 1):find("%S") then return true end
        i = close + 1
    end
    for namespace in value:gmatch("%$(%a+)%.([%w_]+)") do
        if interp_namespaces[namespace] then return true end
    end
    for namespace in value:gmatch("%%(%a+)%.([%w_]+)%%") do
        if interp_namespaces[namespace] then return true end
    end
    return false
end

local function coerceValue(name, spec, raw, whereFn, ctx, static_fields)
    local v = raw
    if spec.type == "number" then
        if type(v) == "number" then
            -- already numeric (embedded eval may pass numbers)
        elseif type(v) == "string" and v:match("^%s*%-?%d+%.?%d*%s*$") then
            v = tonumber(v)
        else
            error(string.format(
                "%s: param '%s' expects a number, got %q", whereFn(), name, tostring(raw)), 0)
        end
        if spec.min and v < spec.min then
            print(string.format("[schema] %s: '%s' clamped %s -> %s (min)",
                whereFn(), name, tostring(v), tostring(spec.min)))
            v = spec.min
        elseif spec.max and v > spec.max then
            print(string.format("[schema] %s: '%s' clamped %s -> %s (max)",
                whereFn(), name, tostring(v), tostring(spec.max)))
            v = spec.max
        end
    elseif spec.type == "boolean" then
        if type(v) == "boolean" then
            -- pass
        elseif type(v) == "string" then
            local low = v:lower()
            if low == "true" or low == "1" or low == "yes" then v = true
            elseif low == "false" or low == "0" or low == "no" then v = false
            else
                error(string.format(
                    "%s: param '%s' expects boolean, got %q", whereFn(), name, tostring(raw)), 0)
            end
        else
            error(string.format(
                "%s: param '%s' expects boolean, got %q", whereFn(), name, tostring(raw)), 0)
        end
    elseif spec.type == "string" then
        v = tostring(v)
        -- Neo-Genesis interpolation: "$f.name" / "$sf.x" / "$tf.y" / "$mp.z"
        -- / "$lf.y" expand from the ctx variable tables; legacy KAG3's
        -- %var% syntax ("%f.hp%") is supported too. ${expr} evaluates a
        -- full expression (beyond KAG3's eval-glue).
        if spec.interpolate and type(v) == "string"
            and (v:find("$", 1, true) or v:find("%", 1, true)) then
            if static_fields then
                if check_static_interpolation(v, name, whereFn) then
                    static_fields[name] = true
                    -- Choices depend on the unresolved runtime value. Literal
                    -- strings still reach the normal choices check below.
                    return v
                end
            else
                -- ${expr}: full expression evaluated in a sandbox env with the
                -- ctx variable tables (f/sf/tf/mp/lf) -- beyond KAG3's eval-glue.
                -- Balanced-brace scanning (round 54): the old pattern
                -- "%${([^{}]+)}" truncated expressions containing literal
                -- braces (${ {a=1,b=2}.a } leaked the raw span into the
                -- text). expand_interp_exprs tracks brace depth and skips
                -- quoted string literals, so ${ "}" .. f.x } balances too.
                v = expand_interp_exprs(v, ctx)
                -- $tbl.key / %tbl.key% variable lookup (f/sf/tf/mp/lf). The
                -- %...% form is KAG3-compatible; bare %ident% stays untouched
                -- (macro placeholders are expanded earlier by the scheduler).
                local varLookup = function(tbl, key)
                    local vars = ({ f = "f", sf = "sf", tf = "tf", mp = "mp", lf = "lf" })[tbl]
                    local t = vars and ctx and ctx[vars]
                    if type(t) == "table" then
                        local val = t[key]
                        if val ~= nil then return tostring(val) end
                    end
                    return "$" .. tbl .. "." .. key  -- leave unresolved as-is
                end
                v = v:gsub("%$(%a+)%.([%w_]+)", varLookup)
                v = v:gsub("%%(%a+)%.([%w_]+)%%", varLookup)
            end
        end
    elseif spec.type == "list" then
        -- Comma-separated value -> array, optionally typed per element.
        -- e.g. colors="red,green,blue" -> {"red","green","blue"}
        if type(v) == "table" then
            -- already a list (programmatic callers may pass arrays)
        elseif type(v) == "string" then
            local out = {}
            for raw_item in v:gmatch("[^,]+") do
                local item = raw_item:match("^%s*(.-)%s*$")  -- trim
                if #item > 0 then
                    if spec.item_type == "number" then
                        local n = tonumber(item)
                        if not n then
                            error(string.format(
                                "%s: param '%s' list element expects a number, got %q",
                                whereFn(), name, item), 0)
                        end
                        item = n
                    elseif spec.item_type == "boolean" then
                        local low = item:lower()
                        if low == "true" or low == "1" or low == "yes" then item = true
                        elseif low == "false" or low == "0" or low == "no" then item = false
                        else
                            error(string.format(
                                "%s: param '%s' list element expects boolean, got %q",
                                whereFn(), name, item), 0)
                        end
                    end
                    out[#out + 1] = item
                end
            end
            v = out
        else
            error(string.format(
                "%s: param '%s' expects a list, got %q", whereFn(), name, tostring(raw)), 0)
        end
    elseif spec.type == "enum" then
        -- Explicit enum type: value must be one of spec.values (or the
        -- legacy `choices` map). Kept as a string after validation.
        local allowed = spec.values or spec.choices
        if not allowed then
            error(string.format(
                "%s: param '%s' enum missing values", whereFn(), name), 0)
        end
        local ok = false
        if type(allowed) == "table" then
            if allowed[v] then
                ok = true
            else
                for _, av in ipairs(allowed) do
                    if tostring(av) == tostring(v) then ok = true break end
                end
            end
        end
        if not ok then
            local list = {}
            if type(allowed) == "table" then
                for k in pairs(allowed) do list[#list + 1] = tostring(k) end
            end
            table.sort(list)
            error(string.format(
                "%s: param '%s' must be one of {%s}, got %q",
                whereFn(), name, table.concat(list, ","), tostring(raw)), 0)
        end
        v = tostring(v)
    elseif spec.type == "file" then
        -- Asset path cross-validation: normalize to string; reject empty
        -- and path traversal (static, no ctx needed); when a ctx with a
        -- resolver is present, additionally verify the file exists.
        v = tostring(v)
        if v == "" then
            error(string.format(
                "%s: param '%s' file path must not be empty", whereFn(), name), 0)
        end
        if v:find("..", 1, true) or v:find("\\", 1, true) or v:sub(1, 1) == "/" then
            error(string.format(
                "%s: param '%s' invalid file path: %q (no traversal/absolute)",
                whereFn(), name, tostring(raw)), 0)
        end
        if ctx and ctx.resolve_file and type(v) == "string" and #v > 0 then
            local okF, resolved = pcall(ctx.resolve_file, v)
            if okF and resolved == nil then
                error(string.format(
                    "%s: param '%s' file not found: %q (asset root)",
                    whereFn(), name, tostring(raw)), 0)
            end
        end
    end
    if spec.type ~= "list" and spec.choices then
        local okC = false
        local allowedC = spec.choices
        if type(allowedC) == "table" then
            if allowedC[v] then
                okC = true
            else
                for _, av in ipairs(allowedC) do
                    if tostring(av) == tostring(v) then okC = true break end
                end
            end
        end
        if not okC then
            local listC = {}
            if type(allowedC) == "table" then
                for k in pairs(allowedC) do
                    if type(k) == "number" then
                        listC[#listC + 1] = tostring(allowedC[k])
                    else
                        listC[#listC + 1] = tostring(k)
                    end
                end
            end
            table.sort(listC)
            error(string.format("%s: param '%s' must be one of {%s}, got %q",
                whereFn(), name, table.concat(listC, ","), tostring(raw)), 0)
        end
    end
    return v
end

--- Schema.coerce(cmd, params, ctx) → coerced params table (or raw on unmigrated)
--  Throws (caller pcall) with a structured message on contract violation.
local function coerceParams(cmd, params, ctx, static_fields)
    local specs = registry[cmd]
    if not specs then return params end  -- unmigrated: pass-through

    -- Lazily-built location string: the common path (params valid) never
    -- formats it; only error paths pay for the string.format.
    local where
    local function W()
        if not where then
            where = string.format("cmd [%s]@%s:%s",
                cmd, ctx and (ctx.current_scene or ctx.currentScene) or "?",
                ctx and ctx.token_index or "?")
        end
        return where
    end
    local out = {}

    -- Any-of requirement: at least one of these params must be present.
    if specs._require_any then
        local found = false
        for _, n in ipairs(specs._require_any) do
            local raw = params[n]
            if raw ~= nil and raw ~= "" then found = true break end
        end
        if not found then
            error(W() .. ": requires one of {" .. table.concat(specs._require_any, ",") .. "}", 0)
        end
    end

    -- Coerce declared params.
    for name, spec in pairs(specs) do
        local raw = params[name]
        -- `positional_index = N`: the param may also arrive as the Nth
        -- bare positional arg (KAG3 style, e.g. [set f.hp 30]).
        local pos = spec.positional_index
        local pos_raw = pos and params[pos]
        local pos_filled = pos_raw ~= nil and pos_raw ~= ""
        -- Empty string means "absent" for most types (KAG3 empty token =
        -- no value). Exception (round 97 dead-code fix): a `file`-typed
        -- EMPTY value is an invalid *provided* path, not an omission -- so
        -- it flows into coerceValue, where the empty-path rejection fires.
        local absent = raw == nil or (raw == "" and spec.type ~= "file")
        if absent then
            if spec.required and not pos_filled then
                error(W() .. ": missing required param '" .. name .. "'", 0)
            end
            if pos_filled then
                -- Positional value fills the slot: coerce it (type convert
                -- + clamp) into BOTH the named key and the positional slot,
                -- so handlers read numbers/booleans regardless of arg form
                -- (round 97: positional bypassed type coercion before). A
                -- default is intentionally NOT applied when the slot is
                -- filled -- the provided value always wins.
                local coerced = coerceValue(name, spec, pos_raw, W, ctx, static_fields)
                out[name] = coerced
                out[pos] = coerced
            elseif spec.default ~= nil then
                -- Defaults are normalized through coerceValue: a typed
                -- default that violates the contract (number param with
                -- default="oops") is rejected instead of emitted verbatim
                -- (round 97).
                out[name] = coerceValue(name, spec, spec.default, W, ctx, static_fields)
            end
        else
            out[name] = coerceValue(name, spec, raw, W, ctx, static_fields)
        end
    end
    -- Copy undeclared params through (compat), but warn on unknown names.
    for name, v in pairs(params) do
        if specs[name] == nil then
            -- numeric keys (bare positional args) pass through silently;
            -- named unknowns still warn
            if type(name) ~= "number" then
                print(string.format("[schema] %s: unknown param '%s' ignored",
                    cmd, tostring(name)))
            end
            -- A positional slot already written by a positional_index
            -- coercion above stays (round 97); otherwise the raw value
            -- (e.g. an undeclared extra positional arg) passes through.
            if out[name] == nil then out[name] = v end
        end
    end
    return out
end

-- Keep runtime dispatch on the shared executor directly, without an extra
-- wrapper call per command. Only validate_static supplies the private map.
Schema.coerce = coerceParams

--- Schema.validate_static(cmd, params, ctx) -> params, sorted dynamic fields
-- Retains literal contracts but never expands author string interpolation.
-- The second result contains field names only, with no expression/ctx data.
function Schema.validate_static(cmd, params, ctx)
    local fields = {}
    local out = coerceParams(cmd, params, ctx, fields)
    local dynamic = {}
    for name in pairs(fields) do dynamic[#dynamic + 1] = name end
    table.sort(dynamic)
    return out, dynamic
end

--- Schema.isMigrated(cmd) → boolean
function Schema.isMigrated(cmd)
    return migrated[cmd] == true
end

--- Schema.specs(cmd) → contract specs table or nil (LIVE reference, no
--  deep copy -- tooling that only reads positional_index etc. uses this;
--  dumpContracts() remains the deep-copy API for doc generation).
function Schema.specs(cmd)
    return registry[cmd]
end

--- Schema.dumpContracts() → { cmd = { param = spec } } — public DEEP copy
--  of the registry for doc generation / editor tooling. The contracts
--  are the single source of truth; docs and editors consume this.
--  Deep-copied so a caller cannot mutate live clamping/coercion.
function Schema.dumpContracts()
    local out = {}
    for cmd, specs in pairs(registry) do
        local copy = {}
        for name, spec in pairs(specs) do
            local sc = {}
            for k, v in pairs(spec) do
                if type(v) == "table" then
                    local t = {}
                    for kk, vv in pairs(v) do t[kk] = vv end
                    sc[k] = t
                else
                    sc[k] = v
                end
            end
            copy[name] = sc
        end
        if registry_meta[cmd] then
            copy._meta = {}
            for k, v in pairs(registry_meta[cmd]) do copy._meta[k] = v end
        end
        out[cmd] = copy
    end
    return out
end

--- Schema.registrySize() → number (for tests)
function Schema.registrySize()
    local n = 0
    for _ in pairs(registry) do n = n + 1 end
    return n
end


--- Schema.checkInterp(text) → list of { offset, error, severity }
--  Validate every ${expr} span in a (interpolatable) string VALUE using
--  the SAME compile path as runtime interpolation (prepare_interp_source +
--  load). Pure: no ctx, no I/O, no env mutation. Used by the editor LSP
--  diagnostics so a bad interpolation is flagged in the IDE instead of
--  silently falling back to the verbatim ${...} at run time.
--  Returns one entry per failed span (empty list = all spans compile):
--    offset    1-based byte offset of the "${" within `text`
--    error     human message
--    severity  1 = error (compile failure), 2 = warning (unterminated)
--  ${} with an empty body and the $tbl.key / %var% lookup shorthand are
--  NOT flagged (they are syntax/interpolation, not expression-compile).
function Schema.checkInterp(text)
    if type(text) ~= "string" then return {} end
    if not text:find("${", 1, true) then return {} end
    local issues = {}
    local i, n = 1, #text
    while i <= n do
        local k = text:find("${", i, true)
        if not k then break end
        local close2 = match_brace(text, k + 1)
        if not close2 then
            issues[#issues + 1] =
                { offset = k, error = "unterminated ${", severity = 2 }
            break
        end
        local body = text:sub(k + 2, close2 - 1)
        if body:find("%S") then
            local src = prepare_interp_source(body)
            if not src or not load(src, "=ks_interp_check", "t", interp_env) then
                issues[#issues + 1] = {
                    offset = k,
                    error = "interpolation does not compile: " .. body,
                    severity = 1,
                }
            end
        end
        i = close2 + 1
    end
    return issues
end

Schema.define("input", {
    _meta = {
        category = "text",
        blocking = true,
        desc = "prompt user for text input via virtual keyboard / IME and store to variable"
    },
    name        = { type = "string", required = true },
    prompt      = { type = "string", default = "", interpolate = true },
    default     = { type = "string", default = "", interpolate = true },
    maxlen      = { type = "number", default = 32, min = 1, max = 512 },
    max_length  = { type = "number", default = 32, min = 1, max = 512 },
    x           = { type = "number", default = 0 },
    y           = { type = "number", default = 0 },
    width       = { type = "number", default = 640, min = 120, max = 1920 },
    height      = { type = "number", default = 180, min = 60, max = 1080 },
    font_size   = { type = "number", default = 28, min = 12, max = 72 },
    color       = { type = "string", default = "#ffffff" },
    bg_color    = { type = "string", default = "#202020" },
    password    = { type = "boolean", default = false },
    cond        = { type = "string" },
    btn_ok      = { type = "string", default = "OK" },
    btn_cancel  = { type = "string", default = "" },
})

Schema.define("edit", {
    _meta = { category = "text", blocking = true, desc = "KAG3 alias of [input]" },
    name        = { type = "string", required = true },
    prompt      = { type = "string", default = "", interpolate = true },
    default     = { type = "string", default = "", interpolate = true },
    maxlen      = { type = "number", default = 32, min = 1, max = 512 },
    max_length  = { type = "number", default = 32, min = 1, max = 512 },
    x           = { type = "number", default = 0 },
    y           = { type = "number", default = 0 },
    width       = { type = "number", default = 640, min = 120, max = 1920 },
    height      = { type = "number", default = 180, min = 60, max = 1080 },
    font_size   = { type = "number", default = 28, min = 12, max = 72 },
    color       = { type = "string", default = "#ffffff" },
    bg_color    = { type = "string", default = "#202020" },
    password    = { type = "boolean", default = false },
    cond        = { type = "string" },
    btn_ok      = { type = "string", default = "OK" },
    btn_cancel  = { type = "string", default = "" },
})

-- [steam_achievement id="ACH_01" cond="f.flag" silent=true]
-- Handler: kag/commands/system.lua SystemCommands.steam_achievement.
-- Never blocks and never fails a scene: when Steam is absent or refuses the id
-- the command warns (unless silent=true) and continues.
Schema.define("steam_achievement", {
    _meta = {
        category = "system",
        blocking = false,
        desc = "unlock a Steamworks achievement (warns, never fails, when Steam is absent)"
    },
    id     = { type = "string" },
    name   = { type = "string" },   -- alias for id (KAG3-style naming)
    cond   = { type = "string" },   -- unlock only when this expression is truthy
    silent = { type = "boolean", default = false },  -- suppress the NOT-unlocked warning
})

return Schema
