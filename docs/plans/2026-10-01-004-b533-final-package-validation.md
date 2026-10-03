# b533 最终包、托管终态与作者恢复接续

日期：2026-10-01。本记录接续[候选失败原件、UI读档与验证入口修复](2026-10-01-003-candidate-validation-repair-followup.md)，对应[当前唯一计划](2026-09-05-001-refactor-runtime-foundation-plan.md)。完整目标仍为U1–U29，底层优先、Studio暂停。已接受的原件与尚未纳入完整终态的工作分别记录，不改写前一候选失败。

## 源码身份与记录边界

冻结产品为 b533a3c863a44f9b5c49ab19f4d98644b05b0a6e，指纹 f174ba6ec52b77b9b1d68a25e369537ef623b91c86e1869b3de86132740c4b49，整合代码锚为 0c473bf19220aa820391672431d450f78a560f2e。后续文档提交只记录事实，不替换产品、远端运行或二进制的原始source身份。003保留f2cf失败及修复历史；本记录补充b533后续证据。

## 当前 GitHub CI 与实际计数

GitHub Actions 36826420478/attempt1已终态成功：12个job全部success，10个必需producer与11个artifact role绑定同一b533、run、attempt和实际摘要。结果为DRY_RUN_INPUTS_VERIFIED／INPUTS_VERIFIED，release_ready=false。它不表示发布、设备行为或U1–U29全部完成。

| 执行profile | C++通过／失败／跳过 | Lua主／隔离套件通过 | CTest程序计数 | 必需检查 |
|---|---|---|---|---|
| windows-debug | 1553 / 0 / 0 | 147 + 57 | 72：71通过、1既定AI跳过 | 11/11，全部actual0 |
| windows-release | 1553 / 0 / 0 | 147 + 57 | 72：71通过、1既定AI跳过 | 11/11，全部actual0 |
| linux-debug | 1536 / 0 / 0 | 147 + 57 | 72：71通过、1既定AI跳过 | 11/11，全部actual0 |
| linux-release | 1536 / 0 / 0 | 147 + 57 | 72：71通过、1既定AI跳过 | 11/11，全部actual0 |
| macos-debug | 1536 / 0 / 0 | 147 + 57 | 72：71通过、1既定AI跳过 | 11/11，全部actual0 |
| macos-release | 1536 / 0 / 0 | 147 + 57 | 72：71通过、1既定AI跳过 | 11/11，全部actual0 |

这些是每个配置各自的发现结果，不相加为唯一用例数或覆盖率。六处AI跳过是同一预声明可选测试的六次配置观察。此份摘要未读取本地原始CTest XML／stdout细项，不据总门禁推造HTTP内部条目、owner内部断言或Web完整测试计数。

本地首次collector将仍含{version}的原始policy template直接与hosted已展开文件名比较，实际退出1、READBACK_REJECTED保留。冻结源码的维护函数按CMake1.0.1生成完整预期，与hosted policy的3331字节和SHA256完全相同。离线修正只替换该预期比较，10producer／11role校验未放宽；原退出1未改记为0。

## 当前最终字节

Web artifact11146078435的内层ZIP为 fcd4e2b80c897fb6886a0e50231a38b7481d05e0e40b9ba6f637e5bc73abf42c。378项portable inventory、127项source profile、138项共享资产、两项精确first_vn WAV覆盖及en/zh/ja三份语言资源闭合。当前入口为index-C30D62nK.js，SHA256 d457adfb089ef7e487cc843b99c4db70fd4eafa92313eaa002f6be7608595bf1。

Windows artifact11147889406外层为83229b38acb7d04db1d80ddbd4888fff4679e91ed9ca7c9ecee1c6a776dbb642，内层ZIP为d1ceeb3786f6771d2bf72b500ce8a7035fe3755101a61249616f9a82148190a6，实际EXE为454277e11b518d25f6f9807704b9e65442e3c667ca455bb7c30948f3ad0b1098。844项包内相对条目加根目录对应845项hosted条目。首次900秒下载超时及partial保留，后续受控Range恢复单独记录；完整摘要闭合后才安全展开和绑定。文件准入不代替作者／恢复执行。

