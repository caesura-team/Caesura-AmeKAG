-- ===========================================================================
--  Caesura (AmeKAG) - Full Feature Test Demo v2
--  Bright colors, video playback, WAV audio
-- ===========================================================================

local layers  = require("layers")
local backend = require("backend")
local System  = require("system")
local w, h = backend.get_resolution()
if not w then w, h = require("viewport").wh() end  -- viewport-relative (was 1280/720)

-- Helpers
local function solid(r,g,b,a)
    return backend.create_solid_texture(math.floor(r),math.floor(g),math.floor(b),math.floor(a or 255))
end
local function load_tex(path)
    local id = backend.load_texture(path)
    if id and id > 0 then print("[OK] " .. path .. " -> " .. tostring(id))
    else print("[MISS] " .. path) end
    return id or 0
end
local function draw_text(x,y,text,color,scale)
    backend.render_text(text, x, y, scale or 1.0, color[1],color[2],color[3],color[4])
end
local function center(y,text,color,scale)
    local tw = #text * (scale or 1.0) * 10
    draw_text((w-tw)/2, y, text, color, scale)
end

-- Bright colors
local C = {
    bg   = {40,35,70,255}, gold={255,200,60,255}, white={255,255,255,255},
    dim  = {180,180,210,255}, blue={80,160,255,255}, green={60,220,100,255},
    red  = {255,60,60,255}, yellow={255,255,80,255},
}

-- State
local S = {frame=0,phase=-1,timer=0,autoTmr=0,init=false,tex={},lyrs={},vid=nil}

-- Preload
local function preload()
    if S.tex.done then return end
    S.tex.classroom = load_tex("assets/bg/classroom.png")
    S.tex.hana      = load_tex("assets/bg/hana.png")
    S.tex.girl      = load_tex("assets/fg/girl_uniform.png")
    S.tex.done      = true
end

-- Clear
local function clear()
    local root = layers.get_root()
    if root and root.children then
        for _,c in ipairs(root.children) do
            pcall(function() layers.remove_layer(c) end)
        end
    end
    S.init = false; S.lyrs = {}
end

-- ============================== SCENE -1: TITLE ==============================
local function t_init()
    layers.init()
    local root = layers.get_root()
    -- Bright red flash to confirm rendering
    root.texture = solid(180,20,20,255)
    layers.set_layer_image(root,root.texture)
    S.autoTmr = 0; S.init = true
    preload()
    print("[DEMO] Title ready - click to start")
end
local function t_update()
    center(h*0.35, "CAESURA (AmeKAG)", C.gold, 3.2)
    center(h*0.45, "Full Feature Test Demo", C.white, 1.8)
    if S.timer > 2 then
        local blink = 140 + math.floor(math.abs(math.sin(S.timer*4))*115)
        center(h-60, "[ Click to start ]", {C.dim[1],C.dim[2],C.dim[3],blink}, 1.0)
    end
end

-- ============================== SCENE 0: TEXT ==============================
local function s0_init()
    clear(); preload()
    local bg = layers.get_root()
    if S.tex.classroom>0 then layers.set_layer_image(bg,S.tex.classroom) end
    S.autoTmr = 5; S.init = true
end
local function s0_update()
    center(40, "=== Scene 01: Text & Dialogue ===", C.gold, 1.8)
    draw_text(80,130,"* Font colors:",C.blue,1.3)
    draw_text(100,170,"White - narration text",C.white,1.2)
    draw_text(100,205,"Gold - emphasis text",C.gold,1.2)
    draw_text(100,240,"Green - status text",C.green,1.2)
    draw_text(80,310,"* Ruby annotation:",C.blue,1.3)
    backend.render_ruby("Kanji","reading",100,350)
    draw_text(80,430,"* Dialogue:",C.blue,1.3)
    draw_text(100,470,'"Welcome home, Haru-chan."',C.white,1.3)
end

-- ============================== SCENE 1: LAYERS ==============================
local function s1_init()
    clear(); preload()
    local bg = layers.get_root()
    if S.tex.classroom>0 then layers.set_layer_image(bg,S.tex.classroom) end
    if S.tex.girl>0 then
        S.lyrs.chara = layers.add_layer(bg,{name="chara",z=10,x=w-420,y=60,w=350,h=550,visible=true})
        layers.set_layer_image(S.lyrs.chara,S.tex.girl)
    end
    S.autoTmr = 6; S.init = true
