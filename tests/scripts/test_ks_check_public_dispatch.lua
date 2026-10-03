-- Fresh-process static-checker regression (orphan runner discovers this file).
-- Calls the actual shipped ks_check.checkScene against owned temporary files;
-- captures only diagnostic output, never substitutes registry or checker logic.
package.path="scripts/?.lua;scripts/?/init.lua;"..package.path
local ks=assert(require("ks_check"))
local passed,failed=0,0
local function check(name,value)
    if value then passed=passed+1;print("PASS "..name)
    else failed=failed+1;print("FAIL "..name) end
end
local function analyze(source)
    local path=os.tmpname()
    local file=assert(io.open(path,"wb"));file:write(source);file:close()
    local prior=print;local lines={}
    print=function(line)lines[#lines+1]=tostring(line)end
    local ok,err=pcall(ks.checkScene,path)
    print=prior
    assert(os.remove(path))
    assert(ok,tostring(err))
    return table.concat(lines,"\n")
end
for _,name in ipairs({"_hideHr","_relocalizeCC","_relocalizeChoices","_renderNameplate","flush_cache","get_texture","has_pending_transition","is_loaded","is_pending","preload_transition","promote_transition_slot","push_backlog","relocalize_backlog","relocalize_page","render","return_to_caller","update","wait_click","Bezier","LUTCache","_audioCache","_pendingAudio","_pendingTextures","_textureCache","_transitionSlot","gesture_defaults"}) do
    local diagnostic=analyze("["..name.."]\n[end]\n")
    check("internal/table name rejected by checker "..name,
        diagnostic:find("unknown KAG command '"..name.."'",1,true)~=nil)
end
do
    local diagnostic=analyze("[endcase]\n[end]\n")
    check("undeclared endcase is not admitted by stale lint whitelist",
        diagnostic:find("unknown KAG command 'endcase'",1,true)~=nil)
end
for _,name in ipairs({"flush_cache","render","Bezier"}) do
    local diagnostic=analyze("[macro "..name.."]\n[set f.value 1]\n[endmacro]\n["..name.."]\n[end]\n")
    check("private macro keeps independent name "..name,
        not diagnostic:find("unknown KAG command",1,true) and not diagnostic:find("shadows built-in command",1,true))
end
do
    local diagnostic=analyze('[set f.value 1]\n[if exp="f.value == 1"]\n[showtext text="PUBLIC"]\n[endif]\n[clear]\n[ct]\n[end]\n')
    check("public and compatibility commands remain known",not diagnostic:find("unknown KAG command",1,true))
    diagnostic=analyze('[macro ch]\n[set f.value 1]\n[endmacro]\n[ch]\n[end]\n')
    check("public macro shadow warning retained",diagnostic:find("shadows built-in command 'ch'",1,true)~=nil)
end
print(string.format("KS PUBLIC DISPATCH: %d passed, %d failed",passed,failed))
assert(failed==0,"static dispatch boundary failures: "..failed)
