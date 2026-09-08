# 运行时底层计划执行记录 — 2026-09-05

当前计划：docs/plans/2026-09-05-001-refactor-runtime-foundation-plan.md。

路线图保留 U1–U29，底层优先，Studio 暂停。U11 已合并，master `35245667` 的 CI `34180028627` 已确认10/10全部成功。2026-09-08 用户明确恢复继续开发，取代此前“U11 后暂停”的安排；当前推进U12。完整路线图尚未完成，本记录不把排期当成完成证据。

## 工作区

- 分支：codex/runtime-foundation。
- 开始时 HEAD：87a96f3d846caf5ad2aac6e3b8585133e7a5aed5。
- 开始时已有改动：AGENTS.md、CLAUDE.md、新计划及计划入口；未跟踪的 %SystemDrive%/ 保留。
- 执行过程使用独立日志目录 artifacts/validation/bootstrap-20260905/。这些是证据采集器完成前的诊断原始日志，尚不是发布候选证据。

## 单元状态

| 单元 | 状态 | 现状与下一项证明 |
|---|---|---|
| U1 | 本机适用验收通过 | 53项证据测试、57项隔离mutation及真实run→collect→verify通过；无自动RC-GO；CI发布身份继续由U23承担 |
| U2 | 进行中 | U10源码已通过Windows Debug与独立干净检出Release完整profile；完整跨平台/sanitizer、Web skipped分类与性能基线保留后续 |
| U3 | 本机回归通过 | 真实SDL所有权、过滤器重入与取消释放计数通过；Engine统一暂停/恢复消费路径有真实Lua回归；跨平台整机释放观测仍依U2/U16 |
| U4 | 本机适用验收通过 | 默认provider、两种策略、legacy导入、HTTP/Steam替身staging及metadata回归通过；云校验mutation两例必红且原源码摘要已恢复 |
| U5 | 本机适用验收通过 | 请求epoch、旧worker/cache隔离、SDL重入取消、Engine暂停/回调重入、完整与局部脚本重载均已有运行回归；平台扩展仍依U2 |
| U7 | 本机及POSIX定向验收通过 | 两条存档写入路径共用独占临时文件与原生替换；Windows13项存档/云回归及POSIX18场景ASan/UBSan通过；不等同断电保证 |
| U6 | 本机适用验收通过 | worker-only等待、有限回调快照、重入关停/重启与Engine销毁屏障有回归；JobSystem的POSIX sanitizer及两种Job实现的行覆盖率已测量 |
| U8 | 本机适用验收通过 | 预启动宿主公钥、全部识别CARC层固定验证、失败回滚与CLI已接通；12项核心及39项CLI回归通过；不认证整个分发目录 |
| U9 | 本机及POSIX定向验收通过 | 合法签名边界、并发/短读、真实分配失败及恢复已测；Reader有界拒绝、OpenSSL上下文异常释放已修复；真实包内存/耗时留样，不宣称全平台或穷尽fuzz |
| U10 | 本机适用验收通过 | 唯一会话引用、start/stop/reload/EOF/失败终态、真实协程与输入清理、资源/AI回调失效及Engine关停顺序已验证；Cpp1229与Lua147/30全绿 |
| U11 | 已交付 | 存档契约、真实旧格式/冷启动、原生及Web恢复与取消已验证；完整Debug证据通过，master 35245667 CI 10/10全绿 |
| U12 | 进行中 | 稳定点、局部/控制帧、取消重入、表达式命名空间已有修复与Lua147/38历史证据；接续完整Web494/494通过，宏/选择、资源准备与真实迟到完成仍待处理 |
| U13–U29 | 待执行 | 用户已恢复后续迭代；按依赖推进，Studio继续暂停 |

### U12 当前执行证据（2026-09-08）

接续核对：[2026-09-08 U12 交接记录](2026-09-08-001-u12-resume-handoff.md)。当前分支 `codex/u12-rollback-safety`，HEAD `84e766ed`，接手时12个脏文件与两项未跟踪内容均保留。已有 skip 手动继续修复无需重做；本轮补齐配对统计红/绿回归并完成Web35文件494/494、0失败0跳过，源码摘要前后一致，记录`artifacts/validation/u12/web-resume-full-01.json`。实际扩容比率中位数2.307289，小于原2.5门槛，未改变性能预算。下方485/488等日志是此前失败记录，不代表最新结果；Lua全套、原生构建及CTest未在本轮重跑，U12仍未完成。

- 首个真实runner探针19检查中3项失败：lf未回退、完整文字在下一帧消失并重新打字。没有复现普通ch稳定点被旧协程覆盖游标的猜测，保留该反证。
- 首批本地提交 `a5ba8c49` 分离显示与下一执行位置，回退先关闭/取消旧操作，再安装完整显示并等待点击；inline回退在下一update提交。session52/52、值回归15/15、memory29/29、reload26/26与U11事务316/316通过，独立审查无阻断问题。
- 第二批恢复六类控制字段及CG/音乐/结局集合，保留tf原语义；真实回归由66通过/16失败到82/82。另发现普通双callee基准奖励8而非10：表达式缓存仅按f命中，仍绑定旧lf。缓存改为匹配f/sf/tf/mp/lf身份，变更时重绑而不修改在途函数环境；双调用及回退续跑现为奖励10、return2。expr109/55、cache8、compiler63、rollback82全部通过。
- CancelToken在执行前分离回调队列，修复回调再次取消同一token导致重复清理；14检查从3项失败到全部通过，逆序、立即注册与错误隔离合同保持。控制/表达式及取消增量均通过独立审查。
- 当前完整Lua主147/147、隔离38/38通过；完整Web485/488，三个失败来自旧测试仍假定已有历史的rollback为空操作，正在替换为真实非空历史回归并核对桥接呈现。未声称U12完成，资源恢复、准备失败原子性、宏/选择边界、真实迟到completion及扩展历史内存仍待验收。证据目录：`artifacts/validation/u12/`。

### U11 实施顺序

1. 建立字段所有权与持久值契约，先验证坏字段/坏场景不能污染旧会话，以及未来f/tf、lf/mp和空控制栈的恢复缺口。
2. 分离解码校验、场景/调用帧准备与提交；复用U10的协程关闭和请求失效边界。提交前失败保留旧会话，提交后严重后端失败统一停止新会话。
3. 补齐声明式图层/文字/音频状态及候选资源生命周期；共享纹理不能作为候选独占资源销毁，准备阶段不更改全局图层或开始播放。
4. 通过真实SaveBinding/SaveManager验证JSON往返和迁移边界；历史产物必须有来源，合成旧格式样本单独标注。
5. 用独立OS进程验证加密磁盘槽的冷启动恢复、后续分支和失败时状态，再执行适用完整构建/测试及独立审查。未完成的后端/历史样本证明不能被单元测试替代。

### U11 当前证据与未完成边界

迁移边界先取得4个原生失败用例，再通过包含既有失败元数据保护的6项检查、128条断言。真实SaveBinding的9项回归、321条断言覆盖空对象、数组顺序、NUL/Unicode、混合/稀疏键拒绝、循环/深度/非法值和旧槽保护。旧发行版v1.0.1 ZIP/二进制/输入/场景/存档摘要已核对，实际产出的envelope5/data2样本纳入tests/golden_saves；原手工v1..v5样本明确标为合成，不能代替历史产物证明。

变量/局部帧/控制状态的准备与提交已分离，调用者控制帧独立于被调场景。已保留正确前置条件下的旧实现RED与当前GREEN；真实双进程检查验证加密磁盘槽、错密钥保留旧会话、热/冷状态及后续循环一致，并实际读取旧发行版KAG样本。该检查明确不证明GPU像素或真实音频。核心集成run的Debug构建、Cpp1242/1242（389096断言、0 failed/0 skipped）、Lua147/147与孤儿31/31、新进程检查均通过，记录artifacts/validation/u11-restore/core-result.json。

随后新增语言资源准备/提交：损坏字典和非法行翻译先拒绝，commit只使用已准备的字典，不再读文件。语言定向21/21、含损坏语言资源的恢复事务62/62通过；新增孤儿套件后门槛为32，后续完整集成仍需复核。

2026-09-06继续推进图层与原生准备边界：

