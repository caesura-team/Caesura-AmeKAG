# U22 最终包隔离验证执行记录

开始：2026-09-13。沿用当前计划 U22，底层优先、Studio 暂停。工作分支先整合 U21 候选 4c007407；U21 的最终完整门禁与合并结果仍独立跟踪，不能因下游准备开始而宣布上游完成。

## 合同与实现顺序

调用者必须指定最终分发文件或目录，以及在验证前固定的 archive SHA256 或目录 inventory SHA256。签名、staple、压缩等改变分发字节的操作必须在该身份之前完成；后置变换重新产生独立验收。验证器不按 mtime、glob 第一项或包内自报清单决定接受哪个包。

1. `scripts/package_verification.py` 负责新尝试目录、独立解包、路径与链接边界、完整内容 inventory 和前后稳定性。ZIP/TGZ 的 archive 摘要与解包内容摘要分开；这层只输出 PREPARED/STABLE，runtime=NOT_RUN。
2. 新 Web 与 Native 静态检查器读取最终副本，检查当前安装/打包合同中的必需文件、入口和依赖。验证工具 Python、Lua、Node、浏览器使用显式路径并记录身份；不能从源码仓库补齐包内资源。动态媒体、可选能力与未运行平台保持单列。
3. 运行层从仓外新目录启动明确的本包程序，以受控环境、外部超时、本轮进程与端口身份验证 readiness。原生保留既有编辑器静态兼容、认证和包内 create/build/run 路径；Web 保留普通玩家启动、真实点击解锁、保存、根/子路径与离线检查。进程/端口关闭必须实际确认，不能将同名程序或别的监听者当本轮程序。
4. 顶层 receipt 绑定精确源码、平台配置、最终包身份、静态与运行结果、原始日志摘要和回收结果。既有 shell 入口与 CI 改为消费显式产物；Windows ZIP、Linux TGZ/AppImage、macOS TGZ/DMG、Web 各最终格式分别记录，不能彼此替代。
5. 负控制包括损坏包、缺 DLL/Lua/模板/WASM/所引用 chunk/静态资源、坏入口、路径逃逸、占用端口、提前退出与超时。只变异本轮副本，保留原包、首个失败及无关进程。U23 以后再从受控 workflow/run/attempt 与固定 artifact ID 消费这些结果，U22 本身不授予发布批准。

主代理负责顶层编排、进程管理、构建与工作流整合；Web 与 Native 静态检查分配给不重叠的新文件。生产公共接口不在本切片内调整。若实际运行暴露源码缺陷，先保存真实失败，再做有界回归和修复。

## 已验证的准备层

`package_verification.py` 和 `test_release_package_contract.py` 先完成 18 项文件系统合同。独立审查随后复现：在后一个文件完成摘要时等长修改先前已摘要的文件，末尾扫描可能误报 STABLE。新回归先失败，修复保存初始文件 stat 签名并在最终扫描比较；序列化 inventory 和内容摘要格式保持不变。单次完整测试 19/19 通过、0 skipped，独立增量审查接受。

实现 SHA256 为 `10205bfa9b10038f3488b515dce17f1cefea874a06b78aba64b8093ab6df18d8`，测试为 `308a7c4616e69e153d7cfd0075b45bc5b4d9356c8e3894a2d45956b1f6fa42f1`；证据位于本工作树 `artifacts/validation/u22-foundation/`。Linux SONAME、macOS framework 相对链接与 tar hardlink 的格式用例在 Windows 主机实际创建并检查，不等于对应 POSIX 主机的权限、动态加载或窗口运行证明。

当前没有最终平台包验收通过结论。静态检查器、运行编排、工作流与各平台实际最终产物继续实施；所有尚未执行的运行范围保持 NOT_RUN，覆盖率未测量。

## 集成追加证据（2026-09-13）

U21最终文档提交9731fbaf的CI34733640509七项执行任务全部成功，三项PR包按条件跳过；PR24已合并65e5b42545585c6cc7e5a172b0b1453a116def50。主工作树和本U22工作树均已同步该基线，U21合并后的CI34734686509仍独立跟踪。U21原生严格收据、608项Web、107项CLI与两模板真实作者验收见其执行记录，不重跑未变化范围，也不将它们升级为本U22最终包证明。

两个静态检查器实际读取含.exe/.bat的包时，共同复现Windows路径stat会按扩展名推断执行位而fstat不会。准备层_signature仅在Windows比较时屏蔽0o111，保留文件类型/读写位/dev/ino/大小/mtime，POSIX比较不变。新增真实扩展名文件复制与修改拒绝回归先失败，完整准备层20/20通过；独立增量审查接受。源码SHA为eeb6e63eff416b7bd3c30d1f946d8a8933e9c940c77df6383b13affa4b068624，历史19项结果和新red-windows-executable-mode-01/green-windows-executable-mode-full-01均保留。

