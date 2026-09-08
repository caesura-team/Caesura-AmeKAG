[eval exp="f.number = 3"]
[eval exp="f.short = false && missing.value"]
[eval exp="f.pick = f.number > 1 ? '甲' : '乙'"]
[ch name="雨" text="你好，世界 ${f.pick}"]
[ch text="引号：\"你好\"；路径：A\\B"]
第一行：春雨
第二行：夏夜[p]
[end]
