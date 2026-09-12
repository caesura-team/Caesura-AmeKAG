-- U17 IME composition/form ownership regression. NOT an OS IME event trace.
-- Reuses the U10 private-environment approach with actual TextCommands.input,
-- Operation, CancelToken, TextScene, layers, layout, and viewport modules.
-- Only the native backend boundary is stubbed; no input algorithm is copied.
-- Optional argv[1] selects a production root, permitting an artifact-only CWD.
local source_root = arg and arg[1] or "."
local passed, failed = 0, 0
local function check(name, condition)
    print((condition and "PASS " or "FAIL ") .. name)
    if condition then passed = passed + 1 else failed = failed + 1 end
end
local handler_names = {
    "_KAG_onTextInput", "_KAG_onTextEditing", "_KAG_onKeyDown", "_KAG_onClick",
}
local HAN = "\xE6\xB1\x89"
local ZI = "\xE5\xAD\x97"
local SMILE = "\xF0\x9F\x99\x82"
local E_ACUTE = "\xC3\xA9"

local function fixture()
    local env = setmetatable({}, {__index=_G})
    env._G = env
    local state = {starts=0, stops=0, active=false, forwarded=0}
    local originals = {}
    for _, name in ipairs(handler_names) do
        originals[name] = function() state.forwarded = state.forwarded + 1 end
        env[name] = originals[name]
    end
    local modules = {backend={
        line_height=function() return 24 end,
        get_resolution=function() return 1280,720 end,
        set_text_input_rect=function(x,y,w,h,cursor) state.rect={x=x,y=y,w=w,h=h,cursor=cursor} end,
        start_text_input=function() state.starts=state.starts+1; state.active=true end,
        stop_text_input=function() state.stops=state.stops+1; state.active=false end,
    }}
    -- Shared-suite package.loaded entries must not supply a mocked module.
    env.package = setmetatable({loaded=modules}, {__index=package})
    env.require = function(name)
        if modules[name] == nil then
            local path = source_root .. "/scripts/" .. name:gsub("%.","/") .. ".lua"
            local file = assert(io.open(path,"rb"))
            local source = file:read("*a"); file:close()
            source = source:gsub("^\239\187\191", "")
            modules[name] = assert(load(source,"@"..path,"t",env))()
        end
        return modules[name]
    end
    return {env=env,state=state,originals=originals,
        commands=env.require("kag.commands.text"),
        operation=env.require("kag.operation"),scene=env.require("kag.text_scene")}
end
local function context()
    return {f={},tf={},sf={},mp={},_session_active=true}
end
local function start(f,ctx,params)
    params = params or {}
    params.name = params.name or "f.name"
    local co = coroutine.create(function() f.commands.input(ctx,params) end)
    local ok,err = coroutine.resume(co)
    assert(ok,tostring(err))
    assert(coroutine.status(co)=="suspended", "real input command did not open")
    return co
end
local function key(f,name)
    local codes={backspace=8,["return"]=13,escape=27}
    f.env._KAG_onKeyDown(assert(codes[name]),name)
end
local function field(f,ctx)
    for _,draw in ipairs(f.scene.get_state(ctx).draws) do
        if draw.group=="text_input" and type(draw.text)=="string" and draw.text:sub(-1)=="|" then
            return draw.text
        end
    end
end
local function restored(f)
    for _,name in ipairs(handler_names) do
        if f.env[name]~=f.originals[name] then return false end
    end
    return true