Native静态检查先15项缺实现RED，现有有效配置、必需Lua/脚本/模板/字体/编辑器引用、PE/ELF/Mach基本签名与包内库路径检查；独立审查发现HTML的base href可把相对JS指向站外，四种真实变异先失败后统一拒绝，最终22/22通过。Web静态检查使用显式宿主Lua在空数据环境读取实际bundle、复用受信compiler/collector，记录实际送入Lua的源码字节并比较47个验证模块摘要；17/17通过，包括中文嵌套入口、manifest相对URL、逐组件精确大小写、Lua时间/指令边界和必需资源负控制。两类静态测试均未运行引擎/浏览器；PWA安装、动态媒体及运行行为仍未由静态结果证明。

最终包检查新增PWA清单递归后，U21 prepared-06两包实际STATIC_FAIL：Vite将manifest移到web-assets，但其start_url和icons仍相对该错误目录。原静态范围的STATIC_PASS和扩展后的失败分别保留在u22-web-static/actual-packages-02与actual-manifest-fail-01。生产构建现在通过post HTML钩子引用与index同层的manifest；实际资源回归从5通过/1失败到6/6。修复后的两份作者ZIP均重新生成，basic archive SHA为13d235386568826484f73121cd9ce2a1b5c4828d1c5c90ffdb582988684d454b，kag3为13ab3ec7414c2400a31a29695afe42ff93e16af805ab6f570d9c65f7b8d2c615。按这两个预先记录摘要解压到仓外新目录，两包完整STATIC_PASS、原ZIP与解包内容STABLE，runtime=NOT_RUN。报告为artifacts/validation/u22-pwa/isolated-static-01，不能将此描述为浏览器安装/离线已验收。

运行工具使用明确环境、PID/创建时间/映像/TCP归属与现有owned runner。新真实超时测试发现Windows Job ActiveProcesses归零早于实际进程句柄signaled；32个后代在旧API返回时均未退出。现在cleanup先封住新成员创建，枚举本Job并持有经归属核对的句柄，终止后按共同截止等待全部句柄。进程套件11/11、上层工具17/17通过，源码前后一致；独立审查的32成员自然退出屏障未复现遗漏。原失败、完整日志、PID重用无关进程存活与异常范围见u22-process-reap和u22-runtime；POSIX实现未在此次Windows修复中修改或实跑。两个Windows profile的进程最低发现数7提升为11，其他平台保持6。

CMake现生成包外package-requirements-<config>.json，包含确定CPack名称、真实SDL目标名/SONAME和有效SDK要求，不安装到包里供包自降要求。纯CMake生成回归经宿主nmake/VS发现前提修正后真实缺模块RED，再3/3通过；当前Windows独立配置实际生成Debug/Release要求，完整Debug构建成功。Release构建与最终ZIP仍在进行，既有编辑器TypeScript/生产静态构建已成功，不恢复Studio功能开发。

顶层run_package_validation.py绑定预先选择的源码/包/外部要求/宿主工具，依次准备、静态、实际运行、前后稳定性并保存新receipt。首9项真实归档边界回归通过；自审追加输入目录重叠的真实RED后，将拒绝前移到mkdir之前，现10/10通过。这里静态/运行阶段是明确单元测试边界替身，不是实际Engine验收；dirty证据只返回DIAGNOSTIC_PASS且accepted=false。六个桌面profile的CTest最低发现数33提升到39，新增六个维护套件已注册，后续实际运行器新增测试仍待接入。

Native运行器与Web服务/浏览器控制层继续整合，尚无本U22最终平台包RUNTIME_PASS。纯CDP探针最新11项协议测试通过，但先前final-01三项在首命令前失败，原临时report已被测试清理，剩余TAP不足以定位原因；这个首FAIL与证据缺口明确保持未闭合，不通过重跑取绿，下一步以保留完整证据的实际Chrome正控定位。DMG/AppImage最终容器路径、CI/release/deploy接线与全候选门禁尚未完成。整个U1–U29目标继续保留。

## 实际包运行与审查修复追加（2026-09-13 13:40）

U21 合并后的 CI 34734686509 已完成，精确 head 为 65e5b42545585c6cc7e5a172b0b1453a116def50，10 个任务全部 success，原始结果保存为 artifacts/validation/u21-ci-34734686509-final.json。U22 的完整 Release 构建也已成功结束；中断后通过既有执行句柄确认，没有重复启动构建。U22 尚未提交，没有候选 CI 或合并结论。

顶层收据经独立复现再补三处 P2：static.json 被替换/删除仍通过、读取失败漏写最终结果、其他仓库内尝试目录未被拒绝。现于各阶段写完立即固定摘要，最终逐项检查原摘要并捕获读取异常，mkdir 前检查目标物理父目录的仓库边界。维护回归 13/13 与原独立复现的五个案例均符合预期，首个失败和原阶段结果不改写。后续加入外部预锁 UI 路线配置后，顶层套件 15/15 通过；运行阶段仍是明确替身，不能据此证明 Engine/Chrome。

