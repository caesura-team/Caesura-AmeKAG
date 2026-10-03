-- =============================================================================
--  Caesura (AmeKAG) — tokenizer.lua (KAG Neo-Genesis grammar)
-- =============================================================================
--  Design: cmd_pat (generic [command param=value ...]) handles 95% of the
--  KAG3-compatible tag set (KAG Neo-Genesis is a superset of KAG3).
--  Only non-standard syntax (labels, inline code, block markers) needs
--  explicit LPeg patterns. This keeps the grammar compact while covering all
--  167 KAG3-compatible commands plus the Neo-Genesis extensions.
-- =============================================================================

local lpeg = require("lpeg")
local P, S, R, C, Ct, Cc, Cg, Cp = lpeg.P, lpeg.S, lpeg.R, lpeg.C, lpeg.Ct, lpeg.Cc, lpeg.Cg, lpeg.Cp

local tokenizer = { _VERSION = "2.0.0" }

local space   = S(" \t\r\n")^0        -- optional whitespace
local ws1     = S(" \t\r\n")^1        -- required whitespace
local dquote  = P('"')
local squote  = P("'")
local ident   = R("az", "AZ", "__") * R("az", "AZ", "09", "__")^0

-- Quoted value with escape support for \" and \'
local dq_esc  = P("\\") * P('"')           -- \" escape
local sq_esc  = P("\\") * P("'")           -- \' escape
local qval    = dquote * C((dq_esc + (1 - dquote))^0) * dquote
              + squote * C((sq_esc + (1 - squote))^0) * squote
local uval    = C((1 - S(" \t\r\n]"))^1)
-- Bare positional value (KAG3 syntax: [delay 500], [se 1], [gallery 2]):
-- params[1] gets the raw string; handlers tonumber it. Comes AFTER the
-- ident=value branch so "x=5" never parses as a bare value.
-- Dotted key path: f.name / f.hp / sf.chapter (KAG3 [set f.x = v]).
-- One capture spanning the whole dotted key, so the pair is {1, "f.name"}.
local dkey    = C(ident * (P(".") * ident)^0)
local param   = Ct(dkey * space * "=" * space * (qval + uval))
              + Ct(Cc("1") * (qval + uval))

-- Command body: ["cmd", name, {{key,val},...}]
local cmd_body = Ct(Cc("cmd") * C(ident) * space * Ct(param * space)^0)
-- Text body: ["text", content]
local text_body = Ct(Cc("text") * C((1 - S("[\r\n*"))^1))
local cmd_pat  = P("[") * space * cmd_body * space * P("]")

-- Skip: whitespace (1+) or comment (; ... optional newline), repeated
local comment = P(";") * (1 - S("\r\n"))^0 * (P("\r")^-1 * P("\n"))^-1
local skip    = (ws1 + comment)^0

-- BOM
local bom     = P("\239\187\191")^-1

-- Label: *identifier (star-prefixed label marker)
local label_pat = Ct(Cc("label") * P("*") * C(ident))

-- Explicit command patterns for non-standard syntax
local wsp   = space
local attrs = Ct(param * space)^0

-- Embedded script block: [iscript] ... [endscript]
-- Captured as single token with raw body for Lua execution.
local iscript_close = P"[endscript]" + P"[/endscript]"
local iscript_body = C((P(1) - P"[endscript]" - P"[/endscript]")^0)
local iscript_pat = Ct(Cc("iscript") * P"[iscript]" * skip * iscript_body * skip * iscript_close)

-- Full grammar: embedded-script block first (its body is a raw region,
-- not tag syntax), then the generic cmd_pat. The dedicated prefix
-- patterns (P_se/P_stopse/P_fadebgm/P_fadevoice/P_fadese/P_wait/P_delay/
-- P_skip/eval_pat) are STRICTLY REDUNDANT with cmd_pat: its ident capture
-- stops at the same boundary characters cmd_pre checked (space, =, ]),
-- its bare-value param branch covers [wait 500]/[se 1]/[eval f.x = 1],
-- and the resulting cmd name is identical. Removing them cuts the
-- per-token ordered-choice cost from 11 patterns to 2 (measured:
-- ~30% faster on the 4000-token benchmark).
local block_text = Ct(Cc("blocktext") * P('"""')
    * C((P(1) - P('"""'))^0) * P('"""'))
