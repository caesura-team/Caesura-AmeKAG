# U24 Android 构建与包验证执行记录

本记录属于当前唯一运行时可靠性计划的U24。第五次真实执行已通过受控NDK Release编译、ELF检查、Gradle打包、zipalign、APK/AAB临时测试签名和最终包字节验证，独立审计无可行动发现。当前adb查询无连接设备，安装与设备运行仍为NOT_RUN；AAB manifest语义验证及正式发布签名也未建立。第四次签名失败和之前失败原件继续保留，整个U24及发布状态不因本地自动部分通过而关闭。

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


## 第三次实际编译后的工具链证据缺口

第三次请求锁定干净47374409ea3085400316cbf6f8671e6633fd8b50，request SHA256为bd000a8d8e7bcc70c3fa0999078e53df870ea861ff07d2ca6d52b5a1f544e52a，独占目录D:/caesura-u24-android-47374409-03。实际NDK配置与编译再次exit0，owned树cleanup COMPLETE；七项原命令均成功。修正后的缓存解析已越过Release/ABI/路径校验，但编译器来源检查FAIL：Compiler is outside selected NDK。原android-validation.json摘要509cf8a87fdf9a2783cee5dd1eac849250549d8a3468ea386351766318fcc733保持FAIL，private_cleanup=NOT_CREATED，后续Gradle/签名仍未执行。

实际缓存没有CMAKE_C_COMPILER和CMAKE_CXX_COMPILER两项；旧检查将空字符串作为路径解析成当前工作目录，误判在NDK之外。原442条compile_commands仍实际指向选定NDK。原libCaesuraAmeKAG.so摘要f5c09889727ad492f04285ee9f856803161e590f1d5d5353ab11a6042ec241ea，后续独立只读llvm-readelf检查exit0，SDL_main全局导出存在，所需库为已选SDL3和声明的Android系统库；SDL本身依赖也在现有允许范围。只读原件复核见u24-cache-parser/actual-native-readback-03.json，未执行原driver的后续阶段，不改判整体结果。

下一修正将以CMake File API的toolchains-v1响应记录实际编译器，再与已锁定NDK普通文件清单和实际编译目标核对；缓存若显式给出路径还需相互一致。官方接口定义见 https://cmake.org/cmake/help/latest/manual/cmake-file-api.7.html#object-kind-toolchains 。真实新建最小CMake/NDK项目已独立观察到缓存无两项、File API含C/CXX、Clang18.0.4和aarch64-none-linux-android24；它是接口前提诊断，不是Engine完整包验收。原第三次目录与收据均未重写，完整新请求须待回归、独审和新干净源码后另行执行。


File API修复已完成：预先锁定codemodel-v2/toolchains-v1查询，要求唯一完成index与objects/reply引用相等、版本为严格整数、JSON安全basename及CMake/Ninja/source/build身份一致。唯一C/CXX普通编译器文件须在选定NDK中且SHA匹配原清单；cache若显式提供则同一路径。query/index/响应均进入最终稳定性检查，原配置、目标、ELF及依赖合同保持。真实小型configure与冻结解析器读取原响应通过；生产源码摘要e2b1da1e6c0ac76853d9f0be356c6d717dc405a3de131662a890d30d299bb3a1，测试摘要3ae25b0e09bc601af6d45a32a24bc6e8b91bf54ec7e90fd7452bcaf28d32f6f5。

有效旧实现RED02为3方法、23失败子例及正控1错误，最终GREEN02为3/3、27负控。首次GBK夹具错误、布尔版本误接受和中间异常类断言错误分别保留；未用这些代替有效回归。完整Windows47/47、250.904秒，WSL45/45、73.998秒，均0失败0跳过；45个旧方法定义全部保留，新增3个。证据u24-compiler-evidence-01/handoff-01.md及freeze-02.json（最终35项锁）可复核，freeze01自输出摘要错误保留为历史。独审逐项核对原件、5份FileAPI响应和完整终态日志，无可行动发现；independent-review-01.md摘要6d32a00c023f43a14b895d56bbb474bc8766d7f04c49f7975d5012478963dc16，JSON摘要a905e46e770da5e413ee25a766420f4d6a495992b703fdf2540aba341fe55071。下一步仍是新干净源码的完整实际Android执行，以上不构成Gradle/签名/设备或发布通过。

