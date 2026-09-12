# U18 运行错误与 RPC 退出、超时执行记录

日期：2026-09-12。依据唯一迭代计划U18，依赖U6/U10；底层优先，Studio暂停。当前在独立工作区的 `codex/u18-runtime-errors`，起点为U17候选 `4a7487e0306dcf9c1450929dc543f4174ed9d5c9`，已快进到同树合并提交 `0c7e2e86cb5647e461d8a4308dbde4b431443f1f`。U17的PR #20已在最终CI通过后合并；U18实现、定向回归与独立审查正在进行，完整验证和交付尚未完成。本记录不代表U1–U29完成。

## 行为合同与实施范围

- 非owner请求继续只携带拥有值的RPC数据，Lua/GPU操作只在owner执行。沿用IRpcDispatcher、现有组合根和HTTP/stdio入口。
- 内部pending使用Queued、Running、Completed、Cancelled状态。等待超时与Queued晋升Running共用一个同步判定；已经取到局部batch的Queued请求也可被取消，取消后不得执行。关闭停止接收并唤醒等待者，Stop建立对同批后续请求的关闭边界。
- 已进入Running的请求超时只表示结果未知，不能宣称撤销；沿用status/code/message回复封装表达差异。内部请求ID关联开始、超时、完成或失败记录，只记录ID、操作类型、阶段、结果码和耗时，不记录任意请求正文、脚本、eval内容或认证值。没有外部终态查询、持久账本或幂等协议。
- 默认KAG错误处理停止当前命令批次，ErrorUI请求退出后不能继续执行后续命令。已存在的自定义错误处理器保留明确的恢复控制权；缺资源、脚本异常和失败的错误处理链都要有可定位结果。不能将失败执行输出成无条件成功。
- 瞬时engine_update异常仍有恢复机会；同一连续失败阶段只输出有限条详细错误和必要恢复摘要，初始上限锁定为3条，变化的错误内容不能绕过总量限制。严重内存错误走安全停止边界，不能继续逐帧重试。
- stdio关闭要覆盖输出背压，owner不承担阻塞写；退出测试必须把测试PID兜底终止算作失败。仅维护已有客户端的错误传递与无自动重试合同，不增加Studio功能。

预期修改范围为 `src/entry/`、`src/main.cpp`、`src/rpc/`、`scripts/kag_runner.lua`、`scripts/scheduler.lua`、`scripts/kag/commands/save.lua`、`scripts/kag/save_state.lua`、`scripts/kag/commands/text.lua` 与相应C++/Lua/真实进程测试。若内部pending同步从main提取到entry辅助组件，必须接回同一生产路径再测试，不能测试复制出来的替身状态机。公共接口仅在现有回复封装不足时修改，并同步所有实现和注册；不增加并行RPC框架。

## 已观察到的事实与红灯

以下是实施前运行于U17 Debug引擎的历史红灯，运行前后二进制hash一致；文件位于主工作区 `artifacts/validation/u18-rpc-repro/`。对应后续修复与证据见本记录后半部分。

| 路径 | 实际结果 | 边界 |
| --- | --- | --- |
| `queued-timeout.py` / `red-01` | 正常请求ping/eval返回42、变更一次、自然退出；owner stall为5000ms、等待预算100ms时，收到engine_busy后仍执行一次变更。有限1帧进程自然退出，取消后不执行的断言失败 | 使用现有诊断stall作真实进程初始复现；正式同步边界仍须明确barrier回归，不能靠竞速重跑 |
| `error-boundaries.py` / `error-red-03` 的frame-error | 实际engine_update连续20次报错输出20条详细日志，第21次恢复并自然退出，日志数量上限断言失败 | 真实Engine/Lua帧循环；没有伪造日志或禁用回调 |
| 同批command-positive / command-fatal | 正常命令后的计数为1；实际runner的一次on_click在错误进入Engine Runtime Error链后仍执行下一命令，计数为1，停止断言失败；均自然退出 | 仅场景加载由夹具提供实际tokenizer解析结果；runner、scheduler、KAG默认错误处理、原生绑定和ErrorUI均为生产路径 |

