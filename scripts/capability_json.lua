-- Strict, bounded JSON for capability catalogs, project declarations and
-- CLI reports. This deliberately does not replace or register "json.lua".
-- decode/encode return value-or-text, nil on success; nil, fixed error on failure.
local M = {}
local MAX_BYTES, MAX_STRING_BYTES, MAX_DEPTH, MAX_NODES = 1048576, 262144, 64, 65536
M.MAX_BYTES, M.MAX_STRING_BYTES = MAX_BYTES, MAX_STRING_BYTES
M.MAX_DEPTH, M.MAX_NODES = MAX_DEPTH, MAX_NODES

local NULL = setmetatable({}, {
    __metatable = "capability_json.null",
    __newindex = function() error("capability_json: null is immutable", 2) end,
    __tostring = function() return "capability_json.null" end,
})
M.null = NULL
-- Private weak tags retain decoded empty-container identity without exposing
-- a metatable that application data could accidentally inherit or overwrite.
local arrays = setmetatable({}, {__mode="k"})
local objects = setmetatable({}, {__mode="k"})
local raw_next, raw_get, raw_equal = next, rawget, rawequal
local ERROR = {}

local function fail(reason, position)
    error({[ERROR]=true, reason=reason, position=position}, 0)
end

local function protected(fn, value)
    local ok, result = pcall(fn, value)
    if ok then return result, nil end
    if type(result) == "table" and raw_get(result, ERROR) then
        local position = raw_get(result, "position")
        return nil, "capability_json: " .. raw_get(result, "reason")
            .. (position and (" at byte " .. position) or "")
    end
    -- Never forward an exception that could contain document keys or values.
    return nil, "capability_json: resource or codec failure"
end

local function tagged(value, own, other)
    if value == nil then value = {} end
    if type(value) ~= "table" or raw_equal(value, NULL) then
        return nil, "capability_json: container must be a table"
    end
    if other[value] then return nil, "capability_json: conflicting container shape" end
    own[value] = true
    return value
end
function M.array(value) return tagged(value, arrays, objects) end
function M.object(value) return tagged(value, objects, arrays) end
function M.is_array(value) return type(value) == "table" and arrays[value] == true end
function M.is_object(value) return type(value) == "table" and objects[value] == true end

local function continuation(text, position)
    local byte = string.byte(text, position)
    return byte and byte >= 0x80 and byte <= 0xbf
end

-- Reject overlong sequences, surrogate code points, truncated sequences and
-- values beyond U+10FFFF. Raw UTF-8 bytes otherwise remain unchanged.
local function invalid_utf8(text)
    local position = 1
    while position <= #text do
        local first = string.byte(text, position)
        if first < 0x80 then position = position + 1
        elseif first >= 0xc2 and first <= 0xdf then
            if not continuation(text, position+1) then return position end
            position = position + 2
        elseif first >= 0xe0 and first <= 0xef then
            local second = string.byte(text, position+1)
            local minimum = first == 0xe0 and 0xa0 or 0x80
            local maximum = first == 0xed and 0x9f or 0xbf
            if not second or second < minimum or second > maximum
                or not continuation(text, position+2) then return position end
            position = position + 3
        elseif first >= 0xf0 and first <= 0xf4 then
            local second = string.byte(text, position+1)
            local minimum = first == 0xf0 and 0x90 or 0x80
            local maximum = first == 0xf4 and 0x8f or 0xbf
            if not second or second < minimum or second > maximum
                or not continuation(text, position+2)
                or not continuation(text, position+3) then return position end
            position = position + 4
        else return position end
    end
end

local function utf8_character(code)
    if code <= 0x7f then return string.char(code) end
    if code <= 0x7ff then
        return string.char(0xc0 + math.floor(code/64), 0x80 + code%64)
    end
    if code <= 0xffff then
        return string.char(0xe0 + math.floor(code/4096),
            0x80 + math.floor(code/64)%64, 0x80 + code%64)
    end
    return string.char(0xf0 + math.floor(code/262144),
        0x80 + math.floor(code/4096)%64, 0x80 + math.floor(code/64)%64, 0x80 + code%64)
end

local escapes = {['"']='"', ['\\']='\\', ['/']='/', b='\b', f='\f', n='\n', r='\r', t='\t'}
local function finite(number)
    return number == number and number ~= math.huge and number ~= -math.huge
end