end
local function s1_update()
    center(40,"=== Scene 02: Layers & Sprites ===",C.gold,1.8)
    draw_text(60,130,"* classroom.png background",C.blue,1.3)
    draw_text(60,170,"* girl_uniform.png sprite ->",C.blue,1.3)
    local cy = math.floor(S.timer/1.5)%4
    local msgs = {"Default position","Opacity 80%","Moved to left","Hidden"}
    draw_text(80,250,"  "..msgs[cy+1],C.green,1.2)
    if cy==1 and S.lyrs.chara then layers.set_layer_opacity(S.lyrs.chara,204) end
    if cy==2 and S.lyrs.chara then layers.move_layer(S.lyrs.chara,80,60); layers.set_layer_opacity(S.lyrs.chara,255) end
    if cy==3 and S.lyrs.chara then layers.set_layer_visible(S.lyrs.chara,false) end
end

-- ============================== SCENE 2: AUDIO ==============================
local function s2_init()
    clear(); preload()
    local bg = layers.get_root()
    if S.tex.hana>0 then layers.set_layer_image(bg,S.tex.hana) end
    S.autoTmr = 8; S.init = true
end
local function s2_update()
    center(40,"=== Scene 03: Audio System ===",C.gold,1.8)
    local t = S.timer
    if t>0.5 and t<0.6 then backend.audio_play("bgm","assets/bgm/daily.wav",{fadein=2.0}) end
    if t>6.0 and t<6.1 then backend.audio_stop("bgm",{fadeout=2.0}) end
    if t>2.0 and t<2.1 then backend.audio_play("se","assets/se/click.wav") end
    if t>4.0 and t<4.1 then backend.audio_play("voice","assets/voice/line01.wav") end
    draw_text(80,150,"BGM: "..(t>0.6 and t<6.0 and "Playing daily.wav" or "Stopped"),C.green,1.2)
    draw_text(80,220,"SE: click.wav fired",C.green,1.2)
    draw_text(80,290,'Voice: "Welcome home, Haru-chan"',C.green,1.2)
    draw_text(80,380,"BGM/SE/Voice independent bus control",C.dim,1.1)
end

-- ============================== SCENE 3: FLOW ==============================
local function s3_init()
    clear()
    local bg = layers.get_root()
    bg.texture = solid(C.bg[1],C.bg[2],C.bg[3],255)
    layers.set_layer_image(bg,bg.texture)
    S.autoTmr = 0; S.init = true
end
local function s3_update()
    center(40,"=== Scene 04: Flow Control ===",C.gold,1.8)
    local items = {"if/else | jump | call/return | macro","button (choices) | eval | wait | end/stop"}
    for i,item in ipairs(items) do
        draw_text(100,180+(i-1)*50,"* "..item,C.white,1.2)
    end
    draw_text(80,340,"Tokenizer + Scheduler + 69 KAG Neo-Genesis Commands",C.blue,1.2)
    draw_text(80,h-50,"Click to continue",C.dim,0.9)
end

-- ============================== SCENE 4: TRANSITIONS ==============================
local function s4_init()
    clear(); preload()
    local bg = layers.get_root()
    if S.tex.classroom>0 then layers.set_layer_image(bg,S.tex.classroom) end
    S.autoTmr = 8; S.init = true
end
local function s4_update()
    center(40,"=== Scene 05: Transitions & VFX ===",C.gold,1.8)
    if S.timer>3 then
        local bg = layers.get_root()
        if S.tex.hana>0 then layers.set_layer_image(bg,S.tex.hana) end
        draw_text(80,130,"* hana.png (flower field) - switched!",C.green,1.3)
    else
        draw_text(80,130,"* classroom.png -> switching to hana.png...",C.blue,1.3)
    end
    draw_text(80,220,"* trans: crossfade/dissolve/wipe",C.dim,1.2)
    draw_text(80,280,"* quake: screen shake",C.dim,1.2)
    draw_text(80,340,"* vfx: fade/flash/blur/sepia",C.dim,1.2)
    draw_text(80,400,"* move: position animation",C.dim,1.2)
end

