# U13 多执行路径语言等价语料执行记录

本任务继承唯一计划的U13范围；U14缓存兼容、后续后端验证与全部U1–U29仍保留。Studio暂停。源代码从已通过U12候选门禁的daec64bf开始，随后整合其master合并99a65d16。隔离worktree内实现和定向验证，不改动并行运行的U12原生候选。

## 行为合同与实施

共同支持的源文本、AST、磁盘缓存、Web bundle必须产生相同的规范化事件和终态。真实runner负责执行、调用/局部帧、选择与存档恢复；测试仅替换宿主绑定、存储及场景内容供应。使用现有编译器、parity comparator和Web bridge，不重写scheduler。可运行输入在`tests/projects/runtime_contracts/`，六组分别覆盖语言文本与表达式、宏、调用、条件选择、wait/tween和存档重放。

实际差异驱动最小修复：semantic的标签名保留`*`供工具显示，但compiler生成运行时标签索引时去掉该前缀，与scheduler一致。semantic的参数直接复用compiler既有正规化函数，修复字符串位置键与数字位置键不同、点号赋值未产生var/value的问题，保留显式命名参数优先级。没有新C++接口或后端依赖。

每组语料使用原生source、AST、实际写入后读回的`.ksc`，各自输入7ms与31ms的dt，共36次；Web通过实际`ks_bake.lua --web`生成九场景bundle，再经`runScene/runFromBundle`执行同一语料，共12次。Web bundle不提供源回退。对话、分页、可见选择、变量、调用栈及正常结束逐项比较，排除句柄、绝对路径、帧计数；tween检查精确终点，save replay须与原路径后缀一致。缓存失效、签名及旧bundle兼容属于U14，当前不据此声称这些合同通过。

## 真实回归与证据

- `artifacts/validation/u13-ast/semantic-red.log`：32通过、4失败；GREEN为36/36。
- `compiler-red-02.log`：73通过、6失败；GREEN为79/79。使用真正scheduler，覆盖named/bare call/jump/return、switch、点号赋值、命名优先级、缺失标签和重复标签。
- 原runner探索及额外最小场景的source/AST/cache六条对照全部等价；offset、label_index、bareval、alias_bare现有入口通过。源码和解释器哈希保留在`u13-ast/identity.json`。
- `artifacts/validation/u13-corpus/native-diagnostic-03.log`：36/36。早期语料驱动器的错误前提保留在01/02日志：原始文本每行暂停、显式p另有分页、保存caller须用允许的scene路径；这些夹具修正不作为生产缺陷的RED。
- `web-storage-green-02.log`：完整新文件2/2通过；本次新`runtime-contracts-*`目录包含36次原生和12次Web的实际JSON、真正生成的story.lua、bake日志、比较结果。六个负控制复制本次真实事件后分别破坏变量、顺序、调用帧、正常结束、bundle路径、save replay，全部被拒绝，原记录不变。
- 独立审查发现Web测试默认localStorage跨lane共享可能误读旧slot41。`storage-isolation-red-01.log`实际复现“第二实例保存失败但加载得到ok”；每lane改为独立Map后该负控制通过，同lane原执行与重放仍共用存储。审查闭环见`artifacts/validation/u13-review/review.md`，无未解决P1/P2。

上述为定向真实运行证据。完整Debug构建、全部C++/Lua/CTest、完整Web、冻结候选身份与组织仓库CI尚待最后整合；不把定向计数冒充完整门禁或覆盖率，也不提供真实GPU/设备音频证明。

## 最终本机整合验收

干净候选`43394fa093235fa978996ac1dbdbbf05abf215ac`于2026-09-08完成顺序门禁：先真实bake全部demo，再重建Web产物、运行完整Web，最后完整Windows Debug profile。Web为36个测试文件、498/498用例、0失败/0跳过，92.88秒；原吞吐、内存和配对缩放预算通过。`artifacts/validation/u13/final-pipeline-01.json`四阶段全部退出0，源文件前后摘要一致。

原生run ID为`95429185-2f41-4a5f-934b-0e61771983c6`，`raw/windows-debug-u13-01/run.json`记录11项required检查全部通过：完整Debug构建、C++1292/1292、Lua主147/147与隔离44/44、Python17/7/53/57、耦合、测试注册以及CTest25通过/0失败/1预声明外部AI smoke跳过。源码及夹具未在运行期间变化。collector与严格verifier通过，manifest在`artifacts/validation/u13/evidence/43394fa093235fa978996ac1dbdbbf05abf215ac/95429185-2f41-4a5f-934b-0e61771983c6/windows-debug/manifest.json`。

U13本机最终验收通过，组织仓库候选CI与主分支合并继续单独核验；后续U14开发在隔离工作树进行，不混入本候选。
