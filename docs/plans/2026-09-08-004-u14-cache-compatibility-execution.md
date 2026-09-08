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

## 完整输入重名场景修正

首次冻结候选`ee2f89d1`的完整pipeline在demo bake阶段真实失败，错误为`duplicate-bundle-scene:story.ks`；后续阶段没有运行。严格集合检查暴露`demo/example_game/story.ks`与`demo/template/story.ks`此前被basename键静默覆盖的问题。完整输入集合和拒绝条件保留，两个场景都继续交付。

新增纯函数`compiler.bundleSceneKeys(paths)`，制作端及最终包runtime共用：唯一basename保持兼容；冲突项移除本批输入共同目录前缀后保留相对子目录。因此两个demo分别成为`example_game/story.ks`与`template/story.ks`。绝对路径前缀、不能安全归一化的父级路径和重复来源仍被拒绝。Node源码副本也使用同组key复制，两个story不再覆盖。

`artifacts/validation/u14-scene-keys/`保留正式RED及GREEN：Lua bake41/41、全部demo24场景/6资产实际bake成功、Node完整CLI6/6无跳过、Web定向33/33无跳过。两个重名场景都经Wasmoon运行到DONE，并各自与源路径backlog对照；最终copy的语义变异和遗漏callee负控制继续被拒绝。Web运行时查找逻辑没有改动。该实质修改纳入下一次冻结候选完整pipeline。

## 最终本机整合验收

干净候选`c227dcc70490b362c82739696f8a39ea955438b5`的`artifacts/validation/u14/final-pipeline-02.json`已完成全部五阶段：全demo bake、Web build、完整Web、完整Node包CLI、完整Windows Debug profile。Web为37文件、506/506用例、0失败/0跳过，120.81秒；原吞吐、内存和配对缩放预算通过。Node完整CLI6/6通过。全部阶段源码前后摘要稳定。

原生run `7bf04603-ff94-41f7-a0df-4ff3fd4654d8`的11项required检查全部通过：完整Debug构建、C++1292/1292与399003断言、Lua主147/147和隔离45/45、Python17/7/53/57、耦合、注册、CTest25通过/0失败/1预声明外部AI smoke跳过。CTest共273.17秒。dirty=false，source/fixture fingerprint前后相同；collector和严格verifier通过。manifest在`artifacts/validation/u14/evidence/c227dcc70490b362c82739696f8a39ea955438b5/7bf04603-ff94-41f7-a0df-4ff3fd4654d8/windows-debug/manifest.json`。

本机最终验收已通过，组织仓库候选CI和master合并/打包仍单独核验。U2 Expo云端任务与U15后端生命周期继续保持各自证据边界。

## PR #16 旧缓存测试修正

首次候选CI`34217073981`保留失败：Windows Debug/Release和Linux均在同一个Web测试中把新场景format 2断言为1。本机因`cache/ksc-web/demo_galgame_demo.ksc`旧文件仍为1而没有暴露该旧断言；该测试只解析表并未真正执行，原本的zero-parse名称缺乏对应证明。

测试现在始终使用当前编译器新生成的序列化流，明确断言format 2，再禁用tokenizer.parse并拒绝对未编译tokens调用compile，通过实际Web bundle入口执行到DONE，核对变量与对话结果，finally恢复测试钩子。本机旧文件不再参与这项判断，也未删除旧产物。定向新测试通过；原生/C++/Lua源码未修改，c227dcc7的完整原生证据保持原来源，新的完整Web与CI结果另行记录。

随后完整本机`web-u14-ci-fix-03`为505通过/1失败、0跳过：synthetic1000三样本2414.2/2337.1/2212.0ms，中位2337.1ms，未满足原绝对吞吐预算；配对缩放和内存通过。该次FAIL保留，不能被前次全绿覆盖。两次638个source指纹只有上述flow测试不同，声明依赖digest相同；独立审查未发现跨文件Lua hook污染路径，但历史运行没有CPU/频率/GC采样，尚不能确定变慢原因。后续固定样本诊断不调整预算，不挑选通过结果。

第二轮CI`34220173064`的Web阶段退出0：Linux完整506/506、0跳过，Windows两配置各475通过/32既有产物条件跳过。三平台的synthetic1000中位耗时分别为Linux1893.2ms、Windows Debug1667.6ms、Release1581.2ms，均满足原预算，但不改写本机FAIL。Windows Release/macOS/Android/iOS任务成功；Windows Debug在后续真实Unicode RPC打包返回500，Linux被能力矩阵新鲜度检查阻断。原始日志保留在`artifacts/validation/u14/ci16-run34220173064-failed.log`和`ci16-run34220173064-full.log`。后者源于新增Web场景断言后未再次运行矩阵生成器，生成结果必须随测试更新；前者继续按真实打包响应复现，不能删除或跳过E2E。PR #16仍未合并。

Unicode问题在本机现有Lua、当前Debug Lua和冷Web构建的真实RPC中均未复现，冷路径19项断言全部通过。云端测试只打印响应前300字符，丢失最终runtime校验失败原因；本次仅补齐失败时完整打包logTail、子进程OS错误/退出码/信号及实际解释器信息。真实不存在解释器的spawn负控制先失败于缺失诊断，再通过；完整Node CLI现为7/7、0跳过。未推测性修改编码或兼容语义，云端根因待增强日志定位。本轮证据在`artifacts/validation/u14/unicode-ci-fix/`。
