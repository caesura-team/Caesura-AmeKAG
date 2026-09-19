# U22 最终包隔离验证执行记录

## 当前状态（2026-09-20）

**U22 尚未完成四平台最终包验收。** 最近完成托管执行的候选为 `016f77f7af611c83bdac87c6dae12fd1a877d6ea`；[CI 35467654286](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35467654286) attempt 1 已终态 FAILURE，11 个 job 中 **9 成功、2 失败、0 job 跳过**。实际执行的是 PR merge 源码 `cc258c6ae9293a24eec82ae7bfe7fbcf155bef2b`，不能与 API 的 PR head 混同；job 无跳过也不表示内部所有测试或条件步骤均无跳过。

| 本轮范围 | 已有事实 | 尚未建立的验收 |
|---|---|---|
| Windows Debug/Release、Linux GCC、macOS Clang；Android/iOS 三项编译或静态探针 | 7 个 job 成功 | 编译/静态探针不证明移动设备运行 |
| Windows ZIP；Linux TGZ、AppImage | 两个 package job 成功，验证、上传前复核及上传步骤成功 | 本记录尚未下载并逐字节审计本轮这三个最终包，旧候选下载审计不能移用于本轮 |
| macOS TGZ | 原件已审计；新建游戏实际退出 0，但加载库来源检查失败，`accepted=false` | DMG 未到；不能因 Engine 正常退出改判整个 TGZ 通过 |
| Web | Chrome 152 原始 CDP 记录确认离线覆盖后为 `online=false`，重载后变回 `true`，目录根场景失败 | 子路径和最终 ZIP 验收未到；Linux Chrome 153 尚无运行通过证据 |

后续候选包含固定 Chrome 153 归档选择和严格 lsof 字节路径解析两项有界修复，证据详见末尾追加。Chrome 153 的 WSL 启动被缺失 `libnspr4.so` 阻止；lsof 修复已完成真实 Linux wire 控制及 Windows/WSL 脚本回归，但原 Mac 失败未保留被拒绝路径，转义问题仍是 **INFERENCE**。候选修复的本地回归不替代新的完整门禁和真实托管包验收，也不授予发布批准。

历史起点为 2026-09-13，沿用当前计划 U22，底层优先、Studio 暂停。工作分支最初整合 U21 候选 `4c007407`，其后合流与验证记录保留如下。下文历史段落中的“当前”指该段记录时点；最新状态以本节和末尾追加为准，历史失败不覆盖、不改判。

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

## 最终包首次运行失败与显式软件混音（2026-09-20）

托管 run35451773753 的 head0785b697 对应实际 PR merge fa2a2c1d06cfa9b56c15d22aec5566f41734aaeb。Windows Debug/Release、macOS Debug 及三个移动平台 probe 成功；Linux 完整 CTest 与 Web626通过，随后生成能力矩阵指纹过期导致 job 失败。独立 Git blob 清单证明新增 offline-manifest.test.js 和资源回归改变了输入；使用原生成器更新文档，仅生成时间和指纹变化，能力计数与状态不变。原始日志及反证见 u22-linux-latest-audit/。

macOS 最终 TGZ 静态检查通过，但实际 editor 运行的映像观察器对已消失的无关系统日志缓存严格 resolve，导致 loaded_modules=NOT_VERIFIED。保留原 path/标准化 path 后只容忍 Darwin 的 FileNotFoundError；必需库消失、同名外部库、权限失败和身份改变仍拒绝。真实文件删除负控先失败后通过，Windows36/36、WSL34/34，独审无发现；这些替身边界不能代替新 macOS 包运行。后续 U23 run35452996878 的固定 artifact10588215587（11148992B，SHA256 95065c5f030c45f5d2d4d5855d6ff18f7b258be693bfc0c6fe1e041acc5b7f58）再次确认相同观察器错误类别，但不同缓存文件。原 Engine 受控退出0、无强杀、清理完整，TGZ仍FAIL、DMG未到；见 u22-macos-tgz-audit/ 与 U23 u23-hosted-79e8b439/audit-handoff-02.md。

Windows 最终 ZIP 的固定诊断 artifact10587806605（11110474B，SHA256 4f37426bdaa163a7c2fd23cd5501e07c4b748da561d634f03aa1d6161d4ccf13）证明包内 SDL 来源与 D3D11 初始化通过，但 SoLoud init 返回7、回退 Null；默认 demo 的 playbgm 正确报告 backend_unavailable，之后 engine_frames 超时并被本轮拥有的 runner 清理。没有独立枚举托管音频设备，因此不能将其归因为“没有声卡”。原 package FAIL 与 timeout 收据完整保留在 u22-windows-package-audit/。

