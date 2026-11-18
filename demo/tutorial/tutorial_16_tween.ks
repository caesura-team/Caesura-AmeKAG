; =============================================================================
; Caesura KAG 教程 16 —— 声明式补间 [tween]（round 106 / R106-A）
;
; [tween] 是 engine 的 sprite 属性补间命令：把某个图层（sprite）上的一个
; 数值属性，从起点值平滑插值到终点值（N 毫秒内完成）。与 [move]（只做
; x/y 位移）相比，[tween] 统一了坐标 x/y、透明度 alpha、缩放 scale 四种
; 数值属性，并内置 5 种缓动函数 + 表达式起点/终点（${expr} 插值）+ 延迟 +
; 阻塞/非阻塞两种模式，取代手写的 [sprite_fade]+[sprite_move]+[wait] 序列。
;
; ⚠️ 状态标注：本教程针对 scripts/kag/commands/tween.lua 的已实现契约编写。
;   kag_runner 的每秒帧推进钩子（TweenCommands.update）已接线，但模块尚未
;   登记进 kag/init.lua 预加载清单，故 [tween] 在 ks_check 仍被判为未知命令。
;   登记完成后本教程应通过 ks_check 零警告 + Web sweep 一路到 [end]。
;
; （契约 · 参考 tween.lua schema.define + tests/scripts/test_tween.lua）
;   [tween target=<说话人|图层> attr=<x|y|alpha|scale>
;           from=<起点|expr> to=<终点|expr> dur=<毫秒>
;           delay=<毫秒> ease=<缓动> wait=true|false]
;
;   target  (必填) 目标图层名或说话人（说话人会回落到 _char_<name> 图层）
;   attr    (必填) x | y | alpha（0..255 不透明度）| scale（缩放，1.0=原大）
;   from    (可选) 起点值；缺省 = 该属性当前值；支持 ${expr} 插值
;   to      (必填) 终点值；支持 ${expr} 插值
;   dur     (必填) 补间时长（毫秒，契约钳制 100..30000）
;   delay   (可选) 延迟启动（毫秒，默认 0）
;   ease    (可选) linear | ease_in | ease_out | ease_in_out | back_out
;                   （默认 linear）—— 教程演示 5 种
;   wait    (可选) true=阻塞协程，等待补间完成（默认）；false=即时返回
;                   （非阻塞），配 [wait ms=...] 自行控制节奏
;
; 本教程覆盖命令： [font] [pt] [bg] [ch] [p] [csp] [csl]
;                  [tween] [wait] [set] [end]
; =============================================================================

[font face="default" size=22]
[pt speed=50]

; ---- 1. 开场 ---------------------------------------------------------------
[bg storage="assets/bg/classroom.png"]
[ch name="Narrator" text="教程 16 开始：一起用 [tween] 做声明式补间动画。"]
[p]

; 先准备一个可补间的目标图层：用 [csp] 放一个立绘。注意 [csp layer=...]
; 决定图层名，后续 [tween target=...] 就指向这个图层名。
[csp name="t0" layer="t0" x=80 y=420 storage="assets/fg/girl_uniform.png"]

; ---- 2. 基础 [tween x] -----------------------------------------------------
; 最朴素的用法：把图层从当前位置平滑移到 x=900。
; 只给 target/attr/to/dur —— from 缺省为当前值（x=80），一步到位。
[tween target="t0" attr=x to=900 dur=800]
[ch name="Narrator" text="第一步：基础 [tween x]。图层从起点平滑滑到右边，耗时 800ms。"]
[p]

; 再补一次 from，把补间语义写完整：显式起点 900 → 终点 200。
[tween target="t0" attr=x from=900 to=200 dur=600]
[ch name="Narrator" text="显式写了 from=900 到 to=200，图层左移回中景。"]
[p]

; ---- 3. 缓动函数对比（5 种 ease 同屏移动）-----------------------------------
; 放置 5 个图层（不同 layer 名），用 5 种不同缓动同时横向移动同样距离。
; ease 差异：linear 匀速；ease_in 先慢后快；ease_out 先快后慢；
; ease_in_out 慢-快-慢最自然；back_out 会「过冲」再回到终点（有回弹）。
[csp name="e1" layer="e1" x=80 y=150 storage="assets/fg/girl_uniform.png"]
[csp name="e2" layer="e2" x=80 y=230 storage="assets/fg/girl_uniform.png"]
[csp name="e3" layer="e3" x=80 y=310 storage="assets/fg/girl_uniform.png"]
[csp name="e4" layer="e4" x=80 y=390 storage="assets/fg/girl_uniform.png"]
[csp name="e5" layer="e5" x=80 y=470 storage="assets/fg/girl_uniform.png"]

; 五条线在同一屏幕（自上而下），各自以不同缓动从左跑到右，
; 起点相同、时长相同 —— 只有「中间段的速度曲线」不同。
[tween target="e1" attr=x from=80 to=1100 dur=1500 ease=linear]
[tween target="e2" attr=x from=80 to=1100 dur=1500 ease=ease_in]
[tween target="e3" attr=x from=80 to=1100 dur=1500 ease=ease_out]
[tween target="e4" attr=x from=80 to=1100 dur=1500 ease=ease_in_out]
[tween target="e5" attr=x from=80 to=1100 dur=1500 ease=back_out]
[ch name="Narrator" text="看到 5 条线同时右移了吗？它们用 linear / ease_in / ease_out / ease_in_out / back_out 五种缓动，中途速度各不相同，back_out 最后还有一点过冲回弹。"]
[p]
[ch name="Narrator" text="上到下依次：匀速、先慢后快、先快后慢、慢-快-慢、带回弹。你更喜欢哪一种？"]
[p]