- Engine通过BackendRegistry注册IAssetReader和IImageDecoder。资源读取保持最高优先级来源的所有权，读取失败不悄悄换来源；Restore绑定先读/解码为Lua持有的像素，再于提交时创建独占纹理。纹理API能描述资源路径或纯色来源，准备对象的取消、重复使用和创建失败均有实际绑定检查。
- 独立图片接口审查发现截断TGA/KTX及纹理注册后预算异常的三个问题。保留失败记录并修复：完整图片与逐段截断对照通过；真实D3D11子进程确认注册后异常会释放纹理、来源、字节统计和配额，且随后可以再次创建纹理。该GPU检查不等于整页恢复后的像素一致性证明。
- 新增kag.layer_state按父子顺序保存可重建图层，准备不改旧树；提交安装新树，失败继续清理已创建的纹理与RTT。记录型Layers.render的27条检查覆盖变换、裁切、同z顺序、失败停止、取消与旧owner隔离；真实runner恢复检查增加到80条。文字仅通过TextScene提交，其message几何节点不再申请无内容的RTT。f/sf混合键现在明确拒绝，密集数组保留，避免静默丢值。
- 上述图层快照Debug全量构建退出0；完整CaesuraTests **1249/1249、389322断言、0 failed/0 skipped**，证据为`artifacts/validation/u11-restore/layer-cpp-full-2.json`及同名日志。Lua主套件**147/147**、孤儿套件**34/34**，耦合与182 Lua/81 C++文件注册检查通过。主套件首次失败的两个旧夹具已补空图层隔离及精确状态断言；失败日志保留，没有减少测试。
- 双进程冷启动检查首次发现沙箱锁定前未预加载新模块，修复后`artifacts/validation/u11-cold/layer-attempt-2/result.json`记录PASS、两进程退出状态和二进制/存档摘要。范围仍是原生磁盘槽、变量/控制及文字提交，不宣称图层GPU像素或声音一致。
- 最近一次Web定向bundle套件为6/7，存档捕获失败于Web适配器的数字根节点；真实Lua图层与JS AdapterCore需要统一恢复入口。首次完整Web运行及一次C++运行被会话中断，未生成终态，不能记为通过；C++已用关闭stdin的独立启动器完成复测。音频接线后的Web全套与完整CTest尚需在后续集成后重跑。
- 在以上完整快照之后修复KAG音频播放绑定：BGM、voice、SE、3D SE不再丢弃后端返回的0，也不让C++异常穿过Lua调用边界。新用例先复现失败，再通过含实际音频集成的91项/425断言检查（`audio-result-green.log`）；该阶段Cpp门槛为1250，音频准备/位置恢复/硬停止当时尚未实现。该次定向检查不能替代修改后的完整验收。

随后完成音频恢复接线：

- 新增IAudioRestore，通过原AudioBackend实例在Engine注册非拥有接口；独立流式数据准备不改旧音源、缓存或配额，提交后恢复BGM位置/声源增益/循环，voice与SE停止，用户主音量及总线设置不随槽回滚。Lua准备对象由GC或显式取消释放，消费后不能再次应用。
- 新增presentation统一图层与音频准备、提交及清理。准备失败保留旧上下文和声音；提交中任一恢复失败清理部分图像/音源并停止；旧owner不能停止新会话。真实runner检查增加到88条，图层27条保持通过。
- 真实SoLoud回归定位并修复WavStream将codec union的WAV指针误作Ogg，以及Ogg seek错误未传播、旧帧缓存/读取API不匹配导致PCM错位的问题。保留失败证据；当前WAV/Ogg/FLAC及非静音PCM片段对照通过。详见`docs/solutions/runtime-crashes/soloud-stream-seek-codec-union.md`。该检查证明解码/混音器行为，不冒充声卡输出证明。
- 初次全量Cpp有4项独立LuaVM生命周期失败，原因是无音频服务也被当作恢复接口缺失；无后端的停止现为安全no-op，已注册但不支持恢复的后端仍报告失败。4项原失败及相关20项检查通过后，完整Cpp **1255/1255、389409断言、0 failed/0 skipped**（`audio-cpp-full-2.json`及同名日志）；Lua主147/147、孤儿34/34，耦合与182 Lua/82 C++注册检查通过。当前Cpp门槛1255，Web模块索引84项。
- 当前原生双进程基础探针`u11-cold/audio-attempt-1/result.json`通过；该探针仍以无音频设备的运行方式验证磁盘槽、变量/控制/文字，不是BGM或图层像素跨进程验收。音频/presentation有界独立审查已落盘`audio-presentation-review.md`，不替代U11最终整体验收。
- 补上图层提交的预算边界：准备时拒绝超过原生4096上限的有图像RTT，物化整组纹理后检查所有ID仍有效，避免后续上传淘汰前面的必要图像却报告成功。3条新增失败先复现，修复后图层30/30、runner88/88通过；独立增量审查已记录。最新全量Debug构建退出0，双进程探针`audio-attempt-2`通过。

随后补齐字体恢复和持久值准备：

- 场景准备在单次候选内按路径复用同一份已解析内容，下一次恢复重新读取；语言主字典和回退字典一起准备，旧槽缺失字段使用历史默认zh/en，不沿用未来会话。语言25/25通过。文字快照拒绝分数计数、非法布尔值、非有限数字及损坏的来源/布局字段，文字21/21通过，保留各自RED/GREEN记录。
- 字体接口经IRenderDevice暴露纯值描述和单次准备对象。TTF由IAssetReader一次读取，候选持有字节、FreeType face及CPU图集；提交不重新读路径，先成功创建新纹理再替换活动字体。Small/Large共享不可变位图源，Large实际二维放大2倍。真实GPU读回先复现旧Large像素错误，再验证修复；设备恢复复用CPU内容，逻辑路径在磁盘不存在也不影响已准备字体恢复。
- Lua字体快照进入presentation准备/提交/清理链，旧槽使用后端启动默认字体。真实D3D11检查覆盖位图/TTF/空字体、listener及Bgfx设备恢复、正常脚本选择和准备一次读取/提交零读取；CPU定向39项/336断言通过。独立审查发现的外层提交失败残留字体已补回归（105通过/1失败→106/106），并由审查者只读核对关闭；详见`artifacts/validation/u11-restore/font-review.md`。
- 字体阶段全量Debug构建退出0（`font-full-build-3.json`），完整Cpp **1259/1259、389438断言、0 failed/0 skipped**（`font-cpp-full-2.json`及日志），Lua主147/147（`font-lua-main-3.json`）与孤儿34/34。初次Cpp旧源码位置断言及Lua隔离依赖夹具失败均保留并修正，未删除用例。Web模块索引为85项。基础双进程`u11-cold/font-attempt-1/result.json`通过，仍是无窗口的磁盘槽/控制/文字证明。之后的新增修改需要新验证，不能沿用本次结果。

再补齐加载入口和原生值边界：

- 独立scheduler宿主在语言提交失败后也清空字体，真实fallback路径先108/109再109/109；独立审查关闭同源P2。自动/NVL/静音/跳读字段的非法类型现在在准备阶段拒绝，保留旧上下文、协程和临时字段；事务回归141/141。
- 原生save/load/list元数据使用完整Lua字符串长度，包含NUL的场景与缩略图不再静默截断；非法UTF8不能覆盖旧槽。列表使用受保护的Lua构造过程，避免分配失败跳过C++容器析构。原9项检查先2失败，再9/9、329断言通过；独立元数据审查未发现新问题。新增真实lua_setallocf逐点失败验证load/list恢复后VM和旧槽仍可用，10项/899断言通过；Cpp发现门槛随新增用例递增。
- 删除native按当前load位置跳过保存点的旧逻辑。五种入口、菜单保存及有界两次回读使用同一份测试先80/91再91/91，恢复后第一条赋值、分支和MARKER都实际重放；无条件回读由脚本自身控制，不能伪造执行load之后的命令。独立复核确认RED/GREEN测试摘要完全一致，记录`cursor-native-result.md`。
- 扩展真实Engine双进程探针：producer执行同场景inline load，退出后consumer仅从加密磁盘槽外部load；比较提交瞬间与随后分支/文字提交，原call/loop及真实历史槽验证同时保留。`u11-cold/cursor-attempt-1/result.json`为PASS，两槽摘要在读取前后不变。仍不宣称整页GPU像素或BGM输出已跨进程对齐。
- 上述游标/元数据快照通过完整windows-debug profile，run `dd983f3a-8b61-4c30-b2a1-e736221f3a75`，原始receipt为`artifacts/validation/raw/windows-debug-u11-cursor-01/run.json`。全量构建退出0，Cpp1260/1260、390016断言、0failed/0skipped，Lua147/147和34/34，全部验证器/耦合/注册及CTest通过（外部AI服务按固定配置跳过，255.21秒）。源码与夹具运行前后摘要一致，collector/diagnostic verifier均PASS；该结果仅证明本机dirty快照，不代表U11最终验收或发布资格。