新增显式 `--audio-output device|software`，默认 Device 保留，既有 ManualMix 仍由外部手动推进。Software 使用真实 SoLoud NULLDRIVER，在 update 中按有限正 dt 推进48kHz双声道混音，单次最多0.25秒、固定缓冲、分数样本累计；暂停不积压补偿、重新初始化重置会话。正常 shutdown 输出实际 PCM 统计。三个托管桌面包 lane 预声明 software；原 argv、包收据和模式必须一致，默认 demo 必须实际产生非零 PCM，缺失/重复/非法统计、Null fallback 或未完成混音均拒绝。物理音频输出始终 NOT_RUN，不注入假 PCM，不改变剧情/ErrorUI，不自动改模式重试。

真实 C++ RED为6项中1通过5失败；实现后6/6、24059断言通过，1408项仅过滤未选中。真实 Engine CLI 从1/10变为10/10，新CTest入口实际通过；CTest现发现46项，六个平台profile门槛同步增加一个CTest及六个C++用例。Python controller/lane/runner Windows41/34/22、WSL39/34/21全部通过，零跳过；平台数量差异是预定义OS条件。独审19组统计/日志反证和实际adapter/ZIP合同无发现。WSL手动测试首次发现Lua仅返回PATH名字，改为测试夹具显式锁定实际绝对解释器；原失败与实际lane错误保留，生产路径规则未放宽。

本机真实 dirty Debug 诊断启动 D3D11 默认 demo，正常达到60帧并 exit0，无超时/强杀、owned cleanup COMPLETE。Engine SHA256 45551eb6c9389eab67c1efdaeecdf80e5753ba332ba152fd5069d74bcd22e713，实际26256混音帧、52512样本、6011非零、0非有限；原日志明确记录 daily.wav BGM 与清理完成。见 u22-software-audio/debug-demo-driver-02.log 及其绑定的Temp原件。这不是最终发行包验收，也不证明物理输出；首次辅助脚本路径错误日志同样保留。

Web 真正 producer 还复现了仓外 --out 被原打包器拒绝。修复在新建、Git实际忽略的仓内唯一暂存目录运行原 Node/Lua 打包，再完整复制到仓外新目标并核对目录身份、全部文件摘要及前后稳定性；原打包器仓内输出限制不变。首次真实RED、Windows/WSL32项回归、实际已有播放器正控、禁止读取原web/dist的自含夹具均保留在 u22-web-lane-output/，主代理独审无发现。加入音频接线后helper为34项。完整新候选、实际仓外目录/ZIP/Pages tar及托管包验收继续待执行，U22/U23均未因此完成。

## 四平台最终包缺口与第二轮针对性修复（2026-09-20）

干净候选 effc6b2adbe8de3a2aec1bfc587d8e29de634212 的完整 windows-debug 门禁通过：run0c1f2b69-e122-4579-8c23-716ef7e9e3fa，原始 run.json SHA256 a636b75c681619c85f6badd94be8991c224d8aa01516bf9c08fa5099bb53754b；Debug 全量构建、C++1414/1414（427080断言，0失败0跳过）、Lua147/147与56/56、CTest46项中45通过和一个预声明AI服务跳过，runner/collector/strict verifier均0。源码在全程保持干净且不变；此结果不能迁移给后续修改。

托管 run35455884122 已终止为7个job成功、4个最终包job失败。API head为effc6b2a，但checkout及包收据的真实执行身份是PR merge0d2db03c458b1bcfeeb7c3636fc2ee616d21c301。Windows Debug/Release、Linux/macOS构建测试和三个移动probe成功；四包失败分开保留，未合并或发布。

