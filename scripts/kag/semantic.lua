-- =============================================================================
--  Caesura (AmeKAG) — kag/semantic.lua
--  KAG Unified Semantic Representation & AST Layer
--
--  Single source of truth for understanding KAG (.ks) scripts:
--    KAG Source -> tokenizer (LPeg) -> semantic model / AST
--                       ├── Runtime (compiler / scheduler)
--                       ├── Story Flow Graph (Mermaid / JSON)
--                       ├── i18n Extraction & Lint
--                       ├── LSP / Diagnostics
--                       └── AI Tooling
-- =============================================================================

local semantic = { _VERSION = "1.0.0" }

-- Resolve scripts/ from this file's location
local BS = string.char(92)
local here = (arg and arg[0] and arg[0]:match("(.*[/" .. BS .. "])")) or "scripts/"
if not package.path:find(here, 1, true) then
    package.path = here .. "?.lua;" .. here .. "?/init.lua;" .. package.path
end

local tokenizer = require("tokenizer")
local schema = require("kag.schema")
local exprLang = require("kag.expr")
local normalize_params = require("kag.compiler").normalize_params

-- Pre-load known command modules so contracts are registered
pcall(require, "kag.commands.text")
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.audio")
pcall(require, "kag.commands.transition")
pcall(require, "kag.commands.layer")
pcall(require, "kag.commands.vfx")
pcall(require, "kag.commands.video")
pcall(require, "kag.commands.save")
pcall(require, "kag.commands.tween")
pcall(require, "kag.commands.character")
pcall(require, "kag.commands.resource")
pcall(require, "kag.commands.math")

-- -----------------------------------------------------------------------------
-- Helper: Simple fast MD5 / Content Hash for deterministic translation keys
-- -----------------------------------------------------------------------------
local function hash_content(str)
    if not str or str == "" then return "000000" end
    local h = 5381
    for i = 1, #str do
        h = ((h * 33) + str:byte(i)) % 0x100000000
    end
    return string.format("%08x", h):sub(1, 6)
end