local explicit_cmds = iscript_pat + cmd_pat

-- Robustness: the token sequence is optional (zero or more), so a
-- comment-only or whitespace-only .ks source parses to an empty token
-- table ({}), exactly like the empty string -- never a "parse failed"
-- error. The grammar still rejects genuinely malformed input (unclosed
-- "[" / "[]"): when no token matches, the trailing skip * -1 fails on the
-- dangling bytes.
local grammar = Ct(
    bom *
    (skip * (explicit_cmds + label_pat + block_text + text_body))^0 *
    skip * -1
)

-- One-token pattern with an END-position capture: used by parse_with_offsets
-- to advance through the source token by token (LPeg's init argument gives
-- the next match start; the captured end position is the offset).
-- Two Cp() captures: the first records the position AFTER leading
-- whitespace/comments (the command's own start -- the exact byte of
-- '['), the second the match end for advancing. Audit: the old form
-- reported the match START including leading comments, so editor
-- jumps landed on the comment, not the command.
local one_token = Ct(skip * Cp() * (explicit_cmds + label_pat + block_text + text_body) * Cp())

-- Bare positional args ([set f.hp 30], [wait 500]): the LPeg param
-- pattern emits a fixed "1" position marker for every bare value;
-- renumber bare args by order of appearance so params[1], params[2], ...
-- land correctly (named key=value pairs do not consume a slot, matching
-- KAG3 semantics where [tag x=1 500] -> params.x + params[1]).
local function renumber_bare_params(params)
    local bare_n = 0
    for _, pair in ipairs(params) do
        if type(pair) == "table" and pair[1] == "1" and pair[2] ~= nil then
            bare_n = bare_n + 1
            pair[1] = tostring(bare_n)
        end
    end
    return params
end

