# U23 发布输入来源与统一门禁执行记录

当前依据为 `2026-09-05-001-refactor-runtime-foundation-plan.md` 的 U23/AE7。U22 的首候选仍在修复门禁失败，本任务在独立 `codex/u23-validation-inputs` 工作区提前实现无设备依赖的边界；不将 U22 或 U23 标为完成，不执行实际发布。

## 合同与边界

统一入口应绑定完整执行源码 SHA、预锁定候选 policy、调用/被调用 workflow 身份、明确 run ID 与 attempt。GitHub PR 的 `head_sha` 与默认合并检出的源码 SHA 不一定相同，须分别记录并从 PR/commit API 验证关联。每个 required 角色映射到精确 job 名称并取得该 attempt 的唯一 job；失败、取消、跳过、缺失、未结束与混用 attempt 均拒绝。aggregate 自身尚在运行时允许整个 run 为 in_progress，但其必需生产作业必须已成功结束。历史 attempt 原始状态保留，不能只挑最新绿色结果。

最终包采用生产作业独立输出的 artifact ID、Actions 传输 ZIP digest、包清单摘要与原始验证收据摘要。下载按固定 ID，包内声明不决定 required 集合或可信执行收据。复用 `verify_release_candidate.verify_evidence()` 校验 U1 原始日志，不另建结果解析器。最终聚合收据由调用者另行锁定摘要，dry-run/上传前再次打开明确文件重算；签名或重压缩等改变字节的步骤须先重新完成 U22 验收。

版本在引擎 CMake、配置产生的 requirements、各平台/Web 上传清单及显式标签之间核对。Pages 的 artifact ID 与其 tar 内容单独绑定，不假设 `deploy-pages@v4` 存在 artifact_id 输入。发布 dry-run 之外的操作依具体授权执行。

## 已执行的生产端切片

初始源码为 U22 候选00944056086e6e2ffa54047a2cdc988b5bf37eae。已有包装控制器提供原始最终包执行与逐字节稳定性检查。本轮在其上增加 `caesura.package-upload.v2`：包含 Release 配置、CMake 版本、准确的最终文件列表、每个最终文件对应的原始 package-run.json 精确副本，以及原生 requirements 副本和摘要。Web ZIP 绑定其压缩完成后的验证收据；前置目录的通过不能替代 ZIP 验收。所有副本继续进入生产端上传前的稳定性检查。

GitHub 环境上下文仅记录为 `authentication=NOT_VERIFIED`；本地运行明确为 local。托管执行缺必需上下文字段时拒绝，不能降级标为 local。环境声明还必须经过后续真实 API 来源认证。

原四项回归在旧实现均失败，保留 `artifacts/validation/u23-producer/red-01.log`；第一轮完整25/25通过，增加环境上下文正负控后27/27通过。独立审查发现 main 实际输出的 upload_files 漏掉新增收据和 requirements：本地 outputs 存在不能证明已上传。两种实际 CLI main/_outputs 到明确传输列表的回归分别复现缺文件，见 `upload-closure-red-01.log`。现由同一显式依赖清单生成 upload_files，再在测试模拟的下载目录逐个检查引用和摘要，完整28/28通过、0失败0跳过，见 `green-full-03.log`。这些是实际文件及命令边界夹具结果，不是实际 Engine/Chrome 包验证，也不是托管发布通过。

## 待整合与验收

- 托管 API 层、严格 U1 回执的下载绑定、v2 清单的接收校验和最终重算仍在实现。
- 共用 validate-engine.yml、CI/release/Pages 接线、显式 policy、版本标签与两项 hosted dry-run 负控制待执行。
- 服务端 GET 的已核实状态：master 一次批准、没有 required_status_checks，有效 rules 列表为空；管理员强制与 CODEOWNER 规则未启用。本轮未修改服务端。稳定聚合 check 名称与 app 身份确定后单独配置/读回，不附带改变用户现有审查或管理员策略。
- U22 新候选及三桌面/Web 实际最终包门禁仍由 U22 记录承担；本任务不以局部先行工作替代其验收。

