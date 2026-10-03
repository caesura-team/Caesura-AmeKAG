-- U22 static data inspector. Python supplies trusted validator source modules
-- and package data via length-delimited stdin frames. No package module or
-- author iscript executes. This program is a host tool, not packaged runtime.
local host_load, host_read, host_write = load, io.read, io.write
local host_hook, host_exit = debug.sethook, os.exit
local function frame()
    local header = assert(host_read('*l'), 'missing inspector input frame')
    assert(header:match('^%d+$'), 'invalid inspector frame length')
    local size = tonumber(header)
    assert(size <= 32 * 1024 * 1024, 'inspector frame exceeds size limit')
    local value = size == 0 and '' or assert(host_read(size), 'truncated inspector frame')
    assert(#value == size, 'truncated inspector frame')
    return value
end

-- This codec is injected from the verifier's own source tree, never the package.
local json = assert(host_load(frame(), '@trusted/capability_json.lua', 't'))()
local request = assert(json.decode(frame()))
assert(type(request.module_count) == 'number' and request.module_count <= 1000,
    'invalid trusted module count')
local modules = {}
for _ = 1, request.module_count do
    local name, source = frame(), frame()
    assert(modules[name] == nil, 'duplicate trusted module')
    modules[name] = source
end
local bundle_source, capability_source = frame(), frame()
local sources = {}
for i = 1, #request.paths do sources[i] = frame() end

local function inspect()
    local ticks = 0
    host_hook(function()
        ticks = ticks + 100
        if ticks > request.instruction_limit then error('inspector instruction budget exceeded', 0) end
    end, '', 100)

    -- A token whitelist admits the compiler's literal return format and Lua
    -- string escapes/long strings, but rejects functions/loops/calls/operators.
    -- load below remains the actual Lua parser/decoder, with an empty env.
    local function literal_only(text)
        local p, words = 1, {['return']=true, ['true']=true, ['false']=true, ['nil']=true}
        while p <= #text do
            local char = text:sub(p,p)
            if char:match('%s') then p = p + 1
            elseif char == '"' or char == "'" then
                local quote = char
                p = p + 1
                while p <= #text and text:sub(p,p) ~= quote do
                    p = p + (text:sub(p,p) == '\\' and 2 or 1)
                end
                assert(p <= #text, 'invalid bundle literal string')
                p = p + 1
            elseif text:sub(p,p+1) == '--' then
                p = (text:find('\n', p+2, true) or #text) + 1
            elseif char == '[' and text:sub(p):match('^%[(=*)%[') ~= nil then
                local equals = text:sub(p):match('^%[(=*)%[')
                local close = assert(text:find(']'..equals..']', p+#equals+2, true),
                    'invalid bundle literal long string')
                p = close + #equals + 2
            elseif char:match('[%a_]') then
                local word = text:sub(p):match('^[%a_][%w_]*')
                assert(words[word], 'bundle must be a literal return, found '..word)
                p = p + #word
            elseif char:match('%d') or (char == '-' and text:sub(p+1,p+1):match('%d')) then
                local number = text:sub(p):match('^%-?%d+%.?%d*[eE]?[+%-]?%d*')
                assert(number and tonumber(number), 'invalid bundle literal number')
                p = p + #number
            else
                assert(char:match('[{}%[%]=,;]'), 'bundle must be a literal return')
                p = p + 1
            end
        end
    end
    literal_only(bundle_source)
    local chunk = assert(host_load(bundle_source, '@package/cache/story/story.lua', 't', {}))
    local bundle = chunk()
    local visited, nodes = {}, 0
    local function plain(value, depth)
        nodes = nodes + 1
        assert(nodes <= 250000 and depth <= 64, 'bundle data limit exceeded')
        local kind = type(value)
        if kind == 'table' then
            assert(not visited[value] and getmetatable(value) == nil, 'bundle must contain plain acyclic data')
            visited[value] = true
            for key, child in pairs(value) do
                assert(type(key) == 'string' or type(key) == 'number', 'invalid bundle data key')
                plain(child, depth+1)
            end
            visited[value] = nil
        else
            assert(kind == 'string' or kind == 'number' or kind == 'boolean' or kind == 'nil',
                'non-data value in bundle literal')
        end
    end
    plain(bundle, 0)

    -- Compile-time modules receive only an in-memory require and safe language
    -- primitives. Even trusted modules cannot open files or launch processes.
    local loaded, loading, used = {}, {}, {}
    local env = {assert=assert, error=error, ipairs=ipairs, pairs=pairs, next=next,
        pcall=pcall, xpcall=xpcall, select=select, tonumber=tonumber, tostring=tostring,
        type=type, rawequal=rawequal, rawget=rawget, rawset=rawset,
        getmetatable=getmetatable, setmetatable=setmetatable,
        math=math, string=string, table=table, utf8=utf8, coroutine=coroutine,
        package={loaded=loaded, config=package.config}, os={clock=os.clock},
        io={open=function() return nil, 'host files unavailable to static validator' end},
        print=function() end, _VERSION=_VERSION}
    env._G = env
    env.load = function(text, name, mode, target)
        assert(mode == nil or mode == 't', 'only text compilation is available')
        return host_load(text, name, 't', target or {})
    end
    env.require = function(name)
        if loaded[name] ~= nil then return loaded[name] end
        assert(not loading[name], 'cyclic validator module: '..name)
        local source = assert(modules[name], 'undeclared validator module: '..tostring(name))
        loading[name] = true
        local result = assert(host_load(source, '@trusted/'..name, 't', env))()
        loaded[name], loading[name], used[name] = result == nil and true or result, nil, true
        return loaded[name]
    end
    -- Preserve the real codec's array tags used by asset_dependencies.
    loaded.capability_json = json
    used.capability_json = true
    env.require('kag')
    local compiler = env.require('kag.compiler')
    local tokenizer = env.require('tokenizer')
    local collector = env.require('kag.asset_dependencies')
    local keys = assert(compiler.bundleSceneKeys(request.paths))
    local compatible, reason = compiler.validateBundle(bundle, keys)
    assert(compatible, reason)
    assert(type(bundle.entry) == 'string' and bundle.scenes[bundle.entry], 'bundle entry scene missing')
    assert(bundle.entry == request.entry, 'bundle entry differs from MANIFEST entry scene')

    local dependencies = collector.new_report()
    for index, source in ipairs(sources) do
        local tokens = tokenizer.parse(source)
        collector.extend(dependencies, collector.collect(tokens, keys[index]))
        compiler.compile(tokens)
        local expected = assert(compiler.serialize(tokens))
        assert(compiler.encode_lua_literal(expected) == compiler.encode_lua_literal(bundle.scenes[keys[index]]),
            'compiled bundle differs from packaged source scene: '..keys[index])
    end
    local capabilities = assert(json.decode(capability_source))
    dependencies = collector.apply_capabilities(dependencies, capabilities)
    assert(#dependencies.invalid == 0, 'invalid static media paths in package sources')
    local declared = bundle.asset_dependencies
    assert(type(declared) == 'table' and declared.schema == 1
        and declared.analysis_scope == dependencies.analysis_scope, 'invalid bundle asset_dependencies schema')
    local function array(value, label)
        assert(type(value) == 'table', 'dependencies array missing: '..label)
        local count = 0
        for key in pairs(value) do
            assert(type(key) == 'number' and key >= 1 and key % 1 == 0 and key <= #value,
                'invalid dependencies array: '..label)
            count = count + 1
        end
        assert(count == #value, 'sparse dependencies array: '..label)
        return value
    end
    local function sorted_values(value, label)
        local result = {}
        for _, item in ipairs(array(value, label)) do
            result[#result+1] = type(item) == 'table' and compiler.encode_lua_literal(item)
                or type(item)..':'..tostring(item)
        end
        table.sort(result)
        return table.concat(result, '\0')
    end
    for _, field in ipairs({'media','static','dynamic','skipped','invalid'}) do
        assert(sorted_values(declared[field], field) == sorted_values(dependencies[field], field),
            'bundle dependencies differ from packaged source: '..field)
    end
    assert(sorted_values(bundle.assets, 'assets') == sorted_values(dependencies.static, 'static'),
        'bundle assets differ from required static dependencies')
    local module_names = json.array()
    for name in pairs(used) do module_names[#module_names+1] = name end
    table.sort(module_names)
    local scene_keys = json.array()
    for _, key in ipairs(keys) do scene_keys[#scene_keys+1] = key end
    local result = {entry=bundle.entry,scene_keys=scene_keys,dependencies=dependencies,
        used_validator_modules=module_names,author_iscript='NOT_EXECUTED',
        dynamic_media='NOT_PROVEN',skipped_media='NOT_VERIFIED',runtime='NOT_RUN'}
    return assert(json.encode(result))
end

local ok, result = pcall(inspect)
host_hook()
if ok then host_write(result, '\n')
else
    host_write(assert(json.encode({error=tostring(result),runtime='NOT_RUN'})), '\n')
    host_exit(1)
end
