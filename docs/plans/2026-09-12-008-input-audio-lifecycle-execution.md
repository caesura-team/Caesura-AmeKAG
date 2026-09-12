# U17 输入、音频与前后台生命周期执行记录

日期：2026-09-12。依据唯一计划U17，依赖U6/U10；底层优先、Studio暂停。当前在 `codex/u17-input-lifecycle`，基线为U16合并 `074f5f7c2e1aa958a488f7645692307361d29134`。本任务尚未完成，不以当前切片代替完整U17或U1–U29。

## 合同与范围

- 窗口尺寸取当前SDL窗口单位，物理drawable仍按U16路径交给呈现层。排队事件使用自身坐标；每帧最多一次故事点击，后续移动不能改变已接受点击的位置。
- 生命周期Background/Foreground、Pause/Resume、音频FocusLost/Gained、InterruptionBegin/End分别配对。只有所有音频阻止原因解除后才恢复；重复通知不重复调用后端或Lua生命周期钩子。
- `voice_wait` 仍可点击/skip跳过，同时逐帧观察自然完成；关闭或替换协程清理该等待拥有的状态，下一普通页面不应自动越过。
- 继续覆盖触摸身份/取消、普通点击与长按互斥、IME提交/取消、旧会话完成事件、真实SoLoud混音及适用设备行为。沿用BackendRegistry、组合根和现有输入/音频服务，不增加另一套输入框架。

## 当前实际回归

| 范围 | 未修实现 | 当前结果及证据边界 |
| --- | --- | --- |
| 交错暂停原因 | `u17-focus-red-01.log`：2项用例均失败，21断言中12失败；提前resume及重复通知均可复现 | 分别保留四类原因，按聚合状态变化调用音频；`u17-focus-green-01.log`中35项相关Entry/AudioFocus用例、599断言通过。音频后端为计数替身，尚不代表混音或设备输出 |
| 当前窗口尺寸与排队坐标 | `u17-input-red-01.log`：2项用例、53断言中6失败。SDL实际800×400而getter仍400×200；实际事件点击/移动坐标均被即时设备状态0覆盖 | `u17-input-green-01.log`中2项用例53/53断言通过。真实SDL dummy事件循环、固定200×100逻辑渲染替身；不创建GPU，不证明物理显示器DPI切换 |
| 真实runner语音等待 | 暂存及整合后的相同53观察均为46通过、7失败，包括正常/自动/仅跳过已读模式下自然结束仍卡住 | `u17-voice-wait-integrated-green-01.log`：53/53。实际tokenizer/compiler/scheduler/runner与命令，仅音频宿主绑定受控；新测试注册孤儿套件，临时场景由测试清理。无真实声卡/SoLoud证明 |

Lua两文件暂存基线与当前checkout仅换行字节不同，整合前逐字比对规范化文本，再保留当前换行风格；实际前后hash见 `u17-voice-wait-integration.json`。早期失败均保留，未替换为通过日志。

## 尚未完成

- 适用设备运行。2026-09-12执行 `adb devices -l` 启动本机ADB服务后列表为空；没有Android连接/触屏/设备运行通过证据。下文合成事件与离线混音不能提升此范围。
- 完整Debug/C++/Lua/CTest候选通过、Web性能失败归因与处理、候选CI及交付。输入、IME、完成事件与暂停逻辑的定向回归及独立审查已完成，证据分别见下文。

原始日志均在当前实施工作区的 `artifacts/validation/` 下；只读准备与暂存产物在主工作区的同名目录下。覆盖率尚未测量，不使用测试数量替代覆盖率。

## 真实离线混音与完成事件归属

`u17-manualmix-run-01` 实际执行真实Engine/Lua/KAGBinding/SoLoud的一个C++用例、两个subcase，661断言通过。强制实际NULLDRIVER（ID16）、48kHz双声道，每次1024帧；四个交错暂停阶段各8192帧PCM peak/RMS均0，语音位置漂移0。全部原因解除后同一handle位置从0.512s推进至0.810666667s；音源自然结束在第135个owner tick，命令在137继续。提前停止旧音源、容许原50ms淡出后不投递自然完成；第200 tick新会话播放新音源，302结束、304继续，恰好一次合法完成。二进制已归档；这些是实际离线混音和合成生命周期事件，不是设备输出。

完成归属首个测试夹具因替身无AudioRestore在stop时拒绝，第二个采用真实音频但在debug暂停中start被已有保护拒绝；分别保留red-01/red-02，不当作归属缺陷证据。修正为暂停中stop、调试恢复后owner开始新会话的合法序列，`u17-completion-owner-red-03.log` 真实SoLoud/runner用例52断言中2失败：旧音源已由Engine消费但尚未投递的完成事件污染了新owner。当前修复为待投递批次固定Lua runner表引用，在owner变化时丢弃旧批次；`u17-completion-owner-green-01.log` 中4项相关用例783断言通过，保留同owner的debug延迟完成、无runner完成、非字符串回调错误和真实ManualMix验证。不宣称覆盖同表内部热重载的所有语义代次。