- macOS TGZ静态通过，editor映像观察遭遇无关 `/private/var/db/analyticsd/events.allowlist` 的PermissionError。原artifact10588562773（11136562B，SHA256 7d1c4c0c43c34ea1b94a8304fe2afba1f4b2eda4d89224d6c6ff2189a044ed6f）绑定收据与原日志；PID31441受控exit0、无超时/强杀、cleanup COMPLETE，但runtime仍FAIL，DMG未到。新实现仅对Darwin记录不可读取的映像路径；所需包内库仍须真实观察和哈希验证。独审再发现声明SONAME与真实文件名不同的alias漏检，已新增真实symlink正负回归并修复：原路径与规范路径均参与必需/外部同名判定。最终Windows44/44、WSL44/44、独立7/7控制通过；见u22-macos-permission-audit/review-02.md。没有新的真实Mac通过证据。
- Linux TGZ准备阶段正确拒绝 `libSDL3.so.0 -> libSDL3.so.0.2.0` 的缺失目标。artifact10587939892 SHA256 bcb8536aa15644c12f26b9d047454ca138b1a444bfa0b375d9954ee4da712dd5；原TGZ摘要8bca1d1cb089894e8a4a53ded171959723f25c4927f083cdc6b6d14fa693cd2b来自绑定收据，未下载原TGZ字节。生产安装规则原来复制SONAME符号链接而未带入真实文件。新增CMake helper在安装时解析所选配置的shared target真实文件，再按DLL/SONAME文件名复制实际字节，保留RPATH与iOS排除。真实小型CPack复现原错误，最终Windows6/6与WSL9/9通过，零跳过；这些明确的库字节夹具不证明SDL ABI/Engine。证据u22-runtime-install/含原RED、最终CPack与STABLE检查。
- Web目录静态检查发现缺失 `web/node_modules/wasmoon/dist/glue.wasm`，runtime尚未启动。artifact10588199108 SHA256 15ed3a050a11213ee7378e97da084e25ff064b180049279481359d831d1c2e4e；原包内vendored WASM存在，诊断artifact不含原JS/WASM payload。实际Vite对照复现：普通npm ci目录位于publicDir内时，被视为公共资源的URL不随copyPublicDir=false复制；本地依赖junction恰好掩盖该问题。修复仅在build模式关闭publicDir，dev继续提供仓库资源。真实Vite回归从1失败1通过到2/2通过；另实际完整生产插件构建及原HTML/JS/CSS引用检查通过。前两次fixture的Node cpSync Unicode崩溃记录保留，改用既有copyDirectorySync后才取得有效RED。见u22-hosted-effc6b2a/web-fix-handoff-01.md；不宣称本轮完整Web或浏览器已通过。
- Windows ZIP静态、两次editor、默认Demo及作者create/build全部通过。原artifact10588820674（14678148B，SHA256 d60530e4bdfb1ac1c360e04b6c3b8f8e1ce1cc953c0975f86eb7f2e1565e3e2d）的50个原始条目及21个命令日志/收据绑定已核验，见u22-macos-permission-audit/windows-bindings-01.json。默认Demo真实software PCM为44688帧/89376样本、48002非零、0非有限，物理输出NOT_RUN。随后同引擎摘要db7691ab…在中文作品目录的created_game_frames以3221226505（0xC0000409）退出，仅37B首条启动日志；SDL来源VERIFIED，无超时/强杀，cleanup COMPLETE。诊断artifact不含原exe/DLL/ZIP字节，未借此声称实际重放原二进制。

Windows路径回归使用实际Engine和新建目录：仓外ASCII控制通过，包含补充平面字符的CWD与父目录搜索案例在旧Debug引擎上均超时。首修仅将成功chdir后的path.string日志替换为已有安全UTF-8 helper，真实重建后日志正确，但两案例继续在DebugProtocol初始化失败；green-cli-01名称不代表通过，实际仍1/3。新增直接C++回归真实捕获 `No mapping for the Unicode character exists in the target multi-byte code page.`，定位其工作目录generic_string转换。修复改为generic_u8string，绝对断点测试输入也明确使用UTF-8；原断点和非阻塞协程断言保持。最终新C++回归1/1、8断言，全部DebugProtocol定向15/15、243断言；真实Engine CLI3/3，源目录、UTF-8日志、实际Lua config、ping及正常退出同时成立。Engine摘要24567ecf86a8daa61cb6313080e95ab53d4cbc108134a85542225fcf91b9d8a8。此前Debug超时和托管Release fastfail分别记录，不以退出类别相似冒充相同堆栈。

独审又以真实WSL目录symlink证明新增C++夹具的原TMPDIR别名与current_path物理路径不一致。仅将夹具绝对断点从chdir后的current_path构造，保留生产lexical路径合同及原断言；重建后15/15、243断言继续通过。该反例不冒充真实macOS运行。

以上路径定向是headless最小资源夹具，不是完整游戏/GPU/PCM验收。CMake已实际重新配置并发现48项CTest；新增CaesuraResourceCwdCli和CaesuraPackage_package_runtime_install，后者经CTest真实执行通过。六配置CTest门槛46→48，新增一个实际C++用例后各平台C++最低数同步加一。独审、原RED/中间失败与原始摘要保留；新干净候选完整构建、C++/Lua/CTest、完整Web以及托管最终包仍须继续，U22–U29未完成项不变。

## fd5b426b 完整本地门禁与托管最终包结果（2026-09-20）

干净候选 `fd5b426b0886a585874e2e22f739cc30160c87c4`（代码提交 `f779cc5d`）的 windows-debug 严格验证通过。run `8edc0a29-4313-4810-868e-44ed6641c6cd` 完成 Debug 全量构建，C++ **1415/1415、427088 断言**，Lua **147/147 和 56/56**；CTest 发现48项、47通过、0失败，仅预声明可选AI服务一项跳过。真实HTTP检查、runner、collector和strict verifier均退出0，源码保持干净且不变。原始 `u22-foundation/candidate-fd5b426b-01/run.json` SHA256 为 `380dcaad1dbb965f88ff6226bff32b5e33035cd0b1e913c85dbfbbecaeb5e477`。同一干净候选完整Web套件 **628/628、0失败0跳过**，原始 `u22-final-package-fixes/web-full-fd5b426b-01.json` SHA256 为 `1a6459bb8683d5bd91192108ba641822fd98bc87af381db9c358b12a5ad2149f`。以上是该源码的执行事实，不迁移到后续修改后的候选。