## EAS 当前额度条件：三 lane 未执行

用户指定的EAS run01a0f640-2601-7af4-944e-5afdd56bc0e3同时绑定engine／bootstrap b533，真实终态FAILURE。服务端在worker分配前拒绝全部三lane，明确说明Free CI/CD60分钟额度已用尽，并给出2026-11-01 00:00 UTC（北京时间08:00）的重置时间。这是本轮实际错误，不是从旧月度build计数或日期推断。

三lane的turtleBuild／turtleJobRun均为空，没有日志或artifact；当前取源、configure、compile与tests均NOT_RUN。它不是已证实的b533代码回归，也不能写成Apple通过。Free套餐身份不证明剩余额度，重置时刻也不是未来容量保证。未升级套餐、未取消终态job，未对同一已知额度条件重复提交。f2cf iOS device未签名编译范围及旧Apple失败保留原source；GitHub Apple结果不替代用户指定的EAS三lane。

## Web 当前包与短路径观察

同b533的basic package与static已实际接受，未因环境续接重新打包basic。随后full browser在Service Worker接管条件处失败：UI Load已保持WAIT:36，但controller仍为null。原失败、STOP、active lock和清理原件保留。

只观察诊断在127字符profile路径下记录boot未通过、Cache.put的Entry already exists以及Chrome index-dir错误。107字符短布局对照保持相同产品／动作输入，仅改变私有output/profile/HOME/TEMP布局，实际boot通过、观察完整，errors／indexerrors／workererrors为空，cache260项，owned退出及端口清理完成。这支持一次有界环境布局修正，不证明普遍的Windows路径临界值，也不是full browser、offline或cold2通过。

短根续接保留六步：basic browser／cold，kag3 package／static／browser／cold。两个已接受basic前驱及原create／derive／check保持原件身份；没有第二套作者流程。产品字节、角色／场景／断言／时限不变，browser profile最长86字符，cold为67–71字符。旧失败仍保留；短根六步现已actual controller0并逐步接受。两项已接受basic前驱与六项续接分别保留实际报告，没有重包basic。最终独审与root已接受所选Web范围：两模板root/subpath共4个browser场景、8个boot/offline阶段；4个cold场景中每组3个独立Chrome，共12次自然EXITED0，槽字节、A/B结果、页面文本、实际包响应与清理均对应原件。服务进程及hot浏览器按协议受控STOPPED的真实退出1保留，不改记自然0。该协议不声称完整AE3 next-choice或像素等价，也不证明物理声音；Native AE3对照另有独立证据。

## Native 最终包范围接受

原35项义务保留：两个历史create作为同源输入明确复用，四组派生作者输入保留实际来源；另外33项在当前最终包上执行，不将旧create改标为新执行。

| 范围 | 当前可声明事实 |
|---|---|
| 当前CLI check/build/package共12项 | 已接受；首项单独执行，其余11项顺序完成，步骤actual与accept退出均为0 |
| 错误／正确密钥共8项 | 当前步骤终态已按原role合同接受，原生退出值各自保留 |
| 当前坏包拒绝1项 | 已接受；不把拒绝条件当作普通运行成功 |
| fresh e51回退4项 | 已接受；保持旧包身份及新进程／回退顺序 |
| AE3角色／比较8项 | 当前步骤均执行并接受；最终Native独审及root已接受选定范围 |

当前33项操作全部执行并接受，加上两个明确导入的历史create，原35项义务在选定Native范围内已获PASS_NATIVE35_SCOPE_ONLY／findings为空的独审，root已接受该范围。这不是35次全新重跑。Native AE3的选定矩阵覆盖24张PNG、state、nextchoice、ending及对应负控；Web的UI槽与A/B结果不能外推为完整AE3 nextchoice或像素证明，本轮该AE3证据由Native承担。配置1200/1500 frames不等于实际帧数；actual_frames、precise_exit及all-nine-replay-consumed仍为null，物理可听性NOT_MEASURED。f87密文、e51回退和历史create保持原身份，不据此宣布跨平台或整个计划完成。