第一份真实 Windows ZIP 在静态阶段失败：editor/dist/index.html 使用 /assets 根路径，无法按随包目录搬迁。editor/vite.config.ts 仅增加相对 base，TypeScript/生产构建结束（Vite 30.48 秒），保留 Studio 静态兼容，不开发其功能。第二份 ZIP 为 artifacts/validation/u22-native-package-02/CaesuraAmeKAG-1.0.1-Windows-AMD64.zip，SHA256 181a97c5eba1b3f6525c9b8aebeaeac91b2ecc292a92f11ca2a7c06fc1b7d933；外部 Release 要求 SHA256 为 85ec875f975a1e12a4dfb985ae364c58f9fc3fb9656edd5db6790033174578cc，SDL3 shared，FFmpeg/Steam/Live2D 关闭。

第二份 ZIP 静态通过后，实际普通 Engine 虽退出 0，运行控制器仍在 SDL3 完成加载前单次观察而拒绝；另有 NVIDIA Corporation/umdlogs 在包内新建。独立 DLL 屏障也复现过早观察。现在仅对尚未出现的必需库在原期限内继续观察，身份变化或同名外来库立即失败。独立审查还复现后续阶段篡改前序日志仍通过，现最终重新核对各命令最初 stdout/stderr/run.json 与 BUILD-INFO 摘要，删除、改写和 Windows 独占读取失败均保留最终 FAIL。Native 控制器完整 19/19 通过，源码 b9020c14012b46e7baf2cadecece550c800521b4b273a94a39cf908dd2cb4abe。

驱动目录问题由真实环境对照定位：隔离环境缺少 ProgramData/ALLUSERSPROFILE，驱动将共享日志路径退化为 CWD 相对路径。指定已有私有 home 后，真实 Engine 只在包内增加原有合同允许的日志/缓存/存档，NVIDIA 日志转入私有 home；未放宽可变路径白名单。环境工具维护回归先 KeyError，修复后完整 17/17。沿用第二份 ZIP 原始字节重新执行后，五阶段全部 RUNTIME_PASS：显式 token 编辑器、生成 token 编辑器、60 帧普通 Engine、包内 Lua/CLI 创建和构建中文路径 basic 作品、60 帧新作品。ZIP/准备副本 STABLE，所有初始文件未变，日志收据稳定，进程树和端口清理完成，三份控制器源码前后相同。实际 Engine SHA256 为 12529f17be49cbee5a8ff4bb795cc0acd54b5760c40a6dcb274155f15c7c9514；证据 artifacts/validation/u22-native-real-03，仓外运行目录 C:/Users/34021/AppData/Local/Temp/caesura-u22-native-fixed-6b3653206b284ea2b50f758a8bbeee72。这是同包真实诊断运行，仍不是干净候选的全量门禁证明。

Web 控制器独立审查真实复现停止请求写失败遗留 HTTP 进程，以及超时后 run.json 被 Windows 独占导致清理见证句柄与最终收据遗漏。两处均先失败再修复；完整 17/17 后继续接入路线输入，当前控制器套件 19/19。首次 Chrome 早退日志记录自动降权重启，后添加仅 Windows 的 --do-not-de-elevate 以保持已记录 PID；Chromium 原始提交证据在 u22-web-review。下一轮明确记录缺少重定向 AppData 目录，预创建私有 Local/Roaming 后 Chrome 153.0.8010.36 成功建立本轮 CDP PID/端口，普通播放器、WASM/Lua、音频真实点击解锁已执行。

第三轮真实 Web 在 UI 保存阶段失败：作者样例初始为选择状态，按已有 Lua 存档合同此时必须拒绝。此为探针路线前提不足，不能修改产品来允许非法快照。探针正扩展由调用者预锁摘要的显式 DOM 点击/可见文本等待序列，boot 解锁后走到可存档段落，offline 仅核对同一路线配置而不重复执行。前三次原始 Chrome/HTTP/CDP/失败收据均保留在 u22-web-real-01/02/03；最新真实 Web 尚无 RUNTIME_PASS。旧 fake-CDP 首次失败与本次 Chrome 启动问题没有足够证据证明同因，继续保留未定位状态。

DMG/AppImage 准备模块已有 20/20 文件系统/宿主协议回归；独立审查修复合法父目录 alias 被拒及清理前设备变化被重新绑定的两处 P2。macOS /tmp、/var 可解析至物理父目录；已经证明的挂载设备不得被随后观察覆盖，变化时失败且不卸载新设备。实际 hdiutil/AppImage 在 Windows 保持 NOT_RUN。另已确认旧 AppImage 构建脚本按 mtime/首目录取包、遗漏 demo/projects/动态库且搬移 Engine，正在改为显式输入与完整安装树。CTest 接线计划发现门槛已从 39 调至 44，新增运行、容器、AppImage 构建和 Node 探针入口；须在文件冻结后由实际 CTest 发现与全量门禁确认，当前不能宣称 44 项已通过。最终容器启动、跨平台 CI 接线、U22 候选门禁及 U23–U29 后续范围仍未完成。

