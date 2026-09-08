[set var="lf.owner" value="callee"]
[eval exp="f.nested = lf.owner"]
[ch text="跨场景 ${f.nested}"]
[return]
