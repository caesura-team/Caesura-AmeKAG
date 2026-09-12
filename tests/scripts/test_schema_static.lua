-- U19 preflight must validate contracts without evaluating author strings.
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

local Schema = require("kag.schema")
local passed, failed = 0, 0
local function check(name, condition, detail)
    if condition then
        passed = passed + 1
        print("  [PASS] " .. name)
    else
        failed = failed + 1
        print("  [FAIL] " .. name .. (detail and (" -- " .. tostring(detail)) or ""))
    end
end

Schema.define("_u19_static_text", {
    text = { type = "string", required = true, interpolate = true, positional_index = 1 },
})
Schema.define("_u19_static_default", {
    text = { type = "string", default = "${f.probe()}", interpolate = true },
})

-- The old ks_check path is observable using the real runtime coerce function.
-- No replacement evaluator, filesystem effect or host global is needed.
local calls = 0
local ctx = { current_scene = "static.ks", token_index = 7, f = {
    probe = function() calls = calls + 1; return "observed" end,
} }
local expression = "${f.probe()}"
local runtime = Schema.coerce("_u19_static_text", { text = expression }, ctx)
check("runtime baseline executes the author callback once", calls == 1 and runtime.text == "observed")
check("static API is provided", type(Schema.validate_static) == "function")

calls = 0
local ok, result = pcall(Schema.validate_static, "_u19_static_text", { text = expression }, ctx)
check("static validation preserves a cached expression without calling it",
    ok and result.text == expression and calls == 0, result)

calls = 0
ok, result = pcall(Schema.validate_static, "_u19_static_text", { expression }, ctx)
check("positional interpolation stays raw in named and positional slots",
    ok and result.text == expression and result[1] == expression and calls == 0, result)

calls = 0
ok, result = pcall(Schema.validate_static, "_u19_static_default", {}, ctx)
check("default interpolation stays raw without calling author code",
    ok and result.text == expression and calls == 0, result)

calls = 0
local cold = "before ${f.probe() .. '-cold'} after"
ok, result = pcall(Schema.validate_static, "_u19_static_text", { text = cold }, ctx)
check("uncached interpolation is compile-only", ok and result.text == cold and calls == 0, result)

-- Static reads must not traverse variable tables through either shorthand.
local lookups = 0
local variables = {}
for _, namespace in ipairs({ "f", "sf", "tf", "mp", "lf" }) do
    variables[namespace] = setmetatable({}, {
        __index = function() lookups = lookups + 1; return "value" end,
    })
end
local shorthand = "$f.key %sf.key% $tf.key %mp.key% $lf.key %macro%"
ok, result = pcall(Schema.validate_static, "_u19_static_text", { text = shorthand }, variables)
check("static shorthand remains unchanged without context lookups",
    ok and result.text == shorthand and lookups == 0, result)
runtime = Schema.coerce("_u19_static_text", { text = shorthand }, variables)
check("runtime shorthand still reads all five namespaces",
    lookups == 5 and runtime.text == "value value value value value %macro%")