-- ---- Normalization ----
local function normalize(raw_tokens)
    local result = {}
    for _, t in ipairs(raw_tokens) do
        if type(t) ~= "table" or #t < 2 then goto continue end
        local typ = t[1]
        if typ == "cmd" then
            local params = {}
            for i = 3, #t do
                if type(t[i]) == "table" then
                    local pair = t[i][1]
                    if type(pair) == "table" and pair[1] then
                        params[#params + 1] = pair                    end
                end
            end
            renumber_bare_params(params)
            result[#result + 1] = { type = "command", cmd = tokenizer.normalize_cmd(t[2]), params = params }
        elseif typ == "text" then
            local txt = t[2] or ""
            if not txt:match("^%s*$") then
                result[#result + 1] = { type = "text", content = txt }
            end
        elseif typ == "blocktext" then
            -- Multi-line text block: """ ... """ -- content keeps newlines
            -- (trim one leading/trailing line break from the delimiters);
            -- the scheduler renders it as one [ch] (multi-line dialogue).
            local txt = (t[2] or ""):gsub("^\r?\n", ""):gsub("\r?\n$", "")
            if not txt:match("^%s*$") then
                result[#result + 1] = { type = "text", content = txt }
            end
        elseif typ == "label" then
            result[#result + 1] = { type = "label", name = t[2] }
        elseif typ == "iscript" then
            result[#result + 1] = { type = "iscript", body = t[2] or "" }
        end
        ::continue::
    end
    return result
end

-- KAG3 command-name aliases: normalize at the token level so the scheduler,
-- skip_to and ks_check all see one spelling.
local ALIASES = {
    elsif = "elseif",   -- KAG3 spelling
}
for alias, canonical in pairs(ALIASES) do
    ALIASES[alias] = canonical
end

--- tokenizer.normalize_cmd(name) → canonical command name (aliases applied).
function tokenizer.normalize_cmd(name)
    return ALIASES[name] or name
end

--- tokenizer.parse_with_offsets(text) → tokens with a byte `offset` each.
--  Advances through the source with lpeg.match(one_token, text, init): the
--  LPeg `init` argument starts each match after the previous token's end,
--  and a Cp() end-position capture yields the exact byte offset. Comments
--  and blank lines are consumed by the pattern between tokens, so offsets
--  are source-accurate (the durable fix for ks_check line numbers).
function tokenizer.parse_with_offsets(ks_text)
    if not ks_text or ks_text == "" then return {} end
    if ks_text:byte(1) == 0xEF and ks_text:byte(2) == 0xBB and ks_text:byte(3) == 0xBF then
        ks_text = ks_text:sub(4)
    end
    local out = {}
    local init = 1
    while true do
        local cap = lpeg.match(one_token, ks_text, init)
        if not cap then break end
        local startpos = cap[1] or init  -- after leading skip (cmd's '[')
        local t = cap[2]                -- the token capture table
        local endpos = cap[3] or init   -- match end
        if type(t) == "table" and t[1] then
            local tok = { offset = startpos, end_offset = endpos - 1 }
            local typ = t[1]
            if typ == "cmd" then
                -- Alias normalization MUST match tokenizer.parse (line ~130):
                -- ks_check/LSP consume this path, so an un-normalized [elsif]
                -- would be flagged "unknown" there while the runtime scheduler
                -- executes it fine (P1 toolchain inconsistency, round 122).
                -- String-only rewrite: offset/end_offset come from Cp()
                -- captures and are unaffected.
                tok.type = "command"; tok.cmd = tokenizer.normalize_cmd(t[2])
                -- one_token's Ct nesting wraps EACH param capture in its
                -- own sub-array: t[3..#t] = { {pair} } per bare/named
                -- arg (audit: the old `tok.params = t[3]` read only the
                -- FIRST param's wrapper, dropping every later argument —
                -- [set f.hp 80] lost "80" and ks_check mis-validated).
                local params = {}
                for i = 3, #t do
                    if type(t[i]) == "table" then
                        for _, p in ipairs(t[i]) do
                            if type(p) == "table" and p[1] ~= nil then
                                params[#params + 1] = p
                            end
                        end
                    end
                end
                tok.params = params
                renumber_bare_params(tok.params)
            elseif typ == "text" then
                tok.type = "text"; tok.content = t[2] or ""
            elseif typ == "label" then
                tok.type = "label"; tok.name = t[2]
            elseif typ == "iscript" then
                tok.type = "iscript"; tok.body = t[2] or ""
            end
            out[#out + 1] = tok
        end
        -- Cp() returns the position AFTER the match (index of the next
        -- char) -- advancing +1 skips a byte and a mid-line command
        -- ([ch] following text on the same line) would be re-tokenized
        -- as text (review blocking). endpos IS the next start.
        init = endpos
        if init > #ks_text then break end
    end
    return out
end

function tokenizer.parse(ks_text)
    if not ks_text or ks_text == "" then return {} end
    -- Strip BOM
    -- BOM is 3 bytes (EF BB BF). Grammar also has optional BOM prefix.
    if ks_text:byte(1)==0xEF and ks_text:byte(2)==0xBB and ks_text:byte(3)==0xBF then
        ks_text = ks_text:sub(4) end
    -- Locate the failure offset for actionable errors: try the grammar,
    -- then report where the longest successful prefix stopped.
    local raw = lpeg.match(grammar, ks_text)
    if not raw then
        -- Report the failing line via binary search on the longest
        -- successful prefix (grammar matches prefix-free up to the error).
        local lo, hi = 1, #ks_text
        while lo < hi do
            local mid = math.floor((lo + hi + 1) / 2)
            if lpeg.match(grammar, ks_text:sub(1, mid)) then
                lo = mid
            else
                hi = mid - 1
            end
        end
        local line = 1
        for i = 1, lo do
            if ks_text:byte(i) == 10 then line = line + 1 end
        end
        error(string.format("Tokenizer: parse failed near line %d (byte %d)",
                            line, lo))
    end
    return normalize(raw)
end

function tokenizer.parse_file(path)
    local f = io.open(path, "r")
    if not f then error("Tokenizer: cannot open file: " .. path) end
    -- Cap scene size (16MB) so a corrupted/giant .ks cannot exhaust the
    -- Lua memory limit and trigger the fatal-error dialog.
    f:seek("end")
    local size = f:seek("cur")
    f:seek("set")
    if size > 16 * 1024 * 1024 then
        f:close()
        error("Tokenizer: scene too large (>" .. (16 * 1024 * 1024) ..
              " bytes): " .. path)
    end
    local content = f:read("*a")
    f:close()
    return tokenizer.parse(content)
end

return tokenizer
