# U24 Android 构建与包验证执行记录

本记录属于当前唯一运行时可靠性计划的U24。当前交付是受控构建驱动和APK/AAB验证合同；本轮真实NDK Release编译已成功，但首次到达编译的整体运行在CMake缓存校验失败，**Gradle构建、签名、安装与设备运行尚未验收**。不能以编译或维护夹具通过关闭U24或提升发布状态。

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

## 2026-09-20 实际输入冻结与首次候选执行

完整工具输入已建立并由生产validator接受，原始 `D:/caesura-u24-inputs-01/toolchain.json` 为13,495,496字节，SHA256 `49e8a5d1a76f4785cf5c81f93db88a5ec8b9caf8d90dd737031b2f9abe8ee7f7`，覆盖92,446文件、8,395,836,839选定字节。Git在新目录保留完整运行时的9,618文件/417,218,012字节，逐文件复制为普通独立文件，保留原172个硬链接路径/85组的审计身份。11项实际owned版本/路径探针均exit0、无超时/强杀、cleanup COMPLETE；前后完整工具清单相同。Python选择包含现有第三方site-packages，不称stdlib-only或hermetic；SDL/OpenSSL原编译来源和宿主OS仍未认证。证据见 `u24-controlled-driver/tool-inputs-02/`。

独立依赖准备在 `D:/caesura-u24-dependency-prep-01` 执行项目原Google Maven/Maven Central入口，真实Gradle8.9/JDK17完成54项任务，5分39秒exit0，owned cleanup COMPLETE。此staging没有Engine JNI，仅有SDL JNI，其APK/AAB仍不是候选。原 `preparation.json` SHA256 `f7fbe8915ce36246418512bb87ca0c9656bec5b80b0a9685caa2fd63afd8321f` 保持历史ACQUIRED_UNREVIEWED值，后续审核不回写旧收据。

401个实际制品全部重算SHA256/SHA1，来源日志恰对应235项Maven Central和166项Google Maven URL；373条verification XML记录及28个因Gradle优先module metadata而未列入XML的同坐标POM均核对。官方HTTPS校验和旁证合计389/401相符，另12项不可取得，观测到的摘要不匹配为0。两次不同算法的原旁证请求仍各自保留FAIL，不通过重试改绿；HTTPS摘要相符和Gradle生成XML不等于发布方签名认证。独审原module坐标假设失败及root最初两次审计假设错误同样保留。详见 `u24-controlled-driver/dependency-review-01.md/json`，审查JSON SHA256 `7d8d9f2be5d00f11531012e24724b726d8d28d88891f5072b328f800bbb37848`。

实际选定的plain seed包含401个制品、217个module metadata文件及verification XML，共619文件/243,276,541字节，未复制daemon/transforms、项目cache、用户设置或构建产物。原XML SHA256 `6c8b8613eead6ac5be23655c710d380281a6cf381943981f00e681d4676ac6a2`；`dependencies.json` SHA256 `12718b9d61d1c1a76c3447f6beb93aadb453a5d80ae8ffde930ffd446b3d1950`。输入选择、原件/副本摘要和12项旁证缺失在 `D:/caesura-u24-inputs-01/selection-01.json` 明确记录，生产 `_dependencies` 接受该固定输入，只授权本地验证。

首次真实请求绑定干净源码 `dccaf211995d642ad1e02ef2b07db272a0d20aec`，`request-dccaf211-01.json` SHA256 `7ba90d2694d6ea5bc5e200a1ea332eefb31e6ea169a4afac36e0243d35a91c4d`。执行前在scope内将21个既有ignored Python bytecode逐件摘要后移到ignored证据备份，不删除用户源码；本轮Python使用 `-B` 和 `PYTHONDONTWRITEBYTECODE=1`。

第一次实际driver在inputs阶段FAIL，`commands=[]`、private_cleanup=NOT_CREATED，**尚未开始native编译、Gradle或签名**。原回执 `D:/caesura-u24-android-dccaf211-01/android-validation.json` SHA256 `891edab96ad15ab4aa202482fcd8e9d8a8eeaa4d34c03d1f75f179573a167930`。完整工具/依赖树随后只读复核一致；精确trace定位到源码中的Git120000链接 `external/zstd/tests/cli-tests/bin/unzstd`，同目录还有 `zstdcat`，两者均指向仓内tracked普通文件 `zstd`。`_source` 对所有tracked文件调用证据层的拒绝链接规则，因而拒绝合法仓库输入。原trace位于 `u24-controlled-driver/source-input-failure-path-01.log`。下一步仅修源码清单合同并建立真实链接正负回归；工具、依赖、证据路径的严格无链接合同保持。首次FAIL不会被后续运行覆盖，完整候选、最终包及设备范围仍待验收。

## 源码符号链接修正与独立复核

源码选择现在将Git120000记录与真实物理symlink、原始相对target、单跳仓内tracked普通终点逐项核对，并把alias内容SHA与链接关系都写入source receipt。source-only枚举不遍历目录链接，stage从重新核验的普通终点复制后再次检查alias与内容。普通工具、依赖、证据清单和最终归档的禁止链接规则保持；没有删除或展平仓库的两个zstd链接。

