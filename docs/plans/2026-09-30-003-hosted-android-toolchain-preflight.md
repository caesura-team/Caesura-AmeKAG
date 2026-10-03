# 托管 Android 工具路径前置诊断与 Windows 回执编码回归

状态：本次是保持严格拒绝规则的诊断增量。d880 托管 Android 的具体失效链接尚未由原件定位，后续一次前置 CI 观察用于获取该事实；不把本地夹具或上游布局推断当作托管实测。完整 U1–U29 仍在推进，没有合并、发布或标签变更。

## 已发生的两个独立失败

`d8801e073888e8a54ce94e0aafaebbabbe614265` 的托管运行 `36697982088/attempt1` 中，Android 作业 `109830563658` 已实际完成 SDL、OpenSSL、CMake 配置和原生模块编译。随后严格包适配器在约四秒内失败，原报告为 `Evidence path must not traverse a link`，`commands=[]`、`private_cleanup=NOT_CREATED`。没有执行 Gradle、签名或 v2 包验证，不能将该失败称为签名或原生编译失败。

原报告在组件选择返回前失败，没有 component、具体路径或 traceback。SDL 日志仅安装 `libSDL3.so`；相同 SDL commit 的 CMake 仅在 `UNIX AND NOT ANDROID` 分支设置版本别名，因此 SDL 版本链接猜测已有反证。OpenJDK 同版本源码有内部 `lib/server/libjsig.so` 别名依据，但当前托管 cache 中是否存在、它是否为首个失败点仍未直接观察；未据此添加链接白名单或物化镜像。

同一 d880 本地完整 Release 也实际失败，原因不同：完整构建、C++ 1553/453641 断言和 Lua 均通过，CTest 唯一失败为新增 Android 签名负控按默认 GBK 读取 UTF-8 JSON 回执，触发 UnicodeDecodeError。该失败仍是完整门禁 FAIL，不创建通过清单，也不用于启动要求完整门禁的性能或 soak 阶段。

## 本次最小变化

- 组件采集仍调用既有严格树清点及拒绝链接逻辑。仅从原异常中既有拒绝函数的已知路径局部变量提取 component、路径／父链元数据、链接目标与原异常，再原样拒绝；不输出环境变量或任意栈帧内容，不跟随额外链接来接受输入。
- 新只读 preflight 与最终包链路复用同一 JDK、SDK、NDK、Gradle、bundletool 基础选择。它位于固定工具准备之后、SDL／OpenSSL／引擎编译之前，单步上限两分钟；不运行 Java、Gradle、签名或构建。最终包链路仍另行强制检查实际编译的 SDL。
- 默认必需作业、聚合策略、最终包重验与失败退出不变。没有新增能跳过必需门禁而得到绿色结果的诊断模式。
- 两处新增测试读取 JSON 改用原始字节解析。没有修改全局 Python UTF-8 设置或生产读取规则来掩盖 CP936 失败。

## 回归原件

前置诊断维护回归先失败再通过，定向 4/4。UTF-8 环境下完整受影响三套 116/116；随后明确关闭 UTF-8 模式、不使用 `-X utf8`，实测默认编码为 CP936。原签名回归与新增诊断读取各复现一次 UnicodeDecodeError（RED 两项错误），修正后同环境定向 2/2，完整 Android 驱动与包合同 88/88。两次完整运行的源码与环境分别记录，不改写此前通过或失败范围。

恢复根为 `E:/CaesuraRecovery/20260924-1446`：

- 托管失败：`u29-d880-hosted-ci-01/android-producer-readback-01/failure-analysis.json`。
- 路径与 SDL 反证：`u24-d880-toolchain-path-diagnosis-01/handoff.json`。
- 本地原 Release FAIL：`u29-d880-package-gates-01/release-verification-01/report.json`，原 CTest 日志在同门禁目录 `release-raw-01/ctest.stdout.log`。
- 新增诊断与编码红绿：`u24-hosted-toolchain-preflight-tdd-01/`，默认编码完整运行在 `locale-full/`。

四文件增量独审已通过，见恢复根 `u24-hosted-toolchain-preflight-independent-01/review-01.json`。下一步冻结诊断提交、同步平台代码锚并进行标准提交后检查。前置 CI 取回具体被拒绝路径后，才决定是否需要针对已证合法组件别名的最小输入准备修复。原 d880 其他托管作业继续保留各自终态，不能以本次诊断替代原生、包、设备或完整候选验收。
