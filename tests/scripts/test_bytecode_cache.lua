-- test_bytecode_cache.lua — Battle 1b: .ksc bytecode persistence tests.
-- Covers compiler.serialize/deserialize/writeCache/readCache roundtrip,
-- freshness (source-size marker), scheduler execution of a restored
-- stream, and cache-failure degradation (never breaks scene loading).
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name .. (detail and (" -- " .. tostring(detail)) or ""))
        failed = failed + 1 end
end

local compiler = require("kag.compiler")
local tokenizer = require("tokenizer")
local scheduler = require("scheduler")

local TMP = "cache/ksc/test_ksc.ksc"

-- ---------------------------------------------------------------------------
-- 1. serialize keeps only serializable data (no handlers)
-- ---------------------------------------------------------------------------
local src = '[if exp="f.x > 1 && f.y != 0"]\n[ch text="a"]\n[else]\n[ch text="b"]\n[endif]\n*start\n[bg storage="bg.png"]\n'
local tokens = tokenizer.parse(src)
compiler.compile(tokens)
local data = compiler.serialize(tokens)
check("serialize produces current format data", data ~= nil and data.version == 2)
if data then
    check("serialize keeps flow jump table", data.flow[1].kind == "if"
          and data.flow[1].jump_false == 3)
    check("serialize keeps labels", data.labels["start"] == 6)
    check("serialize keeps pre-translated exprs",
          data.exprs[1] == "f.x > 1 and f.y ~= 0")
    check("serialize strips _compiled from tokens", data.tokens._compiled == nil)
end