`error-red-01`因读取混合编码stderr失败，属于夹具问题；原始日志未改。`error-red-02`对runner.update的正常waiting-input返回作了错误断言，两个命令场景没有进入待测行为，其中一个进程按超时被兜底终止并记失败；这些不能作为错误停止缺陷的红灯。修正夹具后才得到上述 `error-red-03` 真实行为证据。

只读审查 `artifacts/validation/u18-rpc-review/report.md` 还确认：HTTP Stop仅设置engine退出，局部batch仍继续；Running超时与Queued共用“未服务”消息，晚终态缺少ID关联；stdio写满管道不在现有读取消范围内。它们仍需要动态回归。已审现有客户端没有超时后自动重发变更；evalRaw会丢弃非2xx正文，需要维护其错误可观察性。启动时既有认证引导输出与新增请求诊断分别对待，不能未经处理就声称整个旧进程日志不含token。

## 实施与验收顺序

1. 建立实际pending组件的确定性Queued/Running/取消/关闭回归：包含进入局部batch后取消、[Stop,变更]、重复close、异常与新请求正控制。
2. 接回main，保持每项恰好一次终态；真实HTTP/stdio验证取消不执行、Running结果未知和晚到结果关联。断开和输出背压使用有界reader/writer及明确PID回收。
3. 为默认KAG错误停止与帧错误限流建立正式回归，保留自定义恢复、正常命令、瞬时异常恢复和严重错误停止的正负控制。
4. 独立审查实质增量；运行受影响定向检查，再固定当前源码执行完整Debug、全部C++、Lua主/隔离入口、CTest、受影响Web和跨平台CI。注册计数由发现结果产生，覆盖率未测量。

开始实施时已完成只读核对、真实初始红灯与独立工作区配置；以下继续记录实现结果，最终验证和交付仍单独验收。

## U18错误边界定向进度

独立工作区首次完整Debug构建通过。正式tests/headless_error_smoke.py在旧实现上得到真实红灯u18-error-native-red-01：20条帧详细日志、默认命令失败后下一命令仍执行1次且click返回成功；正常命令与自然退出正控制通过。修复后u18-error-native-green-02的3个真实进程全部通过：20次失败仅3条详细日志；恢复后第二阶段2次失败正常输出2条，并分别记录20/2次恢复摘要；致命命令后计数为0、click返回command-error，正常命令后计数1。全部自然退出，未用兜底kill取得通过。

实际Lua allocator拒绝engine_update内的表分配后，旧实现将LUA_ERRMEM当成普通异常，随后未保护的Lua状态发布发生panic/SIGABRT（u18-error-oom-red-01）。修复在错误处停止后续帧Lua访问，u18-error-oom-green-01为1项6断言通过。该测试在清理前恢复allocator，不证明持续内存耗尽下整个shutdown、所有其他Lua入口或重新run的安全性。

Lua错误回归的初始正确红灯为u18-error-lua-red-02（25通过/6失败，独立重置计数后的自定义恢复及正常会话正控制通过）。默认处理器先停止当前会话，再进行原生错误报告；runner返回command-error并保留命令及实际token index。独立审查又复现异常对象__tostring再次抛错、成功rollback后错误锁存未清两项问题，实际红灯format为37/4、rollback为41/6；受保护格式化和成功恢复清锁存后u18-error-review-green-01为47/47。诊断位置是token index，不能称作物理源代码行。恢复候选重新安装绑定新ctx的默认handler，自定义处理器仍按既有约定保留。

## U18排队与退出定向进度

生产OwnerRpcQueue从main提取到既有CaesuraEntry静态库，保留旧行为的提取版先执行同一生产路径回归。首次测试源少一个namespace闭括号，修正语法后u18-queue-red-01实际8项中4失败、107断言中17失败；主线程正控制、普通关闭、有限batch和日志边界控制通过。提取版真实进程u18-queue-process-red-01仍收到engine_busy后执行1次变更，保持自然有限帧退出。

候选使用同一mutex仲裁Queued/Running/Completed/Cancelled，未开始的batch尾仍可取消，取消释放内部请求数据；Running超时返回result_unknown并保留同ID晚终态，Stop关闭同批后续请求。原8项107断言在u18-queue-green-01全部通过；真实进程u18-queue-process-green-01收到request_cancelled且变更次数为0，正常请求仍返回42并变更1次，均自然退出。这里仅验证dispatch命令终态，managed coroutine脚本终态关联及完整HTTP/stdio兼容仍继续。

