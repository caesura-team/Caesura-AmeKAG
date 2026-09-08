# U14 编译缓存与 bundle 兼容验证

U14按唯一计划继续；U12和U13均已合并，master CI分别为34206114925、34209414785，均10个任务全部成功。用户另指定U2的Apple测试使用Expo，该工作在独立分支接入；本补丁继续处理缓存和最终包兼容性，Studio暂停。

## 合同与实现

源内容标记、缓存容器格式和编译语义分开：`.ksc`及bundle内场景采用format 2，Web bundle外层容器继续version 1。每个编译流保存`caesura-kag-2`语义标识和实际使用命令的完整规范化schema字符串；验证比较完整字符串，支持原地默认值变更，不依赖短hash。整数用Lua整数精确十进制编码，非有限契约值不能持久化。源文件仍使用既有FNV-1a内容标记，该标记不代表发行者信任。

冷compile、semantic和deserialize/readCache入口先初始化同一组运行时合同，避免正常冷生成物到启动后的runtime反而不兼容。旧格式或语义不匹配的磁盘缓存被丢弃并从源重新编译；无源码bundle在改变旧会话/场景provider之前检查所有场景，错误后原会话仍可继续。

readCache以完整文件内容判断外部改写，缓存解析后的数据，每次返回独立token图；不再用长度和前64字节近似身份，也不共享活跃数组。flow内存模板隔离于runner，逐次核对当前resolved源内容和schema，热重载清除当前mod解析结果；写失败/读异常仍可从源执行。私有模板使用弱scene键，兼容公开scene_cache被清空后的资源释放。

`ks_bake --check-web`可独立校验生成文件；`--web`在写入后再读回，并对照原始输入scene集合。Node Web打包在最终输出目录中调用该包的Lua runtime，验证最终copy及完整输入集合，之后才写manifest。game-only打包同样检查包内返回tokens、真实磁盘cache和源hash，用逐输入报告确认全部场景；明确不兼容硬失败，缓存缺失/损坏则保留源码降级并记录partial。没有把stdout的成功字样当作证据。

整合时发现真实sandbox只允许`r`而拒绝`rb`，导致源hash不可用和preload后重复解析。最小补丁允许已有白名单内的`r/rb`并透传原模式，保留全部写入、遍历和路径限制。`cache/ksc`仍非sandbox白名单；锁定后的证明是内存模板复用，未声称锁定后的磁盘缓存命中。

## 定向证据与审查

证据在隔离开发目录的`artifacts/validation/u14*`中，原始失败均保留。

- bytecode缓存最初21通过/10失败，修复后31通过；大整数和非有限值两项真实RED后，最终33/33。
- flow原始3通过/5失败；当前compiler配原flow仍5通过/5失败，最终10/10。正常路径实测一次parse、一次compile、两次disk read、一次write。
- Web兼容入口5项先全部失败，修复后5/5；包括未知版本、被调用场景不兼容、schema默认值变更、拒绝后原会话继续。
- 冷compiler真实RED后通过；独立冷`ks_bake --check`又复现28通过/1失败，完整初始化修复后29/29。写后Web文件的兼容和截断负控制均通过。
- game-only实际CLI最终35/35、0跳过；覆盖cache缺失、部分写入、四种磁盘不兼容、live token不兼容、伪stdout成功、遗漏输入与空格路径。
- Node打包最终copy的语义变异和删除非入口callee均真实复现误报PACKAGE COMPLETE；加入包内runtime和预期输入集合校验后，完整CLI5/5。首次隔离中文工作树冷打包还暴露demo输出绝对路径问题，改为固定ROOT的相对cache/story输出后，实际首次打包通过。
- 原完整Lua主146通过/1失败为preload/hash沙箱问题；修复后定向sandbox41、preload8、escape58、create8全部通过，Windows原始CRLF字节被保留。此前完整隔离Lua45/45通过。
- 独立审查发现的冷初始化、数值表示、冷读取和最终copy遗漏输入问题均已通过真实回归闭环；源码/日志身份及增量审查报告保留。生成文档使用原生成器。

完整Debug profile、完整Web原预算、最终源码身份及候选CI仍待冻结后的最后验收。定向计数不是行/分支覆盖率；现有空/注释流不支持序列化，制作端明确失败，不静默省略场景。
