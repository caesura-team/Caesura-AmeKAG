# U19 目标能力与作者声明执行记录

开始日期：2026-09-12；当前更新：2026-09-13。唯一计划U19，底层优先、Studio暂停。独立分支codex/u19-target-capabilities从U18冻结候选2552e225开始，已整合U18合并提交9b269ab7。最终代码候选2dfe451e完成严格原生门禁、完整Web531/531、实际浏览器与桌面包启动正反验证，候选CI七个执行job通过。PR #22待最终文档提交的CI及合并；本记录不把U20–U29或整个计划标为完成。作者使用方式见[目标能力指南](../guides/target-capabilities.md)。

## 行为合同与实施顺序

能力描述区分supported、approximate、unsupported；这些是实现语义，不替代本次资源加载、真实播放或设备/账号验证结果。静态分析无法判断的动态路径单列not_proven，不以“用了Lua”拒绝整个包，也不从未执行分支推全路径通过。

沿用caesura.project.json中的可选capabilities对象，字段为required、optional、accept_approximate，内容为已知feature键数组。拒绝拼错、类型错误、重复及required/optional冲突；近似须由作者明确接受，声明不会改变宿主真实状态。可静态确认的能力调用默认必需，作者列为optional后才允许带诊断降级；普通skip-check不绕过必需能力验收。项目元数据既有保存入口必须保留新增声明，不扩展Studio功能。

1. 通过真实Wasmoon、backend、runner及DOM建立现有Web后处理虚假支持、粒子0句柄所有权、palette实际应用却返回失败的行为回归，并保留video拒绝无效句柄及普通场景正控制。
2. 新增config/runtime-capabilities.json和小型解析/解析结果模块，复用ks_check的tokenizer/schema分析命令及已知参数。动态kind、资源格式、iscript等分别保留可确认的基础需求和未证明部分；不运行作者脚本来做静态扫描。
3. 现有Web/native宿主和Lua backend接入同一feature目录。原生使用实际生效的构建配置与当前BackendRegistry实例，不能仅凭SDK目录、请求开关或函数存在报支持。FFmpeg OFF仍有pl_mpeg；Live2D KAG只写状态的命令不能因SDK ON升级。必要的查询接口变更须同步全部实现和测试。
4. check、桌面build/package与Node Web打包在最终输出前执行能力校验，并将目标、目录身份、必需结果、可选降级和动态未证明范围写入结果。未知输入不默认为supported。
5. 命令及直接动态backend调用在实际执行前再次检查；无实现不得登记有效资源或假成功。保留旧第一返回值合同，通过统一结果描述明确应用、近似应用、不支持与资源失败。清理操作实际恢复中性状态。

预期改动在计划列出的CLI、Lua shim/命令、Web bridge/renderer与其真实测试；桌面caesura_build.py、必要原生绑定/纯查询接口、CMake有效构建信息及项目元数据保留属于同一合同的接线。先完成有界回归再实施，共享构建由主代理负责。覆盖率尚未测量，SDK/真实浏览器/其他设备证据分别验收。

## 已完成的元数据回归切片

基线完整Debug构建u19-baseline-build-01退出0。在未修ProjectService上新增实际服务/实际临时文件回归：u19-metadata-red-01为6例中1通过、5失败，102断言中36失败；无capabilities旧项目正控制通过。集成仅metaGet/metaSave的声明保留修复后，u19-metadata-green-02为6/6例、130/130断言通过；过滤的1378例未选中，不是全量结果。

首次复制候选保留了较早的文件时间，MSBuild增量未重编译ProjectService；u19-metadata-green-01仍执行旧二进制并失败，原始记录保留。重新以当前写入时间保存同字节源码后，green-build-02实际重编译/链接，green-02才是通过证据。源码候选SHA256为DA1B0E5FAA0EFBC2E529C8F77E87044D0D7562FFAE2E88871BF63B97BF186F9D。

