# U24 Android 构建与包验证执行记录

本记录属于当前唯一运行时可靠性计划的U24。当前交付是受控构建驱动和APK/AAB验证合同，**尚无本轮真实Android编译、Gradle构建、签名、安装或设备运行结果**。不能以维护夹具通过关闭U24或提升发布状态。

## 已实现的合同

`scripts/android_package_contract.py` 接收外部锁定的APK/AAB摘要、源码/版本/SDK/ABI、预期测试证书及工具摘要，检查实际ZIP路径与文件、AArch64 ELF、原生库/资产字节并调用选定工具验证。额外native feature库和错误ABI拒绝；AAB语义manifest和构建来源不由这一层自行认证。签名工具及其他边界替身始终明确标为fixture，不生成设备运行证据。

`scripts/run_android_validation.py` 接收外部请求摘要、干净源码提交、完整选定工具/依赖清单，在新的仓外独占工作目录执行native编译、全新资产staging、offline Gradle、临时TEST签名和最终包验证。配置固定Release/arm64-v8a、JDK17、Gradle8.9、NDK27.3、minSDK24、compile/target35、build-tools34；版本与项目一致。实际CMake cache/compile命令、JNI ELF依赖及最终字节都有检查。所有子进程使用既有owned-process控制，第一次失败保留，不捡旧产物、不自动重试。

工具清单选择的文件必须是普通文件，拒绝符号链接、junction、硬链接和未列入清单的变化。它是所选输入的绑定，不宣称操作系统或任意未锁定的宿主依赖完全封闭。Gradle依赖种子独立锁定并复制到私有home，严格verification metadata与offline模式配合使用。`android/app/build.gradle` 仅增加五项显式属性的窄适配；未使用受控属性时保留开发默认行为。

临时密码通过本轮私有文件交给工具，密钥仅为一次性TEST用途；没有生产签名入口。清理先逐一尝试移除两个已知owned secret，未知临时项仍保留并导致FAIL，原命令错误不被清理错误覆盖。签名前后的APK/AAB业务文件名和SHA集合必须完全相等；仅精确的`META-INF/MANIFEST.MF`、`META-INF/CAESURA.SF`、`META-INF/CAESURA.RSA`允许标准签名变换。AAB及使用V1的APK要求完整非空三件套；V2/V3-only APK仍须保留原manifest。签名有效性由原verifier另外验证，文件名本身不等于认证。稳定性入口重新打开文件并计算闭包。

## 实际维护回归及独立审查

包合同套件Windows20/20、WSL20/20，零跳过。driver最终Windows29/29、WSL29/29，零跳过，分别171.493秒和63.303秒；保留原25个方法并新增4个签名闭包方法。真实Git、文件系统、ZIP与owned Python子进程和生产predicate参与这些回归；Android工具输出明确由fixture提供，顶层始终`FIXTURE_ONLY`、CLI77、`release_ready=false`，device/runtime/install为NOT_RUN。这些数字不是覆盖率。

原RED、工具前提错误和中间失败均保留在`artifacts/validation/u24-package-contract/`、`u24-controlled-driver/`。主代理指出的秘密清理顺序缺陷已真实RED→GREEN。独立审查另以实际ZIP复现签名后新增APK资源、AAB dex仍通过；原P2报告、原始文件和SHA保留于`u24-controlled-driver-review/review-01.md`。窄修的4方法先取得10个失败断言，之后4/4通过；独审又以原反例、额外META-INF文件、正常业务META-INF与晚改prepared文件完成4/4正负复验，见`review-02.md`，没有剩余可行动发现。

曾复用既有公开签名JAR字节验证固定metadata名称；没有新密钥或新签名。真实JDK日志还包含JarInputStream相关告警，该正控只证明其限定的JarFile/公开字节兼容，不代表无告警JAR或Android验收。原始日志不修改。最终driver源码SHA为cde46812016c40dce829455ad7343f260cf3b618145aa298cda0f83e7cbdb148，测试为2cc8ee2e2ca681c528c94c1bc9ee29dddcf4421a1c606da69525be52035e9eee；Gradle适配、包verifier及其测试的独立身份见freeze-02与review-02-locks。

## 真实执行前提和剩余工作

只读清点确认选定JDK、Gradle、NDK、SDK、SDL、OpenSSL、CMake、Ninja目录存在。默认Gradle cache为空，尚无本轮依赖种子及verification metadata。已安装Git完整runtime有正常安装硬链接，按当前合同不能直接选整树；只锁单个launcher也不能冒充完整runtime绑定。后续应在独占工作目录准备普通文件镜像和经审查的实际输入清单，再冻结新的干净候选。

实现提交bbf78ddc已合入U23干净候选ce5e3e5d，合并为4f6536d9；代码注册提交6555d4fb新增两套CTest入口，六配置的实际最低发现数由57升至59，六个adversarial门槛57保持不变。独审确认五个Android文件Git blob及原始字节仍与已审实现一致，其他U23源码完整继承；原57个测试名称/属性没有遗漏。driver入口超时600秒用于真实owned Python子进程夹具，不代表Android构建执行。

新Windows构建目录最初自动选中了System32的WSL Bash launcher，实际发现记录保留于ctest-discovery-59-01.json。随后显式配置Git Bash并保存ctest-discovery-59-02.json，两个Bash入口的命令已核对。两套新增测试经真实CTest入口通过：包合同24.68秒、driver176.57秒，总201.26秒，2/2、零失败零跳过；原始JUnit及日志在u24-controlled-driver/ctest-registered-01.*，审查及增量附记在u24-controlled-driver-review/integration-review-01/02。此时七个native可执行文件尚未在新build目录生成，因此新发现记录不是完整native门禁通过证据。

正在仓外独占目录准备普通文件Git镜像、完整选定工具摘要，以及单独联网获取的Gradle依赖种子。获取只使用项目声明的Maven Central与Google Maven；未进行native编译或签名，准备阶段未包含Engine JNI，产物不作为候选。生成的verification metadata须另行核对后才能用于接受路径，实际受控验收仍为offline。完整候选门禁及native/Gradle/TEST签名/最终APK-AAB验证尚待执行；初次真实执行仍需确认AGP输出位置、JNI不改写、CMake格式和单次Gradle子进程清理。任何失败均留存后再针对修复，设备安装、实际窗口/音频/生命周期和发布签名继续单独验收。