## 浏览器交互、端口与最终容器接线追加

真实 Web 第四轮已经用预锁 actions 点击作者 A 路线、观察完成文本，并通过普通 UI 保存与加载；随后浏览器请求未声明的 favicon.ico 得到 404，严格网络检查失败。播放器生产 HTML 现在明确引用现有包内 icon-192.png；资源回归先 5 PASS/1 FAIL，再 6/6 PASS。生产构建后重新生成两份 ZIP，basic SHA256 为 584874c36ad1fe293ab7cc4171f155d183757a90ccf861e44f067ac818310f04，kag3 为 dbb3551cf332b8bfe8e0ce52aef1a09e4a28c8d43f11d6aef65514cf75ddc104。两包仓外静态检查及原包/副本稳定性通过，见 u22-pwa/fixed-packages-02 与 isolated-static-02；尚未由此得到运行通过。

第五轮在 actions 首次点击遇到 DOM.scrollIntoViewIfNeeded -32000：消息 span 每帧重建，query 得到的节点在 scroll 前已失效。维护 fake-CDP 回归在旧实现失败，修改后显式 surface click 先滚动稳定的 stage，再一次性读取当前 selector 几何和命中关系。唯一性、可见性、包含与实际 Input 点击检查均保留；没有 surface 的失效节点仍拒绝。一次完整探针 21/21 PASS，原真实失败保留在 u22-web-real-05。