stdio输出背压探针u18-stdio-backpressure-red-01已有真实失败：正常消费者收到完整1MiB响应，约3.06秒自然退出；消费者在ping后明确暂停读取，owner请求已接收并完成，但8秒内无法退出。解除背压后进程自然退出，没有强杀；迟到退出不能改写超时失败。原RpcServer同步cout与Engine自身stdout日志共享管道，修复需覆盖整个退出路径，不能只取消stdin。

## 输出取消与客户端错误传递增量

`StdioRpcOutput`为stdio协议保留独立描述符，用单个worker串行输出完整JSON行；待发送数据按32MiB和256项限制，计入正在写的行。producer只入队，输出容量满/读端关闭会停止输入。关闭先允许当前handler提交最终响应，再给健康消费者1秒排空；Windows重复取消同步写以覆盖检查到进入WriteFile的窗口，POSIX使用非阻塞写、poll和线程内SIGPIPE处理。main只在stdio模式保留启动banner后把后续stdout转到stderr，持续到进程退出；协议副本创建或重定向失败则在读取请求前拒绝启动。

`u18-output-pipe-green-01`为5项288断言通过，覆盖真实管道大响应顺序、读端暂停后的取消、部分行后不追加尾行、容量限制和读端关闭。`u18-stdio-backpressure-green-01`中，正常消费者完整收到1MiB后约3.062秒自然退出；暂停消费者没有恢复读取也约4.063秒自然退出，没有兜底终止。对应正式CTest为CaesuraStdioBackpressure。这里只证明启动已就绪后stdout不再被消费、stderr仍独立排空的情形；不证明启动第一字节起两流均无人读取或合并管道的退出。

独立审查还发现测试混用stdout/stderr队列会接受错流JSON、丢失早到恢复marker，已分为协议与诊断各自队列，保留原断言；输出副本不可用时的前置拒绝也已补测试。失败observer抛异常的真实闭管道回归`u18-output-observer-red-01`触发SIGABRT，修复只隔离通知回调；后续u18-output-review-green-01为7项297断言通过，含输出不可用时不读取、不派发的启动负例。已有headless_rpc_smoke也改为从启动起使用一个有界等待reader、按请求ID匹配，并将EOF后兜底kill记为失败。

客户端改动限于既有EngineClient错误传递：以一次body消费保存JSON或原始文本，让RpcError携带HTTP状态、code、message和body，没有新增重试。真实Response回归`u18-client-error-red-01`为5失败/58通过，修复后`u18-client-error-green-01`为63/63；`u18-client-full-01`为38文件636/636，typecheck通过。测试验证eval/run/reload/stop每次失败只发1次请求，显式新调用仍可成功；这是客户端函数测试，不是浏览器端到端证据。

## 两种传输的真实超时与晚终态

正式`headless_rpc_timeout_smoke.py`用实际引擎HTTP/stdio分别运行Queued取消、Running晚成功与晚失败；沿用组件barrier的竞态证据，不将进程计时当作原子仲裁证明。初次`u18-rpc-timeout-process-01`的Queued两项通过；Running夹具尝试创建锁定环境禁止的新全局，导致延迟后的断言失败，不能当作队列实现红灯。改用既有kag模块拥有的夹具计数后，`u18-rpc-timeout-process-02`六个进程均通过、自然退出，二进制运行前后hash一致。

Queued回复request_cancelled，日志从未started，变更marker为0。Running回复result_unknown后有同ID恰好一条completed，分别是成功或eval_error；新请求读取计数为1，证明没有隐式重发或宣称回滚。HTTP状态为503，stdio沿用错误封装；两者最后Stop应答及自然退出都通过。新关联诊断只含ID、操作、阶段、状态码与耗时，不含夹具正文或测试认证token；原HTTP启动引导输出是另外的已知日志范围。

## 结果格式化、异步脚本终态与恢复补查