真实表现层冷进程验证继续推进：

- 新增实际Engine/D3D11/SoLoud probe及Python顺序进程编排，输出整页PNG/RGBA、非静音双声道PCM、加密槽与素材摘要。ManualMix使用真实SoLoud NULLDRIVER、48000Hz、2声道和显式mix时钟，不是NullAudio；默认Device初始化参数未变。新CTest为CaesuraColdPresentationRestore。
- `presentation-cold-attempt-2`两进程均正常退出，保存/热读/冷读整页640×360逐像素差异为0；换图/空页/启动前态各改变230400像素，隐藏文字改变3682像素。该次整体仍FAIL，因为连续播放与恢复PCM不同。结果只证明该页面的恢复像素一致，不能声称所有字形渲染质量或音频已通过。
- 真实混音先发现默认44100Hz总线多余重采样、float混音时间导致一次8192帧推进后seek少一帧；总线对齐实际输出率和double时钟已部分修正。曾尝试整数1:1的linear复制优化，72项专项通过后独立审查指出变速相位跳变，已撤回；该72/72是历史证据，不能作为当前完成依据。多callback累计floor与非整块总线历史仍需专门处理。
- 实际probe还发现运行中setBusVolume只改变AudioSource默认值，没有改变活跃总线；现同步真实handle音量并保持duck比例。正确handle夹具先三项gain仍为1的RED，再8209断言GREEN。初次夹具未解析SoLoud惰性bus handle，以及Copy-Item还原源码保留旧mtime造成构建复用旧对象，两次失败日志均保留；修复后确认重新编译SoLoudAudioEngine.cpp，未沿用陈旧程序。
- `presentation-cold-attempt-4`已执行真实热冷读与65536帧跨EOF输出；原receipt因单调时钟相邻tick相等而如实FAIL。编排器已将重叠判断改为严格大于，允许合法相等tick及OS复用PID；两个Popen分别等待退出，子进程PID仍各自核对。对该既有样本的只读重算显示PCM仅头4个float（2帧）不同，其余样本完全一致；原FAIL receipt没有改写。
- 硬停止后下一块混音残留2个旧样本已先RED。新增SoLoud clearResamplerBuffers在audio mutex内清除三总线缓存/offset/leftover，无分配；同一实际混音检查8209断言GREEN。完整默认linear历史恢复、非512边界、精确样本clock还在实现，不能用清缓存或两帧误差概括为已完成。

精确音频恢复和Web接线的后续证据：

- 音频实现改为整数帧游标、精确codec帧seek、锁外准备与audio mutex内有界短预滚，保留默认linear公式。修复部分立体声读取的声道stride/实际读数、跨512源块的分数相位丢失，并加宽offset运算。`mix-history-green-3.json/.log`记录音频专项74/74、8764断言通过，包含18×512、72×128、非512前缀、起点、44100/1000采样率及独立linear参考。
- `presentation-cold-attempt-5/result.json`整体PASS：真实producer退出后consumer新建Engine，从加密磁盘槽恢复；整页RGBA零差异，65536帧默认linear跨EOF的连续/热读/冷读PCM对照通过，未使用POINT、样本移位或删头。声音证据为实际SoLoud离线混音，不是声卡观测。
- 可选cursor的真实Lua→原生JSON→Lua→恢复以及37类非法输入检查通过（`audio-cursor-length-check.log`，22个doctest断言，内部Lua循环逐项断言）。此前暴露的夹具搜索路径问题及WavStream长度先经float舍入导致合法EOF前分数帧被拒绝已修；生产长度验证现直接以整数样本数/double采样率计算。
- Web加入真实Lua图层facade、共享LayerState最终安装hook、场景prepare和嵌套Promise透传。共享图层36/36、compiler63/63、Promise成功/拒绝与source/bundle准备6项通过。compiler.deserialize不再修改可信bundle或与另一执行共享可变tokens/labels/params/flow。首次整合41/41，审查修正匿名图层ID碰撞及get_world_rect返回契约后，相关47/47通过。
- Web移除两处按load位置跳过保存点的旧guard，并消费已准备tokens；同场景无条件回读由现有frame预算限制，不伪造尾部执行。场景恢复使用Web自有安全语法与可信scene-map白名单，原生文件系统allowlist保持不变。source跨场景续跑已补齐；jump后保存、local call内保存的source/bundle回归均通过。历史记录与结局显示按槽拥有的数据同步，相关30/30通过；损坏backlog形状/字段在提交前拒绝，事务165/165通过。
- 最新完整Web诊断为`web-u11-full-1.json/.log`：385/395、10失败、0 skipped，失败集中在真实资源存在时的存读档链路（flow、slot lifecycle、main UI）。这是待修复结果，不以只含空图层的定向绿灯覆盖。真实图片/字体/WebAudio准备票据及外部loadSlot事务仍未完成。
- 后续独立审查另发现音频P1：WAV原始声道数超过8时被截断，但decoder仍按原声道写入固定缓冲；安全RED只调用prepare，9声道错误通过已复现，尚未执行越界mix，正在修入口拒绝。另有普通BGM在空闲bus残块后启动的时钟及非循环EOF尾音两条待实测边界。审查报告为`audio-final-review.md`。Web审查P1还剩bundle普通load_tokens与prepare的exact/alias优先级分叉；同名冲突最小探针已复现，报告为`web-increment-review.md`。

2026-09-06 网络恢复后的增量：

- 音频原始声道数 P1 已关闭：Wav/WavStream 四类 decoder 的加载与实例重开均检查实际声道数，超出8声道拒绝。完整 Debug 构建退出0；`audio-channels-green.log` 为11/11定向用例、8479断言通过（不是完整C++套件）。普通空闲启动游标及非循环尾段两个P2仍在补实测。
- Web exact/alias优先级已统一，`web-alias-priority-green.log` 为61/61。新增实际浏览器图像解码票据、独占纹理及canvas消费；`web-images-green-5.log` 为37项新测试与22项既有测试共59/59。测试注入decoder不等于实际浏览器证明；编码读取有界，像素检查发生在解码返回后，不宣称解码前峰值内存有界。
- 两个Web入口现共用`kag_runner`，外部`loadSlot`不再创建Loading场景。新增准备失败保留旧owner/co/provider、inline关闭旧co、无owner冷加载、含引号场景名、颜色资源恢复与并发入口拒绝回归。`web-runner-restore-red.log`先复现4项失败；`web-runner-resource-green.log`记录6/6通过。NUL截断由Lua先转JSON再跨Wasmoon边界修复，磁盘内容和冷加载Lua字节均检查；Lua字面量的十进制控制字符转义固定为3位。
- 共享Lua扩展`start(path,{replace=true})`、`prepare_load`和nil-owner冷恢复，初次186/186事务回归通过。独立审查进一步复现准备挂起后owner过期（load/start）及pending提交清理失败时CPU票据丢失；这三条仍在修复，初次绿灯不覆盖它们。Web的明确ch等待与显式p等待改为共用原生语义；未将旧Web跳过等待的断言作为保留条件。

2026-09-06 后续集成与实际浏览器证据：