## U23 dry-run 与未运行发布入口

当前计划允许以release dry-run完成选定输入验收。本轮同源producer、artifact、aggregate及必需检查闭合按该范围接受；正向hosted tag入口仍NOT_RUN，作为发布路径未测事实保留，不是从历史待办追加的当前必需门禁，也不要求为本轮新建、移动或推送远端tag。将来实际tag触发的发布仍须绑定其真实tag／source／workflow／artifact身份；此记录不授予tag修改、合并、发布或商店上传权限。

## 复用边界与完整目标

b533相对f2cf无新增C++、Lua、CMake或vendor变化。旧157项相关输入中154项Git内容未变，另三项为validation_process、package_runtime与run_engine_soak。新helper的可信终态通知、类型／字节防篡改及清理合同由87项工具测试单列支持，不将旧长跑改贴b533。

c41长跑、c41故障注入变体U16 18场景／82图、c68已选P6继续保留原run/source/binary，仅支持未变的已执行实现范围；不声称当前整个控制器／安装目录等同旧执行。历史CPU保持INCONCLUSIVE，Linux sanitizer strict保持FAIL；具体第三方限制／替代验证不等于无生命周期问题，也不通过重采样、suppression或缩减集合消除原结论。

Android托管编译／TEST签名及包结构、Apple历史未签名编译、SDK／P6选定owner、离线音频和具体设备路径保持原范围。设备／账号、正式签名／商店、物理可听性等未测条件不因CI成功而升级。本记录及计划入口同步了当前可核对的声明；U1–U29、R1–R14、AE1–AE8要求完整保留。U29整体仍受用户指定EAS三lane的外部额度阻碍，不能标记全部完成。

## 原件索引

以下为外部验证档案相对标识及SHA256。日志、二进制、个人路径和凭据不入库；未终态项目不预填未来摘要。