语音等待增量后的完整Lua主套件147/147、孤儿套件47/47通过，日志 `u17-lua-main-01.log`、`u17-lua-orphan-01.log`，退出记录 `u17-lua-suite-exits-01.json`。随后新增IME或其他Lua修改须重新验证受影响范围。

## 触摸与生命周期事件链路回归

`u17-touch-red-01.log` 在未改MobileAdapter上实际执行9项新增测试，143断言中34失败。缺少具体类入口的合同诊断单独列项，其余失败来自真实SDL事件观察：过早left-down、motion缺少held状态、触点取消和重入后状态丢失。原测试采用C++20能力检测，仅使旧类可编译；缺入口时不模拟分类算法，仍观察旧SDL_PushEvent。实际候选增加同步event sink、可选延迟点击和取消，保持IMobileAdapter不变、10个触点和默认即时模式。

`u17-route-red-01.log` 在未改Engine路由上实际执行6项测试，90断言中24失败，涵盖完整64位设备/手指标识、跨设备同指名、取消/SDL自动触摸鼠标、长按抬起、焦点往返和失焦/后台/暂停。测试使用真实SDL dummy窗口及Engine事件循环，仅GPU/音频设备替身。候选接入完整标识到8个稳定槽的映射、同步共用分发、抬起前手势分类和独立窗口焦点暂停原因。`u17-route-green-01.log` 68项相关用例、1414断言通过。

新增GAME即时drag及原始finger回调兼容、文本/快捷键焦点和所属窗口测试，`u17-focus-route-red-01.log` 两项用例81断言中15失败。修复后 `u17-focus-route-green-01.log` 中8项SDL链路用例171断言通过。初次同步提取曾遗漏原始finger转发，该回归已恢复；全局F5/F6保存快捷键仍保持既有焦点策略。

## 组合文字与表单提交

`test_ime_composition.lua` 私有环境加载实际TextCommands.input、Operation、TextScene等模块，仅宿主输入/布局查询替身。原实现27通过13失败；最小修改后 `u17-ime-lua-green-01.log` 40/40通过。组合中Enter保留输入框；Backspace等待宿主Editing更新，不误删已提交字符；Escape先取消预编辑，再次Escape取消表单。验证还包含多字节字符长度/删除、操作关闭与旧handler隔离。旧strict-sandbox输入测试仅补“IME已清空预编辑”的宿主Editing事件，原断言未删除或放宽。

实际SDL事件→Engine→原生绑定→TextCommands.input的C++用例包含提交/取消两个subcase。测试初次缺少Lua package.path（red-01至red-03，属于夹具启动失败）；补齐既有Lua搜索路径后 `u17-ime-command-red-04.log` 33断言中5失败。正确同步当前Lua夹具后 `u17-ime-command-green-02.log` 33/33通过；早期错误CTest名称未选中任何测试，连带green-01仍运行旧Lua而失败，均保留且不作为通过证据。有效同步为 `u17-ime-sync-02.log`，实际CaesuraSyncTestAssets 1/1。该验证包含SDL实际文本输入启停与合成composition/key事件，不证明OS候选窗实际事件顺序。

## 独立审查跟进

主工作区 `artifacts/validation/u17-route-review/report.md` 的有界审查指出三项：Android `-2/-3→gain` 与独立中断原因不配对；GAME后台取消的配对抬起被暂停过滤；多指长停后逐抬起触发额外LongPress。实际 `u17-review-red-01.log` 3项用例69断言中12失败；`u17-review-green-01.log` 69/69通过。Android全部失焦码使用同一生产转换函数配对FocusLost/Gained，保持原pause-on-duck策略且不解除其他来源的中断；暂停只放行同步适配层已经发出的配对抬起；LongPress要求整个触摸序列最高仅有1指。

复核发现同步抬起回调立即Resume时，外层通知仍使用取消前的暂停状态。新增 `u17-reentry-red-01.log` 实际18断言中5失败；修复后重取聚合状态，并与MobileAdapter已发布的暂停状态比较。`u17-reentry-green-01.log` 包含17项Entry/SDL/IME/审查回归及ManualMix、1082断言全部通过。独立复核已关闭三项原始发现及该重入增量，没有新增发现；完整候选门禁仍待执行。

## 当前完整验证进度

