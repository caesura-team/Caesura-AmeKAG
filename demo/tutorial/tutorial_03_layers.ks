; =============================================================================
; Caesura KAG 教程 03 —— 图层与角色立绘
;
; 目标：理解图层系统：背景层（bg）、前景层（fg）、角色层（ch sprite）、
;       图层清理（cl）、位置控制（position）与立绘动画（sprite_move/fade）。
;
; 图层概念：
;   - bg   = 背景层（最底层）
;   - fg   = 前景层（在背景之上）
;   - 角色 = [ch] 带 sprite 参数时自动创建 _char_<name> 层
;   - 层级越高越靠前（先画的在下层）
;
; 本教程覆盖命令：[bg] [wait] [fg] [position] [ch sprite=] [sprite_move]
;               [sprite_fade] [cl] [trans] [end]
; =============================================================================

[font face="default" size=22]
[pt speed=60]

; ---- 1. 显示背景 ----------------------------------------------------------
; [bg] 把一张图片放到背景层。assets/bg/ 下有两张示例图：classroom.png / hana.png。
[bg storage="assets/bg/classroom.png"]
[wait time=300]
[ch name="Narrator" text="这是教室背景。背景图片加载在 bg 层。"]
[p]

; ---- 2. 切换背景 + 转场 ---------------------------------------------------
; [trans] 让背景切换带过渡动画（method 可选 crossfade/dissolve 等）。
[bg storage="assets/bg/hana.png"]
[trans time=400 method=dissolve]
[ch name="Narrator" text="切换到了花田背景，并带有溶解转场。"]
[p]

; ---- 3. 前景层 ------------------------------------------------------------
; [fg] 把图片放到前景层（角色/物件之上、对话之下）。
[fg storage="assets/fg/girl_uniform.png"]
[ch name="Narrator" text="现在前景层多了一张图片。"]
[p]

; ---- 4. 前景位置 ----------------------------------------------------------
; [position] 控制某层的位置：pos 可选 left/center/right，也可用 x/y 精确指定。
[position layer="fg" pos="left"]
[ch name="Narrator" text="前景图片移到了左边。"]
[p]
[position layer="fg" pos="right"]
[ch name="Narrator" text="又移到了右边。position 可以随时调整图层位置。"]
[p]

; ---- 5. 角色立绘 ----------------------------------------------------------
; [ch] 带 sprite 参数时创建角色层。角色层名自动为 _char_<name>。
[ch name="Sakura" text="我是樱！这次以立绘形式登场。"
     sprite="assets/fg/girl_uniform.png"]
[p]

; ---- 6. 立绘移动与淡出 ----------------------------------------------------
; sprite_move：把指定说话人的立绘移动到 (x, y)，time 是动画时长(ms)。
[sprite_move speaker="Sakura" x=120 y=200 time=400]
[ch name="Sakura" text="我向左移动了！"]
[p]

; sprite_fade：立绘透明度渐变为 to（0..255，255=不透明）。
[sprite_fade speaker="Sakura" to=120 time=300]
[ch name="Sakura" text="我现在半透明了。"]
[p]

; ---- 7. 清理图层 ----------------------------------------------------------
; [cl] 清空图层（默认清空所有层）。layer 参数可只清指定层。
[cl]
[ch name="Narrator" text="图层清空了，立绘与前景都消失。"]
[p]

; ---- 8. 收尾 --------------------------------------------------------------
[bg storage="assets/bg/classroom.png"]
[ch name="Narrator" text="图层教程完成！下一个教程：音频（tutorial_04）。"]
[p]
[end]
