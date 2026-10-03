; =============================================================================
; Caesura KAG 教程 06 —— 特效与转场
;
; 目标：掌握画面特效：闪白（[flash]）、震动（[vib]/[quake]/[shake]）、
;       转场（[trans]）、等待（[wait]）与结局解锁（[ending]）。
;
; 特效命令均为阻塞式（播完才继续），适合用来强调剧情节点。
;
; 本教程覆盖命令：[bg] [playbgm] [flash] [vib] [trans] [wait] [ch]
;               [ending] [scroll] [stopbgm] [end]
; =============================================================================

[font face="default" size=22]
[pt speed=60]

[bg storage="assets/bg/classroom.png"]
[playbgm storage="assets/bgm/daily.wav" volume=0.6]
[wait time=300]

; ---- 1. 闪白 flash ----------------------------------------------------------
; [flash] 全屏闪白：time 是时长(ms)，r/g/b 是闪光的颜色（0..255）。
[flash time=200 r=255 g=255 b=255]
[ch name="Narrator" text="刚刚有一道闪光划过屏幕。"]
[p]

; ---- 2. 震动 vib ------------------------------------------------------------
; [vib] 画面震动：time 时长，intensity 强度（0..50）。
[vib time=150 intensity=3]
[ch name="Narrator" text="屏幕震了一下——发生了什么？"]
[p]

; ---- 3. 转场 trans ----------------------------------------------------------
; [trans] 配合 [bg] 实现场景切换动画。method 支持 crossfade/dissolve 等。
[bg storage="assets/bg/hana.png"]
[trans time=500 method=dissolve]
[ch name="Narrator" text="转场完成，我们来到了花田。"]
[p]

; ---- 4. 组合特效：剧情高潮 --------------------------------------------------
; 转场、闪白、震动可以组合使用，制造强烈的剧情冲击。
[flash time=150 r=255 g=255 b=255]
[vib time=200 intensity=5]
[ch name="Narrator" text="（一道强光！大地震动！）"]
[p]

; ---- 5. 结局解锁 ------------------------------------------------------------
; [ending] 记录结局（id 唯一、name 显示名），可用于画廊/成就系统。
[ending id="tutorial_06_done" name="特效教程完成"]
[scroll text="Caesura KAG 教程系列 — 完结" speed=80]
[wait time=600]
[stopbgm time=400]
[ch name="Narrator" text="教程 06 完成——特效与转场。你已经掌握了 Caesura 剧本语言的这部分基础。"]
[p]

; ---- 6. 正式收尾 ------------------------------------------------------------
[scroll text="感谢游玩 — See you next time!" speed=80]
[wait time=500]
[end]
