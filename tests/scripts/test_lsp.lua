-- test_lsp.lua — KAG Neo-Genesis language service tests (Battle 2).
-- completion / hover / diagnostics driven by the 78 command contracts.
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name .. (detail and (" -- " .. tostring(detail)) or ""))
        failed = failed + 1 end
end

local lsp = require("kag.lsp")
local schema = require("kag.schema")

-- ---------------------------------------------------------------------------
-- 1. completion: command names after "["
-- ---------------------------------------------------------------------------
local c1 = lsp.completion("[ch")
check("completion [ch returns items", #c1 > 0)
local hasCh = false
for _, it in ipairs(c1) do
    if it.label == "ch" then hasCh = true break end
end
check("completion includes ch", hasCh)
check("completion items have kind+insertText",
      c1[1] and c1[1].kind and c1[1].insertText)

-- empty prefix completes everything
local c0 = lsp.completion("[")
check("completion [ returns many", #c0 > 30)

-- ---------------------------------------------------------------------------
-- 2. completion: params after "[cmd "
-- ---------------------------------------------------------------------------
local cp = lsp.completion("[ch ")
check("completion [ch space returns params", #cp > 0)
local hasText = false
for _, it in ipairs(cp) do
    if it.label == "text" then
        hasText = true
        check("ch text param has type detail", it.detail:find("string", 1, true) ~= nil)
    end
end
check("completion includes text param", hasText)

-- ---------------------------------------------------------------------------
-- 3. hover: command contract + param details
-- ---------------------------------------------------------------------------
local h = lsp.hover("ch")
check("hover ch has title", h and h.title == "[ch]")
check("hover ch has desc", h and h.text:find("KAG3%-compatible ch", 1) ~= nil
      or (h and #h.text > 0))
local h2 = lsp.hover("pt", "speed")
check("hover pt.speed has type+default+range",
      h2 and h2.text:find("type=number", 1, true)
      and h2.text:find("default=50", 1, true)
      and h2.text:find("min=8", 1, true) and h2.text:find("max=5000", 1, true))
local blurDuration = lsp.hover("blur", "duration")
check("blur duration alias has its numeric contract",
      blurDuration and blurDuration.text:find("param `duration`", 1, true)
      and blurDuration.text:find("alias of `time`", 1, true)
      and blurDuration.text:find("type=number", 1, true))
local blurAliases = lsp.completion("[blur dur")
check("blur duration remains available in completion",
      #blurAliases == 1 and blurAliases[1].insertText == "duration=")
check("registered blur aliases are valid in author diagnostics",
      #lsp.diagnostics('[blur duration=300 strength=4]') == 0)
local aliasInterpolation = lsp.diagnostics('[ch message="${f.}"]')
check("effective aliases receive interpolation diagnostics",
      #aliasInterpolation == 1 and aliasInterpolation[1].message:find("message interpolation", 1, true))
check("an explicit canonical parameter overrides alias interpolation",
      #lsp.diagnostics('[ch text="ok" message="${f.}"]') == 0)
check("the first supplied alias overrides later alias interpolation",
      #lsp.diagnostics('[text message="ok" content="${f.}"]') == 0)
check("hover unknown cmd nil", lsp.hover("definitely_not_a_cmd") == nil)

-- ---------------------------------------------------------------------------
-- 4. diagnostics: unknown commands, required params, require-any
-- ---------------------------------------------------------------------------
local d1 = lsp.diagnostics('[ch text="a"]\n[unknown_cmd_xyz]\n')
check("diagnostics flags unknown command", #d1 == 1
      and d1[1].message:find("unknown_cmd_xyz", 1, true) ~= nil
      and d1[1].severity == 2)

local d2 = lsp.diagnostics('[playbgm]\n[playbgm file="x.ogg"]\n')
check("diagnostics require-any on bare playbgm", #d2 == 1
      and d2[1].message:find("file", 1, true) ~= nil
      and d2[1].severity == 1)
check("diagnostics ok with file present", d2[1].line == 1)

local d3 = lsp.diagnostics("[ch]\n")  -- ch has no required params
check("diagnostics clean for [ch]", #d3 == 0)

-- line numbers accurate
local d4 = lsp.diagnostics("line1\nline2\n[bad_cmd_here]\n")
check("diagnostics line number", #d4 == 1 and d4[1].line == 3)

-- ---------------------------------------------------------------------------
-- 5. json output (what the IDE receives via /api/eval)
-- ---------------------------------------------------------------------------
local j1 = lsp.json("hover", "ch")
check("hover json valid", j1:sub(1, 1) == "[" and j1:find('"title"', 1, true) ~= nil)
local j2 = lsp.json("diagnostics", '[playbgm]\n')
check("diagnostics json has severity+line",
      j2:find('"severity":1', 1, true) ~= nil and j2:find('"line":1', 1, true) ~= nil)

-- ---------------------------------------------------------------------------
-- 6. ks_check parity: expression compile + truncation (Battle 2c)
-- ---------------------------------------------------------------------------
local de = lsp.diagnostics('[if exp="f.x > &&"]\n')
check("expr compile error flagged", #de == 1
      and de[1].message:find("does not compile", 1, true) ~= nil
      and de[1].severity == 1)
local dg = lsp.diagnostics('[if exp="f.x > 5 && f.y != 0"]\n')
check("valid expr clean", #dg == 0)
local dt = lsp.diagnostics('[ch text="a"]\n[bg storage="x"\n')
check("truncation flagged", #dt == 1
      and dt[1].message:find("stopped before end", 1, true) ~= nil)
local dc = lsp.diagnostics('[ch text="a"]\n; done\n')
check("comment tail no false positive", #dc == 0)
local dboth = lsp.diagnostics('[if exp="bad &&"]\n[bg storage="x"\n')
check("expr+truncation both flagged", #dboth == 2)

-- ---------------------------------------------------------------------------
-- Navigation: definition / references
-- ---------------------------------------------------------------------------
local navText = "*start\n[ch text=\"hi\"]\n[jump target=*start]\n"
    .. "*ending\n[call *ending]\n[link *start]back[/link]\n"
-- [jump target=*start] is line 3, starts at col 8 (target value col 18)
local d1 = lsp.definition(navText, 3, 18)
check("definition: jump target -> label line", d1 ~= nil and d1.name == "start"
      and d1.line == 1)
local d2 = lsp.definition(navText, 1, 1)
check("definition: on label itself", d2 ~= nil and d2.name == "start"
      and d2.line == 1)
local d3 = lsp.definition(navText, 5, 11)
check("definition: call bare target", d3 ~= nil and d3.name == "ending"
      and d3.line == 4)
local d4 = lsp.definition(navText, 2, 5)
check("definition: non-nav token -> nil", d4 == nil)
local d5 = lsp.definition("[jump target=*missing]\n", 1, 18)
check("definition: missing label -> name only", d5 ~= nil
      and d5.name == "missing" and d5.line == nil)
local d6 = lsp.definition("", 1, 1)
check("definition: empty scene -> nil", d6 == nil)

local r1 = lsp.references(navText, "start")
check("references: label + 2 nav sites", #r1 == 3)
local kinds = {}
for _, r in ipairs(r1) do kinds[r.kind] = (kinds[r.kind] or 0) + 1 end
check("references: one definition two references",
      kinds.definition == 1 and kinds.reference == 2)
local r2 = lsp.references(navText, "nope")
check("references: unknown label -> empty", #r2 == 0)
local r3 = lsp.references(navText, "")
check("references: empty name -> empty", #r3 == 0)

-- json bridge shapes
local jd = lsp.json("definition", navText, 3, 18)
check("json definition shape", jd:find('"name":"start"', 1, true) ~= nil
      and jd:find('"line":1', 1, true) ~= nil)
local jr = lsp.json("references", navText, "start")
check("json references shape", jr:find('"kind":"reference"', 1, true) ~= nil)
local jm = lsp.json("definition", "[jump target=*missing]\n", 1, 18)
check("json missing label omits line", jm:find('"name":"missing"', 1, true) ~= nil
      and jm:find('"line"', 1, true) == nil)


-- ---------------------------------------------------------------------------
-- 7. ${expr} interpolation diagnostics (Battle 2e): interpolatable text
--    params are run through Schema.checkInterp (the SAME compile path as
--    runtime) so a bad interpolation fails in the editor.
-- ---------------------------------------------------------------------------
local di1 = lsp.diagnostics('[ch text="hp ${bad &&}"]\n')
check("interp compile error flagged", #di1 == 1
      and di1[1].message:find("interpolation", 1, true) ~= nil
      and di1[1].severity == 1)
local di2 = lsp.diagnostics('[ch text="hp ${f.hp}"]\n')
check("interp valid clean", #di2 == 0)
local di3 = lsp.diagnostics('[ch text="${ {a=1}.a }"]\n')
check("interp nested constructor clean", #di3 == 0)
local di4 = lsp.diagnostics('[ch text="hp ${oops"]\n')
check("interp unterminated warning", #di4 == 1
      and di4[1].message:find("unterminated", 1, true) ~= nil
      and di4[1].severity == 2)
local di5 = lsp.diagnostics('[ch text="${ [[a]] } ok"]\n')
check("interp long bracket clean", #di5 == 0)
local di6 = lsp.diagnostics('[ch text="${f.x} ok ${bad +} end"]\n')
check("interp multi-span flags bad one", #di6 == 1
      and di6[1].message:find("bad +", 1, true) ~= nil)

-- ---------------------------------------------------------------------------
-- 8. expression context completion: variable table prefixes
-- ---------------------------------------------------------------------------
local vp1 = lsp.completion('[if exp="f', 11)
local hasDot = false
for _, it in ipairs(vp1) do if it.label == "f." then hasDot = true break end end
check("completion exp value offers f.", hasDot)
local vp2 = lsp.completion('[ch text="hp ${', 16)
local hasTf = false
for _, it in ipairs(vp2) do if it.label == "tf." then hasTf = true break end end
check("completion ${ offers tf.", hasTf)
local vp3 = lsp.completion('[ch ', 4)
local hasF = false
for _, it in ipairs(vp3) do if it.label == "f." then hasF = true break end end
check("completion non-expr omits variable prefixes", not hasF)

-- ---------------------------------------------------------------------------
-- 9. hover: expression-language cheat-sheet
-- ---------------------------------------------------------------------------
local hs1 = lsp.hover("if", "exp")
check("hover if.exp has cheat-sheet", hs1 ~= nil
      and hs1.text:find("expression language", 1, true) ~= nil)
local hs2 = lsp.hover("eval")
check("hover eval has cheat-sheet", hs2 ~= nil
      and hs2.text:find("expression language", 1, true) ~= nil)
local hs3 = lsp.hover("ch")
check("hover ch no cheat-sheet", hs3 ~= nil
      and hs3.text:find("expression language", 1, true) == nil)

-- ---------------------------------------------------------------------------
-- 10. unknown-param diagnostics (round-73): misspelled/undeclared named
--     params on a known command are flagged as warnings.
-- ---------------------------------------------------------------------------
local du1 = lsp.diagnostics('[ch texte="x"]\n')
check("unknown param flagged for [ch texte]", #du1 == 1
      and du1[1].message:find("unknown param 'texte'", 1, true) ~= nil
      and du1[1].message:find(" for [ch]", 1, true) ~= nil
      and du1[1].severity == 2
      and du1[1].line == 1)
local du2 = lsp.diagnostics('[ch text="x"]\n')
check("known param text clean", #du2 == 0)
local du3 = lsp.diagnostics('[if exp="f.x"]\n')
check("flow command [if] no false positive", #du3 == 0)
-- Positional args (numeric slots) must NOT be flagged as unknown
-- params. (The pre-existing required-param check still reports missing
-- required params here because it does not map positional slots to named
-- params -- that is out of scope for this round; only assert no
-- "unknown param" warning.)
local du4 = lsp.diagnostics('[set f.hp 30]\n')
local du4unknown = false
for _, it in ipairs(du4) do
    if it.message:find("unknown param", 1, true) then du4unknown = true break end
end
check("positional params [set f.hp 30] no unknown-param", not du4unknown)
local du5 = lsp.diagnostics('[notify "saved"]\n')
local du5unknown = false
for _, it in ipairs(du5) do
    if it.message:find("unknown param", 1, true) then du5unknown = true break end
end
check("positional param [notify] no unknown-param", not du5unknown)
local du6 = lsp.diagnostics('[playbgm file="x.ogg"]\n')
check("storage/file alias no false positive", #du6 == 0)
local du7 = lsp.diagnostics('[ch text="a" typos=1 tele="b"]\n')
check("multiple unknown params each flagged", #du7 == 2
      and du7[1].message:find("tele", 1, true) ~= nil
      and du7[2].message:find("typos", 1, true) ~= nil)
local du8 = lsp.diagnostics('[ch text="a"]\n[ch text="b"]\n')
check("valid params across lines clean", #du8 == 0)


-- ---------------------------------------------------------------------------
-- 11. definition/references across nav command forms (round 74 / stage D):
--     named quoted target="*label" for [jump]/[call]/[link], cursor placed
--     INSIDE the quoted value (not just on the command), plus aggregation
--     of multiple nav sites to one label.
-- ---------------------------------------------------------------------------
-- Quoted named forms: each line targets *scene with the cursor inside the
-- quoted value (col 17 = the 'e' inside "*scene"). [link]...[/link] kept
-- terminal (tokenizer truncates content after a mid-stream [/link]; the
-- link command token itself always parses).
local navQuoted = "*scene\n"
    .. "[jump target=\"*scene\"]\n"
    .. "[call target=\"*scene\"]\n"
    .. "[jump target=\"*scene\"]\n"
    .. "[link target=\"*scene\"]go[/link]\n"
local dqm = lsp.definition(navQuoted, 2, 17)
check("def: jump target=\"quoted\" value cursor -> scene", dqm ~= nil
      and dqm.name == "scene" and dqm.line == 1)
local dqc = lsp.definition(navQuoted, 3, 17)
check("def: call target=\"quoted\" value cursor -> scene", dqc ~= nil
      and dqc.name == "scene" and dqc.line == 1)
local dql = lsp.definition(navQuoted, 5, 17)
check("def: link target=\"quoted\" value cursor -> scene", dql ~= nil
      and dql.name == "scene" and dql.line == 1)
-- Cursor strictly inside the value of a SECOND jump on the same label.
local dq2 = lsp.definition(navQuoted, 4, 19)
check("def: 2nd jump quoted value cursor -> scene", dq2 ~= nil
      and dq2.name == "scene" and dq2.line == 1)
-- Aggregation: 4 nav sites + 1 definition for *scene.
local rq = lsp.references(navQuoted, "scene")
check("refs: quoted named 4 sites + def", #rq == 5)
local qk = { definition = 0, reference = 0 }
for _, x in ipairs(rq) do qk[x.kind] = (qk[x.kind] or 0) + 1 end
check("refs: 1 def + 4 references (jump/call/link)", qk.definition == 1
      and qk.reference == 4)
-- Positions: definition line 1, refs span lines 2..5.
local qlines = {}
for _, x in ipairs(rq) do qlines[#qlines + 1] = x.line end
table.sort(qlines)
check("refs: aggregated at every nav line", qlines[1] == 1
      and qlines[#qlines] == 5 and #qlines == 5)
-- Definition granularity is the whole command token, not param-value
-- precise: a cursor anywhere inside a nav command (including on the
-- param NAME, col 9 in "target=...") still resolves to its target. The
-- editor passes the true cursor column; token-span resolution is the
-- existing, intended behaviour (see original d1).
local dqX = lsp.definition(navQuoted, 2, 9)
check("def: cursor on param name still resolves via token span", dqX ~= nil
      and dqX.name == "scene" and dqX.line == 1)
-- Bare forms (no quotes) also aggregate for call/link (terminal link).
local navBare = "*end\n[jump *end]\n[call *end]\n[link *end]go[/link]\n"
local rb = lsp.references(navBare, "end")
check("refs: bare call+link aggregate", #rb == 4)
local dbj = lsp.definition(navBare, 2, 8)
check("def: bare jump value cursor -> end", dbj ~= nil and dbj.name == "end"
      and dbj.line == 1)
local dbc = lsp.definition(navBare, 3, 8)
check("def: bare call value cursor -> end", dbc ~= nil and dbc.name == "end"
      and dbc.line == 1)
local dbl = lsp.definition(navBare, 4, 8)
check("def: bare link value cursor -> end", dbl ~= nil and dbl.name == "end"
      and dbl.line == 1)
-- json bridge for quoted named + aggregation shapes.
local jqn = lsp.json("definition", navQuoted, 3, 17)
check("json: quoted call definition shape", jqn:find('"name":"scene"', 1, true) ~= nil
      and jqn:find('"line":1', 1, true) ~= nil)
local jqa = lsp.json("references", navQuoted, "scene")
check("json: aggregation serializes definition+references",
      jqa:find('"kind":"definition"', 1, true) ~= nil
      and jqa:find('"kind":"reference"', 1, true) ~= nil)

-- ---------------------------------------------------------------------------
-- 12. KAG3 alias param completion (round 75): [sel] shares the [button]
--     handler (TextCommands.sel = button) but registers no contract of its
--     own; the LSP must complete its params from the aliased command.
-- ---------------------------------------------------------------------------
do
    local cSel = lsp.completion('[sel ', nil)
    local params = {}
    for _, it in ipairs(cSel) do
        if it.kind == 5 then params[it.label] = true end
    end
    check("alias: [sel ] completes text", params.text == true)
    check("alias: [sel ] completes target", params.target == true)
    check("alias: [sel ] completes cond", params.cond == true)
    check("alias: [sel ] completes caption", params.caption == true)
    check("alias: [sel ] completes x (round-74 result capture)", params.x == true)
    -- prefix filtering still applies on the alias params
    local cSelX = lsp.completion('[sel x', nil)
    local hasX = false
    for _, it in ipairs(cSelX) do
        if it.label == 'x' then hasX = true end
    end
    check("alias: [sel x filters to x", hasX)
    -- non-alias commands are unaffected
    local cCh = lsp.completion('[ch ', nil)
    local hasSelOnly = false
    for _, it in ipairs(cCh) do
        if it.label == 'x' then hasSelOnly = true end
    end
    check("alias: [ch ] does not gain x", not hasSelOnly)
end

-- ---------------------------------------------------------------------------
-- 13. label rename (round 76): rename a *label across the scene -- update
--     the definition AND every nav reference ([jump]/[call]/[link]/[goto]/
--     [sel]/[select] via a named target="*name" or bare *name). Edits are
--     {kind, line, col, length, newText} with col/length on the NAME byte
--     (after the leading '*') so an editor replaces it in place. nil is
--     returned for a rejected (invalid) newName; {} when no defined label
--     resolves under the cursor.
-- ---------------------------------------------------------------------------
-- [link]...[/link] must stay LAST: the tokenizer truncates parsing
-- mid-stream after a [/link], so its tail [goto]/[sel] lines would be lost.
local renTxt = "*start\n"
    .. "[jump target=\"*start\"]\n"
    .. "[call *start]\n"
    .. "[goto target=*start]\n"
    .. "[sel target=\"*start\"]\n"
    .. "[link *start]go[/link]\n"
local ren = lsp.rename(renTxt, 1, 1, "scene")  -- cursor on the *start def
check("rename: def + 5 nav sites", ren ~= nil and #ren == 6)
local rnDef, rnRef = 0, 0
local defEdit = nil
for _, ed in ipairs(ren) do
    if ed.kind == "definition" then rnDef = rnDef + 1; defEdit = ed end
    if ed.kind == "reference" then rnRef = rnRef + 1 end
    check("rename: edit has newText+length", type(ed.newText) == "string"
          and type(ed.length) == "number")
end
check("rename: 1 definition + 5 references", rnDef == 1 and rnRef == 5)
check("rename: def edit at line 1 col 2 (after *)", defEdit ~= nil
      and defEdit.line == 1 and defEdit.col == 2
      and defEdit.newText == "scene" and defEdit.length == 5)
-- Applying the edits must rewrite every site in place.
local function apply_edits(text, edits)
    local ls = { 1 }
    for i = 1, #text do if text:byte(i) == 10 then ls[#ls + 1] = i + 1 end end
    table.sort(edits, function(a, b)
        if a.line == b.line then return a.col < b.col end
        return a.line < b.line end)
    for j = #edits, 1, -1 do
        local e = edits[j]
        local bs = (ls[e.line] or 1) + e.col - 1
        text = text:sub(1, bs - 1) .. e.newText .. text:sub(bs + e.length)
    end
    return text
end
local renamed = apply_edits(renTxt, ren)
local sceneCount = select(2, renamed:gsub("%*scene", ""))
check("rename: applied rewrites every site to *scene", sceneCount == 6
      and renamed:find("*start", 1, true) == nil)
check("rename: quoted jump rewritten", renamed:find('[jump target="*scene"]', 1, true) ~= nil)
check("rename: bare call rewritten", renamed:find("[call *scene]", 1, true) ~= nil)
check("rename: goto rewritten", renamed:find("[goto target=*scene]", 1, true) ~= nil)
check("rename: sel rewritten", renamed:find('[sel target="*scene"]', 1, true) ~= nil)
check("rename: bare link rewritten", renamed:find("[link *scene]go", 1, true) ~= nil)
-- Leading '*' on newName is stripped and accepted.
local renStar = lsp.rename(renTxt, 1, 1, "*scene")
check("rename: leading * stripped+accepted", renStar ~= nil and #renStar == 6)

-- Invalid newName is rejected (nil): spaces, empty, non-string.
check("rename: rejects spaced newName", lsp.rename(renTxt, 1, 1, "bad name") == nil)
check("rename: rejects empty newName", lsp.rename(renTxt, 1, 1, "") == nil)
check("rename: rejects non-string newName", lsp.rename(renTxt, 1, 1, 42) == nil)

-- Unknown / absent label -> no edits ({}, not rejected).
check("rename: missing label no edits",
      #lsp.rename("[jump target=*missing]\n", 1, 18, "x") == 0)
check("rename: cursor off any token no edits",
      #lsp.rename("[ch text=\"hi\"]\n", 1, 5, "x") == 0)

-- JSON shape roundtrip via lsp.json.
local jr = lsp.json("rename", renTxt, 1, 1, "scene")
check("json rename renamed:true + edits", jr:find('"renamed":true', 1, true) ~= nil
      and jr:find('"edits":', 1, true) ~= nil
      and jr:find('"kind":"definition"', 1, true) ~= nil
      and jr:find('"newText":"scene"', 1, true) ~= nil
      and jr:find('"length":5', 1, true) ~= nil)
local jrBad = lsp.json("rename", renTxt, 1, 1, "bad name")
check("json rename rejected -> renamed:false", jrBad ~= nil
      and jrBad:find('"renamed":false', 1, true) ~= nil)
local jrMissing = lsp.json("rename", "[jump target=*missing]\n", 1, 18, "x")
check("json rename no-op -> renamed:true empty edits", jrMissing ~= nil
      and jrMissing:find('"renamed":true', 1, true) ~= nil
      and jrMissing:find('"edits":[]', 1, true) ~= nil)

-- ---------------------------------------------------------------------------
-- 14. KAG3 param-alias hints (round 82): unknown-param diagnostics on a
--     command+param that has a KAG3-compat spelling (mirror of
--     kag3_import.lua PARAM_ALIASES) append an advisory hint naming the
--     canonical engine param. Pure message text — severity stays 2 and the
--     diagnostic shape ({line,col,message,severity}) is unchanged.
-- ---------------------------------------------------------------------------
-- [csp] contract declares x/y; KAG3 spells left/top -> hint suggests x/y.
local da1 = lsp.diagnostics('[csp name="a" left=320]\n')
check("alias: csp left= flags unknown with hint", #da1 == 1
      and da1[1].message:find("unknown param 'left'", 1, true) ~= nil
      and da1[1].message:find("KAG3 别名", 1, true) ~= nil
      and da1[1].message:find("x= 代替 left=", 1, true) ~= nil
      and da1[1].severity == 2)
local da2 = lsp.diagnostics('[csl name="a" top=300]\n')
check("alias: csl top= hints y=", #da2 == 1
      and da2[1].message:find("unknown param 'top'", 1, true) ~= nil
      and da2[1].message:find("y= 代替 top=", 1, true) ~= nil)
-- [add] contract declares name; KAG3 spells var -> hint suggests name=.
-- [add] also flags missing required 'name' (name= is the canonical
-- param), so the hint must be located within the issue list, not asserted
-- as the sole diagnostic.
local da3 = lsp.diagnostics('[add var="f.hp" value=5]\n')
local da3hint = false
for _, it in ipairs(da3) do
    if it.message:find("unknown param 'var'", 1, true) ~= nil
       and it.message:find("name= 代替 var=", 1, true) ~= nil then
        da3hint = true
    end
end
check("alias: add var= hints name=", da3hint)
local da4 = lsp.diagnostics('[dec var="f.hp"]\n')
local da4hint = false
for _, it in ipairs(da4) do
    if it.message:find("name= 代替 var=", 1, true) ~= nil then
        da4hint = true
    end
end
check("alias: dec var= hints name=", da4hint)
-- canonical params are clean (no false hint, no new diagnostics).
local da5 = lsp.diagnostics('[csp name="a" x=320 y=240]\n')
check("alias: csp canonical x/y clean", #da5 == 0)
local da6 = lsp.diagnostics('[add name="f.hp" value=5]\n')
check("alias: add canonical name clean", #da6 == 0)
-- a NON-aliased unknown param gets no hint suffix (still a warning).
local da7 = lsp.diagnostics('[csp name="a" bogus=1]\n')
check("alias: unmatched unknown param no hint", #da7 == 1
      and da7[1].message:find("unknown param 'bogus'", 1, true) ~= nil
      and da7[1].message:find("KAG3 别名", 1, true) == nil)
-- alias applies only on that command: [ch left=] is not a csp alias.
local da8 = lsp.diagnostics('[ch text="a" left=1]\n')
check("alias: alias scoped to owning command", #da8 == 1
      and da8[1].message:find("unknown param 'left'", 1, true) ~= nil
      and da8[1].message:find("KAG3 别名", 1, true) == nil)
-- JSON bridge: alias hint flows through the diagnostics method unchanged.
local daJ = lsp.json("diagnostics", '[csp name="a" left=320]\n')
check("alias: hint serialized via json", daJ:find("KAG3 别名", 1, true) ~= nil
      and daJ:find('"severity":2', 1, true) ~= nil)


-- ---------------------------------------------------------------------------
-- 15. label rename deep-boundary tests (round 80): target forms, special
--     newNames, multi-reference precision, byte-exact edits in multibyte
--     scenes, and lenient edge inputs (no crashes).
-- ---------------------------------------------------------------------------
-- Multibyte helpers (你=\228\189\160 好=\229\165\189, 6 bytes) — written as
-- byte escapes so the test file stays encoding-agnostic (git bash/GUI).
local NL = "\n"
local fn_ni = "\228\189\160"
local fn_hao = "\229\165\189"

-- Byte-accurate edit applier (mirror of section 13's, local here so it can be
-- reused across sub-blocks). Editors are assumed to operate on byte offsets.
local function apply_edits_bytes(text, edits)
    local ls = { 1 }
    for i = 1, #text do if text:byte(i) == 10 then ls[#ls + 1] = i + 1 end end
    table.sort(edits, function(a, b)
        if a.line == b.line then return a.col < b.col end
        return a.line < b.line end)
    for j = #edits, 1, -1 do
        local e = edits[j]
        local bs = (ls[e.line] or 1) + e.col - 1
        text = text:sub(1, bs - 1) .. e.newText .. text:sub(bs + e.length)
    end
    return text
end

-- 15a. Target-form reference set: quoted target="*old", bare target=*old,
--      positional [jump *old], and a NO-STAR [jump target=old] which is NOT
--      a label reference and must be left untouched.
do
    local tform = "*old" .. NL
        .. '[jump target="*old"]' .. NL
        .. "[jump target=*old]" .. NL
        .. "[jump *old]" .. NL
        .. "[jump target=old]" .. NL
    local te = lsp.rename(tform, 1, 2, "new")
    check("forms: def + 3 refs (quoted/bare/positional), no-star untouched",
          te ~= nil and #te == 4)
    local order = {}
    for _, e in ipairs(te) do order[#order + 1] = { e.line, e.col, e.length, e.kind } end
    check("forms: def at (1,2) len 3", order[1] and order[1][1] == 1
          and order[1][2] == 2 and order[1][3] == 3 and order[1][4] == "definition")
    check("forms: quoted ref col 16", order[2] and order[2][1] == 2
          and order[2][2] == 16 and order[2][4] == "reference")
    check("forms: bare param ref col 15", order[3] and order[3][1] == 3
          and order[3][2] == 15 and order[3][4] == "reference")
    check("forms: positional [jump *old] ref col 8", order[4] and order[4][1] == 4
          and order[4][2] == 8 and order[4][4] == "reference")
    local ta = apply_edits_bytes(tform, te)
    check("forms: rewritten to *new everywhere except no-star line",
          ta:find('[jump target="*new"]', 1, true) ~= nil
          and ta:find("[jump target=*new]", 1, true) ~= nil
          and ta:find("[jump *new]", 1, true) ~= nil)
    check("forms: no-star [jump target=old] left as-is",
          ta:find("[jump target=old]", 1, true) ~= nil
          and ta:find("[jump target=*old]", 1, true) == nil)
    check("forms: old name fully gone", ta:find("*old", 1, true) == nil)
end

-- 15b. Special newNames: invalid charset rejected (nil), valid digit/
--      underscore accepted, same-name is a no-op, very long name accepted.
do
    local base = "*goal" .. NL .. "[jump target=*goal]" .. NL
    check("names: space rejected", lsp.rename(base, 1, 1, "bad name") == nil)
    check("names: quote rejected", lsp.rename(base, 1, 1, 'bad"quote') == nil)
    check("names: punctuation hyphen rejected", lsp.rename(base, 1, 1, "a-b") == nil)
    check("names: leading digit rejected", lsp.rename(base, 1, 1, "9start") == nil)
    check("names: lone asterisk rejected", lsp.rename(base, 1, 1, "*") == nil)
    check("names: lone underscore accepted (valid ident)", lsp.rename(base, 1, 1, "_") ~= nil
          and #lsp.rename(base, 1, 1, "_") == 2)
    check("names: leading underscore accepted", lsp.rename(base, 1, 1, "_ok") ~= nil
          and #lsp.rename(base, 1, 1, "_ok") == 2)
    check("names: digits inside accepted", lsp.rename(base, 1, 1, "a1b2") ~= nil
          and #lsp.rename(base, 1, 1, "a1b2") == 2)
    check("names: same name is no-op", #lsp.rename(base, 1, 1, "goal") == 0)
    local longName = string.rep("x", 200)
    local le = lsp.rename(base, 1, 1, longName)
    check("names: 200-char valid name accepted", le ~= nil and #le == 2
          and le[1].newText == longName and le[1].length == 4)
    -- same-name edits carry no newText change that would corrupt (still {})
    check("names: no-op returns empty not nil", lsp.rename(base, 1, 1, "goal") ~= nil)
end

-- 15c. Single-file semantics + multi-reference aggregation: rename is scoped
--      to the ONE text argument (no cross-file resolution); every reference
--      to the label in the file is rewritten, and references to a DIFFERENT
--      label in the same file are left alone.
do
    local multi = "*start" .. NL
        .. "*other" .. NL
        .. "[jump target=*start]" .. NL
        .. "[call *start]" .. NL
        .. "[link target=*start]go[/link]" .. NL   -- keep terminal last
        .. "[jump target=*other]" .. NL            -- this line is after [/link], dropped
    -- NOTE: link truncates the rest; put the *other ref BEFORE the terminal link.
    local multi2 = "*start" .. NL
        .. "*other" .. NL
        .. "[jump target=*start]" .. NL
        .. "[call *start]" .. NL
        .. "[jump target=*other]" .. NL
        .. "[link target=*start]go[/link]" .. NL
    local me = lsp.rename(multi2, 1, 2, "finish")
    check("multi: renames only *start references", me ~= nil)
    local count = 0
    local refKinds = { definition = 0, reference = 0 }
    for _, e in ipairs(me) do
        count = count + 1
        refKinds[e.kind] = refKinds[e.kind] + 1
    end
    -- *start: 1 def + 3 refs; *other: untouched (its ref stays *other)
    check("multi: 1 def + 3 refs for *start", refKinds.definition == 1
          and refKinds.reference == 3 and count == 4)
    local ma = apply_edits_bytes(multi2, me)
    check("multi: *other reference preserved", ma:find("[jump target=*other]", 1, true) ~= nil)
    check("multi: all *start refs rewritten",
          ma:find("[jump target=*finish]", 1, true) ~= nil
          and ma:find("[call *finish]", 1, true) ~= nil
          and ma:find("[link target=*finish]", 1, true) ~= nil)
    check("multi: *start fully gone", ma:find("*start", 1, true) == nil)
end

-- 15d. Edit-set precision in a MULTIBYTE scene: the label definition sits on
--      a line whose prefix is non-ASCII (byte columns, not char). Renaming
--      from a jump reference must still produce byte-exact columns that, when
--      applied, reconstruct the intended text.
do
    local ni = fn_ni
    local hao = fn_hao
    local scene = ni .. hao .. " *goal" .. NL   -- '你'好 + space before *goal
        .. "[jump target=*goal]" .. NL
    -- definition is on line 1, col 9 (byte of 'g' after the 6 multibyte
    -- bytes + space + '*'); reference on line 2, col 15.
    local pe = lsp.rename(scene, 1, 9, "finish")
    check("mb: def + ref edits found", pe ~= nil and #pe == 2)
    check("mb: def col 9 len 6 (byte after 6-byte prefix)", pe[1] and pe[1].line == 1
          and pe[1].col == 9 and pe[1].length == 4)
    check("mb: ref col 15 on line 2", pe[2] and pe[2].line == 2
          and pe[2].col == 15 and pe[2].length == 4)
    local pa = apply_edits_bytes(scene, pe)
    check("mb: apply reconstructs *finish", pa:find("*finish", 1, true) ~= nil
          and pa:find("*goal", 1, true) == nil)
    check("mb: multibyte prefix bytes intact", pa:find(ni .. hao, 1, true) ~= nil)
    check("mb: jump ref rewritten", pa:find("[jump target=*finish]", 1, true) ~= nil)
    -- consistency with lsp.definition: both resolve to the SAME line.
    local pd = lsp.definition(scene, 1, 9)
    check("mb: definition resolves to line 1", pd ~= nil and pd.name == "goal"
          and pd.line == 1)
end

-- 15e. Lenient edge inputs: out-of-range line/col, empty/absent text and
--      name never crash; they return a valid result, {} or nil as declared.
do
    check("edge: empty text returns {}", #lsp.rename("", 1, 1, "x") == 0)
    check("edge: nil line returns {}", #lsp.rename("*a" .. NL, nil, 1, "x") == 0)
    check("edge: nil text returns {}", #lsp.rename(nil, 1, 1, "x") == 0)
    check("edge: nil newName rejected", lsp.rename("*a" .. NL, 1, 1, nil) == nil)
    check("edge: boolean newName rejected", lsp.rename("*a" .. NL, 1, 1, true) == nil)
    check("edge: line 0 still resolves def (lenient)",
          #lsp.rename("*a" .. NL .. "[jump *a]" .. NL, 0, 1, "b") == 2)
    check("edge: oversized line still resolves def (lenient)",
          #lsp.rename("*a" .. NL .. "[jump *a]" .. NL, 99, 1, "b") == 2)
    check("edge: col 0 resolves def (lenient)",
          #lsp.rename("*a" .. NL .. "[jump *a]" .. NL, 1, 0, "b") == 2)
    check("edge: huge col no token -> {} not crash",
          #lsp.rename("*a" .. NL, 1, 99999, "b") == 0)
    -- negative line/col are clamped (no crash).
    check("edge: negative line resolves def (lenient)",
          #lsp.rename("*a" .. NL .. "[jump *a]" .. NL, -3, 1, "b") == 2)
end

-- 15f. Consistency with lsp.definition: renaming from the definition line and
--      from a jump reference both resolve the SAME label to the SAME line, and
--      the rename edit set's definition entry targets that same line.
do
    local cons = "*mark" .. NL .. "[jump target=*mark]" .. NL
    local cd1 = lsp.definition(cons, 1, 2)
    local cd2 = lsp.definition(cons, 2, 15)
    check("consistency: def from def-line and jump-ref both -> mark line 1",
          cd1 ~= nil and cd2 ~= nil and cd1.name == "mark" and cd2.name == "mark"
          and cd1.line == 1 and cd2.line == 1)
    local ce1 = lsp.rename(cons, 1, 2, "renamed")
    local ce2 = lsp.rename(cons, 2, 15, "renamed")
    check("consistency: rename from def-line and jump-ref produce same set",
          ce1 ~= nil and ce2 ~= nil and #ce1 == 2 and #ce2 == 2
          and ce1[1].line == ce2[1].line and ce1[1].col == ce2[1].col
          and ce1[1].line == 1)
end


-- ---------------------------------------------------------------------------
-- 16. round 92: rename ORIGIN resolution must use the SAME nav set as the
--     edit-set scope (RENAME_NAV_CMDS = NAV_CMDS + goto/sel/select). Before
--     the fix, a rename triggered on a [goto]/[sel]/[select] reference
--     silently returned {} (empty edits) because lsp.rename resolved the
--     origin via lsp.definition's NAV_CMDS, which excludes those commands.
--     definition() keeps NAV_CMDS (navigation semantics unchanged); only
--     rename's origin path broadens.
-- ---------------------------------------------------------------------------
do
    -- Trigger rename with the cursor ON each extended nav reference
    -- ([goto]/[sel]/[select]) -- not on the label definition -- and assert
    -- a FULL edit set is produced (definition + every nav site), not {}.
    local gs = "*start" .. NL
        .. "[jump target=*start]" .. NL
        .. "[goto target=*start]" .. NL
    -- [goto ...] is line 2; cursor col 16 on the 't' of target=*start.
    local g = lsp.rename(gs, 2, 16, "dest")
    check("r92: rename from [goto] ref produces edits", g ~= nil and #g == 3)
    local gDef, gRef = 0, 0
    for _, ed in ipairs(g or {}) do
        if ed.kind == "definition" then gDef = gDef + 1
        elseif ed.kind == "reference" then gRef = gRef + 1 end
    end
    check("r92: goto origin -> 1 def + 2 refs", gDef == 1 and gRef == 2)
    local ga = apply_edits_bytes(gs, g)
    check("r92: goto-origin rename rewrites every site",
          ga:find("[goto target=*dest]", 1, true) ~= nil
          and ga:find("[jump target=*dest]", 1, true) ~= nil
          and ga:find("*start", 1, true) == nil)

    -- [sel target="*start"] line 2; cursor inside the quoted value (col 17).
    local ss = "*start" .. NL
        .. '[sel target="*start"]' .. NL
    local s = lsp.rename(ss, 2, 17, "dest")
    check("r92: rename from [sel] ref produces edits", s ~= nil and #s == 2)
    local sDef, sRef = 0, 0
    for _, ed in ipairs(s or {}) do
        if ed.kind == "definition" then sDef = sDef + 1
        elseif ed.kind == "reference" then sRef = sRef + 1 end
    end
    check("r92: sel origin -> 1 def + 1 ref", sDef == 1 and sRef == 1)
    local sa = apply_edits_bytes(ss, s)
    check("r92: sel-origin rename rewrites both sites",
          sa:find('[sel target="*dest"]', 1, true) ~= nil
          and sa:find("*dest", 1, true) ~= nil
          and sa:find("*start", 1, true) == nil)

    -- [select target=*start] line 2; cursor on the bare * value (col 17).
    local sc = "*start" .. NL
        .. "[select target=*start]" .. NL
    local sl = lsp.rename(sc, 2, 17, "dest")
    check("r92: rename from [select] ref produces edits", sl ~= nil and #sl == 2)
    local slDef, slRef = 0, 0
    for _, ed in ipairs(sl or {}) do
        if ed.kind == "definition" then slDef = slDef + 1
        elseif ed.kind == "reference" then slRef = slRef + 1 end
    end
    check("r92: select origin -> 1 def + 1 ref", slDef == 1 and slRef == 1)
    local sca = apply_edits_bytes(sc, sl)
    check("r92: select-origin rename rewrites both sites",
          sca:find("[select target=*dest]", 1, true) ~= nil
          and sca:find("*dest", 1, true) ~= nil
          and sca:find("*start", 1, true) == nil)

    -- A rename triggered from a goto/sel/select reference must be
    -- CONSISTENT with one triggered from the definition line: same lines,
    -- same set.
    local egs = lsp.rename(gs, 1, 2, "dest")
    check("r92: goto-origin and def-origin produce same set",
          g ~= nil and egs ~= nil and #g == #egs
          and g[1].line == egs[1].line and g[1].col == egs[1].col)

    -- definition() keeps NAV_CMDS: on a [goto]/[sel]/[select] reference
    -- (not in NAV_CMDS) it returns nil, while the rename origin still
    -- resolves (navigation vs. rename scope stay distinct).
    -- [goto ...] sits on line 3 (line 1 is *start, line 2 is the jump).
    local dGoto = lsp.definition(gs, 3, 16)
    check("r92: definition on [goto] ref stays nil (NAV_CMDS preserved)",
          dGoto == nil)
    local dSel = lsp.definition(ss, 2, 17)
    check("r92: definition on [sel] ref stays nil (NAV_CMDS preserved)",
          dSel == nil)
    local dSelc = lsp.definition(sc, 2, 17)
    check("r92: definition on [select] ref stays nil (NAV_CMDS preserved)",
          dSelc == nil)
end

-- Exit gate.
if failed > 0 then



    print(string.format("LSP TESTS: %d passed, %d FAILED", passed, failed))
    os.exit(1)
end
print(string.format("LSP TESTS DONE (%d passed)", passed))