`u18-managed-process-red-01`使用保存的E10433BA2E4492AAA8503199A10138777312D035764E6B9831F6842CE41D891B引擎运行7个实际场景，全部失败且无强制终止。三个结果格式化场景走到未保护的Lua PANIC；其余managed run真实yield、关闭一次和新请求正控制通过，但没有可关联的终态，失败时还输出任意错误正文。HTTP在脚本仍yield时记录completed。原探针仅检查非空错误code可能把result_unknown计为格式化处理，已收紧为专用格式化错误码，原始RED不改写。

新增Lua C trampoline在pcall内执行luaL_tolstring；C回调只持有平凡局部变量，格式化自身报错时用既有只读取字符串的copyCoroutineError，避免再次调用__tostring。外层保存并恢复Lua栈；eval与KAG调试分别返回eval_result_error和kag_debug_result_error。ManagedRun继续使用原有create/resume/close helper，保存原owner请求ID，按accepted/completed/failed/cancelled和固定code记录终态、Lua状态、关闭状态与耗时。Queue的dispatch完成仍仅表示接受run，HTTP日志改为submitted。

`u18-managed-process-green-01`七个进程均通过、自然退出、二进制hash稳定：正常__tostring仍支持，抛错后能继续执行新请求；run晚成功、失败和Stop取消分别有同ID唯一终态，实际close守卫恰好一次，日志没有正文哨兵。`u18-queue-context-green-01`九项122断言通过，包含嵌套owner请求ID恢复与foreign线程返回0。对应正式CTest为CaesuraManagedRpcSmoke。该组不冒充OOM fault injection或所有关闭失败路径覆盖；独立审查已确认指定增量没有可操作缺陷。

完整恢复补查使用实际save.capture_state、save_state.prepare和runner.restore_candidate，发现恢复[p]页等待后第一次点击只重新进入[p]，第二次才继续。`u18-error-restore-green-01`虽命名为green但实际60通过/3失败，保留原日志；observation-01记录第一次点击返回true/1且仍waiting_input，未触发后续命令。原因是视觉快照恢复waiting_input，阻止新的scheduler协程在update中进入[p]。修复在提交恢复后重建该[p]的暂停位置，保留页面到实际点击后才清除；`u18-error-restore-green-02`为63/63，默认处理器停止新ctx且不把旧ctx标成错误，自定义恢复策略仍继续。该Lua增量的独立审查及完整套件继续验收。

`u18-stdio-disconnect-green-01`补充真实客户端在owner已started后关闭stdout的场景：变更仍实际执行一次、同ID记录一次late completion，输出层识别断开，进程在关闭读端后约0.640秒自然退出。另两个健康1MiB及暂停读取正负场景同时通过；三个进程均没有强杀。该结果不承诺撤销已开始的操作。

当前Windows C++已实际发现1378项（原1361加17），CTest新增Error/Backpressure/Timeout/Managed四个真实入口，最低发现门槛同步到32；其他平台只同步新增源码应有的门槛，等待其CI实际发现。源码尚未宣称通过完整候选批次；覆盖率未测量。

## 页等待恢复的审查修正

初版仅依据waiting_input和下一token为p而立即prime，独立审查发现两个歧义：已经完成[ch]但尚未执行[p]，以及调试暂停拒绝执行。`u18-restored-page-review-red-01`实际73通过/4失败，分别观察到恢复后提前清页/推进、以及新ctx已提交却返回失败。最终使用保存的可选布尔字段resume_page_wait区分实际[p]暂停；prepare校验类型、对应p游标和视觉等待，再建立资源候选。缺少字段的旧存档保持保守等待，不猜测已执行页命令；这可能保留旧槽需要再次点击的既有行为，但不会跳过页面。

恢复提交不再提前执行脚本。update或接受点击前，以原ctx/co为约束重建等待；标记由TextCommands.p实际到达等待位置时消费，经过KAG/原生调试暂停仍可保留。暂停状态重新保存保留身份，取消、reload和跳转清除过期标记；pause probe若嵌套发布新ctx，旧prime不能推进新协程。对应`u18-restored-page-boundary-green-01`为96/96，包含两种游标、暂停重存、非法标记、旧槽、嵌套恢复和新点击正控制。