local function decode(text)
    if type(text) ~= "string" then fail("input must be a string") end
    if #text > MAX_BYTES then fail("input size limit exceeded") end
    local invalid = invalid_utf8(text)
    if invalid then fail("invalid UTF-8", invalid) end
    local position, length, nodes = 1, #text, 0
    local parse_value
    local function skip_space()
        while position <= length do
            local byte = string.byte(text, position)
            if byte ~= 32 and byte ~= 9 and byte ~= 10 and byte ~= 13 then return end
            position = position + 1
        end
    end
    local function hex4()
        if position + 3 > length then fail("incomplete Unicode escape", position) end
        local code = 0
        for _=1,4 do
            local byte = string.byte(text, position)
            local digit
            if byte >= 48 and byte <= 57 then digit = byte-48
            elseif byte >= 65 and byte <= 70 then digit = byte-55
            elseif byte >= 97 and byte <= 102 then digit = byte-87
            else fail("invalid Unicode escape", position) end
            code = code*16 + digit
            position = position + 1
        end
        return code
    end
    local function parse_string()
        position = position + 1 -- opening quote, already checked by the caller
        local parts, bytes, start = {}, 0, position
        local function append(part)
            bytes = bytes + #part
            if bytes > MAX_STRING_BYTES then fail("string size limit exceeded", position) end
            if #part > 0 then parts[#parts+1] = part end
        end
        while position <= length do
            local byte = string.byte(text, position)
            if byte == 34 then
                append(text:sub(start, position-1))
                position = position + 1
                return table.concat(parts)
            elseif byte == 92 then
                append(text:sub(start, position-1))
                position = position + 1
                local escape = text:sub(position, position)
                if escape == "u" then
                    position = position + 1
                    local code = hex4()
                    if code >= 0xd800 and code <= 0xdbff then
                        if text:sub(position, position+1) ~= "\\u" then
                            fail("missing low surrogate", position)
                        end
                        position = position + 2
                        local low = hex4()
                        if low < 0xdc00 or low > 0xdfff then fail("invalid low surrogate", position-4) end
                        code = 0x10000 + (code-0xd800)*0x400 + low-0xdc00
                    elseif code >= 0xdc00 and code <= 0xdfff then
                        fail("unpaired low surrogate", position-4)
                    end
                    append(utf8_character(code))
                else
                    local replacement = escapes[escape]
                    if not replacement then fail("invalid string escape", position) end
                    append(replacement)
                    position = position + 1
                end
                start = position
            elseif byte < 32 then fail("unescaped control character", position)
            else position = position + 1 end
        end
        fail("unterminated string", position)
    end
    local function digit_at(index)
        local byte = string.byte(text, index)
        return byte and byte >= 48 and byte <= 57
    end
    local function parse_number()
        local start = position
        if text:sub(position, position) == "-" then position = position + 1 end
        if text:sub(position, position) == "0" then
            position = position + 1
            if digit_at(position) then fail("leading zero in number", position) end
        elseif digit_at(position) then
            repeat position = position + 1 until not digit_at(position)
        else fail("invalid number", position) end
        if text:sub(position, position) == "." then
            position = position + 1
            if not digit_at(position) then fail("missing fraction digits", position) end
            repeat position = position + 1 until not digit_at(position)
        end
        local exponent = text:sub(position, position)
        if exponent == "e" or exponent == "E" then
            position = position + 1
            local sign = text:sub(position, position)
            if sign == "+" or sign == "-" then position = position + 1 end
            if not digit_at(position) then fail("missing exponent digits", position) end
            repeat position = position + 1 until not digit_at(position)
        end
        local number = tonumber(text:sub(start, position-1))
        if not number or not finite(number) then fail("number is not finite", start) end
        return number
    end
    local function parse_array(depth)
        local result, count = {}, 0
        arrays[result] = true
        position = position + 1
        skip_space()
        if text:sub(position, position) == "]" then position = position + 1; return result end
        while true do
            count = count + 1
            result[count] = parse_value(depth)
            skip_space()
            local separator = text:sub(position, position)
            if separator == "]" then position = position + 1; return result end
            if separator ~= "," then fail("expected array separator", position) end
            position = position + 1
            skip_space()
        end
    end
    local function parse_object(depth)
        local result = {}
        objects[result] = true
        position = position + 1
        skip_space()
        if text:sub(position, position) == "}" then position = position + 1; return result end
        while true do
            if text:sub(position, position) ~= '"' then fail("expected object key", position) end
            local key_position = position
            local key = parse_string()
            if raw_get(result, key) ~= nil then fail("duplicate object key", key_position) end
            skip_space()
            if text:sub(position, position) ~= ":" then fail("expected object colon", position) end
            position = position + 1
            result[key] = parse_value(depth)
            skip_space()
            local separator = text:sub(position, position)
            if separator == "}" then position = position + 1; return result end
            if separator ~= "," then fail("expected object separator", position) end
            position = position + 1
            skip_space()
        end
    end
    parse_value = function(depth)
        skip_space()
        nodes = nodes + 1
        if nodes > MAX_NODES then fail("value count limit exceeded", position) end
        local first = text:sub(position, position)
        if first == '"' then return parse_string() end
        if first == "[" or first == "{" then
            if depth >= MAX_DEPTH then fail("container depth limit exceeded", position) end
            if first == "[" then return parse_array(depth+1) end
            return parse_object(depth+1)
        end
        if first == "-" or digit_at(position) then return parse_number() end
        for _, literal in ipairs({{"true",true}, {"false",false}, {"null",NULL}}) do
            if text:sub(position, position+#literal[1]-1) == literal[1] then
                position = position + #literal[1]
                return literal[2]
            end
        end
        fail("expected JSON value", position)
    end
    local result = parse_value(0)
    skip_space()
    if position <= length then fail("trailing text", position) end
    return result
end

local encoded_escapes = {[34]='\\"', [92]='\\\\', [8]='\\b', [12]='\\f', [10]='\\n', [13]='\\r', [9]='\\t'}
local function byte_less(left, right)
    for index=1,math.min(#left,#right) do
        local a, b = string.byte(left,index), string.byte(right,index)
        if a ~= b then return a < b end
    end
    return #left < #right
end

local function encode(value)
    local parts, bytes, nodes, active = {}, 0, 0, {}
    local emit
    local function append(part)
        bytes = bytes + #part
        if bytes > MAX_BYTES then fail("output size limit exceeded") end
        parts[#parts+1] = part
    end
    local function emit_string(text)
        if #text > MAX_STRING_BYTES then fail("string size limit exceeded") end
        if invalid_utf8(text) then fail("invalid UTF-8 string") end
        append('"')
        local start = 1
        for index=1,#text do
            local byte = string.byte(text,index)
            local escape = encoded_escapes[byte]
            if not escape and byte < 32 then escape = string.format("\\u%04x", byte) end
            if escape then
                if start < index then append(text:sub(start,index-1)) end
                append(escape)
                start = index + 1
            end
        end
        if start <= #text then append(text:sub(start)) end
        append('"')
    end
    emit = function(item, depth)
        nodes = nodes + 1
        if nodes > MAX_NODES then fail("value count limit exceeded") end
        if raw_equal(item, NULL) then append("null"); return end
        local kind = type(item)
        if kind == "string" then emit_string(item)
        elseif kind == "boolean" then append(item and "true" or "false")
        elseif kind == "number" then
            if not finite(item) then fail("number is not finite") end
            if math.type(item) == "integer" then append(string.format("%d", item))
            else append((string.format("%.17g", item):gsub(",", "."))) end
        elseif kind == "table" then
            if active[item] then fail("cyclic table") end
            if depth >= MAX_DEPTH then fail("container depth limit exceeded") end
            active[item] = true
            if arrays[item] then
                local count, maximum = 0, 0
                for key in raw_next, item do
                    if type(key) ~= "number" or not finite(key) or key < 1
                        or key ~= math.floor(key) then fail("invalid array key") end
                    count, maximum = count+1, math.max(maximum,key)
                    if count > MAX_NODES or maximum > MAX_NODES then fail("value count limit exceeded") end
                end
                if maximum ~= count then fail("array contains holes") end
                append("[")
                for index=1,count do
                    if index > 1 then append(",") end
                    emit(raw_get(item,index), depth+1)
                end
                append("]")
            else
                -- Untagged tables are report objects, never guessed arrays.
                local keys, key_bytes = {}, 0
                for key in raw_next, item do
                    if type(key) ~= "string" then fail("object key must be a string") end
                    if #key > MAX_STRING_BYTES then fail("string size limit exceeded") end
                    key_bytes = key_bytes + #key
                    -- Bound sorting work before comparing shared key prefixes.
                    if key_bytes > MAX_BYTES then fail("output size limit exceeded") end
                    keys[#keys+1] = key
                    if #keys > MAX_NODES then fail("value count limit exceeded") end
                end
                table.sort(keys, byte_less)
                append("{")
                for index,key in ipairs(keys) do
                    if index > 1 then append(",") end
                    emit_string(key)
                    append(":")
                    emit(raw_get(item,key), depth+1)
                end
                append("}")
            end
            active[item] = nil
        else fail("unsupported Lua value; use null sentinel for JSON null") end
    end
    emit(value, 0)
    return table.concat(parts)
end

function M.decode(text) return protected(decode, text) end
function M.encode(value) return protected(encode, value) end
return M
