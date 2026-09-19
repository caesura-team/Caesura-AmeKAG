# U26 可选 SDK 与云存档边界执行记录

本记录对应当前唯一计划U26。首个已实证修复是不可用但已注册的Steam后端误替换本地存档provider；完整候选门禁及其余云冲突、分块失败恢复、SDK ON和真实SDK功能仍待执行，U26未完成。

## 证据起点

只读盘点的MAIN源码为65e5b425，U23/U27中24个相关模块、测试和能力入口文件与其Git和原始字节一致。指定Steam1.65/Cubism Native-5-r.5的12个SDK/header/lib/model文件及Haru声明21个资源存在，只证明文件前提，不证明SDK ON编译、真实客户端、模型动作或账号能力。原审查与旧U4/U19日志身份在主工作区artifacts/validation/u26-readiness-01/readiness-01.md与inventory-01.json；旧配置不是本轮验收。

## 不可用 Steam 后端保留本地存档

在独立u26-worktree、3da09e36基线上新增一个真实C++回归，使用实际NullSteamBackend，覆盖steam、steam://、steamcloud三个别名。首先证明本地save/load成功，再要求configure返回false、provider同一指针、原slot仍可发现，JSON与scene/token元数据可读且原文件字节不变。RAII按正确生命周期恢复原注册指针；没有替换被测SaveManager逻辑或真实Null后端。

未修生产源码首次实际Debug目标构建成功，回归1方法失败、42断言中18失败：三个别名均返回配置成功并替换provider，原slot因此不可见/读回null，原物理字节仍在。1415其他用例是定向过滤未选中。原red-01.log摘要527593b830a3571837c3c2af324cd9bc5fd4a92ae1df58a93ca036ce3b0c5de8，原二进制a138cce300e1aa29834d22cd2b3a165d895e839758cc2b0e028d9252cb0f3db8及源码/test快照由red-receipt-01.json锁定。

修复在provider替换前检查后端指针及isAvailable()，使用公共ISteamBackend接口。第一次增量构建因缺少完整接口include报C2027；原build-green-01.log保留，补入../steam/api/ISteamBackend.h后build-green-02成功。相同冻结测试实际1/1、42断言全通过；cloud_save/storage/steam/runtime_backend_availability四个源码过滤得到85/85、1249断言全通过，1331为未选中，不能冒充完整套件。已注册available mock的云存读往返、未注册拒绝和加密写入相邻控制仍通过。

最终SaveManager.cpp摘要2a7ecd87f0dd1311e6706ae8e5deeac6a2aaabd18540b22346cf65b73b207583，测试摘要de01cbcd1d9fb3a89357dc3d4c68ecf4646c2335bf9f1db557720c81205aef2c与RED完全一致。storage耦合仍4/4（archive/debug/di/steam），count_coupling --ci通过，没有具体实现头依赖。独审无可行动发现，u26-null-steam/independent-review-01.md摘要a9dec6450d74e1d0854589a2e3ca02e8d1004089daf5c4be3b6af57388fd8cd7，JSON摘要395d5671472949517243968eb7eed444d3df14433142a5bd6ae2c896939ff8ba。六native profile的C++最低发现数按实际新增1方法各加一，CTest门槛不变。

## 下一步

独立核对已有分块存档覆盖中途失败、large-to-small替换与短读，然后建立共同祖先后本地/云端分叉保留双方的合同。HTTP真实loopback超时/重试与Steam失败/重复/迟到回调、Cubism加载失败及motion释放按实际边界分别回归。只有经过真实复现的疑点才修复；尚无本轮账号、设备、商店发布或真实SDK功能验收，后续完整Debug/C++/Lua/CTest与选定SDK ON产物继续独立验证。