-- ---------------------------------------------------------------------------
-- 2. writeCache -> readCache roundtrip restores a runnable stream
-- ---------------------------------------------------------------------------
compiler.compile(tokens)
tokens._srcHash = compiler.hashFile("tests/scripts/smoke_test.ks") or "abc123"
check("writeCache", compiler.writeCache(tokens, TMP) == true)
local restored = compiler.readCache(TMP)
check("readCache returns tokens", restored ~= nil and #restored == 7)
if restored then
    local c = restored._compiled
    check("restored flow intact", c.flow[1].kind == "if" and c.flow[1].jump_false == 3)
    check("restored labels intact", c.labels["start"] == 6)
    check("restored exprs intact", c.exprs[1] == "f.x > 1 and f.y ~= 0")
    check("restored params sharing (tok[2] === params[i])",
          restored[2][2] == c.params[2])
    check("restored _srcHash marker", c._srcHash == (compiler.hashFile("tests/scripts/smoke_test.ks") or "abc123"))
    check("handlers rebound lazily (empty table)", next(c.handlers) == nil)

    -- the restored stream must execute identically through the scheduler
    local seen = {}
    package.loaded["kag"] = {
        ch = function(_, p) seen[#seen + 1] = p.text end,
        bg = function() end,
    }
    local ctx = { f = { x = 5, y = 5 }, sf = {}, tf = {}, mp = {},
        tokens = restored, token_index = 1, call_stack = {},
        layers = {}, backlog = {}, macros = {}, stop_flag = false,
        load_tokens = function() end }
    local co = coroutine.create(function() scheduler.run(ctx, restored, 1) end)
    local n = 0
    while coroutine.status(co) == "suspended" and n < 20 do
        coroutine.resume(co, 16)
        n = n + 1
    end
    check("restored stream executes (if-taken)", #seen == 1 and seen[1] == "a",
          table.concat(seen, ","))

    -- same scene with f.x=0 takes the else branch
    local seen2 = {}
    package.loaded["kag"] = {
        ch = function(_, p) seen2[#seen2 + 1] = p.text end,
        bg = function() end,
    }
    local ctx2 = { f = { x = 0, y = 5 }, sf = {}, tf = {}, mp = {},
        tokens = restored, token_index = 1, call_stack = {},
        layers = {}, backlog = {}, macros = {}, stop_flag = false,
        load_tokens = function() end }
    local co2 = coroutine.create(function() scheduler.run(ctx2, restored, 1) end)
    local m = 0
    while coroutine.status(co2) == "suspended" and m < 20 do
        coroutine.resume(co2, 16)
        m = m + 1
    end
    check("restored stream executes (else branch)", #seen2 == 1 and seen2[1] == "b",
          table.concat(seen2, ","))
end

-- ---------------------------------------------------------------------------
-- 3. deserialize rejects bad shapes / versions
-- ---------------------------------------------------------------------------
check("deserialize nil", compiler.deserialize(nil) == nil)
check("deserialize bad version", compiler.deserialize({ version = 99 }) == nil)
check("deserialize bad shape",
      compiler.deserialize({ version = 1, tokens = {}, flow = nil }) == nil)

-- ---------------------------------------------------------------------------
-- 4. cache failure degrades gracefully (readCache on missing/corrupt file)
-- ---------------------------------------------------------------------------
check("readCache missing file", compiler.readCache("tmp/does_not_exist.ksc") == nil)
local bad = io.open(TMP, "w")
if not bad then print("DBG io.open failed for " .. TMP) end
bad:write("{not valid json")
bad:close()
check("readCache corrupt file", compiler.readCache(TMP) == nil)
os.remove(TMP)

-- U14: source identity, container format and compiler/contracts identity are
-- distinct. A same-size external cache rewrite must not reuse old live tokens.
do
    local a=tokenizer.parse('[set var="f.padding" value="'..string.rep('x',180)..'"]\n[set var="f.answer" value=1]')
    local b=tokenizer.parse('[set var="f.padding" value="'..string.rep('x',180)..'"]\n[set var="f.answer" value=2]')
    compiler.compile(a);compiler.compile(b)
    local da,db=assert(compiler.serialize(a)),assert(compiler.serialize(b))
    check('U14 serialized compiler identity exists',type(da.compatibility)=='table'
        and da.compatibility.format==2 and type(da.compatibility.semantics)=='string')
    check('U14 public compatibility check accepts current output',type(compiler.isCompatible)=='function'
        and compiler.isCompatible(a)==true)
    local old={};for k,v in pairs(da) do old[k]=v end
    old.version=1
    check('U14 old cache format is rejected',compiler.deserialize(old)==nil)
    local unknown={};for k,v in pairs(da) do unknown[k]=v end
    unknown.compatibility={format=2,semantics='unknown-future',commands={}}
    check('U14 unknown compiler identity is rejected',compiler.deserialize(unknown)==nil)
    assert(compiler.writeCache(a,TMP))
    local first=assert(compiler.readCache(TMP))
    first[2][2].value='corrupted-live-copy'
    local second=assert(compiler.readCache(TMP))
    check('U14 each cache read owns a detached token tree',first~=second and second[2][2].value~='corrupted-live-copy')
    local textA='return '..compiler.encode_lua_literal(da)
    local textB='return '..compiler.encode_lua_literal(db)
    assert(#textA==#textB and textA:sub(1,64)==textB:sub(1,64),'rewrite must collide with old size/head cache key')
    local file=assert(io.open(TMP,'wb'));file:write(textB);file:close()
    local rewritten=assert(compiler.readCache(TMP))
    check('U14 same-size same-prefix external rewrite is detected',tonumber(rewritten[2][2].value)==2)
    file=assert(io.open(TMP,'wb'));file:write(textB:sub(1,#textB-10));file:close()
    check('U14 truncated cache is rejected',compiler.readCache(TMP)==nil)
    os.remove(TMP)
end
do
    local schema=require('kag.schema')
    schema.define('u14_contract_probe',{value={type='number',default=7}})
    local source=tokenizer.parse('[u14_contract_probe]');compiler.compile(source)
    local packed=assert(compiler.serialize(source))
    schema.specs('u14_contract_probe').value.default=9
    check('U14 only changing a schema default invalidates serialized code',compiler.deserialize(packed)==nil)
    check('U14 old compiled tokens are not silently stamped with new contracts',compiler.serialize(source)==nil)
    check('U14 memory compatibility detects schema mutation',type(compiler.isCompatible)=='function'
        and compiler.isCompatible(source)==false)
    schema.specs('u14_contract_probe').value.default=7
    check('U14 restoring the exact contract restores compatibility',compiler.deserialize(packed)~=nil)
    schema.specs('u14_contract_probe').value.default=9007199254740992
    local integer_tokens=tokenizer.parse('[u14_contract_probe]');compiler.compile(integer_tokens)
    schema.specs('u14_contract_probe').value.default=9007199254740993
    check('U14 distinct Lua integers retain distinct contract identities',not compiler.isCompatible(integer_tokens))
    schema.specs('u14_contract_probe').value.default=math.huge
    local infinite_tokens=tokenizer.parse('[u14_contract_probe]');compiler.compile(infinite_tokens)
    check('U14 non-finite contracts cannot enter persisted code',compiler.serialize(infinite_tokens)==nil)
    schema.specs('u14_contract_probe').value.default=7
end

-- Exit gate.
if failed > 0 then
    print(string.format("BYTECODE CACHE TESTS: %d passed, %d FAILED", passed, failed))
    os.exit(1)
end
print(string.format("BYTECODE CACHE TESTS DONE (%d passed)", passed))
