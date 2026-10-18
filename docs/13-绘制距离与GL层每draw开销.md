# 13 - 绘制距离与 GL 层每 draw 开销（R36S）

> 日期：2026-10-17 ~ 10-18。前置：[07](07-RenderDoc帧捕获分析.md)、[08](08-画质开关定位.md)、[09](09-碎片draw合并尝试.md)、[10](10-draw合并重做.md)。
> 结论先行：**最大的收益来自游戏自己的绘制距离**（PX_PCT），比赛起跑线 ~105-120 ms → ~55-60 ms/帧；GL 层的几项微优化各自只有 0~6 ms，有的在 Mali 上反而更慢。

## 1. 结果汇总（R36S，同一次启动里与基线交替测）

| 改动 | 开关 | 结果 | 默认 |
|---|---|---|---|
| 绘制距离缩放 | knob `PX_PCT`（%） | 100→35：dc 2000→1200，-36 ms；→10：dc ~800 | 设备 env.txt 设 10 |
| 大物体保持原距离 | knob `PX_BIG`（半径） | PX 10 时保住天空/大楼，draw 只多 ~0-80 | 设备 env.txt 设 200 |
| c[] 行版本戳 | knob `CSCAN=1` 回退 memcmp | uni-c 6.7→3.5 µs/draw，约 -6 ms | 开 |
| vertex_attrib_binding | knob `GL_LEGACY=0` 打开 | 调用数 -43%，帧时间不变（vertices 阶段反而 +1.4 µs） | 关 |
| c[] 放共享 UBO | env `RECOMP_GL_UBO=1` | 冷机 A/B：85.5/80.2 vs 76.3/76.2 ms，**更慢** | 关 |
| 原生 bMulMatrix / _ftol2 | knob `NATIVE_OFF` 位 0/1 回退 | 与 lifted 逐次比对 0 差异；GL 线程瓶颈时帧时间差在噪声内 | 开（需 gen 里 `_gen` 体） |
| 对比度 ramp 少重建 | knob `RAMP_EPS`（×1e-4，默认 20；0 = 原版） | 行驶中 `sub_000A3CA0` 从游戏线程 ~7% 降到不在前列（§6.1） | 开 |
| 半分辨率 | env `RECOMP_GL_SCALE=0.5` | 冷机单对：72.0 vs 77.8 ms | 未启用 |

噪声：同一设置的相邻窗口差 ±4 ms；温度过 70 ℃（第一个被动温控点）后 CPU 在 1200/1008 MHz 之间跳，每 draw 的驱动耗时随之 29→35 µs。小于 5 ms 的差值都要多轮交替才能下结论。

## 2. 绘制距离：游戏怎么决定画不画

GC 版有符号（`PortMaster/nfsu2-gc/Speed.elf.c`），Xbox 版按结构和常量对上：

| GC 函数 | Xbox | 作用 |
|---|---|---|
| `eDisplayFrame` | `sub_000A1F30` 等 | 每帧调 `SetupSceneryCullInfo`（Xbox `sub_0009E0D0`）、`DoCulling` |
| `WhatSectionsShouldWeDraw` | — | 赛道预烘焙的可见 section 列表，与距离无关 |
| `ScenerySectionHeader::TreeCull` | — | section 内包围盒树 + 6 面视锥 |
| `ScenerySectionHeader::DrawAScenery` | **`sub_001C69C0`** | 逐实例：像素大小阈值 + 选 LOD |
| `eView::GetPixelSize` | **`sub_001C6900`**（全 gen 唯一调用者是上面那个） | `px = r'·K / (d − r')`，r' = 半径 + 6.0 |

Xbox 版阈值（`DrawAScenery` 里的整数比较）：普通视图 px < 17 不画，分屏 < 23，0x20 视图 < 16，阴影/反射视图 < 32；同一个 px 还用来选 LOD（`px / max(r2,6)` 与 9.5 比，常量在 0x35CC5C）。所以可见距离 ≈ r'·(1 + K/17)，物体越大看得越远——这就是 NFSU2 比 GTA VC 画得远很多的原因。

做法：`src/recomp_manual.c` 包一层 `sub_001C6900`，返回值乘 `PX_PCT/100`。`PX_BIG>0` 时半径（+6）≥ PX_BIG 的实例不缩放。`PX_STAT=1` 打印半径直方图（每 40000 次调用一行）：