实际KAG断点补测发现既有debug_resume同名覆盖：前面的场景继续函数被后面的原生DebugProtocol已恢复通知覆盖，RPC会报告OK但仍停在断点。保留原生通知不重复推进的合同，将场景入口命名为continue_scene_debugger并接回同一个kagDebugContinue RPC。`u18-kag-continue-red-01`中实际响应OK，但状态为true:false:0，点击也不继续，进程自然退出；修复后的真实进程验证仍待构建。Lua同路径最新`u18-final-test_errorui_wiring-02`104/104、`u18-final-test_save_restore_transaction-02`316/316、`u18-final-test_rollback_session-02`82/82通过，包含KAG断点前保留标记、继续后实际p入口才消费及下一次点击推进。


## 冻结前定向结果

第一次点击的pause probe也可嵌套替换ctx。补查此前只在prime内部第二次probe覆盖该情形；将同一真实fixture覆盖第一次和第二次probe后，u18-first-click-owner-red-01为109通过/3失败。on_click现在在首次probe之前保存ctx/co并在其后核对，过期点击返回click-owner-expired；u18-first-click-owner-green-01为112/112，替换后的会话仅由新点击推进。

u18-managed-formal-green-01包含新增实际kagDebugContinue的8个进程场景，全部通过、自然退出、无强制终止。此前被覆盖的场景继续路径现在返回false:true:0的正确页等待状态，随后单次点击执行后续命令一次。该二进制已包括页等待标记和场景调试入口；最后三行click-owner guard是之后的Lua增量，完整批次将重新同步并覆盖。

当前实际CTest发现32项，注册检查为196个Lua文件、91个C++文件都有执行入口；模块索引88项与计划事实生成检查通过，能力矩阵已用原生成器同步输入指纹与位置。这些静态计数不是覆盖率，也不代替随后的完整候选执行。

## 首轮完整门禁与合同衔接

冻结提交c906ed1f78ae2aebb8fdfd07f225d928aa1b8712的`u18-native-candidate-01`完整执行结束；run_id为621e8fba-3750-43dd-acb5-7311ec9edeab。Debug构建、1378/1378 C++（402148断言、0失败0跳过）、Lua主套件147/147和验证器自身检查通过。Lua隔离套件47/48、coupling及CTest失败（29通过、2失败、1项profile预声明的可选AI跳过）；collector保存FAIL，严格verifier拒绝这批证据。原始日志和失败bundle保留，不作为成功候选使用。

Lua失败来自U17语音查询异常测试：旧测试在默认handler后要求继续剧情，与U18默认严重命令错误停止合同冲突。显式自定义成功handler现在承接原有后续到页、等待、停止次数、无重复推进及poll清理的全部断言；另增加默认handler返回command-error、保留位置、关闭执行、不能被后续帧或点击推进、显式stop释放音频一次及新会话健康正控制。`u18-voice-error-contract-01`实际67/67，不减少用例或恢复默认静默继续。

coupling发现entry因OwnerRpcQueue增加rpc依赖而达到15/14。最终将同一队列实现归入`src/rpc/OwnerRpcQueue.h/.cpp`和既有CaesuraRpc静态库，仍仅由main组合根创建；不改变队列算法或公共接口。`u18-coupling-correction-01`为entry14/14、rpc2/4并通过，迁移后的完整Debug构建通过；同一队列定向回归9项122断言通过。

两项CTest失败由本工作区首次CMake发现`C:/Windows/System32/bash.exe`（WSL启动器）造成，它不能以Git Bash方式解释含括号的Windows脚本路径。仅将本地CAESURA_BASH_PROGRAM配置为已安装的`C:/Program Files/Git/bin/bash.exe`，未跳过或修改用例；`u18-bash-tests-01`中Golden VN和ValidationOutputPaths两项通过。完整批次将重新固定源码及配置执行，不能把这些定向结果拼作首轮全绿。

## 第二轮完整原生结果与Web边界

源码2552e225e99da3999270b38cd94ae1fc6b5388fe的`u18-native-candidate-02`完整profile、collector与严格verifier均通过，run_id为17b2547f-2f0c-448a-ba53-bd7daa8be31c。实际Debug构建、C++1378/1378（402148断言、0失败0跳过）、Lua主147/147、隔离48/48，以及验证工具17/7/53/57、coupling、注册检查全部通过。CTest发现32项、31通过、0失败；CaesuraHeadlessAiSmoke是profile此前已声明的唯一可选跳过。dirty=false，源码及夹具运行期间均未变化。此结论仅为该windows-debug范围，不代表发布或其他平台。