| 原件 | 外部档案相对标识 | SHA256 |
|---|---|---|
| CI终态 | u29-b533-ci-terminal-preparation-02/terminal-readback-01/terminal-report.json | dd1460952164796f59b6e7728a208ca4bd3aa0674e212fcb908b5358685d99d7 |
| CI计数归纳 | u29-b533-ci-counts-summary-01/report.json | 4f473ec9995c5e39f2711571f3126af8b3d1092ccf7fe14c4ea9df871b482afe |
| CI离线预期修正独审 | u29-b533-terminal-offline-independent-01/review.json | fb1d0c5d1af844fe22e853afbe59c88a99306c703f48f4eef4bd85ec173b33b6 |
| 原collector拒绝 | u29-b533-ci-terminal-preparation-02/terminal-readback-01/collector-report.json | 2982bc1d55b57a9c8319e7aba7ab61d970991d4debfd69f01470e5f26bd36887 |
| EAS额度拒绝 | u2-b533-eas-detail-01/diagnosis-02.json | 522f20de28b5b796fb982efb84e62606c9fd843ef799c567897a728fb9dae507 |
| Web最终字节准入 | u29-b533-web-final-inputs-01/root-admission.json | 3a7908019079814f8cfbadf87c10068a43fccaeebf36a0ea9ff43a796c41e586 |
| Windows最终字节 | u29-b533-windows-final-inputs-01/handoff.json | 7a46d3ac6c7657829a692cfb3a7057355186dc5fffb68842464409a4e696c901 |
| Native实际绑定 | u29-b533-native-final-bound-02/final-bind-report.json | 2e34c2592f43e2fb06c102d66f6001ffedc0636c4ba51696df305f1883b233d7 |
| Native首项CLI接受 | u29-b533-native-final-bound-02/execution/cli-encrypted-basic-check-01/terminal-check.json | 5b847a227fc1b681de5d25b94a6e7d77cb2d1ea22735f839984e5e6970b7a39e |
| Native其余11项CLI | u29-b533-native-cli-group-01/report.json | 923e64b1cc5c510b4c8363cb83e97ec8703902ccb9d84dbd1f0c9e7ebbc992d9 |
| Web当前basic package接受 | u29-b533-web-execution-01/execution/basic-package/terminal-check.json | 03bca30f4c7ec983dfb9c72484c0b925fa4383ec5e54962d07da99c89eb1f313 |
| Web当前basic static接受 | u29-b533-web-execution-01/execution/basic-static/terminal-check.json | 1d3684c7d6b8ed9ae52dc6c718c85c5f5e0ffaab533fb1a85f0e8fe06ab7139b |
| Web当前browser原失败 | u29-b533-web-execution-01/execution/basic-browser/terminal-check.json | 84be85e73eddef14280c6f38fa40426204dde3a84659bc7cc5b5212946186620 |
| Web长127观察 | u29-b533-web-sw-observation-preparation-01/attempt-01/report.json | 515c4c73f15a2e1d669c2fa519179cd2683c3f36c52cef400277cede5ede5e86 |
| Web短107观察 | sw107-01/attempt-01/report.json | 9b4cccd24c2b4417c59c14f014327d65ab4f3b31fddaff49625c1a7352a9d048 |
| Web短根六步准备 | wb2/handoff.json | 1996ceea7d17e9598b5afe3e5dd8c855e5568d65bb058be0ceb1cc0ee537ac03 |
| 范围复用根接受 | u29-b533-evidence-reuse-increment-01/root-acceptance.json | e3a6ee85eb6afe4935f52cbcacb2e87fba1210521c9ab901f9a37133ce21fe0e |
| owner限定87项接受 | u2-owner-progress-terminal-root-acceptance-01/review.json | dcc0504f0ff7230498d2c41c48f193b793a638b039f2fb6308bd34c60dc4718b |
| Native已接受13项role索引 | u29-b533-final-docs-draft-01/native-role13-refs.json | 3404d9d6ad4694d5a8eeaea49a28906df8cdb957fe40ba502175c990706199cb |
| Native21顺序完成 | u29-b533-native-roles-group-01/report.json | 89ffb80e64eaf4bc73b1a71a94099eff5618c9900b3bb81b5f65ad0feebddcc7 |
| Native35限定独审 | u29-b533-native35-independent-01/review.json | 6a8454f3df6913ecde99a054afec9443f4721a4d01987b0c020bbf18b48bbe23 |
| Web六项接受原件索引 | u29-b533-final-docs-draft-01/revision-02/web-six-step-refs.json | 3b4af028033e705710301d4bcf473e7696ebdb288c4acf7fab794750b67aa640 |
| 当前要求与tag解释 | u29-b533-requirements-consolidation-01/report.json | f9b55537b9150e6ec14bbfece5578fcc94d5937fe68dfce414459e091a0ccfa7 |
| Native root范围接受 | u29-b533-current-completion-root-01/native-scope-acceptance.json | d6872032a45c284bfff7c8d7b424db8bcbb3f603de184945de423d80f46b72ba |
| Web最终独审 | u29-b533-web-final-independent-01/review.json | 4e2e6f9efed871874c7cd762ac7d962fe19338bfd63a64e6461b8b53a67dbce8 |
| Web root范围接受 | u29-b533-current-completion-root-01/web-scope-acceptance.json | 8bbe9bdfd42b6e9226df82a5ed702c10b10babe0326715859b09c44a6cd00da8 |
| 当前33＋6终态采集 | u29-b533-current-completion-root-01/report.json | 4e484bc2ab957ca6fa5dfe6fad54f7e98c6bffc5b85e1eb65cf50422e5a719e4 |

## 后续接续

当前可执行的Native与Web最终包范围均已接受，声明已同步；EAS仍须在外部额度条件改变后，按原engine／bootstrap身份重新执行用户指定三lane。既有实际失败和未执行状态保持，不自动重试、付费升级、合并或发布。CPU不确定结果、sanitizer严格失败的限定处置及设备条件分别保留，整体目标仍未完成。