## 第四次实际执行与签名口令消费修复

干净源码0a7d03084aebe498fbd889ddc7a6ef88925fa770使用新请求request-0a7d0308-04（SHA ce3ce5869c87b25b9fc849ee80fddd619e493064f9bb31c8308f1fd4006077f6），在D:/caesura-u24-android-0a7d0308-04完整执行至sign-apk。原收据SHA905824d59cb29e60a29f85c079e7fc071c6b76236fe989318a8593db113ad246保持FAIL：15条实际命令前14条退出0，包括configure、compile、ELF、offline Gradle、测试密钥/证书和zipalign；apksigner退出2。所有owned进程cleanup COMPLETE，无超时/强杀；私有签名目录已清，outputs只有测试证书，没有签名APK/AAB。sign-aab、最终包验证与最终稳定性检查尚未执行。

原stderr明确为第二次从password.txt读取私钥口令时EOF。生产只写一行，却给apksigner的两个密码参数相同file路径。选定工具原JAR内置帮助和实际PasswordRetriever探针确认同文件按行顺序消费；两行相同口令及两个独立文件各一行的正控均成功，一行的第二次读取负控触发EOF。此真实Java探针PID9020退出0、owned/private cleanup COMPLETE，只检验口令读取，没有生成密钥或签名。另核对选定JDK src.zip：keytool/jarsigner各自重新打开文件并读取首行，因此保留原参数并写两行同一随机值即可修复；秘密不进入argv、环境或证据。

两项维护回归先在未修生产取得RED（正常路径ERROR，EOF/不同key负控通过），修复后定向2/2、19.467秒；完整Windows49/49、242.078秒，WSL47/47、80.118秒，均0失败0跳过。原测试方法保留，工具边界模型始终FIXTURE_ONLY，不冒称真实签名。最终生产SHAfc0fa3a23ba6b204796531042d476a1bfd1f4823a521b33ed579697a69500316，测试SHA2c14f7247fb6f6e917a5f93b4d30460110916fda38a6c5253d7bb4fb5272f0e2。u24-signing-01/fix-freeze-01.json锁定27份证据，SHA3644261d0fd607071975de535cc2e4dc9e2162e33dc0b8796598086fb8efff9c；root独立审查核对原RED、真实探针、源码差异及所有摘要，没有可行动发现。下一次实际执行必须使用新干净提交、新请求和新工作目录，不能重写本次失败。


## 第五次实际执行：本地编译、最终包和测试签名闭环通过

本次固定干净源码 `8a0d5e3ea50710148ffb545eeec2bf8c892c8dca`，请求 `D:/caesura-u24-inputs-01/request-8a0d5e3e-05.json` SHA256 `29325b72723a09ee144145b1757621e52056763161b99c61dc1b05ff4ea0a27f`，新独占目录 `D:/caesura-u24-android-8a0d5e3e-05`。driver实际退出0，原 `android-validation.json` 为27,581,685 B、摘要 `bd2bfa353c7c7b81951415e30e84e4b07ecc19e6be5b042f95fb01af64f8af64`，结果 `ANDROID_VALIDATION_VERIFIED / OWNED_COMPILE_PACKAGE_TEST_SIGNATURE_VERIFIED / release_ready=false`，errors为空。原来的四次失败仍各自保留，没有替换原receipt或自动重试。

实际NDK27.3.13750724、arm64-v8a/API24、c++_static、Release编译产生 `libCaesuraAmeKAG.so` SHA256 `ff901e114df2146829a9c66a8180930faf66757e0ecdc9b702981593a2b8101e`；File API、79项native构建引用及最终AArch64 ELF相符。JDK17.0.20驱动Gradle8.9，在独立home执行offline/strict依赖验证的 `:app:assembleRelease :app:bundleRelease`，原日志 `BUILD SUCCESSFUL in 43s`、54 actionable tasks全部实际executed。既有SDL/OpenSSL库的自身编译来源仍未认证。

