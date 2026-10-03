; =============================================================================
;  Caesura (AmeKAG) - full_pipeline_demo.ks
;  Full-pipeline demo: exercises every KAG subsystem in one playable scene.
;  Coverage: layers, text/ruby, audio (bgm/se/voice), flow control (if/jump),
;  eval/iscript, choices, save/load, transitions, vfx, waits.
;  Run with: demo/entry_full.lua (see config.entry_script)
; =============================================================================

[font face="default" size=22]
[pt speed=60]

; ---- Title / background ----
[bg storage="assets/bg/classroom.png"]
[wait time=300]
[playbgm storage="assets/bgm/daily.wav" volume=0.8]
[setbgmvolume volume=0.6]

[ch name="Narrator" text="Welcome to the Caesura full-pipeline demo."]
[p]

; ---- CJK font (TTF face) ----
[font face="assets/fonts/NotoSansCJKsc-Regular.otf" size=24]
[ch name="Narrator" text="现在是中文与日文渲染测试。"]
[p]
[er]
[font face="default" size=22]

; ---- Ruby (furigana) ----
[ch name="Narrator" text="Ruby annotation:"]
[ruby text="日本語" ruby="にほんご"]
[p]
[er]

; ---- Character sprite + voice + SE ----
[fg storage="assets/fg/girl_uniform.png"]
[position layer="fg" pos="right"]
[playvoice storage="assets/voice/line01.wav"]
[ch name="Sakura" text="Hello! Nice to meet you."]
[playse storage="assets/se/click.wav"]
[p]

; ---- Flow control: [if] / [else] / [endif] ----
[if exp="true"]
[ch name="Sakura" text="The condition evaluated to true."]
[else]
[ch name="Sakura" text="The condition evaluated to false."]
[endif]
[p]
[er]

; ---- [eval] + [iscript] inline Lua ----
[eval exp="ctx.tf.eval_result = 42"]
[iscript]
ctx.tf.demo_value = 42
print("[demo] iscript set demo_value=" .. tostring(ctx.tf.demo_value))
[/endscript]
[ch name="Narrator" text="Eval and iscript executed."]
[p]
[er]

; ---- Choices ----
[ch name="Sakura" text="Which route will you take?"]
[button target="*route_a" text="Go to the library"]
[button target="*route_b" text="Go to the rooftop"]
[endbutton]
[p]

; ---- Intra-scene jump ----
[jump target="*route_a"]
*route_a
[ch name="Narrator" text="You chose the library route."]
[p]
[er]
[jump target="*route_common"]

*route_b
[ch name="Narrator" text="You chose the rooftop route."]
[p]
[er]
[jump target="*route_common"]

*route_common
; ---- Transition + quake ----
[trans time=400 method="crossfade"]
[quake time=300 amplitude=4]
[p]

; ---- Save / load ----
[save slot=1]
[ch name="Narrator" text="Progress saved to slot 1."]
[if exp="tf.load_result != 'ok'"]
[load slot=1]
[endif]
[p]
[er]

; ---- VFX ----
[vfx type="flash" time=200 r=255 g=255 b=255]
[vfx type="quake" time=300 amplitudex=6 amplitudey=4]
[wait time=400]
[p]

; ---- Ending ----
[cl]
[bg storage="assets/bg/hana.png"]
[ch name="Narrator" text="The full-pipeline demo has completed."]
[wait time=600]
[stopbgm]
[ch name="Narrator" text="Thank you for playing."]
[p]
[end]