- Web source/bundle 两入口已经共用 `kag_runner`；删除重复协程循环和虚拟 Loading 场景。准备失败保留旧 owner/co/provider，正常替换关闭旧协程；共享 Lua 的异步准备后 owner 复核、pending 清理失败释放责任已通过独立审查。UI 未提供 skip 覆盖时保留存档的 seen-skip；新会话历史去重不会吞掉上一会话相同的首句。
- Web 使用有界读取和真实 ImageBitmap、FontFace、WebAudio 解码。`browser-image-observation.json` 记录教室背景原始分辨率像素的原始/热读/新 VM 摘要一致，坏资源不更换 owner/画面；额外缩放到160×90的 img/canvas 二次采样曾不一致，保留为独立失败，不称整屏截图相等。`browser-audio-font-observation.json` 记录实际浏览器 FontFace 字形原始/热读/新 VM 摘要一致；真实 OfflineAudioContext 在第4864帧保存，热读与新 VM 各4096帧双声道 PCM 与连续参考最大误差0。两项经真实 Lua 槽位链路，传输为内存存储及新 Lua VM，不是新浏览器进程或声卡输出；原生加密磁盘双进程证据仍为 `presentation-cold-attempt-5`。
- Web 保存改为 Lua 先编码 JSON，再跨 Wasmoon 边界，保留合法 UTF-8 与 NUL。超出安全整数范围（包括 math.mininteger）、混合/稀疏表、非有限值及非法 UTF-8 明确失败且保留旧槽；这不是完整64位整数支持。图层 snapshot 保留稳定 id，普通颜色缓存与恢复独占纹理分离。播放查询使用实际 AudioEngine，不能由遥测查询次数结束循环 BGM；停止/销毁后的晚到 fetch/decode 被代次拒绝。字体并发选择由最新请求获胜，字体释放失败时先停止字形提交并保留释放重试责任。
- 运行器接入后的完整 Web 诊断依次为 `web-u11-runner-full-1`（409通过/31失败）、`web-u11-full-2`（476/1）、`web-u11-full-3`（475/2，剩余为性能门槛）。已修正显式 ch/p 点击边界、资源夹具、状态同步等问题；未删除用例或放宽阈值。图层快照减少逐字段跨 Lua/JS 调用并缓存构造代码，仍每次生成独立值表。已读快照的64步容量保持不变，3000标记×64份从6268.9KB降到344.1KB；编码失败不会污染缓存。定向 `web-snapshot-constructor-green` 20/20包含原性能6/6。
- `web-restore-coverage-1` 在不纳入性能基准的功能运行中468/468通过，测量指定JS文件行覆盖96.35%、分支88.43%、函数72.66%。V8对嵌入Lua字符串的行标记不等于Lua执行覆盖；不能将此数字称为整个引擎覆盖率。Skia测试依赖用于jsdom真实图片解码，和浏览器证据分开。
- 原生音频空闲启动/非循环尾音修复通过13/13专项；预算重建又复现真实SIGSEGV，修复悬空resample指针、must-live分支映射和实际active名单判定后14/14通过。映射容量与公开1..1023预算统一后，`audio-budget-capacity-green` 为15/15、8640断言。额外首块渐变假设被8个idle×prefix组合80断言反证，未改生产gain或容差；独立音频增量审查全部闭环。
- 完整 Windows run `434264e3-e2ac-477b-b303-ccde641a25e7` 的源码与夹具前后指纹一致：Debug构建、Cpp1270/1270（398578断言，0failed/0skipped）、Lua主套件147/147、验证器/耦合/注册和CTest通过（25通过+1允许的外部AI跳过，259.19秒）；Lua孤儿32/34，两个显字用例失败。因此整份receipt为FAIL。后来重建替换了该二进制，collector因原binary digest不再匹配而拒绝归一化，保留原始receipt/log，不重贴PASS。名字前缀改为即时显示后，正文显字与typewriter声音回归已恢复。
- 已加入原生 transient 查询/停止绑定，覆盖视频、粒子、模型、后处理；坏计数拒绝，单组件/配额失败仍尝试其它清理，空粒子后端可重试残留配额而不重复重启。有效原6例RED为6/6失败；第一次构建失败后旧binary发现0例的 `transient-binding-red` 无效，禁止作为通过证据。后续8例在联合回归中通过。ParticleSystem shutdown现在清发射器/粒子/空槽/计数；CPU和D3D11重启满池回归通过。动画接口新增计数与clear，Null实际资源回归通过，SDK分支未声称真机验证。
- `tests/audio/restore-video.mpg` 是本机FFmpeg8.1.1生成的2秒32×32 MPEG1/双声道MP2夹具，SHA256为 `29ac6d2f3c1c277de79bafd0cdf8966e7df83fed78e965de161c1c6da8ded952`。真实视频用例揭示声道布局错误重解释导致全部PCM被拒绝；改为正确深拷贝AVChannelLayout并保留校验后，`video-layout-green` 3/3通过，D3D11子进程64断言验证多视频PCM、批量逻辑停止、延迟释放前重开。音频是捕获替身、jobs同步执行，不称扬声器或并发worker证明。
- 六个现有运行时菜单已补Operation/<close>，取消不提交设置或重放音源，焦点与自有资源清理；标题Endings子循环的迟到协程问题经真实RED→GREEN并独立复核关闭。七类旧VFX循环也接入Operation；天气与通用粒子清理索引同步，102/102定向检查及独立审查通过。此为运行时生命周期修复，Studio仍暂停。
- 保存遇到尚无恢复契约的宏运行态、未完成选择/输入、菜单、VFX、粒子、模型、视频和后处理会明确拒绝；宏运行时改写标记在删除定义后仍保留。wait/delay现在保存剩余毫秒及所属场景/光标，读档后首次更新前重存也保留进度；完成/取消清记录，模块在lockdown前预加载。原生prepare-only不再复用可变编译缓存或写缓存。等待事务235/235、预加载/空参数/重存边界7/7，独立审查关闭四项发现。

- 最后两处出口遗漏已通过真实回归闭环：Web `setLanguage` 复用 `TextScene.render` 发布文字，四项 opacity/strike/ruby/reveal 回归从4失败到4通过；原生 SandboxQuotaService 的 count/release 在实际主VM建立保护区，避免协程调用时错误越过清理，定向10项/226断言通过（RED为9通过/1失败、12失败断言）。
- `web-u11-integrated-02` 的34文件/479项全部通过。加入最后4项语言回归后，`web-u11-final-01` 为35文件、482通过/1性能失败（1000行吞吐2.113低于2.5 frames/ms），功能全绿；保持源码与阈值不变，独立 `web-u11-final-perf-recheck` 6/6通过。保留整套失败及限定复跑，不称后两次为单次完整全绿。
- 完整 `windows-debug-u11-final-01`（run `d674b42f-4bdc-4b05-95f7-2715b02e822a`）在源码和夹具指纹一致的快照上完成：构建退出0，Cpp1290/1290、398871断言、0失败/0跳过，Lua孤儿36/36，其它验证器/耦合/注册通过，CTest25通过+1允许的外部AI跳过（189.59秒），包括真实冷画面/PCM和冷存档检查。Lua主套件128通过/19失败，整份receipt已在二进制替换前按FAIL归一化。
- Lua失败根因是新菜单mock测试在主套件沙箱锁定后清除模块缓存再require，首个异常又留下mock污染后续测试。将六组取消回归完整移到专用 `test_runtime_menu_cleanup.lua` 并注册现有孤儿隔离入口；每个fixture用 `<close>` 恢复module cache、KAG与输入键，运行时沙箱不变，原主147项保留。候选207条菜单断言通过，应用后完整主套件147/147通过。

最终完整 `windows-debug-u11-final-02`（run `313c3a36-8401-414d-a28b-7326cffd05ea`）全部必需检查通过：Debug全量构建退出0，Cpp1290/1290、398871断言、0失败/0跳过，Lua147/147和37/37，验证器17/7/53/57项、耦合与185 Lua/85 C++注册检查通过，CTest25通过+1允许的外部AI跳过（220.20秒）。源码与夹具运行前后摘要一致；在重建前立即执行collector与diagnostic verifier，均PASS。证据目录为 `artifacts/validation/7430ca29a83fe844a7329be0b042832d5c1191e2/313c3a36-8401-414d-a28b-7326cffd05ea/windows-debug/`。

U11实现以 `b176a741` 提交，通过 PR #4 合并到master `559a8b84`。本地dirty快照不代表未运行平台或发行批准；当前只处理合并后CI，全部绿色后按用户要求暂停，U12–U29保持原范围并等待恢复。

### U11 合并后 CI

首轮master run `34033990787` 的Linux构建、483/483 Web测试、CTest及耦合/注册检查已通过；平台矩阵门禁因全局源码标记仍停在U10而失败。将标记同步到U11实际源码提交并重新生成文档，保留每项能力原有的证据提交、日期和验证等级，不把本次同步当成新设备或SDK验证。

Windows Debug/Release随后暴露两个真实环境差异：两项恢复测试默认打开声卡，CI无音频设备时初始化失败或转为Null；测试改用现有真实SoLoud ManualMix并断言NULLDRIVER，所有状态/所有权断言保留。视频备用pl_mpeg路径直接调用解码函数却丢弃返回PCM，该函数不会触发高层callback；现在显式转交已解码样本到原队列，seek仍使用原callback，独立源码审查确认不重复交付。本机实际关闭FFmpeg重建后，18项音频/视频测试8652断言通过，其中真实D3D11/pl_mpeg子进程64断言验证非静音PCM、关闭与重开，未降低视频音频断言。

后续Linux run `34034440065` 又确认483/483 Web与CTest通过，失败为能力闭环矩阵未再生成；已按当前源码重新生成。预先执行其下一道Android检查得到82/88：五项字体检查仍定位旧文件/表达式，另有一项真实TTF计数日志在拆分时遗漏。检查器改为核对TextRendererFont.cpp与TTFState的实际atlas/range/光栅循环，恢复真实glyphs.size()日志后88/88通过；17个正负变体确认删除atlas、字形范围、光栅调用或日志，以及伪造固定计数仍失败。这些是U11回归与现有门禁修复，不扩展Android能力声明。