-- ============================== SCENE 5: VIDEO ==============================
local function s5_init()
    clear()
    local bg = layers.get_root()
    bg.texture = solid(0,0,0,255)
    layers.set_layer_image(bg,bg.texture)
    S.autoTmr = 10; S.init = true
end
local function s5_update()
    center(40,"=== Scene 06: Video Playback ===",C.gold,1.8)
    if S.timer>0.5 and not S.vid then
        local ok,vid = pcall(function()
            return backend.load_texture("assets/video/opening.mpg")
        end)
        if ok and vid and vid>0 then
            S.vid = vid
            print("[VIDEO] opening.mpg loaded as texture "..tostring(vid))
        else
            print("[VIDEO] Failed to load opening.mpg")
        end
    end
    if S.vid then
        draw_text(80,200,"opening.mpg loaded (162 MB)",C.green,1.3)
        draw_text(80,260,"API: video_play/stop/pause/resume",C.dim,1.1)
        draw_text(80,300,"API: video_get_texture/get_size",C.dim,1.1)
        draw_text(80,340,"API: video_is_playing/has_ended",C.dim,1.1)
    else
        draw_text(80,200,"Loading opening.mpg...",C.yellow,1.3)
    end
end

-- ============================== SCENE 6: SAVE/LOAD ==============================
local function s6_init()
    clear()
    local bg = layers.get_root()
    bg.texture = solid(C.bg[1],C.bg[2],C.bg[3],255)
    layers.set_layer_image(bg,bg.texture)
    local ok1 = KAG.save_game(1, {scene="demo_test",time=os.time()}, "demo_test", 1)
    local data = KAG.load_game(1)
    print(ok1 and "[PASS] Save" or "[FAIL] Save")
    print((data and data.scene=="demo_test") and "[PASS] Load" or "[FAIL] Load")
    S.autoTmr = 5; S.init = true
end
local function s6_update()
    center(40,"=== Scene 07: Save & Load ===",C.gold,1.8)
    local items = {"KAG.save_game(slot,data)","KAG.load_game(slot)","Auto migration v1->v5","AES-256-GCM encryption"}
    for i,item in ipairs(items) do
        draw_text(100,160+(i-1)*45,"* "..item,C.white,1.2)
    end
end

-- ============================== SCENE 7: END ==============================
local function s7_init()
    clear(); preload()
    local bg = layers.get_root()
    if S.tex.hana>0 then layers.set_layer_image(bg,S.tex.hana) end
    S.autoTmr = 999; S.init = true
    -- Stop any playing audio
    backend.audio_stop("bgm")
end
local function s7_update()
    center(h*0.25,"=== Test Complete ===",C.gold,3.0)
    if S.timer>1.5 then
        center(h*0.40,"Caesura (AmeKAG) Engine",C.white,1.6)
        center(h*0.48,"All systems operational",C.green,1.3)
    end
    if S.timer>3 then
        center(h*0.60,"github.com/caesura-team/Caesura-AmeKAG",C.dim,1.0)
    end
end

-- ============================== DISPATCH ==============================
local scenes = {
    {init=t_init,update=t_update,auto=false},
    {init=s0_init,update=s0_update,auto=true},
    {init=s1_init,update=s1_update,auto=true},
    {init=s2_init,update=s2_update,auto=true},
    {init=s3_init,update=s3_update,auto=true},
    {init=s4_init,update=s4_update,auto=true},
    {init=s5_init,update=s5_update,auto=true},
    {init=s6_init,update=s6_update,auto=true},
    {init=s7_init,update=s7_update,auto=false},
}

function engine_render() layers.render() end

function engine_update(dt)
    S.frame = S.frame + 1
    S.timer = S.timer + (dt or 0.016)
    local idx = S.phase + 2
    local sc = scenes[idx]
    if not sc then return end
    if not S.init and sc.init then sc.init() end
    if sc.update then sc.update() end
    if sc.auto and S.autoTmr>0 and S.timer>=S.autoTmr then
        S.phase = S.phase + 1; S.timer = 0; S.autoTmr = 0; S.init = false
    end
end

function _KAG_onClick()
    local sc = scenes[S.phase+2]
    if sc and not sc.auto and S.autoTmr==0 then
        S.phase = S.phase + 1; S.timer = 0; S.autoTmr = 0; S.init = false
    end
end

print("[TEST-DEMO] v2 loaded - bright colors, video, WAV audio")