for _, text in ipairs({
    "plain text", "empty ${} and ${   }", "nested ${ {a={1,2}}.a[2] }",
    "quoted ${ '}' .. f.name }", "long ${ [=[}]=] }", "TJS ${f.ready ? 'yes' : 'no'}",
}) do
    ok, result = pcall(Schema.validate_static, "_u19_static_text", { text = text }, ctx)
    check("valid interpolation syntax stays raw: " .. text,
        ok and result.text == text and #Schema.checkInterp(text) == 0, result)
end

for _, text in ipairs({ "bad ${1+}", "bad ${f.probe()", "before ${f.probe()} bad ${1+}" }) do
    local issues = Schema.checkInterp(text)
    calls = 0
    ok, result = pcall(Schema.validate_static, "_u19_static_text", { text = text }, ctx)
    check("invalid interpolation uses existing diagnostic: " .. text,
        not ok and #issues > 0 and type(result) == "string"
            and result:find(issues[1].error, 1, true) ~= nil
            and result:find(tostring(issues[1].offset), 1, true) ~= nil
            and result:find("static.ks:7", 1, true) ~= nil and calls == 0, result)
end

Schema.define("_u19_static_literal", { text = { type = "string" } })
ok, result = pcall(Schema.validate_static, "_u19_static_literal", { text = "literal ${1+}" }, ctx)
check("non-interpolated string syntax is not reinterpreted", ok and result.text == "literal ${1+}", result)
ok, result = pcall(Schema.validate_static, "_u19_static_literal", { text = 42 }, ctx)
check("literal string coercion retains existing scalar conversion", ok and result.text == "42", result)

Schema.define("_u19_static_types", {
    amount = { type = "number", min = 1, max = 5, default = "3", positional_index = 1 },
    enabled = { type = "boolean", default = false },
    mode = { type = "enum", values = { "on", "off" } },
    choice = { type = "string", choices = { "one", "two" } },
    values = { type = "list", item_type = "number" },
    file = { type = "file" },
})
ok, result = pcall(Schema.validate_static, "_u19_static_types",
    { amount = "99", enabled = "yes", mode = "on", choice = "two", values = "1, 2,3", file = "assets/test.png" }, ctx)
check("static validation retains number conversion and maximum clamp", ok and result.amount == 5, result)
check("static validation retains boolean enum choices list and file checks",
    ok and result.enabled == true and result.mode == "on" and result.choice == "two"
        and result.values[1] == 1 and result.values[3] == 3 and result.file == "assets/test.png", result)
ok, result = pcall(Schema.validate_static, "_u19_static_types", { "-2" }, ctx)
check("static positional numbers clamp in both slots", ok and result.amount == 1 and result[1] == 1, result)
ok, result = pcall(Schema.validate_static, "_u19_static_types", {}, ctx)
check("static defaults keep typed conversion", ok and result.amount == 3 and result.enabled == false, result)

local bad_values = {
    { { amount = "not-a-number" }, "expects a number" },
    { { enabled = "maybe" }, "expects boolean" },
    { { mode = "unknown" }, "must be one of" },
    { { choice = "unknown" }, "must be one of" },
    { { values = "1,no" }, "list element expects a number" },
    { { file = "" }, "file path must not be empty" },
    { { file = "../outside" }, "invalid file path" },
}
for _, case in ipairs(bad_values) do
    ok, result = pcall(Schema.validate_static, "_u19_static_types", case[1], ctx)
    check("literal error retained: " .. case[2],
        not ok and type(result) == "string" and result:find(case[2], 1, true) ~= nil, result)
end

ok, result = pcall(Schema.validate_static, "_u19_static_text", {}, ctx)
check("required params still fail with location", not ok and type(result) == "string"
    and result:find("missing required param 'text'", 1, true) ~= nil
    and result:find("static.ks:7", 1, true) ~= nil, result)
Schema.define("_u19_static_any", {
    _require_any = { "file", "storage" }, file = { type = "string" }, storage = { type = "string" },
})
ok, result = pcall(Schema.validate_static, "_u19_static_any", {}, ctx)
check("any-of requirement still fails", not ok and type(result) == "string"
    and result:find("requires one of {file,storage}", 1, true) ~= nil, result)
ok, result = pcall(Schema.validate_static, "_u19_static_any", { storage = "present" }, ctx)
check("any-of present alias still succeeds", ok and result.storage == "present", result)
Schema.define("_u19_static_bad_default", { amount = { type = "number", default = "bad" } })
ok, result = pcall(Schema.validate_static, "_u19_static_bad_default", {}, ctx)
check("invalid defaults still fail", not ok and type(result) == "string"
    and result:find("expects a number", 1, true) ~= nil, result)

local unknown = { text = "${f.probe()}" }
calls = 0
ok, result = pcall(Schema.validate_static, "_u19_static_unregistered", unknown, ctx)
check("unmigrated commands retain raw passthrough", ok and result == unknown and calls == 0, result)
local input = { text = expression, extra = "$f.key" }
ok, result = pcall(Schema.validate_static, "_u19_static_text", input, ctx)
check("static validation leaves input params unchanged",
    ok and result ~= input and result.extra == "$f.key" and input.text == expression and calls == 0, result)

Schema.define("_u19_static_metadata", {
    z = { type = "string", default = "${f.probe()}", interpolate = true },
    a = { type = "string", interpolate = true, positional_index = 1 },
    empty = { type = "string", interpolate = true },
    literal = { type = "string" },
})
calls = 0
local dynamic
ok, result, dynamic = pcall(Schema.validate_static, "_u19_static_metadata", {
    "$f.key %sf.key% ${f.probe()}", empty = "${} ${   }", literal = "${f.probe()}",
}, ctx)
check("dynamic metadata is sorted unique field names without expression bodies",
    ok and type(dynamic) == "table" and #dynamic == 2
        and dynamic[1] == "a" and dynamic[2] == "z" and calls == 0, dynamic)
ok, result, dynamic = pcall(Schema.validate_static, "_u19_static_text", { text = "${} %macro% $unknown.key $5" }, ctx)
check("empty expressions and literal shorthand-like text have no dynamic metadata",
    ok and type(dynamic) == "table" and #dynamic == 0, dynamic)
ok, result, dynamic = pcall(Schema.validate_static, "_u19_static_types", { amount = "4" }, ctx)
check("literal typed params return an empty dynamic array",
    ok and type(dynamic) == "table" and #dynamic == 0, dynamic)
ok, result, dynamic = pcall(Schema.validate_static, "_u19_static_unregistered", unknown, ctx)
check("unmigrated passthrough returns an empty dynamic array",
    ok and result == unknown and type(dynamic) == "table" and #dynamic == 0, dynamic)

Schema.define("_u19_static_dynamic_choice", {
    choice = { type = "string", interpolate = true, choices = { "one", "two" } },
})
local choice_calls = 0
local choice_context = { f = { pick = function() choice_calls = choice_calls + 1; return "one" end } }
local choice_expression = "${f.pick()}"
ok, result, dynamic = pcall(Schema.validate_static, "_u19_static_dynamic_choice", { choice = choice_expression }, choice_context)
check("dynamic string choices defer to runtime without evaluating author code",
    ok and result.choice == choice_expression and choice_calls == 0
        and type(dynamic) == "table" and #dynamic == 1 and dynamic[1] == "choice", result)
runtime = Schema.coerce("_u19_static_dynamic_choice", { choice = choice_expression }, choice_context)
check("runtime still evaluates and accepts a valid dynamic choice", runtime.choice == "one" and choice_calls == 1)
choice_context.f.pick = function() choice_calls = choice_calls + 1; return "other" end
ok, result = pcall(Schema.coerce, "_u19_static_dynamic_choice", { choice = choice_expression }, choice_context)
check("runtime still rejects an invalid resolved choice", not ok and choice_calls == 2
    and type(result) == "string" and result:find("must be one of", 1, true) ~= nil, result)
ok, result = pcall(Schema.validate_static, "_u19_static_dynamic_choice", { choice = "other" }, choice_context)
check("literal string choices remain strict in static mode", not ok and choice_calls == 2
    and type(result) == "string" and result:find("must be one of", 1, true) ~= nil, result)

calls = 0
runtime = Schema.coerce("_u19_static_text", { text = expression }, ctx)
check("runtime interpolation still executes after static success and errors",
    runtime.text == "observed" and calls == 1)
runtime = Schema.coerce("_u19_static_text", { text = "bad ${1+}" }, ctx)
check("runtime malformed interpolation still falls back verbatim", runtime.text == "bad ${1+}")
check("runtime coerce retains its single return value",
    select("#", Schema.coerce("_u19_static_types", {}, ctx)) == 1)

print(string.format("SCHEMA STATIC: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
print("ALL SCHEMA STATIC TESTS PASSED")