上述CI修复完成完整Windows Debug验证，FFmpeg关闭以覆盖云端实际备用路径：run `37648d6d-99d0-4654-9bb6-386dd72ce0d1`，原始目录 `raw/windows-debug-u11-ci-plmpeg-final`。全量构建退出0，Cpp1290/1290、398873断言、0失败/0跳过，Lua147/147与37/37，全部验证器/耦合/注册通过，CTest25通过+1允许的外部AI跳过（238.56秒）。源码与夹具前后指纹一致，collector和diagnostic verifier均PASS，之后恢复本机原FFmpeg配置。独立增量审查无待处理问题。

2026-09-08 收尾：master `b836a3a8` 的CI已通过Linux、macOS与移动平台任务，Windows两配置仍因单次性能计时失败。吞吐用例现采用同一VM的一次代表性预热及三次实测的实际中位样本，原吞吐门槛不变；1000/2000行样本交错采集，每次运行均校验完成、帧数、token数和错误事件，持续低吞吐负例仍失败。8项定向检查通过；此门槛衡量预热后运行，不声称覆盖首次启动延迟。

同时修复日志揭示的读档后清屏异常：缺失的可选textbox_style不再变成空表，直接复用宿主上下文时也清除未来样式；存在的样式在prepare阶段按当前textbox契约验证，坏样式在提交前拒绝。Lua事务316/316、Web恢复14/14通过，包含真实load→cl、完整样式中的0/false与失败保留owner/co/画面；两项增量独立审查无阻断问题。

最新完整Web为35文件/488项全绿。完整Windows Debug run `1d097a23-1f7e-4e4a-9b87-0b4e23689795`（`raw/windows-debug-u11-textbox-final`）在源码与夹具前后指纹一致的快照上通过全部检查：全量构建、Cpp1290/1290与398873断言、Lua147/147及37/37、验证器/耦合/注册，CTest25通过+1允许的外部AI跳过（189.48秒）；collector与diagnostic verifier均PASS。本机FFmpeg配置已恢复为ON。

后续CI `34177684705` 的Windows Release及其它平台均通过，Debug仍以1.9566 tokens/ms低于旧2.0预算失败。核对 `7430ca29:web/bridge.js` 确认旧校准驱动不记录回滚历史，而当前驱动每千行维护64历史项和2000已读标记，工作量不同。局部扫描判断改动的前后中位数1337.7/1351.1 ms未显示收益，已撤回，不能称为优化。

独立复核后将这一完整历史负载明确重标为2.0驱动ticks/ms与1.5 tokens/ms（3000 tokens约2000 ms上限），相对旧1500 ms预算放宽33.3%，不是运行时性能提升。每个预热/实测样本验证64历史项及对应2000/4000已读标记，保留完成、精确token、错误、2MB内存和规模翻倍小于2.5倍的门槛；持续低吞吐的统计负例同时覆盖两项新预算。`web-history-budget-final` 35文件/488项全绿，运行时代码与上一份原生PASS证据相同。

## 本轮实际执行

| 执行 | 结果 | 原始记录 |
|---|---|---|
| Debug全量构建，已有生产源码 | 退出0 | artifacts/validation/bootstrap-20260905/build-debug.log |
| C++完整套件（build/tests/Debug CWD） | 1137 passed，0 failed，0 skipped；386033 assertions | artifacts/validation/bootstrap-20260905/cpp-baseline.log |
| Lua主套件 | 145 passed，0 failed | artifacts/validation/bootstrap-20260905/lua-main-baseline.log |
| Lua孤儿套件 | 30 passed，0 failed | artifacts/validation/bootstrap-20260905/lua-orphan-baseline.log |
| 新验证执行器初始红灯 | 缺少实现模块，退出1 | artifacts/validation/bootstrap-20260905/validation-runner-red.log |
| 新验证执行器实现后的隔离测试 | 9 tests，OK | artifacts/validation/bootstrap-20260905/validation-runner-test.log |

## 首批集成验证（2026-09-05）

- 恢复受控mutation后的完整C++：1158 passed、0 failed、0 skipped；386514 assertions，记录 artifacts/validation/bootstrap-20260905/cpp-restored-full.log。
- 云同步变异：临时绕过两处staging校验后，HTTP/Steam两例全部失败；源文件恢复前后SHA256均为5619d9b70cd869c008c84360e0497c61303fade2e50651732aed475886a5fd3e。结果记录 u4-sync-mutation-result.json，标记test-fixture，不能作为发布通过证据。
- 证据工具独立审查完成；110项测试通过。coverage.py报告collector96%、verifier93%、合计95%，原始报告位于 artifacts/validation/bootstrap-20260905/evidence-finish/。
- 真实Windows Debug profile：run_id=9839a6fd-0a91-4af9-bd65-e924006a3556，11个check实际退出0；源码与夹具执行前后指纹一致。Cpp1158/1158、Lua145/145与30/30、Python15/7/53/57，CTest19 passed+1允许的真实AI服务skip。
- 该run原始receipt：artifacts/validation/raw/windows-debug-detached-01/run.json。采集及diagnostic复核均通过，归一化记录：artifacts/validation/87a96f3d846caf5ad2aac6e3b8585133e7a5aed5/9839a6fd-0a91-4af9-bd65-e924006a3556/windows-debug/manifest.json。它记录dirty工作树，不是对某个新commit的发布授权。
- Windows新preset干净configure在显式指定已核实VS实例后成功；禁用Python时完整验证configure按预期失败。WSL中的隔离CMake sanitizer探针正常输入退出0、整数溢出退出1并有UBSan诊断；这不是完整引擎sanitizer通过。
- 最初的windows-debug-initial运行在上下文切换后进程消失，只有部分原始日志，没有run.json；已保留为未完成运行，没有继续冒充通过。后续使用有PID/创建时间记录的隐藏后台执行器，复核进程终态及完整receipt。

执行器覆盖真实退出码、预/后置binary与fixture指纹、配置/源码目录身份、进程树清理、唯一目录和参数不经shell解释。U2的完整跨平台与性能要求仍未完成，计划整体保持active。

## 第二批运行时与存档修复（2026-09-05）

- U5 先复现取消后旧 waiter 回流、pending 不归零；修复后5项确定性核心测试通过。补充两项真实SDL取消/过滤器重入精确释放检查，包含既有回归的42项 AsyncLoader 测试全通过。
- Engine 的真实Lua测试复现无窗口模式忽略调试暂停；统一消费函数现在逐项检查代次，并以unique_ptr持有延迟结果。另以“旧回调取消后连续建立两个新回调”复现 registry 引用复用后误释放，改为执行闭包前移除并释放旧引用。
- 完整脚本重载在旧操作回调和coroutine.close清理之后取消异步任务，再重置上下文。局部场景重载先成功解析、关闭旧协程、取消旧请求，再切换场景；解析失败与非当前场景刷新保留旧任务；暂停或协程正在执行时拒绝切换。修复dot-call、混合Lua/KS变更检测及Windows路径分隔符；局部重载26项Lua检查通过。
- U7 初始6项回归3红，包含真实Windows文件占用导致旧档被删除。新写盘函数通过CREATE_NEW/O_EXCL独占创建同目录临时文件，检查写入/flush/close，单次替换发布且没有remove旧档的回退。Windows下12项原子存档用例及1项HTTP密文拉取故障用例通过，覆盖并发、临时文件撞名、六类受控失败和进程中断。
- POSIX独立验证直接编译当前ISaveProvider.cpp，GCC15.2+ASan/UBSan，18场景、128断言、0失败，无sanitizer诊断；源码前后摘要一致。环境为WSL2、v9fs，不代表完整Linux引擎、原生ext4、所有文件系统或断电持久性。证据：artifacts/validation/u5-u7-posix/result.json。

### 验证记录与范围

