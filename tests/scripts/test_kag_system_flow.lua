package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;" .. package.path

-- Test handlers are explicit DSL extensions; a recording KAG table alone
-- does not register commands. Keep the production public selector intact.
require("kag.commands.text") -- actual ch/text contracts before recorder replacement

local passed, failed = 0, 0
local function ok(name, cond)
    if cond then passed = passed + 1
    else failed = failed + 1; print("  FAIL: " .. name) end
end

-- ==========================================
-- System commands (no C++ deps)
-- ==========================================
print("[system]")

-- wait must run inside coroutine (uses coroutine.yield)
local System = require("kag.commands.system")
local co = coroutine.create(function()
    local ctx = { wait_counter = 0 }
    System.wait(ctx, { time = 100 })
end)
local ok2, err = coroutine.resume(co)
ok("wait resumes in coroutine", ok2)

-- history: toggles history mode
local ctx = { show_history = false }
System.history(ctx, {})
ok("history is callable", type(System.history) == "function")
-- Note: history toggle depends on internal pipeline state; full behavior tested in E2E


-- eval/emb: verify they exist (require sandbox for full execution)
ok("eval exists", type(System.eval) == "function")
ok("emb exists", type(System.emb) == "function")

-- ==========================================
-- Flow control parsing (tokenizer level)
-- ==========================================
print("[flow-control]")
local tokenizer = require("tokenizer")

-- if/else/endif
local t = tokenizer.parse("[if exp=\"true\"]\n[text text=\"yes\"]\n[else]\n[text text=\"no\"]\n[endif]\n")
local cmds = {}; for _, v in ipairs(t) do if v.type == "command" then table.insert(cmds, v.cmd) end end
ok("if", cmds[1] == "if")
ok("else", cmds[3] == "else")
ok("endif", cmds[5] == "endif")

-- jump/link/call
t = tokenizer.parse("[jump target=\"*label\"]")
ok("jump parsed", t[1].type == "command" and t[1].cmd == "jump")
t = tokenizer.parse("[link target=\"chapter2\"]")
ok("link parsed", t[1].cmd == "link")
t = tokenizer.parse("[call target=\"sub\"]")
ok("call parsed", t[1].cmd == "call")

-- switch/case/endswitch
t = tokenizer.parse("[switch exp=\"var\"]\n[case value=\"1\"]\n[text text=\"one\"]\n[endswitch]\n")
cmds = {}; for _, v in ipairs(t) do if v.type == "command" then table.insert(cmds, v.cmd) end end
ok("switch", cmds[1] == "switch")
ok("case", cmds[2] == "case")
ok("endswitch", cmds[4] == "endswitch")

-- end/stop/return
t = tokenizer.parse("[end]")
ok("end", t[1].cmd == "end")
t = tokenizer.parse("[stop]")
ok("stop", t[1].cmd == "stop")
t = tokenizer.parse("[return]")
ok("return", t[1].cmd == "return")

-- [stop] terminates execution (regression: flow_commands declared stop
-- but no scheduler branch handled it -- it fell through to the
-- unknown-command path and rendered as dialogue text)
do
    local sched = require("scheduler")
    local st = tokenizer.parse('[ch text="a"]\n[stop]\n[ch text="b"]')
    local seen = {}
    package.loaded["kag"] = { ch = function(_, p) seen[#seen + 1] = p.text end }
    local sctx = { f = {}, sf = {}, tf = {}, mp = {},
        tokens = st, token_index = 1, call_stack = {},
        layers = {}, backlog = {}, macros = {}, stop_flag = false,
        load_tokens = function() end }
    local sco = coroutine.create(function() sched.run(sctx, st, 1) end)
    local sn = 0
    while coroutine.status(sco) == "suspended" and sn < 10 do
        coroutine.resume(sco, 16)
        sn = sn + 1
    end
    ok("stop halts execution", #seen == 1 and seen[1] == "a")
end

-- macro/endmacro/erasemacro
t = tokenizer.parse("[macro name=\"greet\"]")
ok("macro parsed", t[1].cmd == "macro")
t = tokenizer.parse("[endmacro]")
ok("endmacro parsed", t[1].cmd == "endmacro")
t = tokenizer.parse("[erasemacro name=\"greet\"]")
ok("erasemacro parsed", t[1].cmd == "erasemacro")

print(string.format("\n%d passed, %d failed", passed, failed))
if failed > 0 then error("kag_system_flow: " .. failed .. " checks failed") end