GET保留磁盘声明，旧客户端保存普通字符串不会擦除声明，请求不能替换/引入capabilities；包括错误类型/null的声明也保留给统一预检拒绝。既有元数据文件无法解析或非对象时，保存返回错误且不截断文件。本切片未新增Studio能力编辑协议，未实施并发CAS/原子文件替换；U19整体及完整门禁尚未完成。

## 能力目录与宿主事实接线设计（初始设计，实施证据见后文）

初始目录只覆盖此次已确认差异：render.postfx.bloom/vignette/lut/softblur/lut3d、render.particles、render.blur、video.play、video.ffmpeg、audio.play/audio.fade/audio.crossfade、live2d.cubism、kag.live2d_motion/kag.live2d_expression/kag.live2d_lip_sync、steam.achievements/steam.stats/steam.cloud。Lut3D在Web的状态是approximate/css_fixed_grade，其余未实施Web后处理为unsupported；不把固定CSS滤镜叫作实际采样作者LUT。无接线的KAG Live2D三命令在两目标都为unsupported，独立于Cubism后端功能。

构建资格与本次运行可用性分别输出scope=build/runtime。原生计划提供进程启动前的--capabilities-json纯查询，以有效CMake发现结果生成的构建常量输出目标平台/目录身份/实际编译SDK条件；CLI绑定所选二进制hash，不读取另一build的CMakeCache。该查询不创建GPU/音频/Steam实例。运行中的Engine查询经BackendRegistry读取当前接口，render复用isInitialized/isPostFxSupported，audio/animation/Steam若缺少真实可用状态则新增窄纯虚查询并同步所有实现/替身。不会以名字为判据，也不会从配置阶段结果推断真实播放、SDK账号或设备验收。

能力目录生成单一Lua数据模块，包含JSON来源digest，作为正常scripts资源发布并在sandbox锁定前按已有preload规范可用；避免锁定后额外开放config目录。专用capability_json.lua解析项目与机器报告，保持独立模块名以免改变SMA现有json.lua搜索偏好。预检复用tokenizer/schema，在原始选择参数上识别动态表达式后才做静态转换；运行guard位于实际参数解析后、handler写状态前。目录外路径与动态Lua记录not_proven，不能统计为完整覆盖。

## 原生能力查询与严格JSON增量（2026-09-13）

原生--capabilities-json回归先在旧二进制实际失败（unknown option，u19-native-capability-query-red-01），新二进制u19-native-capability-query-green-01通过：三个SDK有效编译条件均为false、目录digest匹配、仓外临时CWD没有新增文件、没有初始化后端且执行器hash未变。只代表选中foundation/OFF二进制的构建事实。

三个窄纯虚查询已同步全部实现/替身：音频实施会话、Cubism实施会话、Steam实施会话；Null和PNG回退不报告相应能力。Engine运行查询每次经BackendRegistry读取当前状态并返回新表，不持有跨会话backend指针，异常只返回固定reason。独立审查发现粒子仅判已注册会误报；真实ParticleSystem在ready-render边界下的未初始化/失败初始化/关闭三个状态先失败（u19-native-query-review-red-01，11断言8通过3失败），增加现有isInitialized检查后转绿。

首次整合构建u19-native-capability-build-01因测试INFO宏表达式编译失败；第一次补回归red-build-01误继承final NullRender也编译失败；保留两份编译诊断，改用既有Test::RenderDevice边界。最终完整Debug构建u19-native-capability-build-02退出0，定向u19-native-capability-green-01为7例/49断言全通过，1384例过滤未选中。含真实SoLoud ManualMix初始化/关闭/重启，但不是物理声卡证据；SDK ON/模型/Steam账号路径未测。没有全U19通过声明。