1. 完整windows-debug执行：run_id=5ec2d07c-6eb5-4031-b8ff-8842bebc42b9；全量Debug构建、Lua145/30、验证器15/7/53/57、耦合与注册检查通过。Cpp1182中1项旧静态检查失败，CTest同时暴露平台矩阵测试依赖当前Git HEAD的问题。原始receipt为artifacts/validation/raw/windows-debug-u5-u7-01/run.json；已如实采集为FAIL。
2. 移除“未使用RenderBinding.h”这一已经失效的静态断言，保留该测试及其他具体后端禁用断言，未减少测试用例。定向profile再次全量构建与完整C++：1182 passed、0 failed、0 skipped，386930 assertions全部通过。该次CTest为18 passed、1 failed、1允许的AI smoke skip；唯一失败是平台矩阵。receipt为artifacts/validation/raw/windows-debug-u5-u7-02/run.json，run_id=0f80580d-4281-4d46-a24f-f217ba020227，也如实采集为FAIL。
3. 平台矩阵测试改为按历史证据自身的HEAD检查内容一致性，并新增不同HEAD必须拒绝的用例，32项通过。未修改生产生成器、历史矩阵及其证据标签；默认当前HEAD过期检查仍然有效。只复跑失败的CaesuraPlatformMatrixAdversarial，CTest通过，collect与diagnostic verify通过。receipt为artifacts/validation/raw/windows-debug-u5-u7-03/run.json，run_id=c4a751a8-d57b-4ab2-878e-2cc99e8d288e。

三次执行均记录源码与夹具前后摘要一致；最后一次是单项profile，不能冒充一次完整windows-debug全PASS记录。最终采用完整C++通过结果，以及CTest已通过项加修复后的单项复跑；后续Python测试修复未改变生产源码与C++二进制。以上均为dirty工作树诊断证据，未授予发布资格。

### 本次发现的后续验证边界

- 完整C++日志中，既有Ollama与demo_story缺失分支仍以MESSAGE后return显示在“passed”中；统计上的0 skipped不能证明这些分支实际执行。外部AI与Demo资源路径的显式适用性和缺失处理继续归U2/U21，不能据总数宣称已验证。
- 取消失败夹具最初使用短非法图片，触发独立的Debug SEH：ImageDecoder调用bimg::imageParse未传显式bx::Error，错误作用域可能断言。取消测试改用确定性的空资源读取失败，保留晚到失败覆盖；解码器问题尚未修改，不将它归因于取消代次。
- U6源码调查已记录于artifacts/validation/u6-preparation.md：waitIdle按时序决定是否执行callback、shutdown清空剩余callback、回调重入shutdown与Engine先销毁部分后端的问题。尚未应用U6代码或测试。

## 第三批任务系统与关停屏障（2026-09-05）

- 先取得6项Job与2项Engine确定性RED；问题包括worker已完成时shutdown丢弃回调、递归poll提前执行新批次、关停/重启后旧批次继续执行、Null空任务行为不一致、后端早于最终回调销毁，以及owner pump关停后继续更新音频。
- `waitIdle`现在只等worker和回调发布。poll每次执行一个快照并拒绝嵌套poll；关停关闭入口、join全部线程再执行一次最终快照。回调重入shutdown会改变epoch并取消剩余批次；普通poll中的重启保留原poll护栏，旧回调不能进入新生命周期。Null保留inline特性，但空work拒绝和旧生命周期回调抑制与真实实现一致。
- Engine在销毁VFX/渲染/图层/小游戏之前取消脚本异步请求并关闭JobSystem；关停设置运行标志为false。owner pump、job completion以及已识别事件消费出口在关停后停止当前帧。新增活动worker等待入口关闭及pending Lua载入关停回归，确认先关闭入口、线程退出、旧Lua闭包不执行。
- 测试拆分为test_job_shutdown.cpp和test_entry_shutdown.cpp；Job测试移除未使用的Engine头，便于直接编译相同测试到POSIX目标。定向52项Job及47项Engine/Async测试通过；后续完整套件包含新增活动worker案例，共1192项。
- 首次完整windows-debug运行（run_id=f77bc05e-c689-4c39-a8b8-6d4d19a1dcf1）除旧“asset先于job关停”的源码顺序断言外，各检查通过。按新的安全顺序将其更新为job先于async、async先于asset，保留用例及顺序约束。该首轮证据如实采集为FAIL。
- 仅复跑受影响的build/cpp/ctest profile：全量Debug构建退出0，C++1192 passed、0 failed、0 skipped，387023 assertions通过；CTest19 passed、0 failed、1允许的真实AI服务skip（140.97s）。源码和夹具前后摘要均一致。原始receipt：artifacts/validation/raw/windows-debug-u6-02/run.json，run_id=405a6049-f582-4c59-810d-5d85867d61c7；collect及diagnostic verify为PASS，仅对应windows-debug-u6-recheck范围，不是单次11项完整profile或发布批准。未受修改影响的Lua145/30、验证器15/7/53/57、耦合与注册检查沿用首轮结果。
- POSIX直接运行相同仓库中的47项Job/Null测试，174断言通过。标准gcov：JobSystem.cpp为156/159（98.11%），NullJobSystem.h为36/39（92.31%）。JobSystem.cpp单独启用ASan/UBSan，无诊断；不声称Null、Engine或完整Linux引擎被sanitizer覆盖。前后源码摘要一致，并已与当前文件复核。证据：artifacts/validation/u6-posix-02/result.json、evidence-manifest.json。
- 先前对全部测试翻译单元同时插桩的POSIX编译在240s超时，没有测试结果；确认WSL编译进程已终止后，改用独立目标文件及限定插桩范围，一次新构建约13s完成。失败产物保留于u6-posix，未作为通过证据复用。
- U8源码地图与约束见artifacts/validation/u8-preparation.md。U2中MESSAGE后return造成的未执行分支、其他平台/Release验证及后续U8–U29仍未完成。

## 第四批CARC发行者信任（2026-09-05）

- 新增`ArchiveTrustMode::{Compatible,PinnedPublisher}`与按值持有的32字节`archivePublisherKey`。默认兼容模式验证归档自签名；固定模式只使用宿主预先提供的字节，归档内嵌声明及旁置公钥文件不能替换它。保留原公钥文件路径API，新增公钥字节overload复用既有验签、索引解密及解析路径。
- 自动挂载按原名称/优先级/去重规则收集全部候选，固定模式在任何provider进入资源链前验证全部候选；错误模式、缺少宿主公钥、发现失败或任一包验证失败均使Engine初始化失败并走既有关停回滚，不使用部分已验证链。松散文件、入口Lua、manifest和宿主程序仍不在CARC认证范围内。
- `--carc-trust compatible|pinned`和`--carc-public-key PATH`在切换资源根之前解析和验证。固定模式要求显式公钥，文件恰好32字节且只读一次；坏值、长度、未知选项或冲突配置在创建Engine之前拒绝。删除Lua启动之后的重复嵌入公钥检查；旧`carc_verify_on_startup`值不再控制验签。
- 12项C++核心RED为4通过/8失败，覆盖攻击者包被误接受、候选链部分注册和Engine失败未回滚。CLI在旧二进制上9/34通过，明确证明指定pinned A后B包仍被接受；修复后核心及相关archive/entry/源码约束132项通过。
- CLI中发现一个实际Windows中文路径问题：窄argv不能直接当UTF-8解码。仅对公钥参数从原始宽命令行提取路径，检查参数数量和选项索引，RAII释放argv，保留原生路径用于打开与重复值比较；Windows目标链接系统shell32。另修正三处测试诊断字符串匹配，保留非零退出、未执行config、未响应ping的断言。最终39项CLI全部通过，包含中文、空格、冲突参数、A/B替换、错误长度和Lua true/false。
- `CaesuraArchiveTrustCli`已加入CTest，全部原生profiles的CTest发现数下限提高到21，Windows Debug的C++下限提高到1204。独立安全审查覆盖核心、CLI/晚期检查删除及最后宽路径增量，未发现阻断问题。
- 完整windows-debug运行：11个check实际退出0；全量Debug构建通过，C++1204 passed、0 failed、0 skipped、387374 assertions；Lua145/30、验证器15/7/53/57、耦合与注册检查通过；CTest20 passed、0 failed、1允许的真实AI服务skip（131.29s），包含39项CLI脚本。源码和夹具前后摘要一致。receipt：artifacts/validation/raw/windows-debug-u8-01/run.json；run_id=b329f72d-4049-4af4-af92-09d930e8d5cf；collect和diagnostic verify均PASS。归一化证据位于artifacts/validation/3f7e335b5f1ae8112357230df2a62f22d916adc7/b329f72d-4049-4af4-af92-09d930e8d5cf/windows-debug/，仍是dirty快照诊断，不是发布批准或未运行平台证明。
- 当前`carc_pack`每次打包生成新发行者，未新增同密钥签署多个不同包、轮换或多发行者授权。CARC索引密钥可由公钥派生，不能把它称为DRM或内容保密机制。运行时固定公钥验证通过不代表这些作者工具与分发能力完成。
- 后续准备：artifacts/validation/u9-prepared/u9-tests.apply_patch新增8个合法签名边界/共享reader并发用例，尚未应用或运行；artifacts/validation/u2-fixtures-prepared/u2-fixture-repair.patch修复Demo/AI假通过及仅Lua变化时夹具不刷新的问题，亦未应用或运行。二者涉及CMake时应按上下文合并，不能覆盖其他修改。更强的删除文件镜像一致性、实际分配失败、精确上限与真实包性能仍待验证。