```
<8:3652 <16:14073 <32:9013 <64:5066 <128:812 <256:249 <512:4807 <1024:2202 <4096:63 <16384:63
```

半径 ≥ 4096 的两档（各 63 次）是天空/地平线一类；256-1024 是楼。小物体（< 64）占绝大多数 draw。

### 2.1 实测（起跑线，dc / 帧时间）

| PX_PCT | dc | ms | 画面 |
|---|---|---|---|
| 100 | ~2000 | 105-120 | 原版 |
| 70 | 1657 | 88 | 看不出 |
| 50 | 1317 | 80 | 看不出 |
| 35 | 1180-1260 | 74-85 | 用户试玩：不影响体验 |
| 25 | 1141 | 79 | 同 35 |
| 15 | 955 | 66 | 远景楼还在 |
| 10 | 808-922 | 51-56 | 天空比相邻基线暗（22 vs 31-34）：天空被剔掉一部分 |
| 10 + PX_BIG=200 | 890-910 | 57-61 | 天空保住（亮度高于基线） |

行驶中（按住油门，PAD_SCRIPT `36000:rt:150000`）：10/200 dc 470-600、57-67 ms；35 dc 880-900、77-82 ms，截图未见缺块。

**注意天空亮度会随比赛时间自己变暗**（同一设置下 56 → 18），比较截图必须和相邻的基线窗口比，不能和开头比。之前"PX 10 天空变黑"的判断一半是这个。

## 3. GL 层：一个 draw 的 CPU 时间（PX 10，trace 每 16 个 draw 采 1 个）

| 阶段 | µs |
|---|---|
| glDrawElements（驱动） | 29-35 |
| textures（4 个 stage） | 8-10 |
| surface（surf_bind，含采样开销） | 6.6-7.7 |
| vertices | 5 |
| uni-c（c[] 上传） | 3.6-4.3 |
| 其余 7 段 | 各 1-3 |
| 合计 | 65-77（每个标记自带 ~1 µs 记录开销） |

- `glDrawElements` 的 29 µs 与空 draw 基准（~24 µs）接近，只能靠减少 draw。
- **跳过 c[] 的 glUniform4fv（`EXP_NOUNI=1`，画面会坏，只测时）**：帧 120 → 101 ms，`glDrawElements` 28 → 24 µs。uniform 改动会推高驱动在 draw 时的开销。但把 c[] 挪进 UBO 并没有省下来（§1），在这个 blob 上 UBO 切换更贵。
- perf（PX 35，整进程 8 s）：GL 线程 31%、游戏主线程 26%、executor 18%、APU 10%；GL 线程内 libmali 60%、我们 22%、libc 10%。

## 4. 合并的空间（`EXP_MERGESTAT=1`）

PX 10 下每帧 ~800 个 draw **全部是 xform 2（顶点程序）**，HUD 也是：
- 程序、4 个纹理、surface、图元、混合/alpha/深度/combiner 寄存器都相同的连续段：~220-237 段（-71%）。
- 再要求顶点常量也相同（`vp_const_gen` 不变，偏保守）：647-665 段（-19%）。

所以不改 shader 的合并（docs/09 §8 方案 A）上限约 150 个 draw；要拿到 -71% 必须解决每个 draw 不同的模型矩阵（docs/09 在 Mali 上失败的那条路）。

## 5. 画质开关（POKE，PX 10 下重测）

| 地址 | 关掉后 | 每帧收益 | 判断 |
|---|---|---|---|
| 0x3994C4 | 车灯投到路面/场景的光斑消失（守着 `sub_001C7130`，对应 GC `CallbackScenery(..., eTagHeadlightCallback)`） | 两轮 +1.6/+1.9 ms（无收益） | 不砍 |
| 0x3994C8 | 游戏的色彩/对比度滤镜没了，画面发灰 | 两轮 -4.6/-0.9 ms | 可选，收益小 |
| 0x399540 | 灯光光晕（守着 `sub_000AC560` eRenderLightFlares） | 两轮 -0.9/-8 ms | 可选，噪声大 |
| 0x399544 | **整个 HUD 消失** | ~-4.6 ms，-190 draw | 不可用 |
| 0x39949C、0x399530 等 9 个 | 画面无可见变化 | 噪声内 | — |

单窗口测出的 -9 ms（0x3994C4 第一次）是噪声，重复后消失。效果开关的收益都远小于绘制距离。

