# E-mote 替代方案设计 — 骨骼网格动画子系统（Battle 4d / P2-9）

> 2026-08-12 · 设计文档。E-mote（Live2D 同类商业中间件）不可移植，需
> 自研等价物。本设计定义 **Caesura Skeletal Mesh Animation（SMA）**：
> 轻量骨骼 + 网格变形动画子系统，作为精灵家族（刚性）与 Live2D
> （Cubism 全功能）之间的中间层。
> 状态：**设计定稿，实现待排期**（标注为后续迭代项）。

## 1. 定位与范围

| 层级 | 现有能力 | SMA 填补 |
|---|---|---|
| 刚性 | `[sprite_move/scale/fade/swap]` + AffineBlt | — |
| **骨骼网格** | **无** | **本设计：骨骼驱动顶点变形** |
| 全功能 | Live2D（Cubism 5，D3D11 已验证） | — |

**目标场景**：低成本角色动画——头发/裙摆摆动、表情牵动、呼吸起伏，
不需要 Live2D 的整套参数系统。数据用 JSON 声明（对标 Live2D 模型
文件但极简），渲染走引擎自身网格路径。

**非目标**：物理模拟、IK、表情参数系统、Live2D 兼容导入。

## 2. 架构

```
scripts/kag/sma.lua            ← 数据加载/动画驱动（纯 Lua）
src/render/...MeshRender       ← 网格渲染原语（C++，IRenderDevice 扩展）
├── SMAMesh: 顶点数组 + 索引 + 骨骼权重
├── SMAAnimation: 骨骼关键帧（时间轴）
└── SMARenderer: CPU 软变形 → 顶点缓冲 → bgfx 绘制
```

### 2.1 模块边界（AGENTS.md 合规）

- `render` 模块新增 `IMeshRenderer`（`src/render/api/IMeshRenderer.h`）：
  纯虚接口，无数据成员
- 网格数据模型（SMA 顶点/骨骼/动画）放接口头（按值传参的类型）
- `script` 绑定层经 `BackendRegistry` 访问（不直接 include 实现）
- 数据加载（JSON 解析）在 `scripts/kag/sma.lua`（复用存档 JSON 技能）
  ——**渲染热路径零 Lua**（上传顶点缓冲后由 C++ 驱动）

### 2.2 数据格式（JSON，assets/sma/<name>.json）

```json
{
  "texture": "chara/hero_body.png",
  "atlas": { "w": 512, "h": 512 },
  "bones": [
    { "id": 0, "parent": -1, "pivot": [0.5, 0.9] },
    { "id": 1, "parent": 0,  "pivot": [0.5, 0.5] }
  ],
  "mesh": {
    "positions": [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
    "uvs":      [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
    "indices":  [0, 1, 2, 0, 2, 3],
    "weights":  [
      { "bone": 0, "w": 1.0 },
      { "bone": 1, "w": 1.0 }
    ]
  },
  "animations": {
    "idle": {
      "duration": 2.0,
      "tracks": [
        { "bone": 0, "frames": [
            { "t": 0.0, "rot": 0.0, "scale": 1.0, "offset": [0, 0] },
            { "t": 1.0, "rot": 0.1, "scale": 1.0, "offset": [0, 0] } ] }
      ]
    }
  }
}
```

设计要点：
- **线性骨骼链**（parent 树），每骨骼一个 2D 枢轴点 + 旋转/缩放/平移
- 顶点权重：每顶点最多 2 根骨骼，**交错布局**（[2i-1] 骨 A / [2i] 骨 B，
  w 归一化，和必须 = 1——引擎不做运行时归一化）；单骨布局（每顶点 1
  条目）兼容。见 §11 校验器与 demo/assets/sma/template.json。
- 动画 = 时间轴关键帧（rot/scale/offset），线性插值（LERP）
- 纹理来自现有纹理管线（`ITextureManager`，预算/LRU 复用）

## 3. 渲染路径

### 3.1 CPU 软变形（首版，推荐）

```
每帧（C++）:
  1. 按骨骼层级计算世界变换（父→子，矩阵链）
  2. 每顶点按权重混合骨骼变换 → 新位置
  3. 上传顶点缓冲（bgfx::update）→ 绘制
```

