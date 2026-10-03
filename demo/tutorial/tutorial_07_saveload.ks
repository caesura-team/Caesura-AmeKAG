; =============================================================================
; Caesura KAG 教程 07 —— 存档与读档
;
; 目标：掌握存档系统：保存（[save]）、读取（[load]）、存档结果判断
;       （tf.save_result / tf.load_result）。
;
; 概念：
;   - 存档槽位 slot：-2..99（通常 0..99 为手动槽位）
;   - [save slot=N] 把当前场景/变量/解锁状态写入存档
;   - [load slot=N] 从存档恢复；成功后剧本跳回存档点继续
;     （引擎与 Web 播放器都会停止当前执行、从存档位置续玩）
;   - 无存档后端的环境（旧 Web 构建）中 save/load 返回 error，
;     剧本通过 tf.save_result / tf.load_result 优雅降级
;
; 本教程覆盖命令：[save] [load] [set] [if] [else] [endif] [jump] [ch] [p] [end]
; =============================================================================

[font face="default" size=22]
[pt speed=60]

[bg storage="assets/bg/classroom.png"]
[wait time=300]
; 本教程明确保存金币的初值，读取后应恢复为 0。
[set f.coins = 0]

; ---- 1. 保存到槽位 ---------------------------------------------------------
; [save] 是阻塞命令：写入完成后继续。结果在 tf.save_result（"ok"/"error"）。
[ch name="Narrator" text="先把进度保存到槽位 1。"]
[p]
[save slot=1]

; 成功读取会回到 save 后一条指令。识别恢复后的续行，避免再次读档循环。
[if exp="tf.load_result == 'ok'"]
[ch name="Narrator" text="读取成功！金币已恢复为存档时的 ${f.coins} 枚。"]
[p]
[jump *saveload_complete]
[endif]

[if exp="tf.save_result == 'ok'"]
[ch name="Narrator" text="保存成功！存档槽位 1 已写入（Web 播放器会存进浏览器存储）。"]
[p]
[else]
[ch name="Narrator" text="当前环境不支持存档，剧本继续运行。"]
[p]
[endif]

; ---- 2. 修改状态 -----------------------------------------------------------
; 存档里保存的是执行到这里的完整状态（场景位置 + 变量 + 解锁）。
[set f.coins = 100]
[ch name="Narrator" text="存档之后，我捡到了 100 枚金币（f.coins=100）。"]
[p]
[ch name="Narrator" text="现在执行 [load slot=1]：剧本会跳回存档点，金币变量也会恢复为存档时的值。"]
[p]

; ---- 3. 读取本轮成功保存的槽位（只读取一次）---------------------------------
; 成功后从 save 后继续并跳到完成标签；失败则明确提示后结束，不重试。
[if exp="tf.save_result == 'ok'"]
[load slot=1]
[ch name="Narrator" text="读取失败，当前进度保留；本轮不重复读取。"]
[p]
[else]
[ch name="Narrator" text="本轮保存失败，不读取槽位中的旧进度。"]
[p]
[endif]

*saveload_complete
[ch name="Narrator" text="存档教程完成！下一个教程：系统 UI（tutorial_08）。"]
[p]
[end]