## 6. 原生替换（`src/recomp_manual.c`）

- `bMulMatrix`（`sub_002213EA`，4x4 SSE 乘法，游戏线程 8.5%）、`_ftol2`（`sub_002A68EC`，585 个调用点，lifted 版每次走 floor/fmod）。
- 与 lifted 体逐次比对：PC 1022 万 + 设备 183 万次矩阵乘、PC 1678 万 + 设备 734 万次 ftol，0 差异（`RECOMP_MATMUL_VERIFY=1` / `RECOMP_FTOL_VERIFY=1`）。
- 需要 gen 目录里把原函数改名为 `_gen`（`tools/regen.sh` 按 `extern ... _gen` 自动做；手工改名的 gen 要带 `-DNFSU2_NATIVE_MATMUL=ON -DNFSU2_NATIVE_FTOL=ON -DNFSU2_WRAP_PXSIZE=ON -DNFSU2_WRAP_RAMP=ON`）。
- 帧时间上看不出来：GL 线程是瓶颈时游戏线程省下的时间被等待吃掉。PX 10 之后 `wait` 开始出现（GL 线程会等游戏线程），这部分才会显出来。

### 6.1 屏幕滤镜的对比度 ramp

PX 10 下行驶时 GL 线程开始等游戏线程（`wait` 10-22 ms），游戏线程 perf 第一名是 `sub_000A3CA0`（进程样本 2.2%，约游戏线程 7%）：
- 调用链：`sub_000AC6F0`（0x3994C8 = 滤镜开关）→ `sub_0009BD70`（每帧把滤镜参数往目标缓动）→ `sub_000A3FA0` → `sub_000A3CA0`（在 CPU 上逐 texel 生成 ramp 纹理，每个 texel 4 次 _ftol2）。
- `sub_000A3FA0` 本来就有缓存：参数（0x39D304 + 16 i）与上次生成时（0x3E8BF0 + 16 i）完全相等才跳过。但缓动让参数每帧都差一点点，所以几乎每帧重建，GL 线程还要重新上传这张纹理。
- 改法（`src/recomp_manual.c`）：参数与上次生成的差都 ≤ RAMP_EPS×1e-4 时跳过；不更新缓存，漂移累计超过阈值就重建。默认 0.002，小于 ramp 8 位输出的一个台阶。
- 行驶中 perf：`sub_000A3CA0` 和它带出的 `_ftol2` 都掉出前列，游戏线程样本 29.7% → 27.2%。起跑线上参数不动，本来就不重建，帧时间无差别。截图色调正常。

## 7. 复现

```bash
# 设备构建（gen 里 sub_001C6900 / sub_002213EA / sub_002A68EC / sub_000A3FA0 已改名 _gen）
NFSU2_GEN_DIR=$PWD/build/gen-hud EXTRA_CMAKE="-DNFSU2_WRAP_FENGHUD=ON -DNFSU2_WRAP_PXSIZE=ON -DNFSU2_NATIVE_MATMUL=ON -DNFSU2_NATIVE_FTOL=ON -DNFSU2_WRAP_RAMP=ON" bash r36s/build.sh
# 同一次启动里交替比较
python3 -u r36s/autotest.py --host 192.168.10.241 --sweep --set a:PX_PCT=35 --set b:PX_PCT=10,PX_BIG=200
# 每 draw 阶段
python3 -u r36s/autotest.py --host 192.168.10.241 --trace --env RECOMP_KNOB_PX_PCT=10
python3 tools/trace_draw_phases.py build-r36s/autotest/<dir>/xtrace/base.pftrace
```

autotest 的"进入比赛"判定：大 draw（≥50 索引）≥ 350，或 HUD live 之后 250 帧（PX 10 时起跑线的大 draw 不到 350）。

## 8. 下一步

1. 用户试玩 PX 10 + PX_BIG 200，确认行驶中没有明显的冒出/消失。
2. textures 阶段 8-10 µs：每帧每纹理一次 `bytes_hash`（GL 线程 1.7%）和 4 个 stage 的查表，可以按"纹理 VA 与上一 draw 相同"短路。
3. 合并：不改 shader 的只有 ~19%；要更多得先在真机上小规模验证"每 draw 一个矩阵"的 shader 方案（per-vertex 块号属性代替 gl_VertexID 循环），见 §4。
4. 温控：70 ℃ 被动点后降到 1008 MHz，驱动每 draw 慢 ~20%。