托管 run **35460659971** 最终为 **6个job成功、3个失败、2个跳过**。API head为fd5b426b，实际checkout和包身份为PR merge **c258c3b103ab82fa01eb0d6004e7eb858e31b437**。Linux GCC、Windows Release、三个移动probe和Linux最终包job成功；Windows Debug、macOS Clang和Web最终包失败，Windows/macOS最终包因前置失败未执行。完整原始状态保存在 `u22-final-package-fixes/hosted-state-06.json`，不能将前置构建成功改写为对应最终包通过。

Linux最终包job **105947074117** 对TGZ与AppImage均给出accepted结果。最终TGZ SHA256为 `0ada673a5dfe811a3bbb695b32a853e4c9dbf83efc89b48ad71a0efb96000580`，AppImage为 `648e5484bde714687f571d84059678f702cdb6e840150b971782a2ce05bf16e2`，manifest为 `f86684cce8fae994ca2b454dc9b7b87b1a1c072477d95d1be5fdc4f62061877f`。job原日志与下载原包核验属于不同证据步骤；完整下载绑定结果另行追加，尚不由此授予U23发布批准。

### 两项真实测试夹具竞态

Windows Debug原日志记录 `launch_listener` 的readiness JSON被读到半写入状态，实际抛出JSONDecodeError。真实子进程写完首字节后停在屏障的回归先失败。现在所有异步夹具JSON先写完整同目录临时文件，再以 `os.replace` 发布；读端仍对已发布但损坏的JSON立即失败，没有增加容错重试。

macOS原日志记录exec拒绝用例在正确回收非exec shell及其子进程后，继续等待子进程的应用消息。旧临时目录已被清理，不能补称已观察到原平台的精确调度时序。独立WSL屏障复现已启动子进程在应用发布前被正确回收，原断言却等待永远不会出现的文件。修复保留四类exec错误控制及无VERIFIED身份断言；另设真实父子进程握手，先核对实际PID、PPID和映像，再停止父进程，证明父子均退出且应用发布屏障从未释放。

只修改 `tests/scripts/test_package_runtime.py`，原生产 `package_runtime.py` 未变。原维护方法全部保留，增加三项真实回归。完整Windows **32/32**、WSL **36/36**通过，0跳过；最终日志分别为 `u22-exec-fixture-race/windows-full-02.log` 与 `wsl-full-02.log`。测试冻结SHA256为 `3c44fba32f20751ad49eae6d02c4027e1745aeb10ceeb38a8ba036d153d9f5c5`，首个RED和中间结果保留。主代理独审未发现可行动问题；这些本地结果不替代新macOS CI。

### Linux Chrome 的真实 Unix socket 路径上限

Web job **105947074089** 的原artifact **10589054494**（7,573,098 B，SHA256 `8e5781e487c30d04d25e8b48c43f4ad3809ad4da8aa8caf8a0b3db0f8c5d0ca6`）已下载，16项runtime及3项顶层摘要均从原件重新计算。目录静态检查通过后，Chrome以 **-6** 退出，原fatal日志明确记录 **135字节** `SingletonSocket` 路径过长；尚未进入CDP和boot，ZIP验收未到。原HTTP/浏览器清理完整，无超时或强杀。原stderr SHA256为 `ac1e18f809c8abcab37cdc902d248a3942a9c4d8c0a1382a840f6c602d0603d8`，详见 `u22-hosted-fd5b426b/web-audit-01.md/json`。

修复仅给POSIX Chrome的TMP/TEMP/TMPDIR分配canonical `/tmp` 下的唯一短目录，macOS先解析已知 `/tmp` 别名。HOME、profile、日志、控制文件和HTTP/Node probe的环境仍在原attempt。新目录要求本用户所有、0700权限，绑定设备/inode/UID/mode并持有目录描述符；只有owned浏览器确认完整退出后才按描述符清理。目录或所有者改变、进程退出未确认时保留目录并失败。没有弱化Chrome安全参数、超时、PID/端口归属或包字节验证。

真实WSL AF_UNIX控制证明107字节可绑定、108字节拒绝。原控制器给出的200字节路径导致真实RED；修复后短路径GREEN。完整Web控制器Windows **27/27**、WSL **30/30**通过，0跳过，独立只读审查无可行动发现。生产文件冻结SHA256为 `d99eb8910bd77b46eb9feec796899a34ccc692dcb35c595f40ed268f4c8fd960`，测试为 `a8e55f2aada8694de5ed1013457baf329518d28b2972818850f2455196c7fb50`。

首轮RED/GREEN的WSL临时目录后来不存在，删除来源未观察，内部process JSON未能补取；原始日志和collector失败保留。另一次冻结源码控制在同进程finally中立即复制全部原始证据：71字节socket绑定成功，真实HTTP PID449及socket子进程453均退出-15，无强杀/超时，三项cleanup通过，立即确认两PID已不存在。该控制刻意停在未实现的Chrome/CDP边界，不声称浏览器通过。完整证据见 `web-socket-handoff-01.md`、`web-socket-freeze-02.json` 和 `socket-independent-review-01.md/json`。

