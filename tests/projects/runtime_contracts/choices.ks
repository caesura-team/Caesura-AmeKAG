[eval exp="f.enabled = true"]
[ch text="选择路线"]
[button text="隐藏项" target="*wrong" cond="!f.enabled"]
[button text="左路" target="*left" cond="f.enabled && true"]
[button text="右路" target="*right" cond="f.enabled ? true : false"]
[endbutton]
[end]
*wrong
[set var="f.route" value="wrong"]
[jump *finish]
*left
[set var="f.route" value="left"]
[jump *finish]
*right
[set var="f.route" value="right"]
*finish
[ch text="结局 ${f.route}"]
[end]
