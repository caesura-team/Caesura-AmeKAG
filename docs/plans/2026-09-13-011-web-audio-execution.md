# U20 Web 音频异步与增益执行记录

开始：2026-09-13。当前唯一计划U20，底层优先、Studio暂停。独立工作区codex/u20-web-audio从U19最终文档候选716e6ee3开始，已快进至PR #22合并后的98aa63c0；合并树与716e6ee3完全一致。U19代码2dfe451e已有完整原生/Web与实际包证据，最终文档候选的CI34716396395七个执行job成功、三个PR打包job按条件跳过。U20尚未完成，U1–U29其余未验收项保留。

## 已有行为与本轮范围

现有AudioEngine已用每bus代次、context revision、pending状态和source身份阻止旧fetch/decode安装新播放；stop、loop、onended释放及恢复ticket已有U12/U17相关回归。Native ManualMix是原生离线混音证据，不能代替Web Audio或真实浏览器验证。

本轮先处理unlock生命周期，再补齐三个gain入口：play fade-in、stop fade-out、显式bus fade。保留已有clip/bus分层、v1音频快照和用户设置存储。暂不实现`audio.crossfade`专用入口，不新增通用音频插件或自动化框架。

## 行为合同

| 操作 | 增益层与所有权 | 验收重点 |
|---|---|---|
| `play(...,{fadein})` | 新source的clip GainNode从0到已有播放volume；不改bus | 旧代次不能安装fade，0时长立即；失败不报成功 |
| `stop(...,{fadeout})` | 当前clip逻辑停止；物理尾音进入显式retiring集合，按音频时钟停止后释放 | 不立即disconnect，不遗失owner；旧onended不删除替代source |
| `fadeVolume(bus,target,seconds)` | bus GainNode及最新runtime bus目标 | 不改clip/用户持久设置；新fade或setter覆盖旧automation |
| `captureBgm` / restore | 捕获当时clip增益；以v1静态gain恢复新source | 清除旧clip fade/stop owner，不复活旧回调；保留当前bus与更新的用户设置 |
| unlock | resume属于原context/revision | 旧resume成功或拒绝都不能借新context状态报成功，构造异常不能漏成Promise拒绝 |

Native源码确认playBGM/stopBGM使用clip handle包络，`fadeVolume`使用bus handle并更新runtime目标。Web保持source→clipGain→busGain→destination；播放的既有volume语义不迁移到bus。用户设置控制器不由脚本fade或restore写入。