本机WSL没有Chrome，未安装替代浏览器；真正Linux Chrome根/子路径及boot/offline仍等待后续托管验收。新候选需完成适用完整门禁及CI，PR25继续draft，U22/U23及U24–U29未完成范围保持开放，没有发布、部署或商店上传。

## 2026-09-20 d418845d 本地完整门禁与新托管失败

干净候选d418845dc62dfb9d24a04927d0fd030bedfa8ec8的本地windows-debug完整run为e415f572-eeed-4e05-b691-bf6849cdcc50：Debug全量构建exit0，C++1415/1415、427088断言、0失败0跳过，Lua主/隔离147/147及56/56；CTest48项中47通过，仅预声明可选AI服务跳过，0失败，总耗时555.61秒。全部11个profile检查、runner/collector/strict verifier均exit0，源码与夹具前后不变。原run.json摘要609427598e9e3793dfe9bc335095393d4615585cbb9ef54a17e2e8f4e6aeeeed，位于u22-foundation/candidate-d418845d-01/；collected bundle在主目录u22-evidence/d418845dc62dfb9d24a04927d0fd030bedfa8ec8/e415f572-eeed-4e05-b691-bf6849cdcc50/windows-debug/。

前次fd5的Linux最终包下载审计也已完成：固定artifact10590495447与10590875197的原ZIP分别为82473381B/SHA256 af0721dfb20f0a91d0d9fa3cd2ac0a2b3d9288126b23bba17b8a61a1171a0f8e、37696061B/94f8e98104cb32b339fab9767898602048f14a9d419e0970f43da0f84a803711。69个引用全部重新计算，TGZ和AppImage与前述accepted收据字节绑定；两个包各五阶段、各七个受控命令全部exit0、无timeout/forced kill、cleanup COMPLETE，AppRun到Engine身份已验证。实际PCM四会话非零，但物理设备与AppImage FUSE均NOT_RUN；此次是原托管证据审计，没有本地重放。详见u22-hosted-fd5b426b/linux-audit/audit-01.md，摘要7672b1a24908350cd3d81f46a9a9171bf6027f8c88c9769dc3919f6b5492d05c；JSON摘要5fa5d04875247962c4a089c37701de4c41c31aa59bc8f87054c9d0751fdc4cef。原审计中隐式父目录假设失败文件保留，后续按实际提取合同纠正审计，不改包字节。

新CI [35463706218](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35463706218) 的API head为d418845d，实际PR merge源码192dee12f1d95e45234a353ac3d18391f6da442d。最终11项中8成功、2失败、1跳过，整体FAIL；原状态及job日志在u22-hosted-d418845d/。

- Windows Debug、Windows Release、Linux GCC及Android/iOS编译/静态探针成功。Linux最终包job105954909268及Windows最终包job105955637427成功，原job日志均返回lane PASS/accepted=true及UPLOAD_READY；新包artifact分别为10590608319、10590338730，证据artifact分别为10590793009、10591054183。当前仅核job原日志，不能冒称已下载新包逐字节审计，也不把fd5下载结果贴到本次源码。
- macOS Clang job105951998121的CTest只有package_runtime失败：35项中same-PID exec正控和launch-script-change负控未发布mapping/process.json，两项失败；其他native/Web runtime维护入口通过。此问题不同于前次spawned-child夹具；旧等待函数未读取Future原异常，且临时目录已清理，因此原job无法区分观察期限、映像或启动错误。下一修改先保留真实worker异常和受控命令诊断，原deadline不放宽；macOS Package因前置失败跳过。
- Web最终包job105954909284失败。固定artifact10591150104已下载并复算8780027B/SHA256 f3b16dbea036faf0d7d8ce14e5c0aecaccd299707a344bb604910dc5bd177be7。Chrome152.0.7977.82已经真正启动，boot PROBE_PASS，UI保存/读取、包资源及短TMP清理通过。随后同一owned target的Service Worker离线重载保留存档，但initial.online=true，在原严格离线断言处PROBE_FAIL；root场景FAIL，subpath未到。38个已上传runtime文件及3个顶层摘要已核，原boot/offline两份cdp.jsonl未被旧workflow通配符上传，不能声称读到原wire记录；上传清单现补.jsonl保留后续原始命令。仍须真实对照与新候选验收，不能仅据协议文档宣布修复。

PR25保持draft，三桌面与Web整套尚未通过。原本地PASS、Linux/Windows托管成功与macOS/Web失败分别保留，不重试覆盖旧结论，不授予发布或设备能力。


### 新候选的诊断保留与 CDP 兼容性增量

macOS两个exec夹具的原失败仍未定位到生产根因：先前等待process.json的断言掩盖了工作线程原异常，临时目录随后被清除。测试现在在Future完成时传播真实异常；失败时仅保留白名单run/result/process JSON及每项上限64KiB的测试日志，记录SHA、截断状态和4KiB尾部，不保存request.env。pending Future使用有界清理钩子在owned进程结束后、临时目录清除前补收最终原件。原生产package_runtime.py和时间限制均未改变。实际POSIX受控RED证明旧等待吞掉Launch child mapped an undeclared executable；新回归将其原文抛出。完整Windows35/35、8.341秒，WSL40/40、8.371秒，0失败0跳过；测试冻结SHA256为3156a09c1e6e6e4e04a3c583bf4a8d85761d114ad013680d545f0fa7eb44d545。另有一个预期ERROR的真实unittest输出正控，证实原异常、stderr和收据已输出且secret哨兵未输出；该故意失败不计为产品通过。证据在u22-exec-fixture-race/observation-02/，主代理已审增量，无可行动发现。真实Mac结果仍待新CI。