end
local function finish(f,ctx,co,label)
    local ok,err=coroutine.resume(co)
    assert(ok,tostring(err))
    check(label..": operation ends",coroutine.status(co)=="dead" and #(ctx.active_operations or {})==0)
    check(label..": input stops once",f.state.starts==1 and f.state.stops==1 and not f.state.active)
    check(label..": handlers and UI released",restored(f) and field(f,ctx)==nil and not ctx._inputMode
        and not ctx.waiting_input)
end

do
    local f,ctx=fixture(),context()
    local co=start(f,ctx,{default="A",maxlen=8})
    f.env._KAG_onTextEditing("han",0,3)
    check("composition preview is separate from committed field",field(f,ctx)=="A[han]|" and ctx.f.name==nil)
    local handler=f.env._KAG_onKeyDown
    key(f,"return")
    check("composition Enter does not submit or stop input",ctx.f.name==nil and ctx._inputMode
        and ctx.waiting_input and f.state.active and f.state.stops==0)
    check("composition Enter preserves owner and preview",f.env._KAG_onKeyDown==handler
        and field(f,ctx)=="A[han]|" and #(ctx.active_operations or {})==1)
    f.env._KAG_onTextInput(HAN)
    check("final TextInput commits Han into buffer only",field(f,ctx)=="A"..HAN.."|"
        and ctx.f.name==nil and f.state.stops==0)
    key(f,"return")
    check("independent Enter saves the complete committed string",ctx.f.name=="A"..HAN)
    finish(f,ctx,co,"composition submit")
end

do
    local f,ctx=fixture(),context()
    ctx.f.name="saved-before-form"
    local co=start(f,ctx,{default="draft",btn_cancel="Cancel"})
    f.env._KAG_onTextEditing("han",0,3)
    key(f,"escape")
    check("first Escape cancels composition but keeps form",ctx._inputMode and ctx.waiting_input
        and f.state.active and f.state.stops==0 and #(ctx.active_operations or {})==1)
    check("first Escape removes only the preedit preview",field(f,ctx)=="draft|"
        and ctx.f.name=="saved-before-form")
    -- A subsequent empty editing notification must not close the form either.
    f.env._KAG_onTextEditing("",0,0)
    check("empty Editing is not form cancellation",ctx._inputMode and f.state.stops==0)
    key(f,"escape")
    check("second Escape cancels without overwriting saved value",ctx.f.name=="saved-before-form"
        and not ctx._inputMode and f.state.stops==1)
    finish(f,ctx,co,"composition cancel")
end

do
    local f,ctx=fixture(),context()
    local co=start(f,ctx,{default="A"..HAN})
    f.env._KAG_onTextEditing("han",0,3)
    key(f,"backspace")
    key(f,"backspace") -- No Editing callback has arrived yet.
    check("Backspace waits for IME editing updates",field(f,ctx)=="A"..HAN.."[han]|"
        and ctx._inputMode and f.state.stops==0)
    f.env._KAG_onTextEditing("ha",0,2)
    check("Editing callback updates only preedit",field(f,ctx)=="A"..HAN.."[ha]|" and ctx.f.name==nil)
    key(f,"backspace")
    check("repeated preedit Backspace preserves committed UTF8",field(f,ctx)=="A"..HAN.."[ha]|")
    f.env._KAG_onTextEditing("",0,0)
    check("host clears composition explicitly",field(f,ctx)=="A"..HAN.."|")
    key(f,"backspace")
    check("post-composition Backspace removes one complete codepoint",field(f,ctx)=="A|")
    key(f,"return")
    check("Backspace editing preserves intended submitted value",ctx.f.name=="A")
    finish(f,ctx,co,"composition backspace")
end

do
    local f,ctx=fixture(),context()
    local co=start(f,ctx,{default="A",maxlen=3})
    f.env._KAG_onTextEditing("candidate",0,9)
    f.env._KAG_onTextInput(HAN..SMILE..ZI)
    check("maxlen cuts committed input at codepoint boundary",field(f,ctx)=="A"..HAN..SMILE.."|")
    key(f,"backspace")
    check("four-byte UTF8 Backspace is whole-character",field(f,ctx)=="A"..HAN.."|")
    f.env._KAG_onTextInput(E_ACUTE)
    key(f,"return")
    local expected="A"..HAN..E_ACUTE
    check("ASCII two-byte and three-byte committed result remains valid",ctx.f.name==expected
        and utf8.len(ctx.f.name or "")==3)
    finish(f,ctx,co,"UTF8 commit boundary")
end

for _,termination in ipairs({"cancel","close"}) do
    local f,old=fixture(),context()
    old.f.name="old-saved"
    local old_co=start(f,old,{default="old-draft",btn_cancel="Cancel"})
    f.env._KAG_onTextEditing("old-composition",0,3)
    local stale={}
    for _,name in ipairs(handler_names) do stale[name]=f.env[name] end
    if termination=="cancel" then f.operation.cancel_all(old) end
    assert(coroutine.close(old_co))
    check(termination..": composition owner retires once",f.state.stops==1 and not f.state.active
        and restored(f) and field(f,old)==nil and #(old.active_operations or {})==0)

    local successor=context()
    local next_co=start(f,successor,{default="B",btn_cancel="Cancel"})
    f.env._KAG_onTextEditing("new",0,3)
    local next_handlers={}
    for _,name in ipairs(handler_names) do next_handlers[name]=f.env[name] end
    stale._KAG_onTextInput(ZI)
    stale._KAG_onTextEditing("",0,0)
    stale._KAG_onKeyDown(8,"backspace")
    stale._KAG_onKeyDown(13,"return")
    stale._KAG_onKeyDown(27,"escape")
    f.env._GAME_MOUSE_X=f.state.rect.x+f.state.rect.w-100
    f.env._GAME_MOUSE_Y=f.state.rect.y+f.state.rect.h-30
    stale._KAG_onClick()
    local owners_preserved=true
    for _,name in ipairs(handler_names) do owners_preserved=owners_preserved and f.env[name]==next_handlers[name] end
    check(termination..": stale handlers cannot edit or close successor",owners_preserved
        and successor._inputMode and successor.waiting_input and successor.f.name==nil
        and field(f,successor)=="B[new]|" and f.state.active and f.state.stops==1)
    check(termination..": stale handlers do not publish old results or forward",old.f.name=="old-saved"
        and field(f,old)==nil and f.state.forwarded==0)
    f.env._KAG_onTextInput(HAN)
    key(f,"return")
    local ok,err=coroutine.resume(next_co); assert(ok,tostring(err))
    check(termination..": successor submits only its own committed text",successor.f.name=="B"..HAN
        and old.f.name=="old-saved" and coroutine.status(next_co)=="dead")
    check(termination..": successor releases its own operation and native input",restored(f)
        and f.state.starts==2 and f.state.stops==2 and not f.state.active
        and #(successor.active_operations or {})==0 and field(f,successor)==nil)
end

print(string.format("U17 IME COMPOSITION TESTS: %d passed, %d failed",passed,failed))
if failed>0 then os.exit(1) end