| 最终产物 | 字节数 | SHA256 |
| --- | ---: | --- |
| CaesuraAmeKAG-1.0.1-Android-arm64-v8a-test.apk | 125059538 | b3f90a67fbbd4cabbf33b19ac9c4fca6253e31c92dad8ac2620f3694d1581551 |
| CaesuraAmeKAG-1.0.1-Android-arm64-v8a-test.aab | 56692251 | fb83b7726c39a5e684a2a46bbfb19824a1aa19f5f1114f3b7c32fa54bb9e8e71 |
| test-certificate.der | 790 | b646a7e9084aca06ca4bb1a12de412fa8c50e35784d7ebf2791881f45fac2920 |

APK250项、AAB255项业务文件与原未签名包逐项字节一致，每种格式的245份staged JNI/assets与最终包一致。签名仅增加预声明CAESURA三件套，选定Engine库保持原SHA。实际aapt2核对APK为com.caesura.app、1.0.1/code1、min24/target35、arm64-v8a；这些manifest语义不迁移到AAB，后者仍标 `STRUCTURE_AND_EXTERNALLY_LOCKED_BYTES_ONLY / NOT_VERIFIED`。

实际apksigner verify退出0，一名signer，v2/v3验证成功，证书SHA等于上述DER；工具报告v1=false，不能因ZIP内存在签名元数据改写该结果。实际AAB签名退出0；`jarsigner -verify -strict -verbose -certs`退出4，日志精确包含未建立可信PKIX链、自签证书两项允许的测试身份错误，并保留即将到期和无timestamp两项warning。全部255项业务文件均有sm验证行，没有未签名、部分签名、过期、摘要或无效签名错误。因此结论是临时测试签名内容验证通过，而非普通exit0信任链或正式发行身份。测试证书 `CN=Caesura TEST ONLY, O=Unpublished Test`、RSA2048，有效期2026-09-20 05:29:49至09-22 05:29:49 CST。

16条driver命令及5条最终验证命令共21条，均绑定真实PID/creation/executable与原argv/CWD，正常EXITED、launcher exit0、cleanup COMPLETE，无超时、强杀或stop请求；20项工具exit0及上述1项jarsigner exit4分别保留。private_cleanup为COMPLETE，独立文件检查确认private-signing目录不存在；只证明本次私有文件按合同删除，不声称secure erase。

独立审计重新读取完整receipt及request，核对505个原引用、原命令日志/身份、构建/工具/源文件、全部unsigned/final ZIP条目与staging。最终包稳定性入口只读重解析返回ANDROID_PACKAGE_STABLE，末尾再次核验原request/receipt及source HEAD/cleanliness不变。完整约8GB工具/依赖inventory在driver结束前已全量检查；独审未重复读取全部8GB，不能把12个工具可执行文件的复算冒称整套依赖二次验收。审查见本工作树 `artifacts/validation/u24-signing-01/actual05-independent-review-01.md`（SHA `dbfb36715f463c8a9a19eecdcbd747ad589ce3f79bc49c632618afc6dfe56304`）及JSON（SHA `21efb76aa352314986f718fcfaaac3ebb6b0e5cb0b627099158d9008087c4945`），无可行动发现。审阅计算中的4395个谓词不是新增产品测试数量。

2026-09-19 21:45:46 UTC另以已存在的adb35.0.2执行只读version和devices -l，两项均exit0，但设备列表只有标题、无设备。原 `u24-signing-01/device-discovery-01.json` 和stdout/stderr均保留，未尝试历史IP、连接、安装、shell或输入。真机安装/升级、CJK/IME、触摸、音频焦点、前后台和重启恢复仍需连接并授权的实际Android设备。未执行商店上传、发布签名或托管attestation；本轮新Windows完整维护门禁仍独立进行。