Web probe迁移到同一页面session中的Network.emulateNetworkConditionsByRule与Network.overrideNetworkState，分别显式控制传输阻断和navigator状态。必须得到一条已确认规则并在重载前读到online=false；任一接口错误、不支持、缺rule或状态未变均失败，不自动回退。重载后的严格offline、SW文档和响应字节合同不变。新增before/after_override/after_reload诊断，并在托管上传中包含原先缺失的cdp.jsonl。七项旧源码RED02全部失败，新实现7/7通过；完整Windows33/33、9.848秒，WSL33/33、16.088秒，均0失败0跳过。首次RED01中六项fixture等待错误已原样保留，修正fixture后才取得直接合同断言的RED02。

生产probe SHA256为8cdcf53a061f854e478649f6aca78cb1f3df0cbc4e50f0621fdaa7086dcc9c13，测试为c84b6cc7b970d5ebb6fc2ce4d2948152782f87dd37802c05908c23624910f920；冻结收据u22-hosted-d418845d/offline-controls-freeze-01.json摘要f4a77b5c91ac5958988dd2346eab62d6d70de5f240213928bed7b3f53b0974a2。私有真实Chrome153的连续session和断开后重连同target对照中，旧接口与新组合均通过；因此本变更只称兼容性迁移，不能声称复现或已修复托管Chrome152的原问题。对照确认package server已退出、SW可供字节、浏览器网络请求被阻断而独立存活HTTP正控仍返回200；所有owned进程已清理。首次辅助模型SystemInfo路由错误的FAIL独立保留。下一托管运行必须给出Chrome152实际结果，U22未据此验收。

Web兼容性两文件独审无可行动发现，逐项重算冻结源码/日志/实际Chrome对照原件；报告offline-controls-independent-review-01.md摘要dc51fa29e836c6796012251289537d44d601350d029213ad8dfb38078c0cd831，JSON摘要d5a13a3ef698bbef2a55b69c3e9d97db4b75ca0be47f239b49752c46593ca502。独审未启动新浏览器，原托管问题仍等待下一次真实包CI判定。

## 2026-09-20 016f77f7 托管终态与两项修复候选

本节链接到的 `artifacts/validation/` 原始证据位于本工作树忽略目录，不随 Git 提交上传。[最终 API 状态 state-05.json](../../artifacts/validation/u22-hosted-016f77f7/state-05.json) 记录 run `35467654286` attempt 1 整体 failure：9 个成功 job、2 个失败 job、0 个跳过 job。PR head 为 `016f77f7af611c83bdac87c6dae12fd1a877d6ea`，原 job checkout 日志及包报告绑定的实际 merge 源码为 `cc258c6ae9293a24eec82ae7bfe7fbcf155bef2b`。

Windows Debug/Release、Linux GCC、macOS Clang 和三个移动编译/静态探针均成功。[Windows Package 105966018617](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35467654286/job/105966018617) 与 [Linux Package 105965941442](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35467654286/job/105965941442) 的最终格式验证、上传前复核和上传步骤成功；本次未下载审计它们的 Windows ZIP、Linux TGZ、Linux AppImage 原件，因此这里只确认托管步骤状态。此前 `fd5b426b` 的固定 ID 下载审计继续仅属于原候选。两个失败是 [Web Final Package 105965941484](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35467654286/job/105965941484) 和 [macOS Package 105964618274](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35467654286/job/105964618274)。

### Web：已取得 Chrome 152 原始重载记录，153 仍待实际 Linux 验收

固定诊断 artifact `10592760182` 的原 ZIP 为 8,853,666 B，SHA256 `3e70b1dd8e87f3a42b444e60206451d26ea2affa92ea8c370edb9c774270cd88`，见 [artifact 元数据](../../artifacts/validation/u22-hosted-016f77f7/web-artifact-01.json) 与 [原 ZIP](../../artifacts/validation/u22-hosted-016f77f7/web-transfer-01/artifact.zip)。[原始审计](../../artifacts/validation/u22-hosted-016f77f7/web-offline-audit-01.json) 绑定实际 `Chrome/152.0.7977.82`：同一 session `7EC018C65787C9F2EC1CA0ABB0DE45BA` 先发送 `Network.emulateNetworkConditionsByRule` 和 `Network.overrideNetworkState`，得到非空 rule ID 后观察到 `online=false`，随后 `Page.reload` 的文档却为 `online=true`。本轮原 `offline/cdp.jsonl` 已保留并重算 SHA256 `0ee322777a335a9eea2b5f9988690c807c6d04e1851dcc5f9261a796d405765d`；报告摘要为 `c311beb495232f2f10e6e09ca56d11d0617f6f8d5ca7d6e208043f2b743d4f57`。这次实际记录证明前一轮迁移到新 CDP 接口仍未让 152 的重载状态满足严格合同，不能把接口调用成功当离线通过。

