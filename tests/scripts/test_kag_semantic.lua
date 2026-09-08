-- =============================================================================
--  Caesura (AmeKAG) — test_kag_semantic.lua
--  Unit tests for KAG Unified Semantic Representation & AST Layer
-- =============================================================================

local BS = string.char(92)
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
if not package.path:find(here, 1, true) then
    package.path = here .. "?.lua;" .. here .. "?/init.lua;" .. "scripts/?.lua;scripts/?/init.lua;" .. package.path
end

local semantic = require("kag.semantic")

local passed = 0
local failed = 0

local function check(cond, msg)
    if cond then
        passed = passed + 1
        print("  [PASS] " .. msg)
    else
        failed = failed + 1
        print("  [FAIL] " .. msg)
    end
end

print("=== Running KAG Semantic Layer Tests ===")

-- 1. Basic labels and commands
local ks1 = [[
*start
[bg storage="assets/bg/room.png" time=500]
[ch name="Mio" text="Hello world!"]
[p]
[jump target=*next]

*next
[ending name="happy_end"]
]]

local m1 = semantic.parse(ks1, "scene1.ks")
check(#m1.nodes == 7, "Node count equals 7 (2 labels + 5 commands)")
check(m1.labels["*start"] ~= nil, "Label *start recognized")
check(m1.labels["*next"] ~= nil, "Label *next recognized")
check(#m1.jumps == 1, "Jump count equals 1")
check(m1.jumps[1].target == "*next", "Jump target matches *next")
check(#m1.endings == 1, "Ending count equals 1")
check(m1.endings[1].name == "happy_end", "Ending name matches happy_end")

-- 2. Choice branching and Select block
local ks2 = [[
*start
[select]
[sel target=*left text="Go Left"]
[sel target=*right text="Go Right"]
[endselect]

*left
[jump target=*end]

*right
[jump target=*end]

*end
[ending]
]]

local m2 = semantic.parse(ks2, "choices.ks")
check(#m2.choices == 2, "Choice count equals 2")
check(m2.choices[1].target == "*left" and m2.choices[1].text == "Go Left", "First choice targets *left")
check(m2.choices[2].target == "*right" and m2.choices[2].text == "Go Right", "Second choice targets *right")
check(#m2.flow_graph.edges == 4, "Flow graph has 4 edges (2 choices + 2 jumps)")

-- 3. Translatable strings & deterministic hashing
local ks3 = [[
[ch name="Hero" text="Line one."]
[ch name="Hero" text="Line one."]
[notify msg="Saved!"]
]]

local m3 = semantic.parse(ks3, "trans.ks")
check(#m3.translatables == 3, "Translatables extracted (2 ch + 1 notify)")
check(m3.translatables[1].speaker == "Hero", "Speaker identified as Hero")
check(m3.translatables[3].kind == "notify", "Notify translatable identified")
-- Deterministic hashing
local m3_again = semantic.parse(ks3, "trans.ks")
check(m3.translatables[1].key == m3_again.translatables[1].key, "Translation keys are deterministic")

-- 4. Diagnostics: Broken jump target
local ks4 = [[
*start
[jump target=*nonexistent]
]]

local m4 = semantic.parse(ks4, "broken.ks")
local has_broken_error = false
for _, d in ipairs(m4.diagnostics) do
    if d.code == "BROKEN_JUMP_TARGET" then has_broken_error = true end
end
check(has_broken_error, "Broken jump target detected by diagnostics")

-- 5. Diagnostics: Unclosed select block
local ks5 = [[
*start
[select]
[sel target=*start text="Repeat"]
]]

local m5 = semantic.parse(ks5, "unclosed.ks")
local has_unclosed_error = false
for _, d in ipairs(m5.diagnostics) do
    if d.code == "UNCLOSED_BLOCK" then has_unclosed_error = true end
end
check(has_unclosed_error, "Unclosed select block detected by diagnostics")

-- 6. Diagnostics: Unreferenced (orphan) label
local ks6 = [[
*start
[ending]

*orphan_label
[ch text="Never reached."]
]]

local m6 = semantic.parse(ks6, "orphan.ks")
local has_orphan_warn = false
for _, d in ipairs(m6.diagnostics) do
    if d.code == "UNREFERENCED_LABEL" and d.message:find("orphan_label", 1, true) then
        has_orphan_warn = true
    end
end
check(has_orphan_warn, "Unreferenced orphan label detected as warning")

-- 7. Call and return
local ks7 = [[
*start
[call target=*sub]
[ending]

*sub
[ch text="In subroutine"]
[return]
]]

local m7 = semantic.parse(ks7, "sub.ks")
check(#m7.calls == 1, "Call count equals 1")
check(m7.calls[1].target == "*sub", "Call target matches *sub")

-- 8. Quoted strings with escapes
local ks8 = [[
[ch name="Author" text="He said: \"Hello world!\""]
[set var="f.msg" value='It\'s nice!']
]]

local m8 = semantic.parse(ks8, "escapes.ks")
check(m8.nodes[1].params.text == 'He said: \\"Hello world!\\"', "Quoted text with inner escape captured")
check(m8.nodes[2].params.value == "It\\'s nice!", "Quoted single-quote escape captured")

-- 9. Expressions recognition
local ks9 = [[
[if exp="f.trust > 0 && f.clues == 3"]
[ch text="True branch"]
[endif]
]]

local m9 = semantic.parse(ks9, "expr.ks")
check(m9.nodes[1].cmd == "if", "If node parsed")
check(m9.nodes[1].params.exp == "f.trust > 0 && f.clues == 3", "Expression preserved")

-- 10. Exporters: Mermaid, JSON, CSV, PO
local mermaid_str = semantic.to_mermaid(m2)
check(mermaid_str:find("flowchart TD", 1, true) ~= nil, "Mermaid flowchart generated")
check(mermaid_str:find("Go Left", 1, true) ~= nil, "Mermaid contains choice edge label")

local json_str = semantic.to_json(m1)
check(json_str:find('"file":"scene1.ks"', 1, true) ~= nil, "JSON serializer outputs valid file field")

local csv_str = semantic.to_csv(m3)
check(csv_str:find("Key,File,Line,Col,Speaker,Kind,SourceText", 1, true) ~= nil, "CSV header formatted properly")

local po_str = semantic.to_po(m3)
check(po_str:find("msgid", 1, true) ~= nil, "PO format generated with msgid")

-- 11. Compiler AST Integration
local comp = require("kag.compiler")
local compiled1 = comp.compile_from_ast(m1)
check(compiled1._compiled ~= nil, "compile_from_ast creates _compiled side-table")
check(compiled1._compiled.labels.start == 1, "compile_from_ast maps runtime label start to token index 1")

local compiled_src = comp.compile_from_source([[
*main
[ch text="Hello from source"]
[jump target=*main]
]], "test_source.ks")
check(compiled_src._compiled ~= nil, "compile_from_source creates compiled stream")
check(compiled_src._compiled.labels.main == 1, "compile_from_source maps runtime label main")

-- 12. Tooling names retain the star; bare arguments retain their runtime type.
local bare_model = semantic.parse("*entry\n[switch flag]\n[case yes]\n[endswitch]\n"
    .. "[set f.value 7 value=9]", "bare.ks")
check(bare_model.nodes[1].name == "*entry" and bare_model.labels["*entry"] ~= nil,
    "Semantic label names retain their star for tooling")
check(bare_model.nodes[2].params[1] == "flag"
    and bare_model.nodes[2].positional_params[1] == "flag"
    and bare_model.nodes[2].params.flag == nil,
    "Bare flag is a positional string, not a named boolean")
check(bare_model.nodes[5].params[1] == "f.value"
    and bare_model.nodes[5].params[2] == "7"
    and bare_model.nodes[5].params.value == "9",
    "Mixed bare and named parameters keep positions and explicit named precedence")

print(string.format("\nKAG Semantic AST Tests: %d passed, %d failed.", passed, failed))
if failed > 0 then os.exit(1) end