首次实现的独立审查发现P2：对`hop/../payload.h`词法消除会漏过中间的仓外目录链接，实际读到仓外字节而锁定仓内字节。保留的真实POSIX仓库反例确认后，改成依次验证原始target的每个路径分量，先验证实际目录再消费`..`，最后要求OS严格解析到同一普通终点。原反例在未改动fixture/HEAD/index的情况下现在明确拒绝；链、目录、junction、逃逸、悬空、untracked、文本替代与变化路径负控制均保留。

最终生产SHA256为35b6f4725e6eb63271ef78c99519bd55ad5f65794f04b5dddf0049df682ee912，测试为ac5df006c65773540028650f8c4d788d9c4c3d9199529d121a5e2b85d2cdf0ea。完整attempt03：Windows42/42、182.967秒，WSL40/40、68.169秒，均exit0、0跳过；平台数量差异来自显式注册的junction/Windows不可解析表示负控与POSIX普通目录正控。attempt02 Windows的两项ERROR是手工POSIX斜线target在Windows实际不可读的夹具缺陷；仅将两个正控改为同目录可读alias并先断言真实读字节，未放宽生产检查，原FAIL保留。原有29方法全部保留。

证据位于u24-worktree/artifacts/validation/u24-source-links/：freeze-03.json摘要87e59b07cc10666a3f03f999a0e4329de7c924bbea62c5ca7eb569edb5efeeb6，handoff-01.md摘要a3784019b74a1b694b708d70662a0b5078aa902fda3e3d8a97065ddfdcf7817a，独审independent-review-01.md摘要a05717098a8254c6d1bb513af6663d6367377d2f98029a2f72e2cebae8058593、JSON摘要e2ca66b652d5c859cef7ae2258c9531f99fb85eec09d42a0e30ec7d336374951。独审无剩余可行动发现。这些维护测试仍不是实际Android编译、包签名或设备运行验收；下一次实际执行使用新的干净源码请求及独立work目录，原首次FAIL不覆盖。

修正提交66a0d9ab后，merge50c77f72c467b4e218d38af52607ed705f897fbb整合U23 c0045049及U22 d418845d的进程与Chrome临时目录修正；仅文档锚点冲突，U24两个修正文件字节未变。平台锚点用于同步审查，没有扩大历史设备、签名或发布状态；新整合源码尚待完整候选验证。


## 实际 Release 编译与缓存解析回归

第二次实际请求绑定干净源码0953926f556fe335f4ee981d36b04074f34270db，原request SHA256为22abc12b9699a8ecca44de6d0a85916753183ddb0344832a24619bd6657f3134；仓外新目录为D:/caesura-u24-android-0953926f-02。七个owned工具/配置/编译命令全部exit0、无超时或强杀、cleanup COMPLETE。真实NDK arm64 Release生成libCaesuraAmeKAG.so，SHA256为bfabcf09ece58156abbb7b461d690466bf9f1117631d2ec162c90fd8a5d06f17。整体仍FAIL：compile后的实际缓存检查报告Actual CMake selection differs: CMAKE_BUILD_TYPE，原android-validation.json摘要00290af67146de569bbc94d51536d147c8ff8e75ac7a0f2b26e59ec04e8bbf7f。未到staging/Gradle/签名/最终包，private_cleanup=NOT_CREATED，不能将编译成功当作包验收。

原CMakeCache.txt实际含CMAKE_BUILD_TYPE:STRING=Release，共341项，SHA256为c21ecdabae4be8edb922bd22fd0d14711a7fa0f06d7c0e6db8e33ab1480e49fc。旧多行regex的字符类越过空行与//说明，把说明行捕获为key并吞入真正的配置行。新parser逐物理行解析，跳过空行和明确注释，拒绝畸形或重复key，保留原值及全部配置/ABI/编译路径检查。实际原cache与442条compile_commands只读复核通过，未重写原文件或修改首次FAIL。

两个新回归在旧实现真实RED（1 ERROR、1 FAIL），修复后连同原错误ABI负控3/3通过；完整Windows44/44、202.717秒，WSL42/42、71.962秒，均0失败0跳过。源文件SHA256为005714c7da1ddfb8f9b20066132edfbac3a1b004af43f2d32575a525810901cb，测试为08edf9f1cf7963a9bb4145f9d6638b51c09c74d6f3e9e7683891632ba819bca2。独立审查重现旧regex吞key并核验原cache/owned收据，无可行动发现；报告u24-cache-parser/independent-review-01.md摘要4e0d64216555b4e78bdbcb1d3f2b65ba34fa1a148fcf5fc7744957739e6b3327，final.json摘要32327bd0cefaa48a3308d9b3684ad2d417907b21dd71d1a6c09c5ab6d9e801bd。完整日志、RED、原实际只读复核均保留在同目录。下一次真实执行使用新干净源码请求和新work目录；尚无设备安装/运行或发布授权。