失败发生在最终目录的 root 离线场景，子路径及最终 Web ZIP 验收未到。offline probe PID3578 实际退出 1；HTTP server PID3071/port39615 受控停止、退出 -15，Chrome PID3075/port38825 受控停止、退出 0。原收据均无 timeout/forced kill，owned cleanup COMPLETE，两端口已关闭；本次失败不是未回收进程造成的通过假象。

已保存精确版本的 Chromium 上游源码与来源元数据：[152 源码](../../artifacts/validation/u22-hosted-016f77f7/blink-network-agent-152-01.txt)、[152 来源](../../artifacts/validation/u22-hosted-016f77f7/chromium-source-152-01.json)、[153 源码](../../artifacts/validation/u22-hosted-016f77f7/blink-network-agent-153-01.txt)、[153 来源](../../artifacts/validation/u22-hosted-016f77f7/chromium-source-153-01.json)。两份源码 SHA256 分别为 `9026665eb122d3a335ded423519c63c73c8c15c0a0dc6b126d5ee8b25a690d8d`、`29f17657e2df2a3725a0dc9fdca612bd47ba032453c7be33f7ca9b72de8441a7`。152 的 `overrideNetworkState` 未保存这些网络覆盖字段，`Restore` 也没有对应重施加；153 的 2631–2635 行保存 offline/latency/throughput/connection type，837–852 行在 `Restore` 重施加。这个差异与本轮重载观测相符，支持选择具有持久化实现的版本，但没有替代真实 Linux 浏览器验收。

修复候选固定 Chrome for Testing `153.0.8010.52` 的官方 Linux64 归档；[下载原记录](../../artifacts/validation/u22-hosted-016f77f7/chrome-linux-download-01.json) 为 195,708,470 B，SHA256 **`e66f66d4802a46d4a022667e668aa950e277cadbfbed4b3777915b47413a0ef9`**。工作流先验证归档再解压，固定传入所选浏览器路径，并保存实际 binary 摘要、version stdout/stderr、退出码和 selection PASS/FAIL；没有自动替换浏览器或放松 offline/SW 断言。[版本选择独审](../../artifacts/validation/u22-hosted-016f77f7/browser-pin-review-01.md) 的初始 P2 是非零 version 调用丢失原 stderr，修订后已闭合；该独审属于静态接线检查。

[WSL 准备原记录](../../artifacts/validation/u22-hosted-016f77f7/prepare-linux-chrome-01.json) 锁定解出的 executable SHA256 `328fbee82d8e58b05a755b2343abfd192d92ca7066353cb357fad389bc7e3989`，但 `--version` 实际退出 **127**：`libnspr4.so: cannot open shared object file`。因此本地没有 Linux Chrome 153 启动或包运行 PASS，也不能把这台 WSL 的缺依赖推断成托管 Ubuntu 同样缺失。候选仍需新托管运行证明精确浏览器启动、root/子路径、重载离线、SW 字节、网络阻断与清理；原 152 失败继续保留。

### macOS：正常退出的 Engine 被来源检查拒绝，路径转义解释仍为推断

固定诊断 artifact `10591659417` 的原 ZIP 为 14,729,574 B，SHA256 **`8b99ae8848870299032692ef065b514ebb08997532060c1127bc2bfd00b75c5d`**，匹配 [artifact 元数据](../../artifacts/validation/u22-hosted-016f77f7/mac-artifact-01.json) 与 [原 ZIP](../../artifacts/validation/u22-hosted-016f77f7/mac-transfer-01/artifact.zip)。[Mac 原件审计](../../artifacts/validation/u22-hosted-016f77f7/mac-package-audit-01.md) 及 [逐项摘要](../../artifacts/validation/u22-hosted-016f77f7/mac-package-audit-01.json) 绑定最终 TGZ SHA256 `d38dbcabffc811bbd7f95b7bac191689c25db434c93fec7b415e9d7eb14c4678`；21 项命令日志/收据引用重新计算一致，source/runtime-copy/created-game-copy/evidence 稳定。TGZ 准备成功，两个 editor、原始 demo 和 author create/build 阶段通过；`created_game_frames` 因 `A second source for required library was observed: libSDL3.0.dylib` 被拒绝，最终 `Required loaded-library provenance is NOT_VERIFIED`、`accepted=false`，后续 DMG 未到。