严格JSON模块以独立capability_json名字集成，不改SMA的json.lua解析优先级。两个真实行为RED（用户table __eq、数字__tostring）和快照保存在主工作区u19-json-staged；最终候选及U19真实Lua执行均246/246断言通过（u19-json-integrated-01）。模块保留[]/{}与null、拒绝重复key/非法UTF-8和surrogate/错误数值/循环/洞，限额1MiB、单字符串256KiB、64层、65536值；这是codec合同，不是U19策略/包/运行验收。新增孤立套件注册、kag/init预加载及90模块Web索引。19键目录生成Lua/JS纯数据，CRLF归一化后digest供选中二进制和未来包预检对照。

## U18完成接入

U18 PR #21已于2026-09-13 01:32（Asia/Shanghai）按既有管理员合并授权合入master：9b269ab7e555ffb623797aa1372b39f697a3b633。最终CI34705495414七个执行job成功、三个PR不执行的发布包job跳过；PR head e2ba5c55与测试合并候选2db9a617及实际合并提交的tree均为21ff4b3fe6c81808c14e609efdf63fc1114d65d5。主工作区与本U19工作区均已快进该合并，U19未提交改动保留。

Linux完整Web511/511零跳过；Windows Web480通过/32按缺产物条件跳过，不能标作完整lane。最终Linux native receipt109a5e8c-b787-48b8-951b-52ba6d02f265：1364/1364 C++、402080断言零失败/零跳过，Lua147/147+48/48，CTest31通过+1既有可选AI跳过，11命令exit0，源与夹具fingerprint前后稳定。receipt dirty=true，CI只按既有diagnostic模式验证；不升级为干净发布候选证据。完整zip下载未完成；从已下载前缀恢复25个完整CRC校验条目，其中23个原始log/XML散列又与run.json逐一匹配，未声称本地完整bundle strict验证。原U18干净Windows2552e225严格验证凭证、最终各平台CI和本地Web510/511性能失败分别保留。

## 静态声明与打包边界

新增纯策略模块识别19项能力，`Schema.validate_static`在不执行作者插值/变量函数的前提下核对静态合同。有效原生配置、Web固定CSS近似、optional降级及动态未证明范围分别记录。策略241项初始通过，source_files形状增量后259/259；静态schema47/47。两个schema/policy的真实RED分别保留，不把测试注册数当覆盖率。

`check --target`、原生build/package、Node Web打包都接入实际Lua预检；`--skip-check`不能绕过能力要求。原生输入清单绑定场景字节与目录集合、项目声明、复制用Lua及所选引擎；最终复制再核对。预检审查发现输入未绑定、目录过滤差异、来源字段暴露绝对路径等问题，已用场景变更、声明变更、新增场景、实际复制损坏及忽略目录正控制闭环，记录在u19-preflight-review和u19-author-preflight等工件中。

Web profile在Vite buildStart捕获源集合，对照实际模块并在生成profile前核对来源未变。复用构建必须同时匹配当前来源和实际产物；Lua、JS、Wasm、脚本索引、Service Worker及manifest均按其对应范围绑定。最终包沿用已验证来源，并验证实际复制字节，只允许预先计算的项目HTML注入。它不声明游戏资源完整、npm全目录复现或全动态路径支持。

Node独立审查有三项实际发现：HTML声明注入检查晚于旧输出删除；旧dist只核对自身而不核对当前运行时源码；遗漏顶层Service Worker。前移验证、source_files和SW清单分别修复，helper真实文件系统回归21/21。首轮真实Vite因虚拟模块`__vite-browser-external`被当作源文件而失败，closeBundle错误又掩盖原始异常；增加成功写bundle标志并仅识别Vite的确切虚拟ID后，真实构建通过。没有放宽真实项目模块来源检查。

正式Node CLI新增9项场景，覆盖nested/entry/skip-check、旧输出保留、optional实际包、旧源码或篡改SW、最终复制损坏不给成功profile。已有测试因未声明的示例淡出和复用未标记失败输出而先失败；示例作品补上显式元数据，复制变异测试每次清理自己创建的输出，原变异oracle保留。失败日志u19-node-cli-full-01/02保留，最终完整结果以后续候选门禁为准。