## 第五批真实夹具与CARC异常边界（2026-09-05）

- U2保留Demo和AI各5个用例，去掉资源缺失或Ollama不可用时MESSAGE后return的假通过。Demo改为真实tokenizer/compiler/scheduler执行及内容/分支/EOF断言；AI普通回归使用真实loopback HTTP的模型发现与请求响应，真实Ollama仍由独立smoke检查，缺服务明确skip。LuaManager测试夹具显式shutdown，成功pcall不读取无效错误栈。
- 构建依赖和CTest setup共用`tests/SyncTestAssets.cmake`，每次准确镜像7个生成夹具目录，清除陈旧文件但保留二进制、存档等其他输出。删除前预检全部根、祖先与子项，拒绝source重叠、输出越界、符号链接/junction及分号引起的GLOB结果歧义。独立审查发现的方括号路径、POSIX冒号、分号条目问题均已修复；Windows24项、WSL30项通过，0 skipped。
- 实际无重链接检查中，修改复制后的tokenizer并放入陈旧文件，再次构建可恢复精确副本且删除陈旧项，C++二进制SHA与mtime均未变化。另对Demo缺失、full-pipeline缺失、对白篡改、tokenizer空参数、禁用[end]做5次输出夹具敏感性检查，全部按预期失败且逐项恢复SHA。证据：`artifacts/validation/u2-sync-proof/result.json`、`u2-sensitivity/result.json`；均非发布证据。
- U9新增8个合法签名语料用例，覆盖19个subcase：整数/区域边界、index/entry不一致、坏tag、解压尺寸、重复索引既有语义、close/reopen、真实截断后短读与恢复，以及4线程共享reader读取。首轮8/8、764断言通过，没有据此虚构解析器缺陷；相关archive/entry定向套件118/118通过。
- 独立Windows子进程在受限JobObject提交额度中实际复现两条bad_alloc：read抛异常，open还遗留部分打开状态。Reader现在对分配失败返回empty/false；open清理全部打开状态，read保留索引并允许再次读取。新增`CaesuraCarcMemory`持续执行两例，RED均失败、GREEN均通过；限制为基线加8MiB，外层128MiB并带超时。证据：`u9-measure/contract-red*`、`u2-u9-green/result.json`。
- POSIX进一步复现OpenSSL加/解密先创建EVP上下文、后续vector分配异常时漏释放，两条释放断言RED；改为unique_ptr原生deleter后10条检查全部通过。`tests/probes/crypto_allocation`仅在原生非Windows构建注册，standalone亦可运行。独立审查补正了主工程sanitizer继承；复用真实`CaesuraValidation.cmake`的GCC ASan/UBSan检查通过，CryptoEngine与probe编译、最终链接均确认插桩，启用泄漏检测。证据：`u9-crypto-allocation/{red,green,sanitized}`；这是定向POSIX证明，不是完整Linux引擎或Clang验证。
- 真实包使用139个文件、39,824,624字节，归档30,917,289字节；3个新进程417/417路径、长度与SHA精确匹配。Reader修复后的Windows暖缓存样本中位数：open353.8546ms、read调用合计424.7545ms、含摘要循环465.5651ms、峰值工作集50,556,928字节。原始包未重打，配置/构建/读取退出码及输入摘要留存于`u9-measure-after/summary.json`。此样本早于仅影响非Windows的EVP修复；共享主机、未定预算，不作性能PASS或升降判断，也不据此引入流式重构。

### 本批集成证据

1. 完整Windows Debug run `a16fcaf8-b94a-41b8-bf95-d4f1e20b9b0a`：构建、Cpp1212/1212、Lua孤儿30/30及CTest22 passed+1真实AI允许skip通过；Lua主套件2个既有耗时阈值失败，验证器15项中3项Git malloc失败。该完整receipt如实采集为FAIL，路径`artifacts/validation/raw/windows-debug-u2-u9-01/run.json`。
2. 在没有并发构建时仅复跑失败的Lua主套件与验证器，源码、二进制、阈值不变：145/145及15/15通过，collect/diagnostic verify PASS，run `aebc429d-c00f-459b-bbb6-f7c87c52f1f0`，原始目录`windows-debug-u2-u9-02`。结果支持瞬时资源/耗时波动，未定位到具体外部进程，不宣称已消除主机资源风险。
3. EVP修复及新probe注册后，重做完整Debug构建、完整C++、耦合/注册检查及完整CTest：1212 passed、0 failed、0 skipped，388153断言；CTest22 passed、0 failed、1允许的真实AI skip，151.30s。run `28768856-56db-4332-b9a9-ca765c4d4905`，原始目录`windows-debug-u2-u9-03`；collect/diagnostic verify PASS。Lua未受该非Windows修改影响，复用上一条通过证据。

三次receipt的源码与夹具前后摘要均一致。后两次是各自范围的定向profile，不能合称一次完整profile全PASS，更不能把dirty快照当成发布批准。最终独立审查已处理新增probe的sanitizer配置遗漏；其余本批边界测试、资源释放与夹具镜像审查无待处理阻断项。当前共有8个单元达到本机适用或定向验收，U2和U10–U29继续执行。

## 第六批唯一脚本会话与失败终态（2026-09-05）

- 初始8项真实runner回归全部RED：C++ registry与runner/global身份不同、失败start遗留状态、stop缺失、旧load_tokens写入后继会话，以及完整reload未关闭实际wait/tween协程。`GameState`改为只引用table/nil的绑定桥，验证普通栈索引与类型、保留栈和拒绝时旧引用；VM init不再创建影子表。桥的84条断言先通过，Lua runner统一发布到`Engine.bind_active_context`、自身和`_CAESURA_CTX`。
- start先准备场景，成功后才替换旧上下文；场景准备异常返回失败，已开始执行的候选失败则关闭。所有scheduler创建点集中捕获所属ctx并发布同一个co引用。stop/完整reload/自然结束/无效延迟跳转关闭旧协程与操作、停止旧tween、再撤销资源及AI回调；清理期间拒绝重入start/update等操作。自然EOF保留可查询的最终表，并继续返回既有`ended`状态；显式恢复请求可以创建新协程，不能复活旧协程。
- Engine在Job、async及render/layer等后端销毁之前停止runner；LuaManager关闭VM时也调用幂等停止入口。真实Engine回归覆盖A停止/B启动、成功/失败载入、worker与loader两个队列边界，A引用可GC且不进入纹理配额申请；B恰好一次申请并在headless guard处返回失败。另证明scope清理先于Job/render/layer关闭，清理中新排队的载入也不回调。不声称创建了真实GPU资源。
- AI六项原生回归全部先RED。修复数字键记录用字符串键删除导致的重复unref，取消按VM移除回调，移除跨VM全局cancel epoch；completion使用VM主Lua线程，拒收submit时释放引用并返回false。之后补上提交协程实际GC后的投递及真实NullJobSystem同步完成两个分支，均进入最终完整C++验证。
- 独立生命周期审查发现并修复两项P1：HistoryUI没有关闭作用域，reload后残留history输入焦点；选择/文本输入关闭协程后残留全局回调及原生输入模式。HistoryUI新增真实`<close>`，选择和输入新增`Operation <close>`、幂等清理、函数身份恢复及失活closure guard；普通完成、取消与部分初始化失败共享清理。历史18项、输入26项通过，RED分别为14/18及8/26通过。旧choice/i18n夹具改为实际协程挂起，保留并加强原断言。
- 后续独立审查补出缺失label/不安全pending jump绕过终止清理的问题；两种真实runner场景先RED，再加统一finish，GREEN通过。其他引用桥、关停、输入恢复及U2路径修复无新增阻断项；AI与原生回归另经独立只读审查。失败跳转、自然EOF、同场景reload的旧运动取消、失败替换保留pending reload都已进入持续C++测试。

### U2：独立Release检出与明确产物路径

