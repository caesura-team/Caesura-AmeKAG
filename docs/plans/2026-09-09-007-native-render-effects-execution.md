# U16 原生渲染实际效果回归 — 2026-09-09

当前单元仍在实施。唯一计划为 [运行时可靠性与交付闭环](2026-09-05-001-refactor-runtime-foundation-plan.md)；U16 的局部修复不代表 U1–U29 已完成。

## 已交付前提

U15 最终原生源码 `31c22afa4435a80a2b566e0128a0c436a294c172` 的完整 Debug profile 通过：run ID `3d7fd1da-5058-4bc3-9c9d-50721427f87f`，11 项检查退出 0，Cpp1325/1325、399990 断言、Lua147/46，CTest25 通过及 1 项事先允许的外部 AI 服务跳过。源码与夹具稳定，collector 和严格 verifier 均通过。完整 Web 的 `a699bc5c` 证据为511/511，后续只改文档的提交未改变这些运行时输入。

[PR18](https://github.com/caesura-team/Caesura-AmeKAG/pull/18) 首轮 Linux CI 因平台状态证据源码锚点陈旧而失败，原日志保留。仅更新该锚点及生成文档后的 `afd9e703485d1eabba8b26f7a44a589367136702`，候选 CI `34259069558` 的7项验证成功、3项PR打包按规则跳过。PR18 已合并至 `14b43ade5f2bc609680a9ac7edb9c2816b78ce12`；主分支 CI `34266968673` 的10项任务全部成功，包括三桌面打包。没有将初次失败改写为通过。

## 颜色填充的独立红绿证据

真实 D3D11 的34项填充回归先通过31项：首次颜色A和改变后的B错误，缓存A再次使用也错误。将栈上的单像素数据由 `bgfx::makeRef` 改为 `bgfx::copy` 后通过33项；再移除借用方对设备持有的缓存纹理的销毁后，34项全部通过，进程正常退出且没有资源诊断。生产修复分别为 `fd950bb8`、`59d96ab2`，没有改动阈值或顶点投影。

原始目录位于隔离工作树的 `artifacts/validation/u16-fill-{gpu-red-01,copy-only-gpu-01,gpu-green-01}/`。每阶段都保留对应二进制，不能用后来的可执行文件解释早期结果。

## 完整图像基线 v1

提交 `d873f9b91631c5b41e7924085b9e26af12d5a902` 固定了9个场景、D3D11和桌面OpenGL两个实际后端、每后端35张截图。基线工具使用生产 Engine、renderer、TextureManager、PNG decoder；父进程分别记录实际后端、票据、帧号、正常退出以及输入前后哈希。PNG/RGBA与预声明颜色区域由 Python 独立校验，字体参考由独立 FreeType face 和固定字体字节生成，不读取候选图像或生产字形几何。

driver 的目录替换、链接、输出路径和清单降级负例先失败再修复；加入实际探针 checkpoint 协议后共47项合成测试通过。这些测试仅证明验证器拒绝规则，不等于 GPU 图像通过。独立审查发现 `advance_x` 的 JSON 浮点与整数协议不一致，修复为带整除/范围守卫的真实整数度量，未改变任何像素期望。

完整 v1 执行 `artifacts/validation/u16-contract-gpu-red-01/run.json` 的结论是 **FAIL：3/18场景通过，15场景失败**。18个进程全部执行完毕，输入前后稳定；二进制 SHA256 为 `c22f0c89e482f144b9b3706337ed2bae009a3caed8d0600fbcbdf7aecce65026`，并保留在原运行目录。通过的场景为两个后端的核心 Fallback 缺失负控制和 D3D11 的可选转场缺失降级。

D3D11 失败涉及中文/注音、文字和贴图透明度、resize 后的文字缓存、转场进度/模式、非 identity LUT、核心 Blend 缺失时的公开就绪状态，以及 SoftBlur 缺失时的能力声明。OpenGL 还存在基础图形/纹理输出错误。捕获进程退出0只证明捕获协议完成，不能覆盖 Python 像素失败。

## 后续参考版本与实际运行

v2 保留全部 v1 字段，追加 `borrowed-after-clear`：清理效果后，将原四个 LUT owner ID 的纹理按1:1贴出，检查角点和区分 R/B 的内部点。共36张/后端。`u16-contract-gpu-candidate-01` 完整执行18个子进程，8通过、10失败，输入稳定；其失败包括动态字体图集无法更新、实际呈现尺寸没有改变以及GL程序参数未绑定。

v3 再追加4项真实后处理生命周期序列：删除最后stage、仅无效stage、begin后clear、有效swap后接无效尾stage。原36张不变，共40张/后端。`u16-postfx-lifecycle-red-01` 为10进程诊断子集；D3D11原10张LUT图通过，新增4张全黑，确认了视图重定向生命周期缺陷。修复后的完整 `u16-contract-gpu-candidate-02` 为15/18通过，两个LUT进程因真实drawable尺寸不匹配而失败，GL文字还剩ruby边缘不匹配，不能视为完整通过。

`SDL_SyncWindow` 单独没有解决4096x224请求得到4094x216的隐藏窗口尺寸问题；将探针隐藏窗口设为无边框后，`u16-lut-borderless-01` 的D3D11共14张LUT图全部通过，GL仍有实际上下颠倒。没有改画布尺寸或容差来回避该失败。

v4 明确字体为位图外补透明的完整双线性过滤参考，修正v1–v3参考实现额外裁剪半开字形矩形所遗漏的非零过滤边缘。字体、FT参数、ink度量、advance、origin、ROI和2/4容差均不变。另在RTT尾部追加上下异色图案，验证实际方向；原40张配方不变，共41张/后端。清单语义hash为 `87454abdd3448c49d271419d90b95217b84576ce9e1461a1d4073be0ed4ad064`；整个v3清单可通过撤去这两个已声明变化恢复，合成保存性测试核对完整hash。

`u16-v4-edge-orientation-red-01` 因探针仍拒绝v4版本而在渲染前失败；原日志保留，不是像素证据。更新探针版本检查后的 `u16-v4-edge-orientation-red-02` 执行6个诊断进程，输入稳定：两后端LUT各14张通过，D3D11 RTT 8张通过；两后端ruby均缺少过滤边缘，GL RTT上下异色颠倒。仅据此修复生产字形半源像素透明几何边框，以及RTT回贴方向；旧失败没有覆盖或重分类。

## 当前修复

- 字体按实际字符追加准备，保持既有图集位置；动态TTF纹理由可更新资源承载，避免D3D11 immutable纹理拒绝后续上传。透明padding保持白RGB/零alpha。注音按实际advance居中，尺寸变化使缓存几何失效。半源像素几何边框保留完整双线性过滤支持，ink/advance/斜体和删除线度量保持原值。
- 独立纹理调制程序逐次消费颜色/透明度，覆盖直接和批次路径。GL在链接程序前注册全部参数与sampler，解决其按名字一次性查找uniform时尚未注册的问题。没有将未出现的vendor trace当成运行证据。
- 真实呈现尺寸调用bgfx reset，逻辑/物理尺寸分别管理；显式物理尺寸在设备恢复后保留。恢复增量的实际正负控制见下。
- 转场/混合参数与CPU布局一致；LUT依据源RGB进行8点三线性插值，保留alpha。GL全屏几何按实际上传顶点，RTT采样按后端origin处理。
- 后处理begin重设主视图目标，commit保留绑定至真实advance；清空后仍复制本帧scene到backbuffer，查找实际可执行的末级，拒绝无效LUT参数。多级Bloom/复杂多pass没有因此获得完整图像证明。
- `shaderReady` 表示设备可渲染且核心程序完整；可选效果只有实际程序可用时报告支持。Vulkan新调制程序缺失保留显式能力缺口。
- shader生成器实际运行FXC和bgfx shaderc，37项编译/翻译、24项接口hash匹配，生成记录含工具身份、命令、输入与输出；5项生成器合成测试通过。Metal/GLES生成/容器证据不代替设备运行。

## 当前实际通过证据

`artifacts/validation/u16-contract-gpu-candidate-03/run.json` 的v4完整矩阵 **PASS 18/18**，D3D11和OpenGL各41张、共82张截图，全部票据/退出/像素/故障注入协议通过，输入前后稳定。对应二进制已保存在同目录，SHA256 `cb6706f3cf28fb293eb77ca8e21f9564eb835d13f9376dd33e05a6cb68dea0d7`。人工同时查看了GL ruby-CJK、RTT上下异色与swap16图像。没有用后来的probe解释早期失败。

显式呈现尺寸新增独立 `present-recovery` 场景：真实隐藏drawable800x450、逻辑640x360。仅移除恢复尺寸的两行生成负控制，`u16-present-recovery-negative` 中恢复前800x450、恢复后640x360，5/20项失败。按备份原字节恢复生产源码后，`u16-screenshot-gpu-green-01` 的该场景20/20通过，截图尺寸、已缩放的逻辑贴图区域、背景和恢复前后整图完全一致。没有在恢复后另行修正尺寸；不宣称真实显示器DPI切换或原生OS设备移除。

同一最终生产实现同时复验U15实际场景：renderer59/59、RPC77/77、fill34/34，四个进程退出0并完成shutdown。root驱动记录二进制/生产源码hash、完整命令、cwd与原始stdout/stderr。

当前driver合成测试62/62（v4初次因旧hash断言失败，修正版本断言后通过），不等同GPU或FT真实运行。C++定向10/10、291断言通过，1323项未被过滤器选择，完整发现数1333；完整执行结果见下。U16仍待候选CI与合并交付；U17–U29及其他未验收项继续保留。

## 首次完整原生门禁的旧断言失败

`u16-native-full-01` 在干净bed41306运行，源码和夹具前后稳定：完整构建通过；C++1333项发现、1331通过、2失败、0跳过，400251断言中3失败；Lua147/147及46/46通过。CTest28项中仅CaesuraUnitTests因同样两项旧断言失败，外部AI按预声明跳过，其余26项通过。完整日志保留。

独立审查核对生成receipt与当前Metal数组逐字节一致：vs_fullscreen现608字节、hash `2b69f8b35f310449956a1b6d800fd447dc7c4ba7b35002b498c2dddc71090bde`，来自实际shaderc成功输出，原659断言对应旧vertex_id实现。测试保留精确608及容器header并增加LUT VS/FS配对核对，不恢复错误几何。

FreeType原检查依赖 `font.ftLib` 局部拼写，而工厂已用unique_ptr的 `font->ftLib`。无全局Context的文件/引用检查保留；两条局部拼写断言改为两个不同字号的真实atlas同时存活、覆写释放调用者输入后准备新字形，并与独立FT度量/逐像素对照。销毁第一个atlas后第二个仍准备另一个新字形。用例数量没有减少，生产源码没有为这两项失败改变。

## 完整原生通过与CI脚本修复

`u16-native-full-02` 在干净 `805bb3d7063b0d1b39c5a487a0c495f6408a17b8` 执行，run ID `6856fcf9-686c-46db-8f32-94e5e971cbdc`，源码和夹具前后稳定。11项必需检查全部退出0：完整Debug构建、C++1333/1333与400300断言（0失败、0跳过）、Lua主147/147与隔离46/46、4项验证工具套件、耦合、测试注册及CTest。CTest共28项，27通过、外部AI一项按运行前配置跳过。collector与严格verifier均通过；证据目录为 `artifacts/validation/u16-native-full-02-evidence/<source_sha>/<run_id>/windows-debug`。独立增量审查没有可行动缺陷。

PR #19 的候选CI `34688500985` 在iOS源检查失败：`verify_metal_shaders.py` 只接受数字大小，而实际新生成数组使用 `sizeof(本数组)`。本地新增测试先复现5处失败，包括合法生成物被拒绝和旧检查漏报错误字节数、损坏字节、重复数组。修复复用生成器的严格数组解析，保留原10项渲染与2项小游戏资产范围，既接受匹配数字也接受正确sizeof目标，不修改着色器字节。9/9测试、原CI脚本和定向CTest均通过；原失败日志保留。sizeof声明只证明当前数组大小，不能独立证明其字节与编译器原始输出一致，后者仍依生成receipt和容器合同。

同轮macOS完整构建通过，CTest仅 `CaesuraRenderContractDriver` 失败：临时目录使用 `/var/...` 别名，其系统父目录链接至 `/private/var/...`，导致正控制在进入被测逻辑前被路径边界拒绝。Windows Release同样仅此CTest失败，但别名来自 `RUNNER~1` 短文件名。测试根路径改为创建后立即resolve，与生产run_suite开始时的一次规范化一致；随后恶意替换目录/链接仍按原词法路径固定和拒绝。本机62/62通过，生产输出边界没有放宽，CI主机实际通过须由新CI确认。

Linux本轮完整构建与CTest28项零失败，后续Android源码审计的8项旧要求失败：它仍搜索TTFState整段Unicode预加载和旧图集分配。按需图集已经替代该实现，源检查更新为实际所有权、尺寸/格式、UTF-8准备、容量限制、透明padding、动态上传与同图集缺字替代，保留总88项；本机88/88通过，6项移除关键源码连接的隔离负控制均拒绝。真实FT对照另扩充一般标点U+2014、CJK标点U+3002和全角U+FF21，加上原ASCII/Han/Kana共7个代表字符，6项字体用例239断言通过。用例数没有减少；固定字体28px的代表字符不等于整段Unicode或Android/GLES设备证明。上述最终增量未改变生产实现与shader字节，原生/GPU生产代码证据保持有效，最终候选CI仍须通过。

## U16交付

最终干净候选 `1458cf67b6868169ca35940eefa556e9cb046ddf` 的 `u16-native-full-03` 全部11检查通过：C++1333/1333、400327断言，Lua147/147与46/46，CTest27通过及1项预声明AI跳过，源码/夹具前后稳定。run ID `9d6b5263-9411-47c8-879a-4a081ee7a6b1` 的collector和严格verifier通过。

[PR #19](https://github.com/caesura-team/Caesura-AmeKAG/pull/19)最终[CI 34689313983](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/34689313983)成功：Windows Debug/Release、Linux、macOS、iOS编译、Android静态及编译/测试签名产物共7项成功；3项合并后打包任务按PR条件跳过。2026-09-12按既有授权合并为 `074f5f7c2e1aa958a488f7645692307361d29134`。合并后CI另行跟踪，不把PR打包跳过写成已打包。

U16的固定两后端图像合同和必要整合门禁已交付。上文旧失败保持原结论；Metal/GLES/Vulkan及未覆盖复杂效果/真实设备事件的边界不变。后续继续U17–U29和其他未验收项。

合并后 [master CI 34690073493](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/34690073493) 在074f5f7c完成，10项作业全部成功，包括Windows Release、macOS及Linux打包；这是实际合并后结果，不是PR条件跳过的重分类。