## Windows 完整候选门禁及 HTTP 子场景边界

干净源码 `0dfc35a4597069852cae81af5558f2da04f5966c` 的 windows-debug run `0aefd085-f440-4393-a172-184405dd9ade` 完成：Debug 全量构建退出 0，C++ 1415/1415、427088 断言、0失败0跳过，Lua 147/147 与 56/56；CTest 59 项中 58 通过、0失败、仅预声明可选 AI 服务跳过。全部11项profile检查、runner、collector、strict verifier均退出0，源码与夹具首尾稳定。run.json SHA256 `090fbe57a32afde00b695b1c3cae50c14a10def290582fd61e9c3a2653b76299`，manifest `b2643e434a9753325773488eb418d62b1e1986a6d08e355104dd0adbdfeaf71b`。独审重算162个引用/清单项、94个独立文件共148471901字节均匹配，报告 u24-foundation/full-debug-0dfc35a4-01-review.md/json。

实际 HTTP 为73/73通过，另外 package-web-ok 与 package-web-artifacts 两个子场景因该树缺少 web/node_modules/vite 和 web/dist/index.html 而未执行；这与 CTest 层的一个 AI 跳过分别记录，严格 verifier 通过不代表75项HTTP全部覆盖。Engine PID26928、创建身份134343299092066808、端口14780，正常受控STOPPED、实际退出1、无超时或强杀、owned cleanup COMPLETE。原始HTTP结果 SHA256 `cde5fa0f43426dd56e4c43dc4296d70ecb12bb03ac296bfcbd8ea961c36ff83d`；独审时该PID已不存在。

此证据只属于本次 Windows 候选，不替代前述 Android 实际编译/签名证据，也不证明 Android 设备运行、AAB manifest语义或商店发布。两个HTTP Web子场景仍需资源具备后的实际执行；adb当前发现为空的事实与U24尚未完成状态保持。


## 独立 HTTP/Web 补验证

在干净40bf5ae79d5d365040860626844aec545fc49a18上执行一次固定请求的HTTP补验证，复用0dfc35a4构建的同字节Engine/Lua；两提交实际仅执行文档差异，未称重新编译。固定Node22.23.2/npm10.9.8、独占npm配置/cache，依次版本核对、精确CTest发现、npm ci、bake、story校验、Vite构建及HTTP CTest共8个owned命令全部exit0/cleanup COMPLETE。选中HTTP入口及其资源fixture均通过。

新http-smoke-c4618dc5ad2a4e5db284e0c91c96942c实际75个不同检查全部true，含package-web-ok及package-web-artifacts，两项均执行、无NOT_RUN。原结果SHA256 fa0be71d058da0a0d48800715fe27693c1f4f3984a0e3f102b6b7e08912416dc；Engine PID28484、creation134343328197490203、端口11955，受控STOPPED实际exit1、无强杀超时且cleanup COMPLETE，外层CTest/driver均0。两Web检查证明当前HTTP端点成功与本轮产物存在，不构成保留最终发布包字节或浏览器离线验收。

独立只读审计59项全部通过，291引用、15份完整目录inventory、13823个物理文件724399802字节重哈希一致；source/inputs首末稳定。原fullgate 0aefd085及80个预锁历史文件字节不变：其旧HTTP仍为73检查，本次单独75检查不得改写旧结果。审计http-web-completion-01/independent-audit-01.md SHA256 dca79c98700bedc95066e7a9fcb0962fc205286e4e11a074347cb0d7c08fb3f0，JSON 222e8c751b58916cbaca9ef92b17269502e19c717ffabe496f769a6a9e0a5252；请求97e3553bfc951669e1da3ef4227a0d808d1e3a8d1310ae0d5b0f52399b0b0fde和驱动748b98a0d77e0f8ddcfcaa7da02ca090198441888651a2ab2ea494317147bf63保持。Android设备/安装/运行、正式签名/商店及最终发布状态未提升。