采用AudioParam线性调度与`AudioScheduledSourceNode.stop(when)`的音频时钟。暂停时音频时钟停滞，不能用墙钟计时器提前断开尾音；每个clip/bus仅维护当前线性段，用于持有连续增益和快照。[Web Audio标准](https://www.w3.org/TR/webaudio-1.0/#AudioScheduledSourceNode)

普通零时长停止保持幂等清理。正fadeout但没有当前source时仍取消pending，但返回false表示没有执行fade。合法stop的调度异常会立即释放该clip并返回false，防止失控尾音；非法bus/duration前置拒绝不动owner。bus调度中途异常不发布新target，尽力保持原瞬时gain并返回false，不承诺恢复旧WebAudio事件序列的原子性。

现有快照不包含剩余fade或bus字段，本轮不悄悄扩充格式。恢复clip时取消旧active/retiring clip包络，保留当前bus节点、目标及其live automation；较新的直接音量设置应已取消旧bus ramp，恢复不能覆盖它。

## 必需的共同路由修正

当前`stopbgm`/`playbgmstop`先将bus淡到0，再请求clip停止。如果忠实实现bus fade而保留该组合，下一首BGM会被持久的零bus静音。因此两个停止路由改为仅调用一次clip stop，毫秒转换为秒，移除额外0.1秒padding。显式`fadebgm`/`fadevol`仍是bus操作。保留本轮未修改的schema/default/time-shadow语义，不混入兼容别名重设计。

## 实施与验证顺序

1. 精确AudioEngine回归先复现unlock迟到/构造异常，再修复；保留新context正常解锁并播放的正控制。
2. AudioEngine增益、active/retiring所有权、音频时钟、取消/销毁及恢复，用真实类和受控WebAudio边界先RED再GREEN；不以调用了ramp方法当成真实PCM证明。
3. 共同Lua停止路由通过实际handler参数/调用次数回归；同步旧源码字符串锁为真实行为断言，保留原用例。
4. bridge转发fadein/fadeout/显式bus fade并传播实际ACK；通过真实Wasmoon、backend、scheduler及AudioEngine验证三个入口及stop→play不会静音。
5. 使用真实浏览器AudioContext/OfflineAudioContext和已知PCM检验包络、组合增益、暂停/继续与旧回调；手势解锁与离线PCM属于不同证据。
6. 三个fade入口及必要恢复/设置控制全部通过后，才以`audio.fade=supported`交付，并重新生成目录、Web source profile和原生目录标识。仍须按实际后端条件报告不可用/本次失败。
7. 独立增量审查、适用Lua/完整Web/原生门禁及CI；记录源码/配置/二进制/日志身份，不重跑不变快照直到偶然变绿。

核心增益文件由有界子代理维护，主代理维护bridge、目录和集成；另一子代理仅处理共同Lua路由。共享构建由主代理统一协调。没有修改用户全局插件、MCP或凭据。

## 首个unlock切片

u20-unlock-lifecycle使用实际`audio-engine.test.js`：RED41通过/3失败，GREEN44/44，exit0。旧context resume resolve/reject在destroy并重建后不得误报成功；构造失败返回false，下一次手势仍可成功建立三bus。新context循环播放和所有权正控制保留。生产仅修改unlock，源码/测试运行前后稳定；未将其称为完整U20、真实浏览器或fade验证。

## 增益与播放回执

核心增益回归从45通过/27失败转为72/72通过，涵盖三种渐变、取消/销毁、active/retiring source、旧onended、恢复时的静态clip gain与最新bus设置。共同Lua两条停止路由改为单次clip stop；原生解释器执行的相关回归分别34/34、10/10、259/259。撤回音频可用性后的清理仍为51/51，不删除旧断言。

真实Wasmoon→KAG/backend→bridge→AudioEngine集成补齐fade参数和实际ACK。审查复现了成功启动后await继续前被stop/restore替代时的UI发布竞争，以及后续请求404导致前一已提交source被错误隐藏。取消请求代次不能作为已提交source身份：现在每个成功source有独立Symbol回执，bridge只在回执仍对应当前owner时更新UI。原play的布尔结果保留。三条竞争路径正式RED后通过，最终核心72与集成15共87/87；独立增量审查已关闭finding。

主界面现有逐帧状态刷新会核对实际音源，自然结束不会停留在播放文字。新DOM/Wasmoon回归先RED再GREEN，音频UI完整6项随后通过。该主机边界控制AudioContext，不能作为真实解码证据。

## 真实浏览器证据

`scripts/web_audio_smoke.mjs`只开放指定本地模块和生成的已知PCM16 WAV，使用新Chrome profile及生产AudioEngine。OfflineAudioContext直接测量Float32 PCM，覆盖clip淡入/淡出、bus渐变、用户setter覆盖、恢复静态gain与退场源不干扰新source。Live AudioContext在默认autoplay策略下从suspended经可信鼠标手势解锁，以静音输出端前的Analyser测量实际PCM；暂停400ms超过整个250ms fadeout，仍保留尾音owner，继续后按音频时钟释放。

最终`artifacts/validation/u20-real-audio-03/report.json`为46 PASS、errors=[]，Chrome153.0.8010.36，四个源文件hash在运行前后及独立审查时匹配。恢复gain由已知线性包络和独立音频时钟计算，PCM预期不循环使用被测capture输出。source_stable与browser_endpoint_closed为true，随后按确切profile查询剩余Chrome进程为0。没有物理扬声器/耳机或其他浏览器验证。Linux CI新增同一真实PCM步骤并保存报告，不重试音频断言。

## 完整验证进度

独立工作区完整Debug构建exit0，自己的Lua解释器执行主套147/147与隔离54/54通过。最初VS自动发现配置失败已保留；显式选择本机已验证的VS实例与SDL路径后配置及构建成功。最终干净候选的完整原生profile仍待执行。

`u20-web-full-01`完成24场景/6资产bake、Vite及40文件578项：577通过、1失败、0跳过。唯一失败为原有synthetic1000性能预算：样本2215.7/2392.6/1810.1ms，median2215.7ms，1.8053 frames/ms未达到>2；故事median860.6ms通过。源码指纹7be3d24d00b57d1b1fc5a208fa180b8c1f29ea14cf61b7d6132ff1e704b84eb0前后相同，Node22.23.2与Lua二进制未变。本次完整结果为FAIL，不能以577通过或定向音频通过替代；未改性能阈值、未将单轮差异归因于U20，U27独立进程/Release/长跑任务保留。

## 等待行为验收补充

逐项核对U20原始验收时，发现现有Web回归验证了音源查询，但没有把真实Wasmoon等待命令与浏览器持续驱动连起来。当前Web pump在同一次同步调用中反复喂16ms，而真实AudioContext时钟尚未前进；`waitsound`/`waitbgm`可能提前耗尽60s保护，`playvoice`可能耗尽frame-limit。`voice_wait`返回WAIT后主界面又没有持续调用runner.update，可能无法自然继续。先用真实KAG/AudioEngine回归验证这些用户行为。

拟采用已有runner的update/on_click合同：音频命令以作用域标记当前音频等待，返回独立WAIT_AUDIO状态释放宿主；新的窄范围tickAudio入口按AudioContext时钟差值推进一次，再运行到下一个等待点。main现有RAF驱动该入口，暂停时不给音频等待累计虚假时间。显式点击仍可跳过voice_wait，自动播放不得冒充手动点击中断语音。marker在自然结束、取消和协程关闭时清理，不进入v1音频存档，不改非音频同步测试的推进模型。

正式`u20-wait-review/red-02`的14例均进入有效WAV/AudioEngine路径后失败：四种等待的真实脚本语义和main自然RAF推进被确认。首轮部分iscript夹具错误使用未开放backend全局，没有算作产品失败；修正为公开宿主调用进入已有[p]后，正式RED不再包含这类错误。实现上述作用域等待及RAF入口后14/14通过。

追加审查发现共同`kag_runner.on_click`仍能在进入waitsound/waitbgm后批量resume200次，nil帧值会提前消耗约3秒预算。59.9秒边界两例真实RED后，点击批量循环在新音频等待处停止；已有voice_wait仍能接受当前点击一次。另一例独立时钟RED发现异步发布后重读时钟会漏计区间；现在Web runner按等待scope身份保存已消费时钟截止点，下一帧消费发布期间的新增音频时间，新scope另设起点。

最终`artifacts/validation/u20-wait-green-03.log`两文件20/20，保留原14例，覆盖自然结束、重复tick、真实backend stop、手动skip、Auto计时器不skip、暂停中的120秒墙钟前进、59.999/60.001秒保护边界、并发tick、新场景已建立后的旧decode迟到，以及发布期间时钟推进。独立增量审查已关闭这些finding。共同Lua主147/147与隔离54/54在首个scope实现后通过；最终点击循环补丁仍需进入候选完整门禁。

代码5fd87b70及仅同步平台来源锚点的ebd0c7e5已进入草稿PR #23。干净ebd0c7e5的`u20-web-full-02`共42文件598项：597通过、1失败、0跳过，源码指纹57fcd7ded898d5352b151b004696132cadea226381131fe160ca3df4beac5b6f稳定。失败为旧Audio UI助手仅接受advance:WAIT/DONE，无法接受音频自然完成后RAF发布的parked:WAIT。助手现在要求本次click的成功回执，以及advance/parked两种来源的已完成页面；不接受WAIT_AUDIO作为完成，也不接受ERR回执。原有音源及设置断言保留，六项完整UI随后通过。

本次原性能预算全部通过，synthetic1000样本2240.7/1994.2/1719.6ms，中位1994.2ms；规模比中位2.1585。此前性能FAIL仍保留，不据此宣称所有主机稳定。CI34721310444的Linux在相同旧UI助手失败：597通过/1失败；新增真实Audio步骤46项通过，Chrome152.0.7977.82，源码稳定、调试端点实际关闭。该旧候选CI不能算全绿，更新后的候选需重新验证。

## 最终候选验收

最终生产与测试代码为f3e5f1aa，595ba447及f19a5262只更新平台来源锚点与自动生成能力矩阵。

- `u20-web-full-03`在干净595ba447完成bake24场景/6资产、Vite及42文件598/598，0失败/0跳过。指纹49cfd3f82e8bd73b0834a8edf36762d7c8e6be89429e65f602fc79c35d07e312前后相同，Node22.23.2与实际Lua字节未变化。原十二项性能预算通过，故事中位884.4ms，synthetic1000三样本1621.8/1709.7/2182.3ms、中位1709.7ms；配对规模比2.2959/2.3333/2.9292按原中位规则为2.3333。保留慢样本与先前FAIL，不宣称跨主机稳定或U27 Release/长跑完成。
- 最终`u20-real-audio-04`真实Chrome153.0.8010.36的46项PCM/生命周期全部通过，errors=[]、源码稳定、实际调试端点关闭。与前次相比只增加readonly音频时钟访问和已审查的harness清理/CI参数，PCM oracle未改。
- 实际交付播放器`dist/u20-player-audio-03`在Chrome的`/games/u20/`子路径通过9项：已解码真实WAV且默认autoplay等待；可信点击解锁；实际暂停保持声音与游标；恢复后自然结束只推进到下一页；手动下一页继续；没有页面错误或未处理Promise。运行前后包目录digest均为3bbd3becf980830ee615a55e62606dbf3227eff8d879a7b22938a166e69301f5。报告与已目视截图在`u20-player-audio-04/`。使用新profile的零音量偏好，没有物理扬声器证明；本次所有Chrome实例已确认退出。之前三次probe未到音频等待：作者前置ch/text都触发了既有点击等待，第三次日志明确为WAIT:2、audio none、errors=[]；这些失败作为夹具前提错误保留，没有改生产文字语义。
- 首次原生候选receipt ae53bc98-929c-4066-9b90-50997c1fc0d0中，C++1397/402490、Lua147/54与其余检查通过，但CTest两项失败：新CMake缓存选中了System32/bash.exe的WSL启动器。查询能力生成器的`--help`又实际触发了文档生成，因此该次source_changed_during_run=true；不会把它当成稳定候选。修正本工作区CAESURA_BASH_PROGRAM为已验证Git Bash后，两项定向CTest通过，原失败完整保留。
- **干净f19a5262的最终原生receipt cc493c95-8934-473d-99c5-43fa556a8dfd**：11/11命令exit0，完整Debug、C++1397/1397与402490/402490断言、Lua147/147与54/54，CTest32通过及1项预先声明的可选AI服务跳过。源码指纹56f634c2cc4ec8f9aeb2aa64c16356279a3750c551947f6392ec035a81bb62ba及夹具前后稳定。collector与严格verify_release_candidate均PASS；只覆盖windows-debug profile，不授予发布权限。
- CI34721769101的595ba447在完整Web598/598及真实音频46项通过后，仅因生成矩阵过期而失败。同步生成结果后的**CI34722656508（f19a5262）七个执行job全部成功**，三个PR包job按条件跳过。早期失败未被重试记录覆盖。

本轮独立审查提出的gain、回执、等待时钟与harness问题均已闭环。最终计划文档提交的CI与PR #23合并仍待外部状态确认。其他浏览器/物理输出、仓外两模板作者旅程、发布包隔离、U27独立进程Release/一小时长跑及U21–U29继续按原计划执行，未被本次音频验收替代。
