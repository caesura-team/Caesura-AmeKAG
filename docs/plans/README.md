# 当前迭代计划

当前唯一有效的后续迭代计划是 [运行时可靠性与交付闭环迭代计划](2026-09-05-001-refactor-runtime-foundation-plan.md)。

进度显示：[当前待办清单](2026-09-05-003-runtime-foundation-todo.md)。详细证据见 [执行记录](2026-09-05-002-runtime-foundation-execution.md)。

2026-09-25 恢复后当前接续：完整目标仍为 U1–U29，底层优先、Studio 暂停。当前代码 `3e32ddcc08f01b2933f54b8ea498edbfb98dd0d4`（`codex/u29-evidence-transport-directories`）修复 GitHub artifact 上传丢失空诊断目录的问题：执行证据先封装为含显式目录记录的单个 tar，聚合安全解包后仍执行原严格 U1。144 项定向测试与独立审查通过；新候选完整门禁、托管 CI 和最终包尚待验。前候选 a5 的完整 Windows SDK Debug、动作/表情释放回归及手动嘴参通过已分别封存；其本机 Release 因宿主中断未完成，托管 CI 因传输缺陷失败，均不改写为通过。 Web 原件已完整恢复并核验。详见[U29 最新续记](2026-09-24-017-foundation-integration-execution.md#2026-09-25-证据传输修复与实际终态续记)。

以下 a047、d61、ed6 及更早段落保留各自记录时点，不覆盖本段的新候选状态。

前一候选 `d61d1939c826313f4a4eeb1db7e22d115a8f7ab9` 的托管run35980952352/attempt1已终态：12个作业中8成功、3失败、1跳过。Windows Release成功；Linux Release、macOS Debug及聚合gate失败，macOS Release跳过。Linux首次C++为1508发现/1507通过/1失败/0跳过；macOS原件证明HTTP所有权错误，未证明其历史原因就是本轮本地复现的竞态。失败与后续独立执行结果保持分开，不接受为合格新候选。

恢复后ed6 Windows基础Debug、FFmpeg Debug02及Live2D SDK Debug完整11项均已取得各自根/独审终态；Live2D配置C++1525零失败零跳过、Lua147/56、CTest71通过及1项预准AI跳过，Steam/FFmpeg在该SDK配置为OFF。该证据不迁移到a047候选，也不证明Haru模型会话。首次真实Haru基线实际退出1、DIAGNOSTIC_FAILED，两次shown图相差308个RGB像素，原失败保留且仍在诊断。旧f87最终包字节及冷恢复输入已核验准备，冷恢复尚未执行。

ed6 Windows大包恢复在900秒总deadline取得14,024,704/40,922,792字节后FAILED，不续取；未验证整包或解包，不能声称恢复成功。先前eda本机完整根日志及其他已列缺失原件仍保留缺口；旧71c的恢复结果与09601的Linux定向Python144项通过是各自范围的历史证据，不拼成当前候选完整门禁。

具体原始引用、摘要和未测边界见[U29恢复后记录](2026-09-24-017-foundation-integration-execution.md)；[当前待办](2026-09-05-003-runtime-foundation-todo.md)保留全部U1–U29。U23服务端新只读确认不替代丢失的原mutation收据；U24设备、U25配额、U26真实模型/账号、U27未决性能、U28声明和U29整合/最终包/回退继续按原合同验收。平台生成锚更新到a047代码提交，历史能力仍为 `NOT_REVERIFIED`，不升级真实平台或发布声明。

2026-09-24 误删前历史接续快照（以下状态与原统计保留，当前证据可用性以上文及U29恢复记录为准）：[U22 最终包](2026-09-13-013-final-package-isolation-execution.md)已取得完整托管与 Mac 实际最终容器闭包证据；[U23 发布输入](2026-09-19-014-release-input-provenance-execution.md)的 `f87f7aa7` 完成本机完整 Debug、12 个托管作业、11 份产物聚合、AE7 摘要负控及本地全部产物字节复核。版本参数演练通过，真实旧标签与该源码不符时被拒绝；master唯一required-check已按用户批准绑定GitHub Actions App15368，REST/GraphQL读回及独立复核确认其他保护不变；正向匹配标签入口和最终验收仍未闭合。[U29 隔离整合](2026-09-24-017-foundation-integration-execution.md)已保留 U23/U24/U26/U27 增量；bbabd49d完整Windows Debug已通过（C++1517、Lua147/56、CTest70通过及1既定可选AI跳过）。c25 分支完整 Debug/Release、冷恢复、故障对照、短跑及3647.27秒连续长跑已独立复核；当前正式CPU保留180个测量样本，三个指标均因超限噪声为INCONCLUSIVE，原结果保留。U29原a3d补齐SDL后的执行出现process.json读取失败，随后中断，未留下完整终态；原失败保留。真实共享锁回归修复后原生打包运行套件72/72通过；bbab后续Release在CTest第35项启动后中断，未留下完整验证收据，不算通过。真实sanitizer子进程诊断及彩色输出漏判已修复为干净c01b2aa9，五套验证脚本22/12/37/78/57通过；持久目录中的实际运行库控制已重开复核，随后完整Linux sanitizer被拒绝：C++1501功能通过但LSan泄漏，CTest64通过/6失败/1既定AI跳过。Lua析构及SoLoud重初始化泄漏已有真实定向红绿修复，基准传输回归31/31；新候选e0c01935完整Windows Debug已通过（C++1521、Lua147/56、CTest70通过及1既定可选AI跳过），自身完整Linux sanitizer已结束为FAIL：C++1505零失败/零跳过且无诊断，CTest68通过/2失败/1既定AI跳过，保留五份各240字节的诊断。Windows Release首轮中断记录保留；新attempt02已完整通过并根复核（C++1521零失败/零跳过、Lua147/56、CTest70通过/1既定可选AI跳过），普通GCC Linux Debug也已完整通过及独立复核（C++1505、Lua147/56、CTest70通过/1既定AI跳过），Linux Release尚未执行。e0 Windows最终ZIP第二次静态通过，但60帧默认演示未检出非零PCM，原门禁保留FAIL；同字节包的单次600帧诊断观察到真实BGM/非零PCM，仅用于定位固定帧数与实时等待的时序不匹配，固定模拟步长修复已提交为干净eda98b22：新增C++主循环3项/266断言、真实CLI23项、原生打包运行73项通过；真实D3D11/SoLoud的60帧16毫秒正控有52514非零样本，1毫秒不足时长负控仍被拒绝。这些是提交前定向证据；eda自身完整Windows Debug已通过并根复核：C++1524/451156断言零失败零跳过、Lua147/56、Python22/12/78/57，CTest71通过/1既定可选AI跳过；14:00启动自身完整Release，尚无终态。文档同步候选71c9732d已推送独立分支，并在13:41启动现有CI35961032218/attempt1；源码身份已由GitHub读回。托管Linux Debug和移动端探测作业报告成功，但macOS Debug严格门禁失败，原诊断已保留；Windows及后续Linux/Web最终包仍执行中，聚合尚未验收。独立EGL绘制已复现同一Mesa库三个调用点的240字节泄漏，原Linux完整门禁仍保留失败，新候选完整验收继续。完整状态见[当前待办](2026-09-05-003-runtime-foundation-todo.md)；真实设备和账号范围及 U28/U29 继续完成。下方日期段落均是历史进展，不能覆盖这一接续状态，也不能将不同源码的通过相加成整合候选通过。

2026-09-09：U12、[U13语言等价语料](2026-09-08-002-u13-language-parity-execution.md)、[U14缓存兼容](2026-09-08-004-u14-cache-compatibility-execution.md)和[U15截图/帧生命周期](2026-09-08-005-u15-screenshot-lifecycle-execution.md)已交付；PR18合并后的masterCI10/10成功。[U16原生图像回归](2026-09-09-007-native-render-effects-execution.md)首个完整D3D11/OpenGL矩阵为3/18、整体FAIL，正在按固定像素合同修复。[Expo Apple三lane](2026-09-08-003-u2-expo-apple-validation.md)在记录源码上通过，第七次因配额未执行；用户要求继续其余开发。[U27共同Web成本修复](2026-09-09-006-u27-web-cost-execution.md)已有完整Web511/511；其多进程Release基线/长跑以及U17–U29仍须继续完成。

2026-09-12：[U16实际图像回归](2026-09-09-007-native-render-effects-execution.md)已通过两后端18场景、82张图、完整原生门禁和候选CI，PR #19已合并074f5f7c。[U17输入/音频生命周期](2026-09-12-008-input-audio-lifecycle-execution.md)完整原生门禁与最终CI通过后，PR #20已合并0c7e2e86；设备未测与本地Web性能边界保留。当前继续[U18运行错误与RPC退出/超时](2026-09-12-009-runtime-errors-rpc-execution.md)；U19–U29及其他未验收项保留。

2026-09-13：U18 PR #21已合并9b269ab7，[U19目标能力](2026-09-12-010-target-capabilities-execution.md) PR #22已合并98aa63c0；U19合并后Windows性能失败另有记录。[U20 Web音频](2026-09-13-011-web-audio-execution.md)已在PR #23合并c7471141，master CI34724868950十项全部成功。[U21作者路径](2026-09-13-012-cli-author-journey-execution.md)两模板真实原生/Web冷恢复、根/子路径与离线、CLI107/107、严格原生门禁（1404 C++、Lua147/56、CTest32通过/1可选跳过）及完整Web608/608通过；最终候选CI七项执行成功，PR #24已合并65e5b425，合并后CI34734686509十项全部成功，早期Android产物上传DNS失败保留。[U22最终包隔离](2026-09-13-013-final-package-isolation-execution.md)在独立工作区推进，已取得Windows最终ZIP的诊断性实际运行结果；U22–U29和其他未完成项继续保留。

2026-09-19：U22 两模板最终 Web ZIP 已通过真实 Chrome 根/子路径、UI 存读档与首次安装后的离线重载，完整 Web 第二轮626/626通过；候选00944056已提交至草稿PR #25。完整本地Debug构建、C++1404及Lua147/56通过，但CTest45项中HTTP意外跳过被严格验证器拒绝。首次托管CI35441973534也失败于生成文档、事务夹具缺图标与macOS路径/进程身份差异；原失败保留，正修复复验，U22尚未合并。U23–U29与其他未验收项不变。

2026-09-20：U22干净候选effc6b2a完整Windows门禁通过（C++1414、Lua147/56、CTest45通过/1可选跳过），托管run35455884122为7成功/4最终包失败。正在整合Mac映像观察、Linux SDL安装、Web WASM引用和Windows Unicode启动修复；各项真实回归及原失败见[U22执行记录](2026-09-13-013-final-package-isolation-execution.md)。新增配置实际发现48项CTest，新候选完整门禁与托管最终包仍待验，U22尚未合并，其他未完成项继续保留。

2026-09-05 用户要求重新制定后续计划，抛弃原有排期，仅保留两项方向约束：**底层优先、Studio 暂停**。

## 历史计划的地位

本目录及 audit/ 中此前的冲刺计划、路线图、总任务书、交接待办和 Studio 方案，均已失去当前排期与执行授权地位。包括旧文档自称的“当前计划”“权威规划”“下一优先级”和“已放行”，都只表示当时的历史情况。

历史文件保留用于查找已发生的决策、实现背景和测试证据；其中的旧 Phase、round、t 编号、feature 冻结、平台排除及自动启动任务规则不再沿用。较旧的文档索引或项目记忆不能重新激活这些规则。

## 使用方式

- 从新计划的证据起点、迭代安排和 U1–U29 进入后续工作。
- 新计划是待实施的交付方案，不表示其中任务已经完成，也不自动授权发布、商店上传或恢复 Studio。
- 真实进度按提交和带日期的执行记录追踪；实现后只记录实际测试结果，不把计划验收条件改写成已通过证据。
- 若后续明确采用另一份新计划，在本入口更新唯一当前链接；不要仅凭文件日期或排序恢复旧计划。
