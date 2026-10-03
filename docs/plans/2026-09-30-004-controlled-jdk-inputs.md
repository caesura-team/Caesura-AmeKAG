# 托管 JDK 输入隔离与来源闭包

状态：受控输入实现和定向控制已形成；最终 UTF-8 132/132、真实 CP936 103/103 整套复验已通过，新版独审已通过且没有剩余可行动问题，真实 hosted JDK／Gradle／签名链尚未复验。完整 U1–U29 不变。原 JDK、系统信任库和全局 JAVA_HOME 不修改；没有合并、发布或标签动作。

## 已确认的实际来源

诊断提交 `c951381e7fd72358b9f6d640422cef2811a73b1d` 的运行 `36703102836/attempt1` 在编译前停止。真实 JAVA_HOME 为 `/opt/hostedtoolcache/Java_Temurin-Hotspot_jdk/17.0.20-1/x64`，解析到 `/usr/lib/jvm/temurin-17-jdk-amd64`。实际拒绝项为其 `lib/security/cacerts` 文件链接，原始目标和解析目标均为 `/etc/ssl/certs/adoptium/cacerts`，位于 JDK 根外。这不是此前内部 libjsig 别名的假设；诊断没有读取该目标的完整文件类型与内容摘要，因此不能预先称它已被接受。

取到精确证据后只停止该诊断运行的剩余重复作业；终态 cancelled，必需聚合检查实际 failure。原 d880 运行继续完成，12 作业为 10 成功、Android 和聚合两项失败。聚合原件显示其他九个 required role 成功、11 个 artifact role 引用齐全，唯一拒绝为 `Required role android-compile in attempt 1: completed/failure`，且 `downloads={}`，证明新增硬门禁确实在下载前生效。

原 d880 Linux producer 的日期 runtime `2fca8b…` 与 appimagetool `ed4ce8…` 校验成功，TGZ/AppImage 构建和验证四命令均为 0。Windows producer 的 ZIP 两命令为 0，五阶段通过；两 editor 是预期受控 STOPPED/1，其余五命令自然退出 0，全部 launcher 0、cleanup COMPLETE。四个原生命令的模块来源 VERIFIED，未记录 snapshot_attempts，不声称实际触发了新重观察分支。这些 producer 证据不改变整体 d880 FAIL。

## 新输入合同

新增 JDK 输入准备器把原选择的 `bin/lib/conf/release` 物化到新的私有普通文件目录，记录请求根、解析根、源身份、文件和链接元数据、精确目标、实际字节摘要以及镜像锁。

- 根内只允许单层文件别名，拒绝目录链接、链接链、越界、坏路径、hardlink 和不稳定输入。
- 唯一根外能力是逻辑 `lib/security/cacerts` 对应调用方明确声明的 `/etc/ssl/certs/adoptium/cacerts`。先检查规范路径、普通单链接文件，再计算外部锁定摘要；实际物化时再次按该锁检查。其他根外链接不接受。
- 准备限定 200000 条、2 GiB 镜像字节、120 秒；失败保留 partial，不产生可通过结果。
- 来源 receipt 与外部 SHA 同时进入 preflight、最终包稳定性复核和小型 proof，选定 jdk-root 必须等于镜像根。proof 不含 JDK／CA 内容字节或密钥。
- 不禁用 TLS，不增加证书，不下载另一套 JDK，也不放宽共享 `_no_links`、离线 driver、签名及包合同。镜像根通过显式参数选择，不修改原生编译阶段的全局 JAVA_HOME。
- Fixture 来源不能被生产 CLI 或无 runner 的最终链路升级为真实来源。

## 输出目录约束

独审真实文件反例发现：先检查父目录再以路径创建文件，仍可能在目录被替换后产生意外目标副作用。原失败与中间实现均保留。

最终采用更简单的 Windows 目录读取句柄方案。实际六项实验表明：只申请属性读取 `0x80` 时同目录重命名成功；申请目录列表加属性读取 `0x81`，保持 share READ|WRITE 且不 share DELETE，则该重命名被系统以 error 32 拒绝，祖先变动也被拒绝。新实现持有相关目录句柄，文件、目录、receipt 和 GITHUB_OUTPUT 的写入使用一致约束。新增的底层创建接口已经移除，共享 AppImage 源码未修改。

早期十四项定向控制保留其自身源码锁，不冒称最终文件原始字节。最终 UTF-8 132 项和 CP936 103 项两份完整套件均锁定最终 helper，并再次实际覆盖五类目录变动注入点，记录真实重命名尝试与系统拒绝，意外目录保持为空；没有因旧 hook 不再执行而虚报通过。POSIX 仍使用绑定目录描述符；其规范化行尾后的 source segment 不变，不声称原始文件字节完全一致。

## 验证边界与原件

恢复根为 `E:/CaesuraRecovery/20260924-1446`。

| 范围 | 证据与状态 |
|---|---|
| 实际托管 cacerts 诊断 | `u24-c951-hosted-preflight-01/android-preflight-readback-01/evidence-summary.json` |
| d880 整体与两项包修复 | `u29-d880-hosted-ci-01/terminal-report-01.json` 及其 producer/聚合原件 |
| 原输出目录问题 | `u24-jdk-inputs-independent-01/review-01.json`，实际文件反例保留 |
| 较复杂旧冻结版 | `u24-jdk-inputs-tdd-01/handoff02.json`：UTF-8 132、CP936 103、Linux实际文件控制14；该版最终独审曾被服务内容检查中止，没有补记 PASS |
| 更简单的目录访问实验 | `u24-jdk-directory-sharing-experiment-01/summary.json`，六真实一次性用例、全部句柄关闭 |
| 当前普通 CreateFileW 版本 | `u24-jdk-inputs-createfile-tdd-01/`；定向14项、UTF-8完整132项、真实CP936完整103项均通过，0 failed/0 skipped；最终源码锁稳定，见 `handoff03.json`。新版独审已通过，见 `u24-jdk-inputs-createfile-independent-01/review-01.json` |

原 d880 本地 Release 的唯一 CTest 编码失败已按 FAIL 收集为 `u29-d880-package-gates-01/failed-bundle-01/`，执行文件与原日志保留；不会将该失败当作性能或 soak 的通过先决条件。其测试读取修正已有真实 CP936 对照。

当前普通 CreateFileW 版本增量独审已通过，原被中止版本不补记 PASS。接下来冻结源码、单独同步平台锚并进行标准提交后检查，随后验证真实 hosted 输入物化、Java、Gradle、签名和 v2 包原件。本地文件控制不等于实际工具链已经可用，更不等于设备、正式签名或整体验收完成。