同一源码的`u18-web-full-01`先用当前Lua编译24个demo场景/6资产，再用Node22.23.2构建Vite和完整Web套件。38文件中37通过、1失败；511项中510通过、1失败、0跳过。唯一失败为perf-baseline中带history的1000行场景，median约2776.4ms，frames/ms约1.4407低于既定2门槛；没有放宽断言或原样重跑取绿。源码、Node和Lua的hash在运行前后保持一致。其他Web功能检查通过，但本地完整Web结果仍是FAIL，须与后续CI性能结果分别记录；U27长跑/性能总目标仍未验收。

错误及RPC/输出/恢复增量的独立审查已经收束，未发现需要生产修复的剩余问题。最后补充语音错误presentation保留/显式stop销毁两个时点断言；这仅加强测试，不改变2552e225的生产源码。该测试增量的Lua证据与后续最终候选CI另行记录，不把旧完整receipt重新标成新SHA。

该测试增量提交为10a6d9fcd08ebb512f0ddda6ff628c06f6cbddf2，`u18-voice-error-contract-02`为69/69；`u18-final-orphan-increment-01`为完整隔离套件48/48。平台状态YAML只更新源码新鲜度锚点，不修改任何历史平台状态、设备证据或时间。跨平台CI尚待执行，U18仍未合并。

## PR #21 首轮跨平台失败与有界修正（2026-09-12）

CI 34702706010、head e91d15a9257d1945030b7767fc070edc1a1a8415的macOS Test有两项失败，保持失败记录，不以本地Windows通过代替。原始job日志保存在artifacts/validation/u18-macos-ci-job-01.log。

- CaesuraStdioBackpressure的disconnected-running：变更执行一次、迟到终态一次、检测到断输出均通过，但进程exit=-13（SIGPIPE），自然退出断言失败。Apple [XNU fp_writev](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_generic.c)在管道EPIPE分支调用进程级psignal；现有writer线程的pthread_sigmask不能阻止信号投递给其他线程。按Apple [fcntl说明](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/fcntl.2)使用F_SETNOSIGPIPE只抑制此协议流信号，保留EPIPE错误和既有失败通知，不修改全局signal处理。因dup共享open-file状态，构造保存原值，正常析构恢复；标志查询/设置失败时恢复O_NONBLOCK并拒绝该输出。既有关闭消费者用例补原值0/1及析构恢复断言；真正的macOS进程通过证据仍待新CI。
- CaesuraRpcTimeoutSmoke的http-late-success：最初result_unknown、变更一次及新eval均通过，随后stop_acknowledged=false，等待10秒后仍存活，最终清理强制终止。旧报告丢弃了stop响应及其事件，不能确定是100ms测试超时导致Queued取消还是其他问题。补保存stop HTTP状态、响应及全部op=stop事件；100ms、单次stop、ACK与自然退出断言不变。macOS失败时CI保存原始进程报告/日志，避免只剩过滤后的eval事件。没有声称HTTP根因已修复，也没有改测试为GPU替身或放宽阈值。

修正后本地完整Debug构建u18-darwin-output-build-01退出0，Stdio定向6例/294断言通过（1372未选中）；这是Windows路径，Apple-only分支尚未执行。原生2552e225整批通过凭证仍只代表该源码，不能改写成此次Darwin修正后的凭证。

同一构建的Windows真实进程增量也通过：u18-darwin-output-windows-process-01为3/3（包括1MiB完整回包、暂停消费者、Running后断开），u18-timeout-observation-windows-01为6/6；共9个本轮PID自然exit0，无forced termination，二进制前后hash稳定。HTTP两项late用例的stop状态均200、正文status=ok。这只增加Windows路径证据，仍不解释上轮macOS的stop失败。

首轮CI最终为Linux GCC、Windows Debug/Release、Android静态/交叉编译包、iOS编译均成功，macOS失败；3个发布包job按PR事件规则跳过。该CI整体FAIL，不满足合并门禁。
