-- Real runner/tokenizer/compiler/scheduler/choice callback EOF regression.
-- Only host C++ services use the established orphan-suite hardware substitutes.
-- No selected-choice/context/stop flag is injected. Root executes standalone.
package.path="scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;"..package.path
local function callable(t)
    return setmetatable(t or {}, {
        __index = function(self, key)
            if type(key) ~= "string" then return nil end
            rawset(self, key, function(...) return true end)
            return self[key]
        end,
    })
end

_G.KAG = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
})
_G.Render  = callable({})
_G.DevCore = callable({})
_G.Engine  = callable({})
_G.backend = callable({
    is_voice_playing  = function() return false end,
    is_bgm_playing    = function() return false end,
    get_active_voices = function() return 0 end,
})

local runner=require("kag_runner")
local flow=require("flow")
local DIR="assets/script/test_choice_eof_contract"
local windows=package.config:sub(1,1)=="\\"
local files={DIR.."/caller.ks",DIR.."/callee.ks"}
for _,path in ipairs(files) do local prior=io.open(path,"rb");if prior then prior:close();error("Refusing existing fixture "..path) end end
if windows then os.execute('mkdir "'..DIR:gsub('/','\\')..'" 2>nul') else os.execute('mkdir -p "'..DIR..'"') end
local function write(path,text)local f=assert(io.open(path,"wb"));assert(f:write(text));assert(f:close())end
local passed,failed=0,0
local function check(name,condition,actual)
 print((condition and "PASS " or "FAIL ")..name.." actual="..tostring(actual))
 if condition then passed=passed+1 else failed=failed+1 end
end

local function one(kind,at_eof)
    local label=kind..(at_eof and "-EOF" or "-with-tail-control")
    write(files[1],[[
[set var="lf.owner" value="caller"]
[call storage="test_choice_eof_contract/callee.ks"]
[set var="f.tail" value=1]
[end]
]])
    local head=kind=="select" and "[select]\n" or ""
    local item=kind=="select" and "sel" or "button"
    local close=kind=="select" and "endselect" or "endbutton"
    write(files[2],[[
[set var="lf.owner" value="callee"]
[jump target="*menu"]
*wrong
[set var="f.branch" value="wrong"]
[return]
*chosen
[set var="f.branch" value="chosen"]
[return]
*menu
]]..head..'['..item..' text="WRONG FIRST" target="*wrong"]\n'
       ..'['..item..' text="CHOOSE SECOND" target="*chosen"]\n'
       ..'['..close..']\n'..(at_eof and '' or '[end]\n'))
    flow.clear_cache()
    local started,why=runner.start(files[1]);check(label.." starts",started,why)
    local owner=runner.get_ctx();local clicked,ended=0,false
    if started then
        for frame=1,200 do
            local _,reason=runner.update(0.016)
            local ctx=runner.get_ctx()
            if reason=="ended" then ended=true;break end
            assert(ctx==owner,"Unexpected owner replacement")
            if ctx._choiceMode then
                assert(clicked==0,"Choice unexpectedly repeated")
                local button=assert((ctx._choiceButtonsActive or {})[2])
                assert(type(_G._KAG_onClick)=="function","No real installed choice callback")
                _G._GAME_MOUSE_X=100;_G._GAME_MOUSE_Y=button.y+button.h/2
                _G._KAG_onClick();clicked=clicked+1
            end
        end
    end
    check(label.." one actual callback",clicked==1,clicked)
    check(label.." selected callee branch",owner and owner.f.branch=="chosen",owner and owner.f.branch)
    check(label.." caller continuation",owner and owner.f.tail==1,owner and owner.f.tail)
    check(label.." caller local restored",owner and owner.lf.owner=="caller",owner and owner.lf.owner)
    check(label.." complete call stack",owner and #(owner.call_stack or {})==0,owner and #(owner.call_stack or {}))
    check(label.." natural ended",ended,ended)
    runner.stop()
end
local ok,err=xpcall(function()
 for _,kind in ipairs({'button','select'}) do one(kind,false);one(kind,true) end
end,debug.traceback)
-- Only these files were absent before this attempt and created above.
for _,path in ipairs(files) do os.remove(path) end
if not ok then error(err) end
print(string.format("CHOICE_EOF_CONTRACT checks=%d passed=%d failed=%d",passed+failed,passed,failed))
if failed>0 then error("Choice EOF contract failed: "..failed) end
