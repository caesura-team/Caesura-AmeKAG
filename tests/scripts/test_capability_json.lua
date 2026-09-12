-- Standalone strict capability JSON contract. Load only this root's codec.
-- Usage: lua tests/scripts/test_capability_json.lua [source-or-staging-root]
local script = (arg and arg[0] or ""):gsub("\\", "/")
local root = (arg and arg[1]) or script:match("^(.*)/tests/scripts/[^/]+$") or "."
package.path = root .. "/scripts/?.lua"
package.cpath = ""
package.loaded.capability_json = nil

local passed, failed = 0, 0
local function check(name, condition)
    if condition then passed = passed + 1
    else failed = failed + 1; print("[FAIL] " .. name) end
end
local loaded, json = pcall(require, "capability_json")
check("codec module loads", loaded and type(json) == "table")
if not loaded then
    print(string.format("Capability JSON Tests: %d passed, %d failed", passed, failed))
    os.exit(1)
end

local function decode_ok(text, name)
    local value, reason = json.decode(text)
    check(name or "valid document decodes", value ~= nil and reason == nil)
    return value
end
local function decode_bad(text, name)
    local value, reason = json.decode(text)
    check(name, value == nil and type(reason) == "string" and #reason > 0)
    return reason
end
local function encode_ok(value, expected, name)
    local text, reason = json.encode(value)
    check(name, text == expected and reason == nil)
    return text
end
local function encode_bad(value, name)
    local text, reason = json.encode(value)
    check(name, text == nil and type(reason) == "string" and #reason > 0)
    return reason
end

local document = [=[{"title":"雪と 😀","revision":1,"enabled":true,"capabilities":{"required":["video.play"],"optional":[],"accept_approximate":["render.postfx.lut3d"]},"nested":[null,{},[]]}]=]
local value = decode_ok(document, "project and catalog-shaped nested values decode")
check("root retains object identity", json.is_object(value) and not json.is_array(value))
check("required list retains array identity", json.is_array(value.capabilities.required))
check("empty optional list is an array", json.is_array(value.capabilities.optional))
check("null array entry is not a hole", value.nested[1] == json.null and #value.nested == 3)
check("nested empty object and array remain distinct", json.is_object(value.nested[2]) and json.is_array(value.nested[3]))
local canonical = [=[{"capabilities":{"accept_approximate":["render.postfx.lut3d"],"optional":[],"required":["video.play"]},"enabled":true,"nested":[null,{},[]],"revision":1,"title":"雪と 😀"}]=]
encode_ok(value, canonical, "object keys encode in deterministic order")
local again = decode_ok(canonical, "canonical project decodes again")
encode_ok(again, canonical, "canonical roundtrip is stable")

check("JSON false is a valid non-nil result", decode_ok("false", "false decodes") == false)
check("JSON true is preserved", decode_ok("true", "true decodes") == true)
check("JSON null has module identity", decode_ok("null", "null decodes") == json.null)
check("null is not an array or object", not json.is_array(json.null) and not json.is_object(json.null))
encode_ok(json.null, "null", "null encodes")
encode_ok(json.array(), "[]", "constructed empty array encodes")
encode_ok(json.object(), "{}", "constructed empty object encodes")
encode_ok({}, "{}", "untagged empty report table is an object")
encode_ok(json.array({json.null, false, json.object()}), "[null,false,{}]", "array null and false retain their positions")
encode_ok({b=2, a=1}, '{"a":1,"b":2}', "ordinary report object sorts keys")
encode_ok({["雪"]=1, a=2, ["é"]=3}, '{"a":2,"é":3,"雪":1}', "object ordering is bytewise and deterministic")
for _, item in ipairs({{"0",0}, {"-0",0}, {"12",12}, {"-12",-12}, {"1.5",1.5}, {"1e2",100}, {"-2.5e-2",-0.025}, {"0E+4",0}}) do
    check("valid number " .. item[1], decode_ok(item[1]) == item[2])
end
check("large finite number remains finite", decode_ok("1.7976931348623157e308") < math.huge)
local integer = decode_ok("9223372036854775807", "Lua integer maximum decodes")
encode_ok(integer, "9223372036854775807", "Lua integer maximum roundtrips")
local finite = {0.1, -0.025, 1e-200, 1e200, math.pi}
for index, number in ipairs(finite) do
    local encoded, reason = json.encode(number)
    check("finite float encodes " .. index, type(encoded) == "string" and reason == nil)
    check("finite float roundtrips " .. index, decode_ok(encoded) == number)
end

local unicode = decode_ok([["\u4f60\u597d \u20ac \ud83d\ude00"]], "Unicode escapes and surrogate pair decode")
check("Unicode escape values are correct", unicode == "你好 € 😀")
encode_ok(unicode, '"你好 € 😀"', "valid UTF-8 is preserved")
check("raw and escaped UTF-8 agree", decode_ok('"你好 € 😀"') == unicode)
local escaped = decode_ok([["\"\\\/\b\f\n\r\t\u0000"]], "all standard escapes decode")
check("escapes produce their real values", escaped == '"\\/\b\f\n\r\t' .. string.char(0))
local controls = {}
for byte=0,31 do controls[#controls+1] = string.char(byte) end
controls = table.concat(controls)
local encoded_controls, control_reason = json.encode(controls)
check("control characters encode without literal controls", type(encoded_controls) == "string" and control_reason == nil and not encoded_controls:find("[%z\1-\31]"))
check("escaped controls roundtrip", decode_ok(encoded_controls) == controls)
for byte=0,31 do decode_bad('"' .. string.char(byte) .. '"', "raw control rejected " .. byte) end
for index, text in ipairs({
    [["\u007f"]], [["\u0080"]], [["\u07ff"]], [["\u0800"]],
    [["\ud7ff"]], [["\ue000"]], [["\uffff"]], [["\ud800\udc00"]], [["\udbff\udfff"]],
}) do
    local decoded = decode_ok(text, "Unicode boundary decodes " .. index)
    local encoded = json.encode(decoded)
    check("Unicode boundary roundtrips " .. index, decode_ok(encoded) == decoded)
end

local malformed = {
    "", " \t\r\n", "{} []", "true false", "null trailing", "{}/*comment*/",
    "[1,]", '{"a":1,}', "{a:1}", '{"a" 1}', '{"a":}', "[,1]", "[1 2]",
    '{"a":1 "b":2}', "[", "}", "'text'", '"unterminated',
    [["\q"]], [["\u12g4"]], [["\u123"]], [["\ud800"]], [["\udc00"]],
    [["\ud800\u0041"]], [["\udbff\udbff"]], [["\ud800x"]],
    '{"a":1,"a":2}', '{"a":null,"a":false}', '{"a":false,"\\u0061":null}',
    '{"x":{"a":1,"a":1}}', '{"":1,"":2}',
    "01", "-01", "00", "+1", ".1", "1.", "1e", "1e+", "--1", "-", "1e309",
    "-1e309", "NaN", "Infinity", "-Infinity", "0x10", "1_000",
}
for index, text in ipairs(malformed) do decode_bad(text, "malformed document rejected " .. index) end
local invalid_utf8 = {
    string.char(0x80), string.char(0xc0,0x80), string.char(0xc1,0xbf), string.char(0xc2),
    string.char(0xe2,0x82), string.char(0xed,0xa0,0x80), string.char(0xf0,0x80,0x80,0x80),
    string.char(0xf4,0x90,0x80,0x80), string.char(0xf8,0x80,0x80,0x80,0x80), string.char(0xff),
}
for index, bytes in ipairs(invalid_utf8) do
    decode_bad('"' .. bytes .. '"', "invalid raw UTF-8 rejected " .. index)
    encode_bad(bytes, "invalid UTF-8 value cannot encode " .. index)
end
decode_bad(string.char(0xef,0xbb,0xbf) .. "{}", "BOM is not silently accepted")
decode_bad(nil, "nil decode input rejected")
decode_bad({}, "table decode input rejected")

local at_depth = string.rep("[", json.MAX_DEPTH) .. "0" .. string.rep("]", json.MAX_DEPTH)
decode_ok(at_depth, "advertised maximum container depth is accepted")
decode_bad("[" .. at_depth .. "]", "one container beyond depth is rejected")
decode_ok(string.rep(" ", json.MAX_BYTES-1) .. "0", "maximum input size is accepted")
decode_bad(string.rep(" ", json.MAX_BYTES) .. "0", "excess input size is rejected")
local max_string = string.rep("s", json.MAX_STRING_BYTES)
check("maximum string length is accepted", decode_ok('"' .. max_string .. '"') == max_string)
decode_bad('"' .. max_string .. 's"', "excess decoded string size is rejected")
encode_bad(max_string .. "s", "excess encoded string size is rejected")
local max_nodes = "[" .. string.rep("0,", json.MAX_NODES-2) .. "0]"
check("maximum node budget is accepted", #decode_ok(max_nodes) == json.MAX_NODES-1)
decode_bad("[0," .. max_nodes:sub(2), "one value beyond node budget is rejected")
encode_bad(json.array({max_string,max_string,max_string,max_string}), "encoded total output is bounded")

local deep = 0
for _=1,json.MAX_DEPTH do deep = json.array({deep}) end
encode_ok(deep, at_depth, "encoder maximum container depth is accepted")
encode_bad(json.array({deep}), "encoder excess depth is rejected")
local excessive_nodes = json.array()
for index=1,json.MAX_NODES do excessive_nodes[index] = 0 end
encode_bad(excessive_nodes, "encoder total nodes are bounded")
encode_bad(nil, "Lua nil is not silently converted to JSON null")
encode_bad(math.huge, "positive infinity rejected")
encode_bad(-math.huge, "negative infinity rejected")
encode_bad(0/0, "NaN rejected")
encode_bad(function() end, "function rejected")
encode_bad(coroutine.create(function() end), "thread rejected")
encode_bad(io.stdout, "userdata rejected")
encode_bad({1,2}, "untagged numeric-key table is not guessed as an array")
encode_bad(json.array({[1]="a",[3]="c"}), "array hole rejected")
encode_bad(json.array({[2]="b"}), "missing initial array element rejected")
encode_bad(json.array({[0]="z"}), "zero array index rejected")
encode_bad(json.array({[-1]="z"}), "negative array index rejected")
encode_bad(json.array({[1.5]="z"}), "fractional array index rejected")
encode_bad(json.array({a=1}), "string array key rejected")
encode_bad(json.object({[1]=1}), "numeric object key rejected")
encode_bad({[true]=1}, "boolean object key rejected")
encode_bad({[{}]=1}, "table object key rejected without tostring")
local cycle = {}; cycle.self = cycle
encode_bad(cycle, "object cycle rejected")
local array_cycle = json.array(); array_cycle[1] = array_cycle
encode_bad(array_cycle, "array cycle rejected")
local shared = {x=1}
encode_ok({b=shared,a=shared}, '{"a":{"x":1},"b":{"x":1}}', "shared noncyclic objects are allowed")
local hooks = 0
local guarded = setmetatable({a=1}, {
    __eq=function() hooks=hooks+1; return false end,
    __pairs=function() hooks=hooks+1; error("PRIVATE_METAMETHOD_BODY") end,
    __index=function() hooks=hooks+1; error("PRIVATE_METAMETHOD_BODY") end,
    __len=function() hooks=hooks+1; error("PRIVATE_METAMETHOD_BODY") end,
    __tostring=function() hooks=hooks+1; return "PRIVATE_METAMETHOD_BODY" end,
})
encode_ok(guarded, '{"a":1}', "object encoding uses raw own entries")
check("object constructor uses raw identity", json.object(guarded) == guarded)
check("encoding does not invoke user metamethods", hooks == 0)
local false_null = setmetatable({safe=true}, {__eq=function() return true end})
encode_ok(false_null, '{"safe":true}', "equality metamethod cannot masquerade as JSON null")
local prior_number_metatable = debug.getmetatable(0)
local number_hooks = 0
debug.setmetatable(0, {__tostring=function() number_hooks=number_hooks+1; return "PRIVATE_NUMBER_BODY" end})
local encoded_number_ok, encoded_number, encoded_number_reason = pcall(json.encode, 42)
debug.setmetatable(0, prior_number_metatable)
check("integer encoding is independent of number tostring", encoded_number_ok and encoded_number == "42" and encoded_number_reason == nil)
check("integer encoding does not invoke number metamethods", number_hooks == 0)
local conflicting, conflict_reason = json.array(json.object())
check("conflicting shape tags are rejected", conflicting == nil and type(conflict_reason) == "string")
local invalid_tag, invalid_tag_reason = json.object(json.null)
check("null cannot acquire an object tag", invalid_tag == nil and type(invalid_tag_reason) == "string")

local secret = "PRIVATE_DOCUMENT_SECRET_9817"
local duplicate_reason = decode_bad('{"' .. secret .. '":0,"' .. secret .. '":1}', "secret duplicate document rejected")
check("decode error does not echo private keys or body", not duplicate_reason:find(secret,1,true))
local string_reason = encode_bad(secret .. string.char(0xff), "private invalid UTF-8 value rejected")
check("encode error does not echo private values", not string_reason:find(secret,1,true))

print(string.format("Capability JSON Tests: %d passed, %d failed", passed, failed))
if failed == 0 then print("ALL CAPABILITY JSON TESTS PASSED") end
os.exit(failed == 0 and 0 or 1)