失败阶段运行本包新建游戏的 `作品 输出` 目录，argv 含 `--frames 60 --audio-output software`。实际 PID31150、creation `1789850946:571402`，Engine 初始化 Metal 并运行新建 KAG 项目，最终 EXITED/exit0、无 stop/timeout/forced kill、cleanup COMPLETE。原 stdout 的真实软件混音为 76,416 frames、152,832 samples、43,490 nonzero、0 nonfinite，48kHz stereo；physical output 仍 NOT_RUN。来源观察先失败，后续 created-game 音频验收未到，因此这些事实不能将整个阶段改判为 PASS。

旧 observer 把 lsof 的 `n` 字段直接当路径；C locale 对中文祖先目录的字节转义可能保留 SDL basename、改变全路径，进而触发同名异源拒绝。**这是 INFERENCE，不是已证实的该次托管根因。** 原失败未保留被拒绝路径或 lsof stdout，不能排除真正第二份 SDL，也不能用后来的重跑填补这次观察缺口。

[真实 Linux lsof 4.99.4 wire 控制](../../artifacts/validation/u22-hosted-016f77f7/lsof-wire-control-01/report.json) 确认 C locale 转义中文/emoji，`-F0n` 只改变分隔符，UTF-8 locale 仍转义字面反斜杠；`-Di` 不受该版本支持并退出 1，原失败保留。[第二组 wire 控制](../../artifacts/validation/u22-hosted-016f77f7/lsof-wire-control-02/report.json) 又确认部分控制字节与合法字面 caret 名称具有相同输出，无法唯一还原，修复选择拒绝这种歧义。Linux 控制没有冒充 macOS 本地执行。

生产修复显式设置 `LC_ALL=C`、捕获原字节，校验 PID 和字段结构后仅做一次严格 lsof 转义解析。字面双反斜杠保持字面路径；未知/残缺转义、NUL、C locale 非 ASCII 字段、caret 歧义均拒绝。原 PID/creation、文件存在及摘要、包内路径、未知 observer、混合/外来同名 SDL 拒绝合同未放宽。成功和失败观察均保存工具路径/摘要、argv、受控环境、退出码、带大小和摘要的原 stdout/stderr base64、原字段及可得的规范化路径；失败继续保留原错误类型/文本，超时保留已收到的部分输出，便于后续托管诊断。

原测试先取得 RED：5 个方法出现 5 个 subcase failure、1 error；额外失败诊断/caret 控制 2 个方法出现 2 failure、2 error。冻结源码上的实际 GREEN 如下，均 0 failure、0 skipped，原失败未覆盖；命令退出 0，源码前后摘要一致。

| 实际脚本范围 | 通过数 | 测试耗时 |
|---|---:|---:|
| Windows 定向 lsof 与既有 mac mapping 控制 | 17/17 | 0.703s |
| WSL 定向 lsof 与既有 mac mapping 控制 | 20/20 | 0.406s |
| Windows native package runtime 完整脚本 suite | 51/51 | 46.315s |
| WSL native package runtime 完整脚本 suite | 52/52 | 29.350s |

保留的真实 Linux 输出另经生产 parser 读回，20 字段中 14 项精确还原、6 项歧义拒绝，见 [wire 读回结果](../../artifacts/validation/u22-hosted-016f77f7/mac-lsof-retained-wire-readback-01.json)。这些脚本使用真实受控小进程和文件/摘要检查，但 Engine/Lua 协议夹具不证明真实 Engine 包通过。源码冻结 SHA256 为 `7395b3b8a2b113c1fb10fa96e8dfff1ea021c516c1ab8fa3cf333a46ff0ebe0e`，测试为 `16ba88fd25d8522d75afe155ff2c1566b3597fed063b4b73e2001b433bd48874`；[交接](../../artifacts/validation/u22-hosted-016f77f7/mac-lsof-handoff-01.md)、[69 项锁定清单与 RED/GREEN 原日志索引](../../artifacts/validation/u22-hosted-016f77f7/mac-lsof-freeze-02.json)、[精确差异](../../artifacts/validation/u22-hosted-016f77f7/mac-lsof-changes-01.patch) 均已保留。该修复未执行实际 Mac Engine、TGZ/DMG，也未替换原失败收据。

两项候选由主代理统一审查、提交并执行适用完整门禁；需要后续真实托管包结果和当前最终产物的精确字节审计。本文没有重新构建、重跑 CI、变更服务器设置、发布或部署；U22 整体继续开放，旧本地通过、旧候选包审计、本轮托管失败与修复候选分别保留。

主代理随后独立核对完整生产、测试和工作流差异，69项冻结原件逐项重算匹配，无剩余可行动发现；最终actionlint1.7.7实际exit0。审查JSON [candidate-independent-review-01.json](../../artifacts/validation/u22-hosted-016f77f7/candidate-independent-review-01.json) 摘要 `eedf0aeee37e6115bb638cec0554fb704835b5c95104f0c548abbe240c65874d`，当前workflow摘要 `a757cf5148baab00ff4a660ac04d46a3f0ae0c12f95839cc7825c0e7cf192f77`；[审查说明](../../artifacts/validation/u22-hosted-016f77f7/candidate-independent-review-01.md) 保留实际Mac/Linux浏览器未验边界。完整新候选门禁仍另行执行。