- 顶点数目标 <4k/角色（2D VN 规模绰绰有余）
- CPU 变形 4k 顶点 <0.1ms（现代桌面），移动端可接受
- **零新 shader**：用现有 AffineBlt 程序（UV 不变，只变位置）
- 无需 bgfx 新后端能力

### 3.2 GPU 骨骼蒙皮（后续，可选）

骨骼矩阵 uniform 数组 + 蒙皮 shader（`boneMatrices[N]`，顶点 shader
`pos = Σ w_i * boneMat_i * pos`）。仅在 CPU 路径成为瓶颈时引入
（>10k 顶点或大量同屏角色）。

## 4. IRenderDevice 扩展（接口草案）

```cpp
// src/render/api/IMeshRenderer.h（新增，render 模块）
class IMeshRenderer {
public:
    virtual ~IMeshRenderer() = default;

    // 上传网格（顶点/索引/权重），返回句柄
    virtual MeshHandle createMesh(const SMAMesh& mesh) = 0;
    virtual void destroyMesh(MeshHandle h) = 0;

    // 上传动画骨骼姿态（CPU 软变形在引擎侧完成）
    virtual void updateMesh(MeshHandle h,
                            const std::vector<BonePose>& poses) = 0;

    // 绘制到视图（targetView 与图层系统一致）
    virtual void drawMesh(uint16_t targetView, MeshHandle h,
                          uint32_t dstTexId, float x, float y,
                          float scale, float opacity) = 0;
};
```

- `SMAMesh`/`BonePose` 为接口头内 POD 结构（符合 §2.1）
- `BackendRegistry` 增加 `setMeshRenderer/getMeshRenderer`
- 引擎组合根（`src/entry/Engine.cpp`）创建实现

## 5. KAG 命令集

```kag
; 加载模型 + 纹理（阻塞：等待资源就绪）
[sma_load name="hero" model="sma/hero_body.json"]

; 播放动画（loop 可选，阻塞等待完成或非阻塞）
[sma_play name="hero" anim="idle" loop=true]

; 设置骨骼姿态（直接控制，如表情）
[sma_bone name="hero" bone=1 rot=0.3 scale=1.0]

; 放置/变换角色
[sma_move name="hero" x=0.5 y=0.3 scale=1.0 time=500]

; 卸载
[sma_unload name="hero"]
```

契约（schema.define，与 78 命令同规范）：
- `sma_load`：`name`（required）、`model`（file 类型，路径交叉验证）
- `sma_play`：`name`、`anim`（required）、`loop`（boolean，default true）
- `sma_bone`：`name`、`bone`（number）、`rot`（number，clamp ±π）、
  `scale`（number，min 0.1 max 4）、`time`（number，default 0）
- `sma_move`：`name`、`x`/`y`（0..1 归一化）、`scale`、`time`
- 未知参数警告、范围钳制——与现有契约体系一致

## 6. 与 Live2D / 精灵家族分层

```
[sprite_move]  → AffineBlt 刚性     （已有，零成本）
[sma_*]        → 骨骼网格变形        （本设计，CPU 软变形）
[live2d]       → Cubism 全功能      （已有，D3D11 验证）
```

- 同一角色可用三层混合（刚性整体移动 + SMA 局部动画 + Live2D 表情）
- 渲染排序：`sma_*` 输出到与 sprite 相同的图层系统（layer z 序）

## 7. 性能目标与验证

| 指标 | 目标 |
|---|---|
| 4k 顶点 CPU 软变形 | <0.1ms/角色（桌面）/ <0.5ms（移动） |
| 骨骼链深度 | ≤8（父→子线性链） |
| 同屏 SMA 角色 | ≥16 @ 60fps（桌面） |
| 动画 LERP | 每帧 <0.01ms（<32 骨骼） |

验证：`test_sma.lua`（确定性：骨骼变换矩阵数学、权重混合、LERP、
JSON 加载）；渲染测试走 deferred-gpu 模式（无 GPU 环境仅构造+访问器，
见 `docs/solutions/deferred-gpu-tests.md`）。

## 8. 实现排期（后续迭代）

| 步骤 | 内容 | 依赖 |
|---|---|---|
| S1 | `IMeshRenderer` 接口 + `SMAMesh`/`BonePose` POD + BackendRegistry 接入 | 本设计 |
| S2 | CPU 软变形实现 + 顶点缓冲上传 + 绘制（复用 AffineBlt shader） | S1 |
| S3 | `scripts/kag/sma.lua` 数据加载/动画驱动 + KAG 命令契约 | S1 |
| S4 | 测试（确定性矩阵/权重/LERP + deferred-gpu 渲染） | S2/S3 |
| S5 | GPU 蒙皮（bgfx compute）——round18历史实现；当前布局与证据见§9 | S2 |