- 最新Lua主套件147/147、孤儿套件48/48通过：`u17-lua-main-02.log`、`u17-lua-orphan-02.log` 与退出记录。注册检查196个Lua文件、89个C++文件全部有真实入口；这是注册完整性，不是覆盖率。
- 当前Windows C++实际发现1361项，已将本轮新增28项同步到各平台最低发现门槛，并将孤儿套件最低门槛升至48。Linux/macOS新计数需后续CI实际确认，不能从Windows通过推导。
- Web模块索引检查及Vite构建通过；Node22.23.2完整Vitest `u17-web-full-01.log` 为38文件、511用例中510通过、1失败。唯一失败为synthetic1000真实性能预算：3个稳态样本2534.6/2766.1/2591.2ms，中位2591.2ms、1.158 tokens/ms；固定预算为2000ms。场景完成、3000token工作量与2000/1000配对规模比通过，性能门槛仍失败。当前保留失败并进行同环境基线/候选诊断，不修改预算或减少快照工作。
- 首次完整原生批次 `u17-native-candidate-01` 固定在 `c564284f881ca06ee92f1ce74e3edf04be1e0931`，完整Debug构建通过；C++ 1361项中1360通过、1失败、0跳过（401711断言中1失败）；Lua147/147与48/48；CTest28项中26通过、1失败、1预先允许的CaesuraHeadlessAiSmoke跳过。唯一失败均为旧源码定位断言误匹配新增的窗口归属wheel过滤，实际wheel处理未进入pointer分支。
- 修正源码测试的搜索起点，继续检查wheel分支在pointer完整闭括号之后，保留全部原断言；同时在真实SDL事件回归中增加wheel值、KAG/GAME焦点、外部窗口和返回KAG后的计数断言。`u17-wheel-test-green-01.log` 两项用例79/79断言通过，1359项过滤未选中。没有修改生产源码或性能预算；完整候选需在该测试修正后再次冻结执行。
- 首次批次保留完整原始日志和run.json，未生成通过证据包。首次collector与后续增量链接重叠，读取CaesuraTests.exe被锁而失败，记录在 `u17-native-collect-01.log`；后续二进制已重建，不能把它补入首次批次。下一完整批次结束后须先collect/verify，再修改源码或重建。

## Web同机配对诊断

主工作区 `artifacts/validation/u17-web-perf-diagnosis/paired-01/run.json` 使用相同Node22.23.2、同一个实际Wasmoon VM和JS bridge，对 `074f5f7c` 与本轮Lua三文件执行固定AB/BA/AB三对测量，每个成员先完整预热。切换实际Lua模块及KAG命令表后重新安装runner bridge，避免旧闭包继续驱动旧runner。每次均完成3000token、4000帧/点击、64检查点、2000已读标记，无错误，输入hash稳定且dispose成功。

| pair | 顺序 | 基线ms | 候选ms | 候选/基线 |
| --- | --- | ---: | ---: | ---: |
| 1 | 基线→候选 | 2416.7651 | 2356.5516 | 0.975085 |
| 2 | 候选→基线 | 2615.9329 | 2523.7117 | 0.964746 |
| 3 | 基线→候选 | 2182.2225 | 2526.5514 | 1.157788 |

两者均未达到2000ms固定预算，配对比中位0.975085但方向不一致，不能据此宣称U17优化或确定退化。该standalone jsdom配对诊断不是完整Vitest通过证据。另一次 `profile-01/run.json` 对两者实际调用计数相同：3000次snapshot.capture、4000次runner.on_click、2000次ch、1000次p、2次operation.cancel_all；没有进入新增voice_wait/input/runner.update等待分支。带观察器时间不参与预算评价，Node CPU profile及Lua指令样本用于后续定位共同成本，绝不减少快照工作或事后放宽门槛。

## 完整原生候选与首次跨平台CI

第二批 `u17-native-candidate-02` 固定 `aa5012d4af4421e036e96557da360cf42952f5ce`，2026-09-12 20:36–20:43执行完成：完整Debug构建、C++1361/1361（401723断言、0失败0跳过）、Lua147/147与48/48、CTest27通过/0失败/1预先允许的AI服务跳过，验证器17/7/53/57项、耦合与注册门禁均通过。运行前后源码与夹具hash一致。严格collector/verifier均PASS；完整日志、执行二进制和manifest位于 `artifacts/validation/u17/evidence/aa5012d4af4421e036e96557da360cf42952f5ce/ca0cc51b-d08b-4f31-ae99-c47464cf13ec/windows-debug/`。该证据限此配置，不授予发布或设备运行结论。首次为该批次选择了不合规范的扁平collector输出目录而被拒绝（collect-02），改为既有SHA/run/profile结构后collect-03通过，未改验证规则或重跑测试。

[PR #20](https://github.com/caesura-team/Caesura-AmeKAG/pull/20) 首次 [CI 34694212672](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/34694212672) 的Linux job103554773223已通过完整Web38文件、511/511用例：synthetic1000样本1824.7/1842.2/1826.7ms，中位1826.7ms；配对规模比中位2.369038，原预算均通过。此为该CI配置的完整Web证据，不覆盖本地性能失败。该job的CTest也通过，随后因自动生成能力矩阵未同步而失败，原始job日志保存于主工作区 `artifacts/validation/u17-ci34694212672-linux-job.log`。当前仅通过原生成器更新源码指纹、位置和测试引用，未手改能力结论；最终CI与合并仍待完成。

独立性能分析 `u17-web-perf-diagnosis/results.md` 将共同开销定位为待进一步测量项：两版Lua指令样本中pack_seen_flags均12496/44978（27.78%），此比例不是CPU时间；Node估计约68% Wasm、20–22% Wasmoon JS glue。下一U27测量需进一步区分实际发布值的递归转换与跨运行保留，当前没有泄漏或环境成因的确定结论，也未据此修改生产实现。