## 运行时与资源结果

原生Engine profile和Web捕获provider共享目录。宿主provider只捕获一次，状态每次重新查询；项目策略私有且原子提交，返回表修改不能改宿主事实。普通无host组件仍可执行旧路径，但查询不报告已证明支持。适配层初始79项通过，Web还用两个实际Wasmoon实例验证策略隔离。

标签与公开kag表共用调用守卫；动态`[iscript]`中直接调用未接线Live2D三命令的4项真实RED已经转绿。首次包装打破`bgm == play`别名，主Lua套如实失败；弱缓存保留函数身份，调度时解包后只检查/调用原handler一次，主套随后147/147。原生公开VFX创建也在配额申请前检查粒子和渲染会话；3例RED/20个失败断言转为3例77断言通过，合法ID0的真实CPU所有权仍验证。

后端操作保留原第一返回值，以第二值区分applied、approximate、unsupported和failed。Web无实现的复杂后处理、视频、粒子不再登记有效资源。Palette应用和粒子创建先确认返回值再发布owner；weather先准备新emitter、失败保留旧owner。

cleanup审查发现optional淡出会连停止BGM一起跳过：正式51项中14项RED。现在被拒绝的fade先归零并执行停止一次，再保留optional结果/required错误；直接backend.stop同样执行清理并返回fade的unsupported结果，51/51通过。第一次GREEN因未定义raw_next失败，原始39/12日志保留，修复后才计通过。

Palette后续审查又复现夜间模式在应用失败时仍宣称成功，以及clear失败后销毁活动纹理。原有73项正控制保留，新增至123项，RED75/48到GREEN123/123。clear/day/night确认ACK后再发布状态，toggle失败返回实际旧模式，unload保留未能解除引用的纹理。Web set_palette(0/nil)清理别名也允许在撤回近似接受后清除效果，2项实际DOM/Wasmoon RED到GREEN；最后target-capabilities20项与flow68项共88通过。

## 完整验证进度与证据边界

首次完整Debug构建u19-native-integration-build-03退出0。随后C++全量为1397例中1393通过、4失败、0跳过，402478断言中4失败。两例纯stub派发、一例纯Lua代理路由和一例未初始化粒子clamp夹具的前提与新合同冲突；修正夹具、保留原oracle并加强实际配置/owner断言后，7例40断言定向通过。尚不能把该定向结果写成1397全量通过。

首次完整Web管线u19-web-full-01包含24场景/6资产bake、Vite与全Vitest，39文件528/528、零失败零跳过，102.77秒。工作树指纹46335a10403095f4b7a9cae00e00bdc75217a4f4a1283f371b0ebd1c250dcbd1前后相同，Node22.23.2与实际Lua二进制hash未变。原性能阈值未修改；故事median886.9ms，1000行样本median1594.5ms。这一结果不解释其与旧本机样本差异的因果，也不替代U27独立进程/Release/长跑验收。

现有Web画面与存档夹具读取示例作品的显式能力声明，断言真实降级诊断；没有把省略的后处理当作已实现。Audio UI通过受控AudioContext边界检查真实AudioEngine owner与DOM（5/5），不是物理解码或声卡验证。实际浏览器、最终原生receipt和候选CI仍需记录。未测量覆盖率；接口、SDK OFF、Null、ManualMix、Wasmoon/jsdom和真实GPU/浏览器的证据不得互换。