## 9. S5 GPU 蒙皮（当前布局与历史证据）

当前布局以2026-10-02命令合同修复为准，详见[执行记录](../plans/2026-10-02-001-command-contract-validation-followup.md)。`createMesh`只保留网格自身的静态compute输入（pos+uv+bone0/bone1+w0/w1，32B/顶点）；`updateMesh`保存完整姿势。每次`drawMesh`拥有独立的immutable骨骼/绘制快照及COMPUTE_WRITE输出（最终NDC位置+UV，16B/顶点），同view先dispatch再由该draw消费自己的output，不复用按旧transform计算的最终位置，也不让同mesh多次提交互相覆盖。

| 快照float4行 | 内容 |
|---|---|
| 0 | draw x、y、scale、reserved |
| 1 | view width、height、vertexCount原始uint32 bits、poseCount原始uint32 bits |
| 2+i | 真实pose i的(cos(rot)·scale, sin(rot)·scale, ox, oy) |

poseCount为`min(poses.size(), 65536)`，保留全部uint16索引可达姿势；这不是把骨索引夹到64。缺失骨骼不填identity行冒充有效。两个元数据行与骨骼行分离，最大65538行／1,048,608B。HLSL仍用typed `Buffer<float4>`、输出`RWBuffer<float4>`，t0/t1/u2对应绑定stage0/1/2；GLSL保持std430 binding0/1/2。64线程工作组先核vertexCount尾界，才访问输入/输出。

shader按当前CPU `skinMesh`分支执行：两个骨骼有效时，总权重<=0保留原位置，否则归一化混合；只有bone0有效时不论权重都应用其变换；bone0无效时保留原位置，即使bone1有效。有效性仅按索引小于poseCount判断，因此65536个姿势时索引65535有效；不能把接口的默认sentinel注释改造成不同于CPU的额外排除。预乘系数和CPU算式浮点结合次序不同，按维护容差验证，不宣称逐bit等价。

上传使用`bgfx::copy`交付字节所有权。不可复制的每draw资源包在dispatch及submit排入后才排入snapshot/output销毁；compute output是独立底层VB，沿bgfx延迟free退役。先核TIB可用量及实际分配size，分配失败明确拒绝或由既有异常边界转为Lua错误，不用旧输出或默默转CPU掩盖。原`SkinMode::Auto`无compute能力时的CPU选择仍是独立能力规则。

**2026-10-02 初始限定验证**：新布局真实FXC与内嵌生成完成，两个vertex shader字节不变；WARP软件三组9例真实compute与CPU像素对照mismatch均0，含transform、多输出及65535/65536边界；新布局TIB不足时剩余5→5且无绘制，恢复后1582白像素、mismatch0；独立WARP compute数学与headless19用例/3200断言通过。该时点的软件后端证据不等价于物理GPU证明。

**2026-10-03 增量**：Lua 位置数组与 C++ 具名字段已按具名优先、nil 回退位置槽统一；缺省 view 使用 MAIN=1，显式 0 保留。CPU/GPU 每次 draw 使用实际读取 `u_color` 的 modulated shader 应用 opacity。真实 Engine 默认视图/透明度像素与 D3D11 同帧三透明度、两种源 alpha 的 GPU 合成测试通过；五条 SMA DSL 场景执行动画、IK、variant 和 stop，原生网格数回到零。当前完整 Windows C++/Lua/CTest 结果及 Web 性能仍失败的边界见[命令审计结果](../plans/2026-10-03-001-command-native-audit-results.md)。这些数据不替代当前逐 draw 布局的性能基准。

**历史证据，不能外推当前补丁**：round18记录过D3D11隐藏窗GPU/CPU两帧读回、14断言通过；round19在8k顶点/64骨/~16k三角、120帧取后100帧均值的主机侧计时为CPU1.268ms/帧、GPU0.080ms/帧（约15.8×），并记录过Windows WARP的主机提交成本断言。这些属于当时shared64骨/每mesh输出实现及设备配置，不是当前逐draw分配布局的性能或物理GPU验收；当前性能需要独立测量。