- 在`D:/文件存放处/code/Caesura-foundation-validation`创建独立detached检出`1d9a71f15d91870b7b3d079d027e834b7dc8303d`，使用windows-foundation preset，FFmpeg/Steam/Live2D关闭。SDL3为本机已有SDK，96个文件摘要保存在`u2-fresh-preparation/sdl3-sdk.json`；这是新源码/构建目录验证，不是全新机器依赖安装证明。
- 新Release配置与全量构建退出0，Cpp1212/1212、388142断言，Lua145/30及验证器检查通过。原完整run `126615b1-46eb-49f1-87bd-533382480070`因BuildCli找不到preset目录的引擎而skip、Golden找不到Lua而FAIL，如实采集FAIL；该检出执行期间dirty=false且源码/夹具一致。
- CTest现在显式传入当前配置的`CaesuraAmeKAG`和`lua_cli`路径，经`CAESURA_ENGINE`/`CAESURA_LUA`传递到实际CLI、预编译和Golden。显式无效值（含空值和Lua目录）不能回退或skip；无显式配置的手动发现保持可用。Golden契约检查任何非零退出均FAIL，不能把42/127等失败当作通过。
- 9项路径检查通过，包括中文/空格、陈旧默认程序、缺失显式产物与真实CLI失败清理。测试哨兵使用`/bin/sh`，避免宿主PATH中的Windows WSL bash截获`env bash`而造成夹具错误；哨兵仅验证选择路径，始终故意返回失败，不冒充Golden通过。
- 只将U2路径修复应用到独立检出，不混入U10代码；原Release二进制不变，相关3项CTest全部通过，run `c9a0a863-934b-4126-aa4a-6a8096ab1f37`，目录`raw/windows-release-output-paths-02`，collect/diagnostic verify PASS。此补丁快照为dirty诊断，不能改写原完整FAIL为单次全绿；U10新代码的Release仍需复核。

### U10最终集成证据

1. 初次完整run `846932f0-0fca-4e84-a38d-cd4eb17bf60c`如实FAIL。Cpp1227/1228：新增重载测试错误地把重载后新动画的合法执行也当作旧动画继续；改为严格观察旧tween取消、计时不再前进及新co身份。Lua还暴露新依赖未纳入隔离fixture、旧静态检查匹配到错误helper、以及EOF清理后返回状态不兼容；分别修正夹具定位与真实`ended`返回，不放松时限或减少测试。
2. 定向完整运行时复核`772fde91-f1da-4d5e-a920-406b61444588`中Cpp1228、Lua主147及CTest23 passed+1允许AI skip通过，只有旧textspeed夹具失败。它在EOF后重置显示状态，改为在真实wait期间测量skip-off且断言会话仍活跃；仅复跑孤儿套件30/30通过，run `8843ca72-1076-4f47-ba33-31743998bb34`。以上失败和修复receipt保留。
3. 加入最后的延迟跳转终态回归与AI同步/GC验证后，再执行一次完整11项profile：**Cpp1229 passed、0 failed、0 skipped，388674断言；Lua主147/147与孤儿30/30；Python15/7/53/57、耦合、注册检查均通过；CTest23 passed、0 failed、1允许的真实AI服务skip，161.85s。**

最终run为`9fa6f181-343b-4069-b0f3-3f32eb2843ba`，原始receipt `artifacts/validation/raw/windows-debug-u10-04/run.json`；collect和diagnostic verify PASS。执行前后源码与夹具摘要均一致。该次是完整本机Debug验证，仍为dirty快照，不授予发布资格或未运行平台证明。当前9个单元达到本机适用/定向验收；U2及U11–U29继续。

## U10交付检查（用户决定今天不开展U11）

- `5c073cb9753c803f44f01636673498e348f54977`在独立干净检出完成完整Release profile，run `0bbb0ef2-0d76-4814-b62e-cbabcae01d60`，原始目录`raw/windows-release-u10-03`。11项检查全部通过，Cpp1229、Lua147/30、CTest23 passed+1允许AI skip；dirty=false、源码/夹具一致，collector与严格verifier通过，仅证明该profile，不代表发布批准。
- 原独立检出的U2路径修复已核对摘要后保存在stash `cb96db3f6dfb383bc70bc23e786064d608851232`，再更新到U10提交。未删除该备份。
- CI预检查修正Windows测试步骤中多余的`Pop-Location`文本，并将原生会话3项回归纳入Entry专项过滤。Debug全量构建与Entry专项通过；Web Player 370/370、现有Editor 631/631通过，记录`artifacts/validation/delivery-u10/preflight.json`及JSON报告。没有新增Studio功能。
- API普查和能力矩阵按源码重新生成；平台矩阵只同步用于新鲜度检查的源码锚点，每项平台能力仍保留实际历史验证commit/日期，未伪称重测所有平台。平台矩阵和历史计划事实块检查通过。
- 分支已推送`origin/codex/runtime-foundation`。本次交付要求为合并到master并使云端CI全绿；合并及CI结果待后续实际记录。U11仅有只读地图，未开始实现。

### PR #3 首轮 CI 反馈与修复

- 首轮 run `33950845183` 如实失败。Windows/macOS 验证器把短路径或 `/var` 别名根与已解析的夹具路径比较，误报越界；入口统一规范根路径，新增两项别名/越界回归，Windows 17/17、定向 POSIX 2/2 通过。
- Android NDK 的标准库缺少 `std::jthread`/`stop_token`；并发归档测试改为 `std::thread` 与作用域释放/等待守卫，保留创建中途异常时的安全收尾。完整本机 Debug 构建通过，C++ 1229/1229、388674 断言、0 failed、0 skipped；独立审查未发现阻断项。
- Linux 内置 Lua 缺少桌面平台宏，实际产物的 `io.popen` 失败；为 Linux/Darwin 加入对应宏及传递链接依赖，新增直接使用 `lua_cli` 的 POSIX 进程/动态加载回归。云端结果待验证，不以系统 Lua 代替构建产物。
- Linux HTTP smoke 缺少显示环境，普通 CTest 与原生证据执行均接入已有 Xvfb；HTTP 保持必跑，真实外部 AI 服务仍单独记录条件跳过。
- 两项 Lua 场景测试错误使用 Windows `2>nul`，在 Linux 创建/删除根目录文件，触发执行期间源码变化检测。改为各平台原生命令后，Windows 与隔离 Linux 的 11/11、9/9 断言通过；Linux 确认无 `nul`、无场景残留、源码摘要不变。保留源码稳定性检查。
- 修复提交为 `ec749d03`；原始失败日志、定向回归及审查证据保存在 `artifacts/validation/delivery-u10/`。管理员合并已获用户授权，前提仍为本 PR 最新提交 CI 全绿；U11 不开展。

### PR #3 第二轮 CI 反馈与修复

- Run `33951953378` 中 macOS、iOS、Android 静态与实际 Android 构建通过；Windows 两配置被旧 Lua 注册检查阻断，Linux CTest 的 HTTP 打包产物断言为 74/75。
- 新增 Lua POSIX 检查已经在 CTest 使用本次 `lua_cli` 执行，注册检查器现同步识别该形式；六项正反验证覆盖缺失注册、错误解释器、注释与脚本后缀，不放行未执行的测试。独立审查提出的注释/后缀误判已修正；178 Lua + 79 C++ 文件注册检查通过。
- Linux HTTP 端点打包成功，但测试使用 Windows 两层构建目录假设而查错产物位置。改为共用根目录定位，五种临时布局共15项检查通过，缺失 MANIFEST 仍失败；Windows 真实 HTTP smoke 复测通过。生产打包代码与75项HTTP断言未修改。
- 修复前本机完整 CTest 只有 CLI EOF 超时：后台启动器保留标准输入导致进程等待。明确关闭输入后失败项复跑通过，未扩大测试时限；原始失败与复跑日志均保留。此项是本机启动方式问题，不冒充一次完整全绿运行。
- 第二轮及修复证据继续保存在 `artifacts/validation/delivery-u10/`，最终合并仍等待最新提交云端全绿。

## 并行所有权

- resource agent：U3/U5代码与SDL回归已交付，独立只读审查完成。
- storage agent：U4/U7代码及存档回归已交付，POSIX分支另有独立运行证据。
- reload agent：重载边界、Lua回归与平台矩阵测试隔离已交付；Engine接线完成只读审查。
- U6主代理：任务系统、Null模拟、Engine屏障与回归；契约/最终代码只读审查通过；POSIX agent仅负责独立编译与证据工件。
- U8：核心补丁准备、CLI实现与独立安全审查分别负责；主代理应用补丁、统一构建/验证及文档；后续U9/U2 agent仅准备artifact补丁。
- evidence agent：U1实现和独立审查已完成。
- 主代理：构建/测试统一执行、U2执行器与profiles/CMake/CI、组合根接线、集成审查与本记录。

共享构建目录只由主代理使用。任何单元只有全部适用验收有实际证据后才能改为完成。