干净候选36b15aae的u19-native-candidate-01真实receipt为fdf94dce-d959-463e-836d-0f5998859f34：完整Debug、C++1397/1397（402490断言）、Lua147/147与54/54通过；CTest为30通过、2失败、1项预声明AI跳过，源/夹具前后稳定。失败分别是build CLI诊断丢失ks_check/普通lint跳过提示，以及显式无效Lua路径在核验前先查询另一无效引擎、导致路径验证超时。恢复准确且有边界的诊断提示，并在任何外部进程前验证Lua选择后，原有CLI35/35与路径12/12通过；不修改旧测试断言，也不允许skip-check绕过能力。此修复仍须进入新的完整候选门禁。PR #22已作为草稿启动CI，未合并。

## 最终代码候选验收（2dfe451e）

- u19-native-candidate-02的receipt为704c6ccd-6d40-48e0-a597-4a310d40b8a7：11/11命令exit0，完整Debug、C++1397例/402490断言零失败零跳过，Lua147/147与54/54，CTest32通过及1项原先声明的可选AI服务跳过。dirty=false，源码与夹具前后稳定；收集后严格verify_release_candidate通过。该结论仅覆盖windows-debug profile。
- u19-web-full-02：bake、Vite及39个测试文件全部通过，531/531、零失败零跳过，104.01秒。源码2dfe451e且dirty=false，指纹210184e31d3ab16921436612fbc2d243c33a9311b66829451321c9b4d4074968前后相同，Node/Lua二进制未变化。原预算下故事median871.6ms、1000行median1570.2ms、配对2000/1000 medianRatio2.109。
- 实际Web包dist/u19-browser-candidate在Chrome153.0.8010.36、`/games/u19/`子路径、默认autoplay规则下14项通过：图文、本地Wasm、无CDN、存档刷新保留、真实WebAudio source，以及用户手势后suspended→running。截图u19-browser-smoke-01.png已目视核对。冒烟脚本的Chrome启动器退出后仍有属于本次独立profile的子进程，已对确切浏览器实例发送Browser.close并验证剩余进程数0；未操作用户浏览器会话。
- u19-native-boot-03用实际桌面包普通启动路径验证：相同引擎SHA256为6114b24fd5f55f90ab30f2f10ac1d1150d92e3df6515b702324b6228d2d6439e，正控制执行作者entry并exit0；负控制只在包的独立副本中声明必需video.ffmpeg，实际启动在作者entry前拒绝并exit1。设置六帧上限及隐藏启动窗口。之前probe01误用不自动启动作品的headless模式，probe02又请求了锁定后不存在的loadfile；两份失败保留，未将它们算作实现缺陷或通过证据。
- CI34715182298对应2dfe451e：Windows Debug/Release、macOS、Linux、iOS CMake probe、Android静态合同与CMake probe共七个执行job成功；三个PR发布包job按条件跳过。Windows Debug和Linux的真实Node包CLI均23/23通过，三桌面Lua均147/54，CTest33均无失败（可选AI跳过仍单列）。旧36b15aae的CI34714593601已由更新候选替代并取消，不能记为通过。

SDK ON/账号、其他浏览器与设备、仓外完整作者旅程和发行包验收仍按原计划继续。本轮没有测量覆盖率，没有把测试通过数或源文件清单当作覆盖率、发布批准或全动态路径证明。

## 合并后状态复核

最终文档候选716e6ee31ad7780a54c20eefce1d016536b483bd的CI34716396395七个执行job成功，三个PR包job按条件跳过。PR #22于2026-09-12T20:44:13Z合并为98aa63c0748c6ba9dc02e87515fe096312198e1c，树与文档候选一致。

随后master CI34717963659并非全绿：Windows Debug的Web synthetic1000帧吞吐1.976459未达到原>2预算，532项中499通过、1失败、32按该Windows lane条件跳过；Windows Release、Linux、macOS、两项Android及iOS执行成功，macOS/Linux包成功，依赖失败的Windows Release包未执行。此结果不抹去候选验证，也不能称master全部通过。原始失败日志保存在U20工作区artifacts/validation/u19-master-ci-34717963659-failed.log，U27继续核验性能，阈值保持不变。