## 9b. 播放控制与高级动画（round 18）

- **播放控制**：`loop`（设计文档 §4 承诺，默认 true，t 按 duration 取模）、
  `sma.duration`、`rate` 倍速、`pause`/`resume`/`seek`/`set_rate`、
  `play_anim`（切换动画不重建网格）、非 loop 播完 `on_done_anim` 回退。
- **crossfade 混合**：`play_anim(blend_time)` 双动画采样逐骨 LERP，完成后清除。
- **2 骨 IK**：cosine-law 纯函数 `ik2bones`（可达/不可达/退化全分支），
  `set_ik`/`clear_ik` 每帧覆盖链骨骼世界旋转使链条到达目标。
- **部件/表情变体**（E-mote 风格）：资产 `parts` 多网格（每部件独立
  变体几何），`set_variant` 销毁重建该部件网格；单 mesh 路径完全兼容。

## 9c. 创作工具链（round 19）

**校验器** `scripts/kag/sma_check.lua`（库 + CLI 双模式，仿 ks_check）：
- CLI：`lua scripts/kag/sma_check.lua <asset.json> [more...]`，退出码 0/1
  （CI 门禁）；库：`validate(asset)` / `validate_file(path)`（含结构摘要
  meta：bones/anims/parts/verts/tris）
- 规则与运行时消费契约严格一致：骨骼 id 唯一 / parent 引用存在 / 无环 /
  pivot；网格 positions/uvs/indices（%3 且不越界）/weights（1 或 2 交错
  条目、bone 引用、w∈[0,1]、和=1）；动画 duration/track 骨引用/frames t
  升序；parts 变体递归校验；texture/atlas 元数据
- 集成：`sma.load(jsonText, {validate=true})` 加载门禁、`sma.validate`、
  `sma.validate_file`（RPC 复用）
- 修复（round 19）：`create_part_mesh` 双骨权重读取改为 [2i-1]/[2i]
  交错布局（旧实现把相邻条目错位当第二骨；18 轮测试未覆盖 weights 路径）

**资产模板与示例**（demo/assets/sma/）：
- `template.json`：带 `_comment` 字段的完整模板（骨骼/网格/双骨权重/动画/部件）
- `hero.json`：可运行示例角色——8 骨（root→body→head + 双臂 2 骨链 +
  眼睛）、5 部件（body/head/armL/armR/eyes，eyes 含 default/happy 变体）、
  idle（呼吸）与 wave（挥手交叉淡化）动画、肘部双骨混合
- `_broken_example.json`：故意损坏的演示资产（每个错误带 `_intent`），
  服务校验器演示/测试/IDE 面板
- 纹理策略：仓库无二进制素材——demo 运行时经 `backend.create_solid_texture`
  注入纯色纹理，资产 texture 字段留空

**SMA demo 场景**（demo/sma_demo.ks + sma_entry.lua + sma_demo_driver.lua）：
- 独立入口（不动主 demo）：spawn hero（idle loop）→ crossfade 切 wave →
  eyes 变体 → 2 骨 IK 右臂指向目标 → 停止
- `sma_demo_driver.lua` 与测试共享（headless 冒烟驱动真实 demo 逻辑）

**IDE 面板**：`GET /api/sma/validate?path=...`（引擎经 Lua 跑共享校验器，
路径限 assets/ 与 demo/assets/、禁 `..`）；VisualView "SMA ASSET" 面板——
输入路径 → ✓/✗ + 错误列表 + 结构摘要（骨骼/动画/部件/顶点三角）。

## 10. 风险

| 风险 | 对策 |
|---|---|
| 软变形 CPU 成本随角色数线性增长 | 顶点预算 + 同屏上限；GPU蒙皮为后备；§9的round19数据仅为历史，当前逐draw布局成本须另测 |
| 数据格式与 Live2D 生态不兼容 | 定位为"轻量替代"，非 Live2D 导入器 |
| 网格渲染与图层系统集成复杂度 | 复用 sprite 图层路径（z 序/透明度） |
| 手写 SMA JSON 易出错 | **已闭环**：sma_check 校验器（CLI/库/RPC）+ 资产模板 + IDE 校验面板（round 19） |
| 资产权重布局与运行时不一致 | **已闭环**：校验器与 create_part_mesh 消费契约同源（[2i-1]/[2i] 交错，round 19 修复错位） |