官方接口依据：[attempt 专属 jobs API](https://docs.github.com/en/rest/actions/workflow-jobs#list-jobs-for-a-workflow-run-attempt)、[run/attempt API](https://docs.github.com/en/rest/actions/workflow-runs#get-a-workflow-run-attempt)、[artifact ID 与摘要](https://docs.github.com/en/rest/actions/artifacts#get-an-artifact)。

## 接收层、托管层及真实证据适配追加

生产端最终28/28在Windows和WSL均通过，独立审查关闭upload_files遗漏。托管API层使用固定run/attempt、完整required job历史、精确job名称、PR head/merge父提交关系与固定artifact ID/digest。独立真实HTTP反例发现声明Content-Length大于实际完整JSON时bounded read未自动报错，以及`chunked `尾空白被验证器接受但HTTP parser未启用解码；两者均明确拒绝，保留原始RED。随后artifact_roles改为外部明确的`{artifact_role: required_job_role}`映射，同一可信job可产出执行证据与最终包两个artifact，不使用matrix的最后完成输出替代两份身份。最终Windows/WSL各38/38、0失败0跳过，两个独立wire反例在同一冻结源码复核拒绝；见u23-hosted-inputs/freeze-02.json与u23-hosted-review/role-map-review-01.md。单次真实GET200仅证明连接，尚无整个候选来源认证通过结论。

本地包接收层verify_package_bundle.py按调用者外锁manifest摘要、required文件精确集合、source/platform/Release/version及producer上下文校验。每项最终字节必须对应恰好一份原始accepted package-run收据，原生requirements另行绑定，拒绝额外文件、路径逃逸、链接/硬链接、修改和错配。Windows23/23、WSL25/25通过，后者额外执行POSIX符号链接用例；独立审查无发现。包括真实生产端_finish复制到下载目录再接收的文件正控，但Engine/Chrome未在这些夹具测试运行。当前portable收据并未携带其引用的全部static/runtime原始日志，明确返回RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED；这不替代后续托管producer来源认证，也不能冒称原始运行已重放。

U1下载适配verify_execution_bundle.py将外部receipt/profile摘要匹配的原始字节复制到下载目录外的新受控目录，核对完整预选上下文后直接复用verify_release_candidate.verify_evidence(release=True)。Windows/WSL各15/15通过，编排单元正控显式mock U1 verdict，未mock的test-fixture仍拒绝；独立审查接受。之后以真实U22干净候选d7de1b95、执行97a10bd0-d9ba-469b-8abe-6c8e59ccd488的完整windows-debug严格通过证据作不mock正控，得到EXECUTION_BUNDLE_VERIFIED并再次STABLE，见u23-execution/actual-positive-01.json。该证据来自本地真实执行，仍无GitHub来源或发布批准。

固定ID下载器已对真实GitHub artifact10584717700执行API302与不携带Authorization的第二跳，取得4790字节，传输摘要ec4292237d44f4cb469af9922e72fe2350ded5694eec9ec840911c26cf02977b与预锁API值一致，安全解包PREPARED且前后STABLE。该artifact是失败CI的原始ctest日志，只证明下载路径，绝非候选验收。独立真实HTTP测试随后发现IncompleteRead.partial丢失和滴流使单次read超出总时限，修复尚在进行；先前Windows/WSL各8项通过不关闭这些缺口，原失败文件与响应记录保留在u23-download-review/。

U23工作区已快进至U22候选d7de1b95以接收先前共享运行器修复，以上U23源码修改仍未提交。aggregate、六配置严格证据生产、共用工作流、上传前完整重算、版本/标签和真实托管负控制尚未实现完成，新增套件的CTest登记与最终候选全量门禁仍待整合。所有局部返回均保持release_ready=false；受控函数返回的locks后续必须由调用者外锁，不能从下载包自选结果JSON作为可信输入。

## 固定下载、聚合与策略准备追加

下载器两项P2已修复，最终源码6c13f627a6ea84bb345565453670a3d4503578638023df683d8135333e3a4387，测试2319a5a89dace3cdbeb5b9c269333af3a99e6217cff806e2e9c68ca80a8e8863。实际http.client连接由本次调用持有，跨两跳总截止时间通过关闭已持有socket中断响应头/chunk/body读取；保存IncompleteRead中已交付的有效字节，分隔符残片另记framing证据。Windows/WSL各14/14通过，独立wire三例证实缺terminal仍保留152字节、截断CR不混入ZIP、连续滴流在0.1秒预算下实际0.125秒失败并保留40字节。API有认证、blob无认证、watchdog退出。DNS没有可关闭的已持有socket，连接建立仍依赖socket timeout，不承诺任意DNS或连接建立耗时都可强制中断。真实GitHub小artifact的新连接正控发生在最终framing失败分支修改之前，不能写成最终源码的完整托管验收。

aggregate_release_inputs.py连接内存中的受控hosted结果、外锁policy、固定传输ZIP、U22包和U1执行证据；精确核对角色、caller路径/同提交callee、版本/标签与明确上传文件列表。聚合后和上传前复算原传输、展开内容、最终包、执行证据/profile/receipt及源码身份，保留新目录中的首个失败收据。最初3项实际RED为workflow路径、callee SHA和聚合后源码改变未拒绝，修复后通过。独审指出早期正控替换整个execution adapter，不能证明它们互通；增加四锁断言和精准test-fixture拒绝先得到2项RED，再仅替换底层U1 verdict，真实执行adapter、拷贝及所有stable检查。最终Windows/WSL各14/14，生产SHA275da483122a8d438817118ac89657aae787a7df4455ea74f9a138c13beb0bf7，测试50e6ce899310cb1b74efde3a7baee6b8e7afd4486bb586d0149ad6169df9ada4；独审没有生产发现。真实托管身份仍待controller与候选执行。

prepare_release_policy.py在fanout前要求干净且明确的source，冻结版本、完整profile原始字节、CMake和受控模板摘要；模板声明9个required jobs、10个artifact roles、6个execution inputs和4个平台包。准确job全名与平台文件名现为待实际hosted读回的候选预期，不能以模板存在当作来源认证。独审真实clean Git/CLI发现--github-output与policy同路径时会在摘要计算后污染JSON，现写入前拒绝同路径、源码内路径和硬链接。三项RED保留，Windows/WSL各12/12；独立实际CLI四例确认正常输出可由policy_json加LF逐字节恢复，三种冲突均失败且policy未创建/源码不变。最终源码327f1dbb9d2ee694c72640ef5187d5e220496873d97e5c10e2a76b70119fa4b5，见u23-policy/review-02.md。

共用validate-engine及六配置producer正在接线，CI入口显式选PR head并保留workflow commit独立身份；同job可提供独立execution/package输出，不通过matrix最后完成结果混用。CTest已登记8个新维护套件，真实windows-foundation configure后发现53项，据此更新CTest门槛；其中六个冻结接收/策略套件已从实际CTest入口使用配置的Python3.12.9执行，6/6通过。两个新编排套件及完整候选门禁尚待整合。三个caller已改为同一readonly dry-run验证路径，旧自动release/Pages上传步骤不在当前dry-run范围；Pages特有tar传输变换、实际托管运行、AE7取消及调包负控制、服务端check配置/读回和U22最终包仍未完成。这些局部通过不将U23或整个计划标为完成。

两个新编排套件随后也由实际CTest/Python3.12.9执行，2/2通过，故8个新注册入口均有本地执行证据；发现53项不等于全量53项已经通过。ci_execution_lane复用run_profile→collect→verify_evidence严格入口，Windows/WSL各16/16；独审真实clean Git和小进程验证原receipt篡改被拒。6个execution、4个package和LinuxCTest的11处动态诊断上传增加work-env非空条件，早期source guard失败不会展开成根路径。旧133个唯一步骤、展开后147实例已人工核对，原硬门禁保留或由严格profile等价覆盖，仍需真实hosted验证路径。

ci_release_gate.py已接通受控输入、固定ID下载、真实聚合及最终再次托管认证。最终源码e869ceabe114c423c76a2789549f647971ecfa58ad48f01c377850efdfc8884c；Windows/WSL各13/13。正控使用实际临时Git、本地HTTP与ZIP、真实包和执行adapter，只替换最底U1 verdict，fixture不变成GitHub证据；真实未注入成功才返回DRY_RUN_INPUTS_VERIFIED，release_ready仍false。独立6个实际wire检查确认ZIP、execution receipt、aggregate receipt、producer snapshot后改和最终artifact过期均拒绝；恶意把注入transport标github仍保持fixture。当前policy与reusable输出的9jobs/10artifacts/6execution/42flat字段精确匹配。三个caller和共享gate内嵌Python另有21项实际Git正负控通过，五workflow actionlint通过（未启用shellcheck/pyflakes）。真实托管的job名称/outputs/所有平台包及非注入dry-run仍待候选CI；Pages tar变换和实际发布未执行。


## 初次集成冻结

U23 的 windows-foundation 预热全量 Debug 构建退出 0，日志为 artifacts/validation/u23-foundation/warm-build-01.log。该构建仍基于 d7de1b95 且工作区含未提交实现，只用于提前发现编译问题，不作为干净候选验收。六个原生配置的 HTTP 诊断上传路径已接入共享 workflow，Windows 为 build/Debug 或 build/Release 下的实际目录，Linux/macOS 为 build 下目录，保留现有 RPC/stdio/CTest 诊断及非空工作目录守卫。首次实现提交之后仍须整合 U22 新修复，冻结新提交并执行完整候选验证；本记录中的已通过局部测试不提升托管或最终包状态。


## Pages 最终 tar 身份闭合实现

主体实现先本地提交d43bf605，随后整合U22至0785b697并保留共享workflow的HTTP诊断与Windows单一工具路径选择，得到c9ab5858；这不是U23验收提交。新增Pages策略总共9个required jobs、11个artifact roles和45个明确outputs：六份严格执行证据、四个平台包证明以及同Web producer的独立Pages传输。预先指定的artifact.tar必须同时在Web proof bundle中具有自己的一份原accepted收据；Pages专用artifact只传这个已经验收的tar，不能在验收后再调用会重新tar的upload-pages-artifact。通用Release附件列表明确排除Pages tar，单独pages_artifacts计划绑定固定ID、外层ZIP摘要、内层tar摘要及repo/run/attempt/job/source/version，deployment仍NOT_RUN。

plain tar准备层只扩展ZIP/TGZ为ZIP/TGZ/plain TAR，不启用tarfile自动探测其他压缩；沿用路径、链接、重复、限额、digest和失败清理合同。旧实现25方法21失败断言+2错误，修改后Windows/WSL各25/25，0跳过。Pages producer使用冻结site清单逐文件写入一次plain tar，复制时计算实际流SHA，禁止任何链接、硬链接、特殊项，调用同一run_package_validation入口验收最终tar，然后锁定原收据。API/CLI缺Pages的RED保留，最终Windows37/37、WSL38/38（另含POSIX链接/FIFO），0跳过。主代理独审两层增量无发现；这些编排夹具的浏览器边界有替身，不能声称真实Chrome已跑。

独立接收器verify_pages_artifact将实际单tar payload与受控verify_bundle结果、外部manifest SHA匹配，拒绝额外文件/目录、物理或tar内链接/特殊成员、压缩tar与不安全路径。Pages物理tar与声明内容均严格小于10,000,000,000字节；官方1GiB警告不提升成拒绝规则。两份tar、proof原收据、解包内容和布局在后续稳定性检查重新打开。原缺模块13项RED，最终Windows/WSL各15/15、0跳过；主代理独审并联调实际aggregate通过，托管身份仍由上层认证。

策略14/14、聚合17/17通过，Pages三分区/同job与清单/最终字节回归先RED后GREEN。独立审查另执行10项实际文件正负控，确认None/string/bool job ID、三分区缺失/重叠、已刷新传输摘要的调包/额外文件及晚将tar混入Release列表仍拒绝。Pages gate原21方法6失败+2错误，最终Windows/WSL各21/21；实际本地HTTP/Git/ZIP/tar正控与晚改原receipt负控各27次GET、77份原始文件，只有最低U1 verdict被替换；取消替换仍准确拒绝test-fixture。Pages plan仅在最后再次托管查询与全部字节重核后输出，fixture始终为FIXTURE_INPUTS_VERIFIED、CLI77、release_ready=false。

CMake实际重新配置发现54个CTest入口，六profile的最低发现数由此从53更新为54；九个新增release维护入口已由真实CTest/Python3.12.9执行，9/9通过、0跳过、64.16秒，见u23-pages-integration/ctest-nine-01.log与XML。这不是全量54项验收。五workflow actionlint通过（未启用shellcheck/pyflakes）。独立审查与原始失败分别保存在u23-pages-tar、u23-pages-producer、u23-pages-receiver、u23-pages-integration-review和u23-gate-pages等目录，测试通过数不代表覆盖率。

真实候选完整Debug/C++/Lua/CTest、最终tar的实际Web浏览器验收、11个托管artifact与job名称/outputs读回、非注入dry-run及AE7托管取消/调包负控仍待执行。U22新候选0785b697本地严格windows-debug通过，但run35451773753的Mac最终TGZ验收失败正在诊断；不得把本地或维护夹具通过改写成U22、U23或整个U1–U29计划完成。Pages服务接受及真正部署不在本次dry-run内。

Gate Pages独审随后完成，无可行动发现：45字段与workflow精确一致，47项缺字段/额外字段/重复ID负控均拒绝；新增真实wire晚增空目录在preupload拒绝（27GET），最终Pages digest改变在hosted-final拒绝（26GET），均无Pages plan或upload_files。作者测试与独审源码hash一致，证据u23-pages-gate-review/review-01.md。

## 2026-09-20 首次托管结果与包修复集成

干净候选79e8b439的本地完整windows-debug通过：C++1408/1408、403021条断言、Lua147/147与56/56，CTest54项中53通过、1项预声明AI服务跳过，runner/collector/strict verifier均退出0。原始run为5f92871a-17a1-41b2-87bf-0a87cc7fb5ad，run.json摘要6a16ac97aeb69bd86b3f5a2d82ffbf9d697b8a7431fcd8567de6cf5185a6ba85。该候选的本地真实Web producer在Node打包输出路径检查失败，未生成最终Pages tar，也未启动Chrome；失败目录和日志保留，不能将此前夹具通过当作实际tar验收。

首次托管run35452996878/attempt1已失败结束。执行源码为79e8b439，caller/callee workflow为5016925176d01a90ae4a6ce4d898081afd14aacf。九项required jobs实际4成功、3失败、2跳过：Linux Web性能625/626（比例2.5302350980073647超过固定2.5阈值）；macOS最终TGZ因已消失的系统plist-cache映射观察失败；Windows最终ZIP的60帧阶段在SoLoud初始化错误后超时。macOS/Windows的原始静态、运行、进程与日志摘要已逐层核对，不将同类旧失败中的具体Lua错误文本套用到本次。

实际aggregate名称为Verify release inputs / Verify exact release inputs，job105930144140、GitHub Actions App15368。下载的producer-outputs.json共45字段，25个非空、20个为空，2731字节、SHA256为10887f33a2e45fbb829d58d87696fb47540c8a58b05abea4dcdfce6144e6501d。gate在inputs阶段拒绝首个空artifact ID，尚未执行required-job来源认证、下载或聚合。此结果只证明缺输入被真实入口拒绝，不关闭AE7的托管取消/调包负控。原始gate artifact10588092016的传输摘要36fa7c62dafd3a6ccc0e029bedd03f2a07e17c2449ff0d24d1434efc400dbdde；完整审计见artifacts/validation/u23-hosted-79e8b439/audit-handoff-03.md。服务端仍未改动。

merge699f1951整合U22候选effc6b2a的macOS映射观察、Web源内独占暂存/仓外逐字节复制和显式SoLoud软件混音。U23的caller、v2收据、Pages最终tar及上传闭包保留。Windows lane50/50、WSL51/51、release gate21/21通过，五workflow actionlint通过；两边测试方法37与34的并集恰为50，无丢失或重名覆盖。独立merge审查无发现，见u23-package-fix-integration/review-01.md。实际CMake discovery为55，六profile据此从54增至55，C++最低发现数各增加6。

被整合的U22干净effc6b2a已有完整Debug、C++1414/1414与427080条断言、Lua147/56、CTest45通过/1预声明跳过，run0c1f2b69-e122-4579-8c23-716ef7e9e3fa及严格收集/验证均通过。该证据只属于U22原候选，不能替代新U23候选的完整55项验证。U22托管run35455884122仍有macOS最终包新失败待诊断。新U23完整门禁、真实目录/ZIP/Pages tar Chrome、全部托管输入和非注入dry-run仍未完成；物理音频、设备及部署不由软件混音证明。平台YAML仅同步review anchor，逐项历史执行证据不提升。