-- -----------------------------------------------------------------------------
-- Line/Column Mapper from byte offsets
-- -----------------------------------------------------------------------------
local function build_line_index(text)
    local line_starts = { 1 }
    local len = #text
    for i = 1, len do
        if text:byte(i) == 10 then -- \n
            line_starts[#line_starts + 1] = i + 1
        end
    end
    return {
        starts = line_starts,
        offset_to_pos = function(self, offset)
            if not offset or offset < 1 then return 1, 1 end
            local starts = self.starts
            local lo, hi = 1, #starts
            while lo <= hi do
                local mid = math.floor((lo + hi) / 2)
                if starts[mid] <= offset then
                    if mid == #starts or starts[mid + 1] > offset then
                        return mid, offset - starts[mid] + 1
                    else
                        lo = mid + 1
                    end
                else
                    hi = mid - 1
                end
            end
            return 1, offset
        end
    }
end

-- -----------------------------------------------------------------------------
-- Translatable command parameter rules
-- -----------------------------------------------------------------------------
local TRANSLATABLE_PARAMS = {
    ch = { text = true },
    text = { content = true },
    sel = { text = true },
    button = { text = true, label = true },
    notify = { msg = true, text = true },
    toast = { msg = true, text = true },
    caption = { text = true, title = true },
    dialog = { message = true, text = true, title = true },
    link = { text = true },
}

-- -----------------------------------------------------------------------------
-- Core Semantic Analysis
-- -----------------------------------------------------------------------------
function semantic.parse(ks_text, filename)
    filename = filename or "unnamed.ks"
    local scene_basename = filename:match("([^/\\]+)$") or filename
    local line_mapper = build_line_index(ks_text)
    
    local raw_tokens = tokenizer.parse_with_offsets(ks_text)
    
    local nodes = {}
    local labels = {}
    local jumps = {}
    local calls = {}
    local choices = {}
    local endings = {}
    local translatables = {}
    local diagnostics = {}
    
    local current_label = "*_entry_"
    local current_scope = "entry"
    labels[current_label] = {
        name = current_label,
        title = "Start: " .. scene_basename,
        line = 1,
        col = 1,
        node_index = 0,
        is_entry = true,
    }
    
    local block_stack = {} -- for tracking [select], [if], [while], [for]
    
    for idx, tok in ipairs(raw_tokens) do
        local line, col = line_mapper:offset_to_pos(tok.offset)
        local end_line, end_col = line_mapper:offset_to_pos(tok.end_offset)
        
        local node = {
            id = idx,
            type = tok.type,
            line = line,
            col = col,
            end_line = end_line,
            end_col = end_col,
            offset = tok.offset,
            end_offset = tok.end_offset,
            scope = current_scope,
            label = current_label,
        }
        
        if tok.type == "label" then
            local raw_name = tok.name or ""
            local parts = {}
            for p in raw_name:gmatch("[^|]+") do parts[#parts + 1] = p end
            local lbl_name = "*" .. (parts[1] or raw_name):gsub("^%*", "")
            local title = parts[2] or parts[1] or raw_name
            
            node.name = lbl_name
            node.title = title
            current_label = lbl_name
            current_scope = lbl_name:gsub("^%*", "")
            
            if not labels[lbl_name] then
                labels[lbl_name] = {
                    name = lbl_name,
                    title = title,
                    line = line,
                    col = col,
                    node_index = idx,
                    is_entry = false,
                }
            else
                diagnostics[#diagnostics + 1] = {
                    line = line,
                    col = col,
                    severity = "WARN",
                    code = "DUPLICATE_LABEL",
                    message = string.format("Duplicate label '%s' (first defined at line %d)", lbl_name, labels[lbl_name].line),
                }
            end
            
        elseif tok.type == "text" or tok.type == "blocktext" then
            local text_content = tok.content or ""
            node.text = text_content
            node.is_translatable = true
            
            local trans_key = string.format("%s:%s:L%d:%s", scene_basename, current_scope, line, hash_content(text_content))
            local trans_entry = {
                key = trans_key,
                source = text_content,
                speaker = "Narrator",
                line = line,
                col = col,
                kind = "text",
                file = scene_basename,
            }
            translatables[#translatables + 1] = trans_entry
            node.trans_key = trans_key
            
        elseif tok.type == "command" then
            local cmd = tok.cmd
            node.cmd = cmd
            node.params = normalize_params(cmd, tok.params or {})
            node.named_params = {}
            node.positional_params = {}
            
            -- Process parameters
            for _, p in ipairs(tok.params or {}) do
                if type(p) == "table" and p[1] ~= nil then
                    local k, v = p[1], p[2]
                    local num_k = tonumber(k)
                    if num_k then
                        node.positional_params[num_k] = v
                    else
                        node.named_params[k] = v
                    end
                end
            end
            
            -- Map positional to named via schema specs
            local specs = schema.specs(cmd)
            if specs then
                for sname, spec in pairs(specs) do
                    if spec.positional_index and node.named_params[sname] == nil then
                        node.named_params[sname] = node.params[sname]
                    end
                end
            end
            
            -- 1. Control Flow & Story Topology
            if cmd == "jump" or cmd == "goto" then
                local tgt = node.params.target or node.params.label or node.params[1] or ""
                if tgt ~= "" and tgt:sub(1,1) ~= "*" and not tgt:find("%.ks$") then
                    tgt = "*" .. tgt
                end
                local storage = node.params.storage or node.params.file
                local cond = node.params.cond or node.params.exp
                
                node.target = tgt
                node.storage = storage
                node.condition = cond
                node.is_flow = true
                
                jumps[#jumps + 1] = {
                    from = current_label,
                    target = tgt,
                    storage = storage,
                    condition = cond,
                    line = line,
                    col = col,
                    node_id = idx,
                    kind = cmd,
                }
                
            elseif cmd == "call" then
                local tgt = node.params.target or node.params.label or node.params[1] or ""
                if tgt ~= "" and tgt:sub(1,1) ~= "*" and not tgt:find("%.ks$") then
                    tgt = "*" .. tgt
                end
                local storage = node.params.storage
                
                node.target = tgt
                node.storage = storage
                node.is_flow = true
                
                calls[#calls + 1] = {
                    from = current_label,
                    target = tgt,
                    storage = storage,
                    line = line,
                    col = col,
                    node_id = idx,
                }
                
            elseif cmd == "return" then
                node.is_flow = true
                
            elseif cmd == "select" then
                block_stack[#block_stack + 1] = { kind = "select", line = line, col = col, choices = {} }
                node.is_block_open = true
                
            elseif cmd == "endselect" then
                if #block_stack > 0 and block_stack[#block_stack].kind == "select" then
                    local sel_block = table.remove(block_stack)
                    node.choice_count = #sel_block.choices
                else
                    diagnostics[#diagnostics + 1] = {
                        line = line, col = col, severity = "ERROR", code = "UNMATCHED_ENDSELECT",
                        message = "[endselect] without matching [select]",
                    }
                end
                
            elseif cmd == "sel" or cmd == "button" or cmd == "link" then
                local tgt = node.params.target or node.params.label or node.params.jump or node.params[1] or ""
                if tgt ~= "" and tgt:sub(1,1) ~= "*" and not tgt:find("%.ks$") then
                    tgt = "*" .. tgt
                end
                local txt = node.params.text or node.params.label or node.params.title or "Choice"
                
                node.target = tgt
                node.choice_text = txt
                node.is_choice = true
                
                local ch_entry = {
                    from = current_label,
                    target = tgt,
                    text = txt,
                    line = line,
                    col = col,
                    node_id = idx,
                }
                choices[#choices + 1] = ch_entry
                
                if #block_stack > 0 and block_stack[#block_stack].kind == "select" then
                    table.insert(block_stack[#block_stack].choices, ch_entry)
                end
                
                -- Translatable choice text
                if txt and txt ~= "" then
                    local trans_key = string.format("%s:%s:choice:L%d:%s", scene_basename, current_scope, line, hash_content(txt))
                    translatables[#translatables + 1] = {
                        key = trans_key,
                        source = txt,
                        speaker = "Choice",
                        line = line,
                        col = col,
                        kind = "choice",
                        file = scene_basename,
                    }
                    node.trans_key = trans_key
                end
                
            elseif cmd == "ending" or cmd == "close" or cmd == "stop" or cmd == "end" then
                local end_name = node.params.name or node.params.id or node.params[1] or current_scope
                node.is_ending = true
                node.ending_name = end_name
                endings[#endings + 1] = {
                    label = current_label,
                    name = end_name,
                    line = line,
                    col = col,
                    node_id = idx,
                }
                
            elseif cmd == "if" then
                block_stack[#block_stack + 1] = { kind = "if", line = line, col = col }
                node.is_block_open = true
            elseif cmd == "endif" then
                if #block_stack > 0 and block_stack[#block_stack].kind == "if" then
                    table.remove(block_stack)
                else
                    diagnostics[#diagnostics + 1] = {
                        line = line, col = col, severity = "ERROR", code = "UNMATCHED_ENDIF",
                        message = "[endif] without matching [if]",
                    }
                end
                
            elseif cmd == "while" or cmd == "for" then
                block_stack[#block_stack + 1] = { kind = cmd, line = line, col = col }
                node.is_block_open = true
            elseif cmd == "endwhile" or cmd == "endfor" then
                local expected = cmd == "endwhile" and "while" or "for"
                if #block_stack > 0 and block_stack[#block_stack].kind == expected then
                    table.remove(block_stack)
                else
                    diagnostics[#diagnostics + 1] = {
                        line = line, col = col, severity = "ERROR", code = "UNMATCHED_LOOP_END",
                        message = string.format("[%s] without matching [%s]", cmd, expected),
                    }
                end
            end
            
            -- 2. Translatable Command Attributes
            local trans_rules = TRANSLATABLE_PARAMS[cmd]
            if trans_rules then
                for p_name, _ in pairs(trans_rules) do
                    local p_val = node.params[p_name]
                    if type(p_val) == "string" and p_val ~= "" then
                        local speaker = (cmd == "ch" and (node.params.name or "Narrator")) or cmd
                        local trans_key = string.format("%s:%s:%s:L%d:%s", scene_basename, current_scope, cmd, line, hash_content(p_val))
                        translatables[#translatables + 1] = {
                            key = trans_key,
                            source = p_val,
                            speaker = speaker,
                            line = line,
                            col = col,
                            kind = cmd,
                            param = p_name,
                            file = scene_basename,
                        }
                        node.trans_key = trans_key
                    end
                end
            end
            
            -- 3. Schema Contract Validation
            if specs then
                for sname, spec in pairs(specs) do
                    if spec.required and node.params[sname] == nil then
                        diagnostics[#diagnostics + 1] = {
                            line = line, col = col, severity = "ERROR", code = "MISSING_REQUIRED_PARAM",
                            message = string.format("[%s] missing required parameter '%s'", cmd, sname),
                        }
                    end
                end
            end
            
        elseif tok.type == "iscript" then
            node.body = tok.body or ""
        end
        
        nodes[idx] = node
    end
    
    -- Check unclosed blocks
    for _, unclosed in ipairs(block_stack) do
        diagnostics[#diagnostics + 1] = {
            line = unclosed.line,
            col = unclosed.col,
            severity = "ERROR",
            code = "UNCLOSED_BLOCK",
            message = string.format("Unclosed [%s] block starting at line %d", unclosed.kind, unclosed.line),
        }
    end
    
    -- -------------------------------------------------------------------------
    -- Flow Graph & Reachability Analysis
    -- -------------------------------------------------------------------------
    local referenced_targets = {}
    local flow_edges = {}
    
    -- Choice edges
    for _, ch in ipairs(choices) do
        if ch.target and ch.target:sub(1,1) == "*" then
            referenced_targets[ch.target] = true
            flow_edges[#flow_edges + 1] = {
                from = ch.from,
                to = ch.target,
                type = "choice",
                text = ch.text,
                line = ch.line,
                col = ch.col,
            }
            if not labels[ch.target] then
                diagnostics[#diagnostics + 1] = {
                    line = ch.line, col = ch.col, severity = "ERROR", code = "BROKEN_CHOICE_TARGET",
                    message = string.format("Choice references undefined label '%s'", ch.target),
                }
            end
        end
    end
    
    -- Jump edges
    for _, jmp in ipairs(jumps) do
        if jmp.target and jmp.target:sub(1,1) == "*" then
            referenced_targets[jmp.target] = true
            flow_edges[#flow_edges + 1] = {
                from = jmp.from,
                to = jmp.target,
                type = "jump",
                condition = jmp.condition,
                line = jmp.line,
                col = jmp.col,
            }
            if not labels[jmp.target] then
                diagnostics[#diagnostics + 1] = {
                    line = jmp.line, col = jmp.col, severity = "ERROR", code = "BROKEN_JUMP_TARGET",
                    message = string.format("Jump references undefined label '%s'", jmp.target),
                }
            end
        end
    end
    
    -- Call edges
    for _, call in ipairs(calls) do
        if call.target and call.target:sub(1,1) == "*" then
            referenced_targets[call.target] = true
            flow_edges[#flow_edges + 1] = {
                from = call.from,
                to = call.target,
                type = "call",
                line = call.line,
                col = call.col,
            }
            if not labels[call.target] then
                diagnostics[#diagnostics + 1] = {
                    line = call.line, col = call.col, severity = "ERROR", code = "BROKEN_CALL_TARGET",
                    message = string.format("Call references undefined label '%s'", call.target),
                }
            end
        end
    end
    
    -- Orphan label diagnostics
    for lbl_name, lbl_data in pairs(labels) do
        if not lbl_data.is_entry and not referenced_targets[lbl_name] then
            diagnostics[#diagnostics + 1] = {
                line = lbl_data.line, col = lbl_data.col, severity = "WARN", code = "UNREFERENCED_LABEL",
                message = string.format("Label '%s' is defined at line %d but never jumped or called to", lbl_name, lbl_data.line),
            }
        end
    end
    
    return {
        file = filename,
        basename = scene_basename,
        nodes = nodes,
        labels = labels,
        jumps = jumps,
        calls = calls,
        choices = choices,
        endings = endings,
        translatables = translatables,
        diagnostics = diagnostics,
        flow_graph = {
            nodes = labels,
            edges = flow_edges,
        }
    }
end

function semantic.parse_file(filepath)
    local f = io.open(filepath, "r")
    if not f then error("Cannot open file: " .. filepath) end
    local content = f:read("*a")
    f:close()
    return semantic.parse(content, filepath)
end

-- -----------------------------------------------------------------------------
-- JSON Serializer
-- -----------------------------------------------------------------------------
local function json_escape(s)
    if s == nil then return "null" end
    s = tostring(s)
    s = s:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    return '"' .. s .. '"'
end

local function val_to_json(v)
    local tv = type(v)
    if tv == "nil" then return "null"
    elseif tv == "number" or tv == "boolean" then return tostring(v)
    elseif tv == "string" then return json_escape(v)
    elseif tv == "table" then
        local is_array = true
        local n = 0
        for k, _ in pairs(v) do
            n = n + 1
            if type(k) ~= "number" or k < 1 or math.floor(k) ~= k then
                is_array = false
                break
            end
        end
        if is_array and n > 0 and v[1] ~= nil then
            local items = {}
            for i = 1, #v do items[#items + 1] = val_to_json(v[i]) end
            return "[" .. table.concat(items, ",") .. "]"
        else
            local pairs_list = {}
            for k, val in pairs(v) do
                pairs_list[#pairs_list + 1] = json_escape(k) .. ":" .. val_to_json(val)
            end
            return "{" .. table.concat(pairs_list, ",") .. "}"
        end
    end
    return "null"
end

function semantic.to_json(model)
    return val_to_json(model)
end

-- -----------------------------------------------------------------------------
-- Mermaid Exporter
-- -----------------------------------------------------------------------------
function semantic.to_mermaid(model)
    local clean_id = function(name)
        return (name or ""):gsub("^%*", ""):gsub("[^%w_]", "_")
    end
    
    local sname = model.basename or "Story"
    local lines = {
        "```mermaid",
        "flowchart TD",
        "    subgraph " .. clean_id(sname) .. " [\"" .. sname .. "\"]"
    }
    
    for lbl_name, lbl in pairs(model.labels) do
        local nid = clean_id(lbl_name)
        local title = (lbl.title or lbl_name):gsub('"', '')
        if lbl.is_entry then
            lines[#lines + 1] = string.format('        %s(["Start: %s"])', nid, sname)
        else
            -- Check if this label contains choices or ending
            local is_end = false
            for _, ed in ipairs(model.endings) do
                if ed.label == lbl_name then is_end = true break end
            end
            if is_end then
                lines[#lines + 1] = string.format('        %s(["Ending: %s"])', nid, title)
            else
                lines[#lines + 1] = string.format('        %s["%s (L%d)"]', nid, title, lbl.line)
            end
        end
    end
    lines[#lines + 1] = "    end\n"
    
    for _, edge in ipairs(model.flow_graph.edges) do
        local src_id = clean_id(edge.from)
        local tgt_id = clean_id(edge.to)
        if edge.type == "choice" then
            local txt = (edge.text or "Choice"):gsub('"', '')
            lines[#lines + 1] = string.format('    %s -->|"%s"| %s', src_id, txt, tgt_id)
        elseif edge.type == "jump" then
            local cond_str = edge.condition and (" [if " .. edge.condition:gsub('"', '') .. "]") or ""
            lines[#lines + 1] = string.format('    %s -.->|"jump%s"| %s', src_id, cond_str, tgt_id)
        elseif edge.type == "call" then
            lines[#lines + 1] = string.format('    %s ==>|"call"| %s', src_id, tgt_id)
        end
    end
    
    lines[#lines + 1] = "```"
    return table.concat(lines, "\n")
end

-- -----------------------------------------------------------------------------
-- i18n CSV & PO Exporters
-- -----------------------------------------------------------------------------
function semantic.to_csv(model)
    local csv_escape = function(s)
        s = tostring(s or "")
        if s:find('[\",\n\r]') then
            return '"' .. s:gsub('"', '""') .. '"'
        end
        return s
    end
    
    local lines = { "Key,File,Line,Col,Speaker,Kind,SourceText,Translation_zh,Translation_en,Translation_ja" }
    for _, t in ipairs(model.translatables) do
        lines[#lines + 1] = string.format("%s,%s,%d,%d,%s,%s,%s,%s,%s,%s",
            csv_escape(t.key),
            csv_escape(t.file),
            t.line,
            t.col,
            csv_escape(t.speaker),
            csv_escape(t.kind),
            csv_escape(t.source),
            csv_escape(t.source),
            csv_escape(t.source),
            csv_escape(t.source)
        )
    end
    return table.concat(lines, "\n")
end

function semantic.to_po(model, lang)
    lang = lang or "zh"
    local lines = {
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: Caesura Visual Novel\\n"',
        '"Language: ' .. lang .. '\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        "",
    }
    for _, t in ipairs(model.translatables) do
        lines[#lines + 1] = string.format('#: %s:%d (Speaker: %s, Kind: %s)', t.file, t.line, t.speaker, t.kind)
        lines[#lines + 1] = string.format('msgctxt "%s"', t.key)
        lines[#lines + 1] = string.format('msgid "%s"', (t.source or ""):gsub('"', '\\"'):gsub("\n", "\\n"))
        lines[#lines + 1] = 'msgstr ""\n'
    end
    return table.concat(lines, "\n")
end

return semantic