第六轮未到页面加载：Chrome port=0 得到 6665，Node Fetch 在 CDP discovery 报错。实际独占本轮 127.0.0.1:6665 的独立实验确认 cause=bad port，node:http 可读该端口，但内置 WebSocket 仍拒绝且服务端未收到 upgrade。此端口在 [WHATWG Fetch 的端口阻止表](https://fetch.spec.whatwg.org/#port-blocking) 内。探针仅将 CDP discovery 改成受限 node:http，响应大小/总超时/重定向和 cause 有维护负控制；完整 25/25 PASS，6665 上的完整 probe 仍必须失败。控制器为 HTTP 页面和 Chrome debugger 预分配浏览器可用端口，记录拒绝的分配；Chrome 启动后必须由本轮 PID/创建身份独占选定端口，竞争时失败，不连接外来服务。HTTP 端口回归完整 21/21 PASS，新增固定 CDP 端口及实际 OS 所有者/HTTP 协议回归 1/1 PASS；新增后完整控制器门禁待依赖冻结。旧最初三项 fake-CDP 失败缺少原始端口，不能据此推断它们已定位。

顶层最终包入口已接显式 --container-format 与 --payload-relative-path；DMG 使用指定 hdiutil，AppImage 使用最终文件提取的完整 AppDir，不添加 TGZ 前缀。最终复核原容器、执行副本、准备内容、工具与初始收据；准备失败、容器被修改或收据删除均不能进入通过。真实文件与明确提取/运行替身的完整入口套件 20/20 PASS（u22-package-runner/containers-green-full-02.log）；初轮 fixture 的 Windows 换行序列问题也保留，改成与正式收据相同的 LF 后通过。AppImage 的实际 AppRun/Engine 映射增强正在单独实现；FUSE 安装/启动不在提取后入口的证明范围。

AppImage builder 初版完整 14/14 PASS，固定 TGZ、外部 build requirements、appimagetool、type2 runtime 四个输入；保留完整安装根、demo/projects/库与包内作者 CLI，工具不得改变 AppDir 源字节。独立审查再复现输出父目录在发布前被 symlink 替换仍写入外部目标的 P2，已开始以绑定父目录身份/目录 fd 的回归修复，旧通过不覆盖该问题。官方 release API 返回 appimagetool 1.9.1 x86_64 asset 324406736 / SHA256 ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0；type2 runtime x86_64 asset 456065460 / SHA256 1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf，接线固定这些字节，不让工具隐式下载 runtime。实际 Linux AppImage 构建与 macOS DMG 尚未执行。

web/scripts-index.json 已按新增受维护 Lua 脚本重新生成，原 stale 失败保留；现 --check 为 95 modules up to date。Windows CMake 重新配置成功，实际 CTest --show-only=json-v1 发现 44 项，原始清单为 u22-foundation/ctest-discovery-44-01.json；这是发现数，不是 44 项运行通过。当前仍未提交 U22，CI/release/deploy 接线和本候选全量/真实跨平台验收继续。


## 续跑核对与实际离线缺口（2026-09-19）

续跑时核对独立工作区与进程：U22 未提交，未发现遗留 CMake/CTest/Node/Python 构建执行。此前第七轮浏览器读取 text/plain Lua 时按非 UTF-8 解码，捕获响应与包原字节不符；服务明确指定文本 UTF-8。第八轮 CDP 文本去掉开头 UTF-8 BOM，现分别记录原始包摘要和 CDP 解码表示，仅允许一个开头 BOM 的已知转换，二进制仍逐字节匹配。第九轮 ignoreCache=true 的强制刷新绕过 Service Worker，改为禁用 HTTP 缓存、停止本轮服务器、保持 SW 可用的普通重载。第十轮终于暴露真实产品缺口：首次安装未缓存 Vite 哈希脚本，图标预存 Cache API 却走 IndexedDB 读取，离线启动失败。这些原失败均保留，不从后续通过推导首次成功。

上述控制器完整 24/24 与 CDP 探针 26/26 通过，日志为 u22-web-runtime/cdp-text-green-full-01.log 与 offline-reload-green-full-01.log。此次独立复核又执行 15 项定向边界检查，全部通过、无跳过，10 个源码/测试身份前后相同，见 u22-resume-review/review-01.md 与 targeted-01.json。复核范围包括安全端口耗尽/异常释放、AppImage 分发与 CDP 表示绑定，不等于真实最终包验收。

AppRun 同 PID/创建身份的 exec 观察已完成：Windows 18+21 项、WSL 22+19 项全通过（u22-apprun-runtime/full-01.json）；现有独立审查四文件摘要仍匹配，因此复用其 WSL 正负复现证据。AppImage builder 的输出父目录替换缺口已修，Windows 15/15、WSL 16/16 全绿（u22-runtime-review/appimage-review/fix-02.md）；实际最终 Linux Engine 与 DMG 仍未由这些合成协议夹具证明。固定 appimagetool/runtime 已下载并核对上节 SHA256，尚不等于工具或最终 AppImage 已实际运行。

离线修复先保存 offline-resume-red-01.log 的 10 项失败，再增加 scripts/offline_manifest.mjs，在生产构建和作者资源覆盖/最终 story 写入后生成 classic offline-assets.js；生产 worker 必须加载该清单，按实际最终路径、大小、SHA256 预缓存，读取同一 scope/revision 缓存。缺资源、内容变化、配额失败均不得激活。27 项定向测试通过，Vite 生产构建通过；这些是当前局部结果，真实浏览器与独立审查继续。打包诊断脚本首次未创建外层临时目录，prepare_package 按合同拒绝；修复驱动后使用全新 04 目录，两模板打包、作者输入不变、仓外 STATIC_PASS 与 STABLE 均通过，原 03 产物保留。


## 最终 Web 包真实通过与新增平台证据（2026-09-19）

离线独立审查真实执行 classic worker，确认显式 CACHE_ASSET 写 IndexedDB 后生产读取仍 503，以及多个失败 revision 遗留部分缓存。现修复未声明动态资产的读写、相对 URL 删除键一致性、按游戏 scope 隔离 IndexedDB，并在安装失败后等待所有写任务结束再回收未完成缓存；已经完整的同 revision 缓存保留。新增六项负控/正控，offline 16 + classic worker 17 + 生产资源 7 = 40/40，通过且无跳过，见 u22-offline-review/review-01.md。名称带 red 的两个后续 Vitest 日志实际已是修复后 GREEN，报告明确区分，旧缺陷仅以原始 repro-01.json 和当时源码 SHA 为准。

生产构建和打包后最终 basic ZIP SHA256 为 2ab4ef835bf4ff6d5e24758ad059bf9aeddd9c2cb88e4e1bb34b4dd524a13927，kag3 为 ff9b851c325d3d511bf24d5bc0e9ce9cf5220b760d6b7a775fcec126ada7d02e（u22-pwa/fixed-packages-05）。real-12/basic 与 real-13/kag3 均 RUNTIME_PASS：本轮受控 Chrome/HTTP、根路径及子路径、实际 UI 点击路线与存读档、服务器停止且 HTTP cache disabled 的离线普通重载，全部 SCENARIO_PASS。原包/副本前后 STABLE，错误为空；kag3 子路径离线截图人工查看确实显示四条游戏选择。此为这些明确 ZIP 的真实诊断通过，不是尚未提交的干净候选全门禁，也不额外声称 OS PWA 安装、PCM 音频采集或跨进程存档恢复。

完整 Web 首轮共 626 项，623 通过、3 失败：本次调用未设置 U13 要求的 CAESURA_LUA_BIN，另两项性能门槛为吞吐 1.913577 frames/ms（要求 >2）与配对规模比 2.545620（要求 <2.5）。原始日志 web-full-01.log 保留；该次性能采样时存在本代理同时启动的 CLI 构建/包装测试，不能由此确定产品性能退化，也不能删除失败或放宽门槛。待其余负载结束后补正真实 Lua 环境，串行重新执行完整套件；U27 的正式 Release 多进程基线仍独立待验。

CLI 完整回归首轮 29 项有七个失败子项，均先被复制播放器所需图标缺失挡住；旧 capability 测试夹具 assets 为空。本次补入真实播放器声明的两枚图标，保留篡改、能力拒绝、旧产物保护等原断言不变，之后完整 29/29 通过（cli-offline-full-02.log，48.973 秒）。没有以跳过缺资源来取绿。

CI helper 与三 workflow 接线已落盘：Windows ZIP、Linux TGZ/AppImage、macOS TGZ/DMG、Web 目录/ZIP 使用明确路径、最终摘要和独立运行收据；PR 同样执行包门禁。Windows/WSL helper 各 21/21，actionlint 与 38 个 shell/pwsh 脚本语法检查通过。独立审查发现 Linux/macOS 根目录 build.log 污染 clean source，已移入忽略的 build/ 并保留证据上传；见 u22-offline-review/ci-review-01.md。CTest 已实际发现 45 项，六个桌面 profile 门槛同步为 45；全量执行仍待完成。workflow_call 的基础接线不等于 U23 artifact ID、run_attempt、版本与分支保护验收。


实际固定 appimagetool 的首两次执行均 exit 1，第二次保留的原始 stderr 证明 desktop-file-validate 拒绝 CRLF。新增两处已复制 desktop 的字节回归先失败，再将 tools/appimage/caesura-amekag.desktop 固定为 LF，.gitattributes 新增 *.desktop eol=lf。Windows 完整 AppImage 套件16/16、WSL17/17通过，无失败或跳过，10个受影响身份稳定。真实 attempt03 工具 exit0（APPIMAGE_BUILT），真实 --appimage-extract 后 CONTAINER_PREPARED，容器复核 STABLE；最终 AppImage SHA256 d7aed5a5028ad977a04ae44b2365e350ce7a1455ce73424179af12cdadef4bae。其 Engine/Lua/SDL 仍是明确的静态 ELF 夹具，runtime=NOT_RUN、accepted=false，不能提升为 Linux 产品运行证明。

attempt03 辅助 driver 在最后比较完整 inventory 字典时仍 FAIL，未重写。后验分析在同一保留快照上确认295条内容与内容摘要完全相同、所有文件mode相同，46项差异只来自prepare_package创建目录后0777转0755；详见 u22-appimage-real-tool-03/reconciled-stages-01.json 与 report.md。阶段实际成功、辅助断言失败及未执行的引擎范围分别记录。


补正 CAESURA_LUA_BIN 为本工作区 Debug Lua、PYTHON 为固定 Python，并等本代理所有 AppImage/CLI 重负载结束后，完整 Web 串行第二轮 50 文件、626/626 通过，0 failed/0 skipped，157.32秒（u22-pwa/web-full-02.log）。没有改变性能用例或阈值，也不由单次通过推断正式U27长跑/多进程基线已完成。U22现在进入候选源码冻结、完整 Debug/C++/Lua/CTest 与托管跨平台最终包门禁阶段；上述第一轮失败和真实包诊断结果保持各自范围。

## 首个干净候选的门禁失败与修复（2026-09-19）

候选00944056086e6e2ffa54047a2cdc988b5bf37eae已推送至草稿PR #25，并请求TaotianZhufang审查。完整windows-debug执行4d6e4103-1f9b-41e5-a0b9-961ff595bc7f在12:06:43–12:18:30 UTC运行，源码/夹具前后稳定；全量Debug构建、C++1404/1404（402737断言、0失败0跳过）、Lua主147/147和隔离56/56通过。CTest实际发现45项，43通过、HTTP及AI两项跳过。profile仅允许预先声明的AI跳过，因此collector及严格verifier结果均FAIL；执行器exit0不能替代门禁结论。原始run.json、ctest.xml和验证日志保留在u22-foundation/candidate-00944056-01等目录，未删改首次记录。

受控真实复现确认HTTP失败由固定9876落入Windows保留端口9868–9967导致。PID5800启动后自然退出1，stderr记录socket error 10013；同一Debug引擎只改CAESURA_EDITOR_PORT=14784，PID30372即可由真实GPU窗口路径建立本轮拥有的listener并通过/api/ping，随后受控停止并确认端口关闭。此前smoke丢弃stdout/stderr且把任何早退非零都标为NO GPU，这一诊断错误不能继续作为跳过依据；证据见u22-http-skip-audit/real-repro-01。端口选择与启动失败归类正在修复。

托管CI35441973534的首attempt全部结束：Windows Debug/Release在生成文档新鲜度失败（实际Lua源文件93，文档仍92）；Linux在事务测试复制的Web夹具缺少播放器声明的icon-192.png处失败；macOS的六组CTest失败包含系统/var别名、Framework Python启动器exec后的映像身份，以及未固定Node时内部WebSocket诊断文本变化。后续桌面/Web包job均未执行；三个Android/iOS静态或配置probe成功，不提升为最终包或设备运行证明。四份原始失败job日志均保留在u22-foundation/ci-35441973534-*.log。

文档由原生成器重新生成；事务测试仅补复制真实192/512图标，不改原子提交与输出保护断言，本地同源RED为0test/1 setup error，修复后完整45/45通过。资产依赖测试复用相同fixture，仍需其实际执行结果。macOS路径问题用本机真实目录链接复现，三项定向测试修复前1失败1错误，规范化受控temp根后3通过；生产路径检查不变，显式链接拒绝负控保留。完整release static20、native static22、Web probe26项通过、无跳过。独立增量审查未发现可行动缺陷，见u22-offline-review/platform-review-01.md；macOS实际Framework进程和最终包门禁继续待修复与新托管执行。

后续资产依赖完整18/18通过（u22-platform-fixes/asset-dependencies-full-01.log）；actionlint1.7.7复核三workflow通过。macOS Framework Python修复只接受精确当前sys.executable启动入口到控制者OS观测映像的映射，锁定双文件摘要、同PID/创建身份/身份来源与连续两次终态观察。短命Python保留真实退出码且process=null，不发布readiness；AppRun未达到终态仍拒绝。六项回归先失败后通过，Windows process24、WSL28、Windows Web controller24、WSL22全通过且零跳过，见u22-python-framework-fix/freeze-01.json。这些跨平台边界夹具不能代替真实macOS执行，下一托管候选仍必需。

原生编辑器会话现在逐次由OS分配可用loopback端口，明确指定端口则不可回退；释放预留socket到引擎绑定之间仍有竞态，因此在发送请求前后继续检查本轮PID/创建时间与listener归属。HTTP smoke保留stdout/stderr、真实退出码和受控清理收据，启动失败直接FAIL；CTest仅移除该smoke的77跳过约定，AI的预声明可选跳过保持不变。独立真实双进程负控发现urllib自动重定向会转发测试token给外部listener，已禁止跟随重定向并保留原始3xx供断言拒绝；匿名401路径及响应关闭顺序也由实际HTTP回归覆盖。Windows原生套件28/28与自动/显式可用端口两组真实Debug HTTP各75/75通过，9876则在创建进程前明确拒绝10013；见u22-http-skip-audit/handoff-01.md与u22-offline-review/runtime-review-01/review-01.md。

随后WSL原生集成26项有1项失败：AppRun已经达到正确终态映像，但子进程自然退出时/proc映像查询消失早于poll观察，造成退出0被误报启动失败。原native-wsl-final-02.log的25通过/1失败保留。确定性真实Popen屏障先红后绿，修复只在原保留子进程wait于一秒内证实退出时记录exit_observation与实际退出码；仍存活、超时或明确身份变化仍拒绝。新helper完整Windows27/27、WSL31/31通过；所需WSL原生集成重新26/26通过，真实Windows Debug HTTP再次75/75通过，受控停止实际exit1、无强杀、清理完整且无残留PID/listener。独立增量审查关闭此项；最终helper摘要f6498cb0d6b0a98809588d2ff9668d0655ac1144ee0c24e6a625bee4869c771a，证据u22-runtime-exit-race与u22-http-skip-audit/handoff-02.md。上述Windows28项结果对应旧helper，不冒称新helper全套；新候选全量门禁及真实托管Linux/macOS最终包仍待执行。

## 第二候选严格门禁与托管失败修复

干净候选d7de1b95eb445222acfacde9a1d105b8c36681ef的本地windows-debug执行97a10bd0-d9ba-469b-8abe-6c8e59ccd488已完成：执行器、collector及严格verify_release_candidate均为0/PASS。原始目录为u22-foundation/candidate-d7de1b95-01，收集目录为主工作区artifacts/validation/u22-evidence/<完整SHA>/<执行UUID>/windows-debug。HTTP smoke实际执行通过，不再跳过；AI仍为预声明可选跳过。该结论只绑定该干净候选，后续Engine与HTTP修改需要新的完整执行。

该候选托管run35444473803的首attempt最终失败。Windows Debug/Release均在Web probe的offline actions用例失败：托管机TEMP采用RUNNER~1短路径，测试的JavaScript realpath与生产CLI的native realpath结果不同。真实GetShortPathNameW复现保留u22-short-temp/red-01，测试夹具改用realpathSync.native后同一路径green-01通过，完整26/26、0跳过；没有放宽生产端精确路径/SHA校验。

Linux HTTP smoke的75条路由均通过，之后受控停止失败。Engine的editor分支原先不消费SDL事件，而SDL的POSIX信号处理将退出信号转换为QUIT事件；独立实际SDL模型复现无事件泵时强杀-9、消费QUIT后正常0退出。新增真实Engine三项回归在旧代码2失败，初修全部通过；独审随后以65535事件队列实际复现普通事件积压会阻止QUIT入队。第4项跨帧输入回归在初修失败，最终按队列数量快照分批取走所有事件，仅QUIT改变运行状态，其余丢弃且不转发游戏，纯headless保持不碰SDL队列。最终4/4、284断言，以及相关49/49、907断言通过；完整Windows发现1408项据此提高该平台门槛。独立SDL模型处理70000普通事件后队列不积压、QUIT正常，独审闭合。原RED及两轮构建日志保留u22-editor-quit和u22-offline-review/editor-quit-review-01；这些不能替代新托管Linux/macOS Engine验收。

macOS该run的package_runtime实际30/30通过；native/Web在真实Python映像身份确认后、HTTPServer就绪前失败，历史失败artifact缺少原始子进程栈，不能断言DNS就是旧故障根因。受控真实子进程证明固定127.0.0.1服务仍受HTTPServer反向DNS阻塞：释放resolver屏障后真实HTTP正常，而预期无DNS断言在旧实现失败。现在专用回环服务直接使用TCPServer.server_bind，保留数字地址、实际端口及全部PID/监听归属规则。启动第5秒保留faulthandler栈，原15秒期限不变；失败异常携入原stderr末8192字节，避免临时目录清理后诊断丢失。三项真实child RED→GREEN、Windows Web26/26及Native29/29、WSL Web24/24及Native27/27全部通过，零失败零跳过，见u22-http-startup-audit。没有运行Chrome/Engine，原macOS归因和实际平台通过仍待新CI。

CI新增三平台失败HTTP目录和LastTest上传；独审指出Windows必须含Debug/Release层，已修为build/${{ matrix.config }}/artifacts/validation/http-smoke-*/，Linux/macOS单配置路径不变，原证据上传保留。actionlint1.7.7通过。第二候选四份原始job日志保留u22-foundation/ci-35444473803-*-job.log；三个Android/iOS probe成功，四个最终包job均因前置失败未执行。PR25与U22整体继续未通过，未合并、未发布。


## 第三候选原始结果与后续修复

本地干净提交0278910e的windows-debug严格验证12183991-ef9b-4bbf-b814-66da967a9f24通过：完整Debug构建0，C++1408/1408、403021断言、0失败0跳过，Lua147/56两入口均0；CTest45项中44通过、0失败，仅预声明CaesuraHeadlessAiSmoke跳过，真实HTTP检查通过。runner、collector、strict verifier均退出0，原始run.json摘要0a8e93bef540e755495f13502f9f112e3fca126ee438079d4fa6fdd554a4bb32。原件及outcome位于u22-foundation/candidate-0278910e-01，收集件位于主工作区u22-evidence/0278910e0ff735fda4458a1ffd4386e846ad37ea/12183991-ef9b-4bbf-b814-66da967a9f24/windows-debug。

托管run35446845025的PR head为0278910e，三个原始job日志的git log证明实际执行的是PR merge69e0777810ced7c47aa2378b551b9fee69a3e87c，不能以head替代执行身份。Windows Debug105907137963和Release105907137911通过。Linux105907138086完整45项CTest无失败、仅AI跳过，实际HTTP通过，随后平台文档旧anchor使job失败。macOS105907138155的实际HTTP、native/Web runtime通过，但一个合成Framework切换测试因使用真实调度时钟失败。Windows最终包105910356512完成构建后在ci_package_lane.py的参数解析阶段拒绝多余cpack.exe路径，未开始最终包生成；其余最终包job被依赖失败跳过。该run总体失败，全部首次日志保留。

测试提交28575a16只隔离了合成Framework fixture的时钟：原fixture完全替换Popen/OS身份观察却使用真实0.2秒截止时间，受控调度延迟真实复现身份观察次数不足。现该fixture使用可控单调时钟，保留两次最终身份观察及0.2秒合同，并新增未达到终态/仅一次终态仍拒绝的负控。Windows完整29/29、WSL33/33、0跳过，独审见u22-offline-review/framework-clock-review-01.md；实际子进程、AppRun、listener及生产deadline代码未改变。它不构成新的macOS托管通过。后续候选仍需干净源码完整本地门禁与托管最终包验收。

固定Mac诊断artifact10585526480已下载原始141983字节，API与ZIP摘要均为b0bae68a033453ace1f283b8419bd713b0491f50b3f47011caafd467e68dc71a。原HTTP收据证明PID33095、创建身份1789826169:501546、本轮OS分配127.0.0.1:49345，stop_requested=true、自然退出0、forced_kill=false、timed_out=false、cleanup COMPLETE，stdout记录真实清理退出。HTTP实际73/73，另两项Web打包检查因该Mac job无Web工具链跳过；这不等于75项全跑。Native runtime实际25项与Web runtime24项通过；整个Mac job仍因合成Framework测试失败。Linux仅读取原job日志，没有下载189510770字节诊断包，故不额外声称其PID/停止/清理收据已核验。原件、摘要与身份边界见u22-hosted-runtime-027/。

Windows CLI失败根因由真实PowerShell复现：PATH含两份cpack.exe时Get-Command的.Source返回数组，第二完整路径作为多余参数进入argparse；同样的双Python候选会使调用表达式失败。提交46490236仅令两项命令发现按PATH顺序选择首个Application，并让不存在的命令立即失败。旧片段双工具候选RED，新片段正确分派；单个带空格路径通过，缺工具与显式多余参数继续拒绝，actionlint通过，见u22-package-cli-audit/。验证在生产CLI解析后由capture worker故意返回失败，未调用CPack或Engine，不作为最终包通过。平台数据仅同步新代码审阅anchor；实际本地0278910e及托管merge69e07778的执行证据身份分别保留。
