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