; ---- 4. from 缺省 ----------------------------------------------------------
; from 不写时=[该属性当前值]。把 e1 挪回左侧后，直接 [tween attr=x to=80]，
; 不写 from——从它当前的 x 出发。
[csl name="e1" layer="e1" x=1100 y=150]
[tween target="e1" attr=x to=80 dur=500]
[ch name="Narrator" text="第 4 步：from 缺省 = 当前值。e1 被 [csl] 放到 x=1100，随后 [tween] 只用 to=80、不写 from，自动从当前值（1100）补间回去。"]
[p]

; ---- 5. 非阻塞 wait=false + [wait] 组合 --------------------------------------
; 默认 [tween] 阻塞协程（补间跑完才继续下一句）。若把 wait=false 设为
; 非阻塞，则 [tween] 立刻返回、补间在后台进行（由 kag_runner 每帧推进）；
; 想让某个时刻停在某个动作，再配 [wait ms=...] 控制节奏——适合「多个补间
; 并行 + 一句话卡点」。
[csp name="n1" layer="n1" x=0 y=560 storage="assets/fg/girl_uniform.png"]
[tween target="n1" attr=x from=0 to=1200 dur=1000 wait=false]
[ch name="Narrator" text="这是非阻塞 [tween]：命令立刻返回，补间在后台进行。"]
[p]
; 非阻塞补间仍在跑，用 [wait] 卡住 1 秒让动画播完，再说话。
[wait ms=1100]
[ch name="Narrator" text="[wait ms=1100] 让刚才的补间有足够时间播完，再继续后续台词。"]
[p]

; ---- 6. 表达式 from/to -----------------------------------------------------
; from/to 支持 ${expr} 插值：可用变量、算术运算，先求值再补间。
; 先用 [set] 定义起点/位移变量，再让 [tween] 用 ${...} 引用它们计算终点。
[set f.base_x = 400]
[set f.step = 250]
[tween target="t0" attr=x from="${f.base_x}" to="${f.base_x + f.step * 2}" dur=700]
[ch name="Narrator" text="表达式补间：from=${f.base_x}，to=${f.base_x + f.step * 2}。图层移动的起点/终点由变量计算而来。"]
[p]

; 表达式也支持三元等完整语法 —— 这里用三元决定终点：flag 为真就走 1200，
; 否则 200。
[set f.far = true]
[tween target="t0" attr=x from="${f.base_x + f.step * 2}" to="${f.far ? 1200 : 200}" dur=600]
[ch name="Narrator" text="to 用三元表达式 f.far ? 1200 : 200（当前为真，故终点 1200）。表达式让补间终点完全由剧本逻辑决定。"]
[p]

; ---- 7. delay 延迟启动 -----------------------------------------------------
; delay=400：命令先空等 400ms 再开始补间。适合需要「先听到一句台词、
; 半秒后再动」的时序。
[tween target="t0" attr=x from="${f.far ? 1200 : 200}" to=80 dur=700 delay=400]
[ch name="Narrator" text="这句台词先显示，随后 [tween] 因 delay=400 再等半秒才把图层移回左侧。"]
[p]

; ---- 8. alpha 属性 ---------------------------------------------------------
; attr 支持 alpha（0..255 不透明度）：让立绘淡出再淡入。
[tween target="n1" attr=alpha from=255 to=0 dur=600]
[ch name="Narrator" text="attr=alpha：把图层不透明度从 255（不透明）补间到 0（全透明），立绘淡出了。"]
[p]
; 淡入回来：from 缺省 = 当前 alpha（此刻是 0）。
[tween target="n1" attr=alpha to=255 dur=600]
[ch name="Narrator" text="再次 [tween attr=alpha to=255]，from 缺省为当前不透明度（0），立绘淡入复原。"]
[p]

; ---- 9. scale 缩放属性 -----------------------------------------------------
; attr=scale 插值缩放（1.0 = 原始大小）。做一个「贴近放大」的镜头感。
[tween target="n1" attr=scale from=1.0 to=1.6 dur=800 ease=ease_out]
[ch name="Narrator" text="attr=scale 从 1.0 补间到 1.6（ease_out 先快后慢，像镜头推近），最后又缩回原大。"]
[p]
[tween target="n1" attr=scale from=1.6 to=1.0 dur=500 ease=ease_in_out]

; ---- 10. 收尾 --------------------------------------------------------------
[cl]
[bg storage="assets/bg/classroom.png"]
[ch name="Narrator" text="声明式补间教程完成！"]

; [tween] 模块（并入 kag/init.lua 预加载 + kag_runner 帧推进钩子）一旦集成，
; 本场景应经 ks_check 零警告、Web 播放器一路运行到 [end] 零错误。
[end]
