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
# 合并上游后（§9）不再需要手工改名 _gen，也不需要额外的 -D 选项：
# tools/regen.sh 按 recomp_manual.c 的 extern ... _gen 行自动处理
NFSU2_XBE=$PWD/build/game/default.xbe NFSU2_GEN_DIR=$PWD/build/gen-up bash tools/regen.sh
NFSU2_GEN_DIR=$PWD/build/gen-up bash r36s/build.sh
# 同一次启动里交替比较（--host 见 AGENT.md）
python3 -u r36s/autotest.py --host <r36s> --sweep --set a:PX_PCT=35 --set b:PX_PCT=10,PX_BIG=200
# 每 draw 阶段
python3 -u r36s/autotest.py --host <r36s> --trace --env RECOMP_KNOB_PX_PCT=10
python3 tools/trace_draw_phases.py build-r36s/autotest/<dir>/xtrace/base.pftrace
```

autotest 的"进入比赛"判定：大 draw（≥50 索引）≥ 350，或 HUD live 之后 250 帧（PX 10 时起跑线的大 draw 不到 350）。

## 8. 下一步

1. 用户试玩 PX 10 + PX_BIG 200，确认行驶中没有明显的冒出/消失。
2. textures 阶段 8-10 µs：每帧每纹理一次 `bytes_hash`（GL 线程 1.7%）和 4 个 stage 的查表，可以按"纹理 VA 与上一 draw 相同"短路。
3. 合并：不改 shader 的只有 ~19%；要更多得先在真机上小规模验证"每 draw 一个矩阵"的 shader 方案（per-vertex 块号属性代替 gl_VertexID 循环），见 §4。
4. 温控：70 ℃ 被动点后降到 1008 MHz，驱动每 draw 慢 ~20%。

## 9. 合并上游（antoxa2584x/nfsu2-sw v0.5.13，2026-10-18）

上游在我们分叉（5a24c21）之后有 34 个提交，合并提交 fd80e1e。对 R36S 有用的部分：

| 上游改动 | 对我们 |
|---|---|
| 59.94 Hz vblank（原来 now+16 ms 配 10 ms 等待，实际约 46 Hz；游戏隔一个 vblank 翻帧，上限约 23 fps） | `[fps]` 行现在带 vblank 频率，设备上 59.9-60.0 Hz；PC 进比赛 30 s → 12 s |
| 种子 0x930D0（比赛每帧的间接调用失败） | 日志里的 `[15] 0x000930D0` 消失 |
| 执行器里的视野外批次剔除（`RECOMP_CULL`，默认开，跑在 executor 线程） | 起跑线 dc 834-871 → 712-824 |
| 原生重写：_ftol2、矩阵乘、GetPixelSize、颜色查找表、sin、点/矩阵变换等 | 我们自己的 bMulMatrix/_ftol2 删掉，用上游的；PX_PCT/PX_BIG 包在上游原生 GetPixelSize 外面；RAMP_EPS 在上游原生颜色表前面挡掉多余的重建 |
| Vulkan 实例化、Switch 专属改动 | R36S 用不上 |

冲突处理：CMake 保留我们的 LTO/PGO 块；保留 xtrace 标记、ES shader 路径、NFSU2_SAVE_DIR；`[fps]` 行同时带 draws/frame 和 vblank。

**FMA**：设备上 `RECOMP_NATIVE_CHECK=1` 报 `sub_0004B260`（sin）每 100 万次约 4 万次不一致，PC 上为 0。原因是 aarch64 的 GCC 默认把 a*b+c 融合成 fmadd，x86 没有 FMA。`recomp_manual.c` 现在固定 `-ffp-contract=off`（原生实现给出 x86 的结果）；`-DNFSU2_GEN_FP_EXACT=ON` 让 lifted 代码也不融合，用来在设备上验证：这样编出来的版本，运行到的 11 个原生函数全部 0 不一致。正常构建不开这个选项（lifted 代码里的 FMA 一直都在，画面没问题）。

**实测**（起跑线，PX 10 + PX_BIG 200，每次冷却到 ≤ 66 ℃ 后单独启动，7 个窗口取中位数，两个版本交替跑）：

| 版本 | 帧时间（ms） | dc |
|---|---|---|
| 合并前 | 55.7 / 57.0 | 834 / 871 |
| 合并后 | 49.9 / 54.5 / 53.3 | 712 / 824 / 745 |

平均 56.4 → 52.6 ms（-7%），主要来自剔除减掉的 draw。起跑线是 GL 线程瓶颈，vblank 修正和游戏线程的原生重写在这里看不出来；帧时间低于 33 ms，或者游戏线程成为瓶颈（行驶中 `wait` > 0）时才会显出来。

## 10. 合并之后：两条瓶颈线都在 ~50 ms

### 10.1 线程占用（起跑线，PX 10 + PX_BIG 200，`threads.sh 5`）

| 线程 | 一个核的 % | 所在核 |
|---|---|---|
| GL | 93-104 | 0-2 |
| 游戏主线程 | 75-81 | 3（guest 核） |
| executor | 40 | 0-2 |
| APU | 27-30 | 0-2 |
| EA 混音 g:274CA0 | 15-16 | 3 |
| mali-cmar-backend | 11 | 0-2 |
| 流读取 g:2B1A22 | 4.8 → 2.8（§10.4） | 3 |
| timer/DPC、OHCI、g:251E1F | 1.4-3.8 | 3 |

- GL 线程（perf，8 s）：libmali 51-60%，我们的代码 21%，libc 10%（主要是驱动自己调的 memcpy），内核 6%。
- guest 核上主线程 + 混音 + 其余 guest 线程已经接近 100%。sweep 里 `gl` 低于 `ms`、`wait` > 0 的窗口（GL 线程在等游戏线程）和 `gl` = `ms` 的窗口交替出现：两边都在 ~50 ms，只压一边帧时间不动。
- 之前的打断分析（perf switch-events + xtrace）：GL 线程的 draw 只有 4% 时间不在 CPU 上，长 draw 基本是驱动自己在算；APU 很少抢 GL 线程；主要的打断者是 gptokeyb、nv2a flag 线程、pulseaudio、kworker、mali-cmar；所有 IRQ 都在 CPU0。

### 10.2 GL 线程

| 改动 | 内容 | 结果 |
|---|---|---|
| clear 去重（knob `CLR_DEDUP`，默认 1） | `RECOMP_GL_SKIP` 跳过的 pass（道路反射 320x240、车身环境图 128x128）的 draw 没了，clear 还在，每个 ~0.3 ms 驱动时间。clear 的颜色（按 surface）和深度（按深度缓冲，环境图各面共用一个 Z）上一次写入就是同一个 clear 时跳过；任何 draw 都会作废。同一个 clear 做两次等于一次，画面不变 | 每帧 6.5 个 clear 去掉 2-3.5 个；第一轮 A/B 三个窗口 -1.0/-2.2/-1.6 ms，第二轮在噪声内 |
| 纹理 stage 备忘（knob `TEX_MEMO`，默认 1） | 同一帧内、没有纹理删除或新 surface 时，stage 的地址和格式与上个 draw 相同就直接用上次的 GL 纹理，跳过 surface 查找和 `tex_get` | `tex_get` 样本减半；帧时间看不出 |
| surface 地址紧凑数组 | `surf_find` 每 draw 1 次 + 每 stage 1 次，原来每个槽位一条 cache line | 小 |
| 索引 32→16 位用 NEON | 原来每个索引一次 2 字节写到映射的 ring | 小 |
| 每帧 ring 字节数（`RECOMP_GL_SURF_STATS` 行里） | 顶点 ~670 KB、索引 ~106 KB；映射的 ring 上 memcpy 1.5 GB/s，和普通内存一样，所以上传本身只占 ~0.5 ms | 诊断 |

PC 上 `tools/pc/glab.sh` 同一次运行里交替开关 `CLR_DEDUP`、`TEX_MEMO`：A/B 差异不超过场景自身的变化（静止时 0 像素）。试过把 `byte_f` 的除法换成查表：样本没减少（耗时在读寄存器，不在除法），已撤回。

### 10.3 guest 核：混音

EA 混音线程里再换两个叶子函数为原生（`src/recomp_manual.c`，`RECOMP_NATIVE_CHECK=1` 对照 lifted 版）：

- `sub_0027C040`：单极点滤波，x87 上按 double 累加。lifted 代码直接调用它，所以按名字替换（regen 把原体改名为 `_gen`）。
- `sub_0027B5B5`：float → int16，用加魔数取尾数位的方法限幅。只经分发表调用；obj+8 有回调时走 lifted 版（实测没出现过）。

PC 32768 次、设备 65536 次，0 不一致。两者在混音线程里的占比 7.6% + 6.6% → 4.5% + 3.8%，约等于 guest 核的 1%。剩下最大的 `sub_00271417`（12%）是命令分发循环，开销在间接调用本身。

### 10.4 guest 核：内核对象影子表

`ke_shadow_lookup`（guest 对象 VA → 主机 HANDLE）原来顺序扫 4096 个槽位，查不到（kdisp 认识的 guest 对象，最常见）时全扫一遍。流读取线程 g:2B1A22 36% 的时间在这里。改成按 VA 开放寻址（线性探测，删除时回移，空槽结束查找），接口和锁不变；随机插入/删除/查找 500 万次与参照表对比 0 差异。g:2B1A22 4.8% → 2.8%，`ke_shadow_lookup` 从它的 profile 里消失。

### 10.5 结论

这一轮每项都是精确改动（画面、计算结果不变）。冷却到 ≤ 66 ℃ 后单独启动、两个版本交替各 3 次（每次 7 个窗口取中位数）：

| 版本 | 帧时间（ms） | GL 线程（ms） |
|---|---|---|
| 合并后（本轮之前） | 50.9 / 50.1 / 49.8，均值 50.3 | 50.4 / 49.0 / 49.1，均值 49.5 |
| 本轮 | 50.2 / 49.4 / 50.4，均值 50.0 | 50.2 / 48.2 / 46.6，均值 48.3 |

GL 线程少了 ~1.2 ms，帧时间只少 0.3 ms（噪声内）：GL 线程一快，guest 核就成了瓶颈。

### 10.6 行驶中的帧率（本轮版本）

一次启动，`RECOMP_PAD_SCRIPT=36000:rt:200000`（只踩油门不转向，车会贴着墙蹭；截图里 64 MPH、第 1 圈 1:18），11 个窗口配置相同（PX 10 + PX_BIG 200，`env.txt` 现状），开始时 73 ℃：

| 窗口 | ms | gl | wait | dc | CPU MHz |
|---|---|---|---|---|---|
| 1-5 | 51.1 / 53.7 / 60.9 / 53.9 / 61.5 | 45.9 / 42.5 / 42.9 / 41.0 / 51.7 | 4.3 / 10.5 / 11.1 / 5.8 / 10.4 | 753 / 429 / 396 / 415 / 645 | 1200 |
| 6-11 | 57.4 / 54.2 / 53.5 / 57.4 / 58.8 / 58.2 | 57.4 / 53.8 / 53.5 / 57.4 / 58.8 / 58.2 | 0 | 492-645 | 1200，后段 1008 |

- 中位数 57.4 ms（**17.4 fps**），均值 56.4 ms，范围 51-61.5 ms（16.3-19.6 fps）。
- 合并前同样开车的 PX 10 窗口（drive3/drive4）中位数 72.6 / 60.8 ms，现在快了 5-20%。
- 前 5 个窗口 `wait` 4-11 ms、`gl` 41-52 ms：dc 降到 400 多时 GL 线程有余量，帧时间由游戏线程决定（行驶比起跑线多了物理、AI、流读取）。后 6 个窗口 `wait` = 0，`gl` = `ms`，又回到 GL 线程瓶颈，其中一部分在 1008 MHz（温控）。
- 起跑线（§10.5）约 50 ms / 20 fps；开车平均慢 6-7 ms。

### 10.7 提高 CPU 上限：反而更慢

启动脚本把 `scaling_max_freq` 限在 1209600（AGENT.md §3.3，为了少碰 70 ℃ passive trip）；CPU 本身有 1248/1296/1512 MHz。启动脚本加了 `NFSU2_CPU_MAX=<kHz>`（写在 env.txt 里，退出时照常恢复），起跑线冷却后单独启动、交替各 2 次：

| 上限 | 帧时间（ms） | 窗口里的 CPU / GPU MHz |
|---|---|---|
| 1200（现状） | 48.6 / 51.9，均值 50.3 | 全程 1200（偶尔 1008）/ 520 |
| 1296 | 51.1 / 53.9，均值 52.5 | 1008-1296 / 多数 400-480 |
| 1512 | 51.9 / 54.1，均值 53.0 | 1008-1512 / 多数 400-480 |

上限一放开，温度很快过 70 ℃，passive trip 把 CPU、GPU、DMC 一起降下来：CPU 在 1008-1512 之间来回跳，GPU 从 520 掉到 400-480。GL 线程对 GPU/内存频率敏感，结果比固定 1200 慢 2-3 ms。现在的上限是对的；要更高主频，得先解决散热（散热片/风扇）。`NFSU2_CPU_MAX` 留着给加了散热的机器用。

## 11. 合并连续的同状态 draw（knob `GL_MERGE`，现在默认 0，见 §12 的稳定性一节）

做法（`nv2a_gl.c`）：三角形列表 draw 画完前先压住不提交；下一个 draw 走完状态、纹理、uniform 的代码后，如果期间一次 GL 调用都没有（状态代码只在有变化时调 GL，所以"没调用"就等于"状态完全相同"），并且顶点布局相同，就把它的顶点接在 ring 里上一个 draw 后面、索引加上偏移接在后面，两个 draw 共用一次 `glDrawRangeElements`。`gl_api.h` 里每个 GL 调用的计数钩子会先提交压住的 draw，所以顺序和状态都和原来一样。限制：所有属性直接取自顶点流（没有转换的）、单一 stride 交错布局、16 位索引、持久 ring、不跨 ring 段。

- PC（`RECOMP_GL_PERSIST=1` 让桌面 GL 也走持久 ring）：同一次运行里交替开关，静止画面 0 像素差异。
- 设备：每帧 ~790 个 draw 里 163-167 个并进别的 draw（-21%），GL 调用每帧少 600-750 个。
- 起跑线，冷却后单独启动、交替各 3 次：关 52.3 / 50.4 / 52.2（均值 51.6 ms），开 48.7 / 51.0 / 50.7（均值 50.1 ms），-1.5 ms（-3%）。同一次启动里交替的 sweep：-1.4 / -2.3 ms。省下的比 163 × 30 µs 少：被并进去的多是小 draw，驱动开销里有一部分跟顶点数走。
- 行驶中（drive6，开关交替 11 个窗口）：场景变化大（dc 336-719），开 56.4 / 关 55.8 ms 中位数，在噪声内。
- 稳定性：第一次在设备上开着它（同时开 `RECOMP_NATIVE_CHECK=1`）跑到第 1254 帧时整机重启，日志在帧中间截断，没有内核日志（journal 不持久）。之后开着它又跑了 1 次 sweep + 3 次完整启动 + 1 次行驶，没有复现。原因未定；如果再出现，先在 env.txt 里写 `RECOMP_KNOB_GL_MERGE=0` 排除它。

混音线程又加了 `sub_0027A277`（混响里的反馈梳状滤波，4 个一组，按名字替换）：`RECOMP_NATIVE_CHECK=1`（比较输出、延迟线和 eax/ecx/edx）在设备上 17408 次 0 不一致（PC 那次只打印到第 1 次，也是 0）。`sub_0027C040` 和它都补上了原函数留下的 `g_ebp`（之前的对照不比这个值）。混音线程剩下最大的是命令分发循环 `sub_00271417`（12.8%），开销主要在间接调用，单独做原生收益约 guest 核 1%。

再往下要有结构性的改动：
1. GL 线程 60% 是驱动，每个 draw ~30 µs。要么减少 draw（合并需要"每 draw 一个矩阵"的 shader，§4），要么把我们这边的准备工作（顶点/索引拷贝、纹理哈希、uniform 打包）挪到 executor 线程（它只用了 40%）。
2. guest 核：主线程的 profile 很平，最高的 `sub_001C69C0`（DrawAScenery）只有 2.4%；混音还剩 ~14%。

## 12. 上游的"车身反射"开关（Options → Video → Car Reflections）在 R36S 上没有效果

用户关掉后觉得反射还在。查证（`src/recomp_manual.c`、`gl_psh.c`、设备实测）：

- 开关本身生效。`nfsu2x_options.txt` 里 `reflections=0` 时，`sub_0009B720`（环境贴图更新调度）之后把 6 个立方体贴图面的视图字节（`0x3F2AD0` 表，+8）清零，游戏不再渲染这些面。设备日志 `[options] car reflections off`，pass 统计里 128x128 的 pass 从每帧 2 个 draw 变成 0。
- 但它在 GL 后端本来就没有可见效果。像素着色器里 `CUBE_MAP` 纹理模式（车身反射用）只有 Vulkan 后端会采样立方体贴图，GL 后端固定输出黑色（`gl_psh.c`）。设备上的 `env.txt` 里 `RECOMP_GL_SKIP=320x240,128x128` 本来也会丢掉这些 pass。所以在 R36S 上开关无论开还是关，车身上画出来的东西一样。
- 车身上看到的光泽是车漆自己的高光（顶点程序里的镜面项），不是反射。

起跑线实测，每次冷却到 66 ℃ 以下单独启动，关/开交替各 2 次（`GL_MERGE=0`）：

| 车身反射 | 帧时间（ms） | GL 线程（ms） | 128x128 draw/帧 |
|---|---|---|---|
| 关 | 50.2 / 48.8 | 44.7 / 44.8 | 0 |
| 开 | 49.7 / 49.0 | 46.2 / 47.3 | 2 |

车尾区域截图：关与开的像素差不比"同一设置跑两次"的差大。结论：这个开关在 GL 后端只能省 ~2 个 draw 和 ~2 ms 的 GL 线程时间，帧时间没有可测差别；它的价值在 Vulkan 后端（Switch/PC），不在这里。

**稳定性**：`GL_MERGE=1` 的情况下，设备整机重启过两次（§11 的一次，加上这一轮的第一次启动）。两次都在第 1000-1250 帧附近，日志在帧中间截断，系统日志重启后不保留。`GL_MERGE=0` 的 4 次启动 + 之前的一次 sweep 都正常。原因没查清；怀疑和持久 ring 里手工拼接的顶点/索引有关（跨 ring 段、GPU 还在读的区间）。这一节的所有测量都在 `GL_MERGE=0` 下做。因此 `GL_MERGE` 的代码默认改回 0（要用就在 env.txt 里写 `RECOMP_KNOB_GL_MERGE=1`）；另外 `merge_try` 加了一道检查：被并入的 draw 的索引必须都小于它自己的顶点数，否则不并。

## 13. 再扫一遍画质开关，和一帧 draw 的构成

### 13.1 方法

- `tmp/scan_flags.py`（见 docs/08 §6.3 的思路，扫 `build/gen-up`）：列出 .data 里被直接寻址读取、从不被直接寻址写入、初值非 0 的全局，共 759 个，其中初值为 1 的布尔量 63 个。之前测过 12 个（docs/08、本文 §5），这次测了其余里读它们的函数在渲染、车辆渲染或世界逻辑里的 33 个。
- 设备上一次启动，每个开关用 `POKE_VAL=0` 压到 0，前后各夹一个基线窗口，`GL_MERGE=0`（见 §12）。单个窗口的噪声是 ±2 ms、±30 个 draw，所以只有重复测过的结论才算数。
- 画面影响用 PC 上 `tools/pc/glab.sh POKE_VAL`（同一次运行、暂停的比赛里开关对比）看。

### 13.2 结果

| 地址 | 作用 | draw | 帧时间 | 画面 |
|---|---|---|---|---|
| `0x3CC5E4` | `DrawCars` | -120 | -4.0 ms | 车全没了（只作参照） |
| `0x3A2728` | FE 管理器渲染的总开关 | -70..-100 | -2..-3 ms | 整个 HUD 和 EA Trax 提示都没了（截图确认） |
| **`0x3CC86C`** | **天空** | ~0 | **-2.5 / -1.7 / -1.5，均值 -1.9 ms** | **天空变纯黑（云和红色光晕没了）；PC 上同一帧 9% 像素不同，只在天空区域** |
| `0x3CC870`、`0x3CC874` | 与 `0x3CC86C` 同一个函数（`sub_001FECC0`） | ~0 | 噪声内 | PC 上开关对比看不出变化 |
| `0x3CC838` | 车辆部件渲染里读它，像颜色/透明度的数值，不是布尔量 | 稳定 -24..-38 | -4.6 / +0.8 / +0.8 | 起跑线上几乎看不出（82 像素 >6 级），行驶中没验证 |
| 其余 28 个 | `3A4008 3A541C 3CBED0 3CBED4 3CC0BC 3CC824 3CC834 3CC884 3CD990 3A534C 39FF04 3A3F64 3A3FFC 3A400C 3A4A8C 3A53C8 3A5454 3A5458 3A55AC 3A55E0 3A57D0 3A5D30 3A5D3C 3A5D94 3A7A48 3CF6C0 3CFD50` | 噪声内 | 噪声内 | — |

- 天空是 2 个覆盖整个视野的 draw（每个 216 个索引），每帧每像素都被它画一遍，然后被场景盖掉大部分。省的是填充，所以 draw 数不变。对照：把渲染分辨率降到一半（像素数 1/4）整帧只快 5.8 ms，也就是 640x480 下全部像素开销约 8 ms，天空占了约四分之一。
- 想要的话：`env.txt` 里加 `RECOMP_POKE=0x3CC86C:0`（和默认的雨/道路反射 poke 合并，最多 8 项）。代价是夜景失去云和光晕。默认不开。
- 结论：画质开关这条路基本榨干了。没有哪个开关既明显省时又不改画面。

### 13.3 一帧 draw 的构成（`DUMP_DRAWS=1`，现在统计整整一帧）

`DUMP_DRAWS` 现在在一个 flip 处开始计数、下一个 flip 处输出，所以是完整的一帧；输出里新增按纹理、顶点程序、"变换×尺寸"的汇总，以及整帧 draw 顺序的游程编码。

**全部 draw 都走顶点程序（`xform` = 2）**，包括 HUD；没有一个是预变换（screen space）或固定管线的。

| 场景 | draw 总数 | 小于 50 个索引 | 构成 |
|---|---|---|---|
| 起跑线倒计时 | 740-855 | 508（68%） | 顶点程序 12：434 个（379 个小的）；程序 62（车）287 个；程序 36：12 个；程序 0（后处理）：10 个 |
| 行驶中 | 480-540 | 332（约 65%） | 场景 250-300、车 77-82、粒子和光斑四边形 37（4 个索引）、离屏 surface 和后处理 26、HUD 94（一连串 4 个索引的字符四边形） |

- 一个 draw 在 GL 线程上的开销约 45 µs（驱动 30 + 我们 15），所以行驶中的 480-540 个 draw 约占 GL 线程时间的一半以上。
- 其中很大一部分是同一纹理、同一状态的连续小 draw，只有 `c[]`（每个对象一个矩阵）不同。HUD 的 94 个字符四边形是一整段连续的。§11 的合并因为 `c[]` 一变就要调用 `glUniform4fv`，没法合并它们。

### 13.4 下一步

执行器里已经有一个跑在 CPU 上的顶点程序解释器（`nv2a_pb_exec.c` 的 `vp_run`，原本给没有 GPU 顶点程序的旧后端用，只输出位置、漫反射色、纹理坐标 0）。思路：对小 draw 在执行器线程上算好顶点，作为已变换顶点交给 GL，常量不再随 draw 变化，相邻的小 draw 就能合并。见 §14。

## 14. 在 CPU 上预变换小 draw（曾有 knob `VP_PRE`）：没有收益，代码已回退
**这一节只留记录，代码没有进仓库**：`VP_PRE`、生成的顶点程序（`tools/gen_vp_programs.py`、`nv2a_vp_progs.inc`）、转换布局的 draw 合并、strip/fan 转 list、纹理坐标裁剪、缺失属性影子都已撤掉。下面的 `VP_PRE` knob 等名字指的是当时的实验。保留下来的只有 §13 的 draw 普查扩展，以及 `GL_MERGE` 默认 0 加 `merge_try` 的索引范围检查（§12）。
### 14.1 做了什么（实验时）
- 执行器（`nv2a_pb_exec.c` 的 `raw_batch`）对索引数 ≤ `VP_PRE_IDX`（默认 120）、顶点 ≤ 2048 的三角形类 draw，在 CPU 上跑顶点程序，把结果（屏幕位置和 1/w、oD0、oD1、oFog、oT0..oT3）当作 `xform = 0` 的已变换顶点交给 GL。这类 draw 不再依赖随对象变化的常量，相邻的可以合并（`GL_MERGE`，现在也支持这种"全部转换"的布局，以及 ≤ 64 个索引的小 strip/fan 转 list）。
- 顶点程序解释的速度（A35，每顶点）：通用 `vp_exec` 约 10 µs；按表解码的向量解释器 5.8 µs；**离线生成的逐程序直线 C**（`tools/gen_vp_programs.py` → `xboxrecomp/src/kernel/nv2a_vp_progs.inc`，按指令字的 FNV 哈希查找）约 1.4 µs（含取属性）。现在有 5 个程序：`80368534`（vp12 场景，24 条）、`DE9A2CBF`（vp0）、`EDC24FC5`（vp36）、`B683644A`（vp87 蒙皮，48 条）、`BE7B442D`（vp62 车，63 条）。没有生成代码的程序、写常量的程序、w ≤ 1e-4 或非有限的结果，都回退到 GPU 路径。
- 正确性：`RECOMP_VP_PRE_CHECK=1` 把生成代码和 `vp_exec` 逐顶点比，PC 上 1150 万顶点 0 次不一致。PC 的 glab 画面 A/B 在对照噪声内，合并 A/B 完全一致（每帧并入 198-229 个 draw）。
- GL 线程：像素着色器不读的纹理坐标（模式 0、依赖查找、GL 上的立方体）不再上传；转换属性的打包改成整 16 字节的 NEON 读写。

### 14.2 设备上的结果（冷却后单次启动 A/B，起跑线）
| 配置 | 帧时间 ms（中位数） | GL 线程 ms |
|---|---|---|
| 基线（`VP_PRE=0`） | 49.4 / 49.3 / 48.2 / 51.5 | 48.7 / 47.9 / 47.4 / 48.6 |
| `VP_PRE=1` + `GL_MERGE=1`，IDX 120 | 51.7 / 52.4 / 52.5 | 51.7 / 51.7 / 51.6 |
| 同上，IDX 400 | 51.9 | 51.9 |
| 同上，IDX 3000 | 54.8 | 46.8 |

- 同一个二进制的两次基线相差 3 ms，所以结论只看方向：IDX 120/400 比基线慢 2-4 ms，GL 线程是瓶颈（`gl` ≈ `ms`）。
- IDX 3000（几乎所有 draw 都预变换）时 GL 线程才降到 46.8 ms，比基线只快 0.6 ms，而执行器线程被拖到 54.8 ms，整帧更慢。

### 14.3 为什么没有收益（`RECOMP_GL_CALL_STATS=1`，每帧）
| 调用 | 基线 | `VP_PRE=1` + `GL_MERGE=1` |
|---|---|---|
| `glDrawRangeElements` | 848 | 314 |
| `glUniform4fv` | 1030 | 424 |
| `glVertexAttribPointer` | 2557 | 1458 |
| `glVertexAttrib4f` | 116 | 1796 |
| `glEnable/DisableVertexAttribArray` | 18 / 18 | 458 / 458 |
| `glUseProgram` | 65 | 168 |
| 总调用 | 5558 | 6035 |

- draw、uniform、指针都少了，但预变换 draw 和 GPU 程序 draw 交替出现：属性集合不同（要开关属性数组、给缺失的属性重设常量），着色器也不同（`UseProgram` 多约 100 次，随后 sampler 等 uniform 要重设），省下的调用比新增的少。
- 交替只在几乎所有 draw 都预变换时才消失，但那时执行器线程变成瓶颈。算到最后，GL 线程能省的只有零点几毫秒：合并后 draw 少了，但每个 draw 的顶点变大（预变换顶点最多 8 个 vec4，128 字节），写 ring 的字节也多了。
- 结论：每 draw 约 30 µs 的估计只对"状态完全不变的连续 draw"成立（§11 的合并，设备上 −1.5 ms）；换成 `xform 0` 的第二套着色器和属性布局，会带来更多状态切换。`VP_PRE` 保留，默认关；`GL_MERGE` 默认仍为 0（两次设备挂死原因未明）。

### 14.4 顺带做的、与 `VP_PRE` 无关的修改（同样已回退）
- 缺失属性的常量：用 `s_gen_valid`/`s_gen_val` 影子，只在属性刚被数组读过或值变了才调 `glVertexAttrib4f`（基线路径每次属性集合变化原来要把所有缺失属性都设一遍）。每帧 flip 时清空影子（性能 HUD 和呈现会用属性数组）。设备上基线 49.3 → 48.2 ms 左右，但基线自己有 ±3 ms 的启动间波动，没单独测。起跑线截图画面正常。
- 下一步想法（没做）：把 GL 线程上预变换 draw 和 GPU 程序 draw 的属性布局统一，或让预变换只在整段连续的小 draw 上启用（先看 `DUMP_DRAWS` 的游程）。
- 影子那一条是唯一可能有独立收益的改动（基线 49.3 → 48.2 ms 只是启动间波动范围内的差别，没有证实），要的话可以单独重做，几十行。

## 15. 常量批处理（曾有 knob `GL_CBATCH`）和 Vulkan 每 draw 开销：GL 这条路没有收益
**代码已回退，这一节只留记录**。仓库里保留的只有 15.1 的统计（`EXP_MERGESTAT=1` 输出的 `[CBATCH]` 行）；实验代码备份在 `tmp/cbatch_backup/`（不在仓库里）。
### 15.1 统计（`EXP_MERGESTAT=1`，起跑线，每帧）
- 程序 draw 约 800 个；和前一个状态、顶点布局都相同的约 600 个，其中 `c[]` 完全不变 380、差 1-4 行 15、5-8 行 45、9-12 行 125、13-16 行 22。
- 允许最多 4 / 8 / 12 / 16 行不同时，连续段从 ~800 降到 406 / 357 / 231 / 209 段（理想上限）。
- 逐 draw 变化的行：vp 12（场景）是 c96-c99、c104-c109（对象矩阵），vp 62（车）再加 c123-c128。
### 15.2 做法
- 每个程序在前 300 对相邻 draw 里学习"会变的行"，生成一个变体：这些行改从 `nv2a_cd[]` 读，下标 `gl_VertexID >> 7`（每个 draw 的顶点放在 128 的整数倍处）。`GL_MERGE` 的合并逻辑改成按这张表把 draw 拼进一次 `glDrawRangeElements`。PC 上 glab 画面 A/B 在对照噪声内。
- 第一版表放在 UBO 环里（每次调用一个 `glBindBufferRange`）：设备上 59 ms，基线 48.5 ms；第二版改成普通 uniform 数组：58.6 / 62.2 ms，基线 49.6 / 50.9 ms；第三版让行集合在比赛中继续增长：55.8 ms。
### 15.3 为什么没有收益
- 合并不起来：第三版每帧 139 次表调用里 84 次只有一个 draw，结束原因是 `BindTexture` 53、`UseProgram` 36、`Uniform4fv` 32（每帧）。游戏的 draw 顺序在纹理和程序之间来回切，相同状态的连续段很短。
- 剩下的 draw 更贵：合成测试 `tmp/vkbench/glbench.c`（800 个 36 索引的 draw，每个改 5 行 uniform）里，同一着色器再加一个 768 vec4 的 uniform 数组，每 draw 从 14.3 升到 22.4 µs（数组 72 / 144 / 288 / 768 vec4：17.1 / 17.7 / 19.4 / 23.1 µs）。Mali 驱动每次画都按整个默认 uniform 块计费。
- 合并本身有效：同一测试 4 个 draw 合成一次调用、每次上传 4×18 行，每个原 draw 只要 4.3-5.5 µs。问题在于游戏里能合并的段太少。
- 结论：GL 层面上合并类优化（§11、§14、§15）都到头了。
### 15.4 Vulkan：同样的负载每 draw 约快一半
- 设备的 libmali 带 Vulkan 1.0.108（Mali-G31），`vulkaninfo` 能列出；没有 `VK_KHR_surface` / `VK_KHR_display`，只能离屏渲染再拷去呈现。设备上有 `libshaderc.so.1`，可以在运行时编译 GLSL。
- `tmp/vkbench/vkbench.c`（同样 800 个小 draw，离屏 640×480）：每 draw 一个 push constant，录制 6.8 µs；加每 draw 动态偏移的 UBO（整份 3 KB `c[]`），9.6 µs；再加每 4 个 draw 换描述符集、每 8 个换管线，9.8 µs。一帧录制加提交加 GPU 共 10-14 ms。GLES 同样负载 14-17 µs/draw，加 glFinish 15-18 ms。
- 游戏里 GL 每 draw 约 45 µs（驱动 30 + 我们 15），所以 Vulkan 估计能省下 750 个 draw × 约 15-20 µs ≈ 10-15 ms/帧（没验证）。
- 代价：现有的 `nv2a_vk` 需要 Vulkan 1.3（dynamic rendering、push descriptor、extended dynamic state、动态顶点输入），这个驱动全都没有。要改成 render pass、描述符集，把这些状态都做进管线排列组合，再加离屏到 KMS 的呈现，工作量是几天量级。

## 16. Vulkan 后端在 R36S 上的可行性
### 16.1 驱动能力（`vulkaninfo`，libmali Mali-G31，Vulkan 1.0.108）
- 设备扩展只有 15 个，都是 1.1 以前的 KHR 扩展（maintenance1/2、descriptor_update_template、dedicated_allocation、external_memory/fence/semaphore（无 dma-buf 导入导出）等）。没有 `VK_KHR_surface` / `swapchain` / `display`，没有 push descriptor、dynamic rendering、extended dynamic state、vertex input dynamic state。
- 特性：没有 `depthClamp`、`logicOp`、`dualSrcBlend`、`textureCompressionBC`；有各向异性、ETC2/ASTC、independentBlend。
- 限制：push constant 256 B，最多 4 个描述符集，动态 UBO 8 个，UBO 最大 64 KB，偏移对齐 16。
- 内存：一个堆，类型 0 是 device-local + host-visible + coherent，类型 1 是 device-local + host-visible + cached，类型 2 是 lazily allocated。

### 16.2 不用交换链也能显示（`tmp/vkbench/vkpresent.c`，已在设备上跑通）
- 流程：离屏渲染到 640×480 的 B8G8R8A8 图像，`vkCmdCopyImageToBuffer` 拷到 cached 缓冲区，等 fence，按行 memcpy 到 KMS dumb buffer（XRGB8888），`drmModePageFlip`，两个 dumb buffer 轮换。300 帧 300 次 flip 全部成功，面板 640×480@78 Hz。
- 每帧开销：invalidate 0.21 ms，memcpy 到 dumb buffer 1.04 ms（都在 CPU 上）。600 个小 draw 时，提交加 GPU 加拷贝共 4.5 ms。
- 只改固定功能状态（cull、front face、blend、顶点 stride）的管线变体，每个 `vkCreateGraphicsPipelines` 只要 0.2-0.4 ms，第一条 109 ms（编译着色器）。所以把 1.3 的动态状态改成管线静态状态，代价主要是表变大，不会卡顿。shaderc 第一次编译（含初始化）约 1 s。

### 16.3 和 GL 的每 draw 开销对比（§15.4 的测试）
| | 每 draw CPU |
|---|---|
| Vulkan，push constant | 6.3-6.8 µs |
| Vulkan，动态 UBO（整份 3 KB `c[]`）+ 描述符集 / 管线切换 | 9.6-9.8 µs |
| GLES，同样负载 | 14-17 µs |
| GLES，游戏里实测 | 约 45 µs（驱动 30 + 我们 15） |

### 16.4 现有 `nv2a_vk` 需要改的地方（按现有代码估计）
| 部分 | 要做的事 | 规模 |
|---|---|---|
| 实例 / 设备 | apiVersion 1.0，`pEnabledFeatures`，去掉 swapchain、push descriptor、vertex input dynamic 三个扩展和 12 个 1.3/EXT 入口 | 小，约 50 行 |
| 渲染 | dynamic rendering 改成 VkRenderPass + framebuffer 缓存（只需两种：颜色、颜色+深度，都是 LOAD/STORE、GENERAL 布局），表面重建时清掉对应 framebuffer | 中，约 200 行 |
| 管线状态 | cull/front/拓扑/深度/模板/顶点布局并进 PipeKey；strip、fan 在 CPU 上转 list；顶点布局去重成 id | 中到大，约 300 行 |
| 描述符 | push descriptor 改成动态 UBO 偏移（绑定 0、1、6）+ 每帧描述符池（纹理组合各一个集） | 中，约 150 行 |
| 着色器 | glslang C 接口改成 dlopen 的 shaderc，目标 Vulkan 1.0 / SPIR-V 1.0，换 SPIR-V 缓存的魔数 | 小，约 80 行 |
| 呈现 | 换掉交换链：拷到 cached 缓冲区，交给 KMS flip 线程（16.2 的做法） | 中，约 200 行 |
| 纹理 | 没有 BC：已有的 CPU 解码回退能用，但 DXT 是逐 texel 解码，慢；建议加一个按块解码的 BC1/2/3（约 100 行） | 小到中 |
| 其它 | 帧环（2×48 MB）缩小；分辨率默认 1.5 倍要强制成 1；SDL 视频不用 kmsdrm（输入和音频照用） | 小 |

- 全部约 1000-1200 行改动。另有两个风险：一是 1.0.108 这个老驱动在真实负载下是否稳定（只跑过合成测试）；二是 `nv2a_vk` 的录制全在执行器线程上，GL 后端有独立的提交线程，所以 Vulkan 版要么也加一个提交线程，要么接受执行器线程变重（但录制本身已比 GL 便宜一半以上）。
- 建议的第一步：只做到无头（`RECOMP_VK_HEADLESS=1`）跑出第一帧并 dump 出图（前五项），确认驱动稳定、画面对，再接 KMS 呈现和性能工作。

### 16.5 移植进度：Vulkan 1.0 路径（`nv2a_vk.c`，`s_vk10`）
- 设备没有 1.3 / push descriptor / 动态顶点输入时自动走 1.0 路径；`RECOMP_VK_COMPAT=1` 在任何驱动上强制走（PC 上用 lavapipe 测）。
  - 渲染：两个 VkRenderPass（颜色、颜色+深度模板），全部 LOAD/STORE、GENERAL 布局；framebuffer 按（颜色 view、深度 view、尺寸）缓存，view 进垃圾时一起回收。
  - 状态：cull、winding、拓扑、深度、模板测试和操作、顶点布局（去重成 id）放进 PipeKey；动态的只剩 viewport、scissor、blend 常量、模板掩码和参考值。
  - 描述符：绑定 0、1、6 改成动态 UBO（帧环里固定范围，每 draw 只换偏移；帧环 buffer 多留 64 KB 给绑定 6 的固定范围）；纹理组合每帧一个描述符集，从每帧的描述符池里分配，帧开始时重置。
  - FXAA 在 1.0 路径下关闭（它用 dynamic rendering + push descriptor）。帧环 16 MB（`RECOMP_VK_RING_MB`）。
- 构建：`XBOXRECOMP_VK_DLOPEN=ON` 时运行时打开 `libvulkan.so.1` 和 `libshaderc.so.1`（不需要 glslang，也不链接 Vulkan 库），1.0 驱动编译成 SPIR-V 1.0；`VULKAN=1 r36s/build.sh` 出 `build-r36s-vk/nfsu2_recomp`（Vulkan 头文件从宿主机挂进容器）。PC：`cmake -DNFSU2_VULKAN=ON -DXBOXRECOMP_VK_DLOPEN=ON`，shaderc 用 `apt-get download libshaderc1` 解出来的库。
- 呈现：`SDL_VIDEODRIVER=kmsdrm`（R36S 启动脚本）时不初始化 SDL 视频，自己打开 `/dev/dri/card0`：每帧把要显示的表面 blit（加黑边）到面板大小的图像，拷进该帧的 cached 回读缓冲区；一个线程等该帧 fence 后 memcpy 到 dumb buffer 并翻页（16.2 的做法）。`RECOMP_VK_KMS=0` 改为无头。
- PC（lavapipe）验证：1.0 路径进得了比赛，菜单、读取画面、比赛画面正常；关掉 BC（`RECOMP_GL_DXT=0`，和 Mali 一样）也正常，DXT 走 CPU 解码，有几帧 ~90 ms 的纹理卡顿。原来的 1.3 路径同一个二进制照常工作（含 FXAA）。
- 还没做：设备上实测（设备掉线）；DXT 按块快速解码；执行器线程的录制开销。

## 17. 死机后自动恢复（设备配置，`r36s/recovery/`）
### 17.1 为什么"游戏死机 WiFi 也卡死"
- WiFi 是 USB 网卡（RTL8188ETV，`8188eu`），经 USB hub 接在 `dwc2` 控制器上；这个控制器空闲时也有约 8000 次/秒中断，全在 CPU0。机器整体冻结时 WiFi 是外面唯一能看到的东西，所以看起来像"WiFi 卡死"。
- 死机那次启动的控制台（ramoops）停在开机 287 s：`mali ff400000.gpu: job status 0x00000004 (TERMINATED) / GPU fault 0x04 from job slot 0`，之后再无任何输出，也没有 soft lockup / hung task 报告。正常 `kill -9` 游戏时也会出现同样两行而系统无事，所以这两行只说明游戏进程在 GPU 任务未完成时消失了；之后整机停止，最可能是 Mali 驱动清理该进程的 GPU 任务或复位 GPU 时卡死（未证实）。
- 原来的系统：`kernel.panic=0`（panic 后永远挂着）、硬件看门狗在 DTB 里是 `disabled`。所以任何内核级卡死都只能按电源键。

### 17.2 现在的三层恢复（都已装到设备上并实测）
| 层 | 处理什么 | 做法 | 实测 |
|---|---|---|---|
| 硬件看门狗 | 整机冻结（内核/GPU 卡死、关中断） | DTB `watchdog@ff1e0000` 改为 `okay`（`dw_wdt` 驱动内核自带），systemd `RuntimeWatchdogSec=20s` 喂狗（硬件超时 21 s） | `kernel.panic=0` 下 `sysrq c` 让内核死掉，约 60 s 后自己重启回来 |
| 内核 panic | oops、soft lockup、D 状态挂起 120 s | `/etc/sysctl.d/90-r36s-recover.conf`：`panic=10`、`panic_on_oops=1`、`softlockup_panic=1`、`hung_task_panic=1` | panic 日志进 pstore（`dmesg-ramoops-*`） |
| r36s-guard 服务 | 系统活着但游戏卡住 / WiFi 卡住 | `r36s-guard.service`（SCHED_FIFO 10）：游戏进程 180 s 没写 `log.txt` 就按 PID 杀掉并重启 `oga_events`；有载波但网关 120 s 不通就 `wlan0` down/up，再不行就让 USB 网卡重新枚举（不会因为 WiFi 重启机器，离开信号范围时没有载波，不触发）；挂起唤醒后计时重来 | 假游戏进程 21 s 被杀；网关 ARP 被篡改后 30 s 触发 down/up，网络恢复 |
- 取证：guard 每 5 s 往内核日志写一行心跳（负载、内存、温度、GPU 占用、USB 中断计数、WiFi 收发计数、游戏进程状态），会进 ramoops。每次开机把上一次的 `console-ramoops` / `dmesg-ramoops` 拷到 `/roms/ports/nfs8/crashlogs/`（保留 30 份）。下次死机后看最后几行心跳就知道死前在干什么。
- 安装：`r36s/recovery/` 下 4 个文件分别放到 `/usr/local/bin/`、`/etc/systemd/system/`、`/etc/sysctl.d/`、`/etc/systemd/system.conf.d/`，`systemctl enable --now r36s-guard`；DTB 用 `fdtput -t s /boot/rk3326-r36s-linux.dtb /watchdog@ff1e0000 status okay`，原文件在 `/boot/rk3326-r36s-linux.dtb.pre-wdt`。
- 回退：把 `.pre-wdt` 拷回 `/boot/rk3326-r36s-linux.dtb`（设备起不来时在电脑上改 SD 卡第一个 FAT 分区），删掉上面几个文件。
- 注意：硬件看门狗复位等同断电，`/roms`（exFAT）上正在写的文件可能损坏（开机已见过 "Volume was not properly unmounted"）；系统升级若换掉 DTB，看门狗会回到关闭状态。

### 16.6 设备上的第一组数据（KMS 显示，起跑线，冷却后单次启动）
| | 帧时间 ms（各窗口） | 后端在执行器线程上的时间 ms | draw / 调用 |
|---|---|---|---|
| GL（3 次） | 47.6-52.0 | GL 线程 46-52 | ~720 / ~5000 GL 调用 |
| Vulkan 1.0（2 次） | 51-60、55-69 | 36-45 | ~720-820 / ~600-730 次 vkCmdDrawIndexed |
- Vulkan 版画面正常（截图亮度和 GL 一致），渲染倍率锁 1.0（游戏的 1.5 倍选项在 1.0 路径下忽略，`RECOMP_VK_GAME_SCALE=1` 恢复），`RECOMP_GL_SKIP` 也在 Vulkan 后端实现了（同样丢掉路面反射和车身环境图）。
- 结论：现在比 GL 慢 5-10 ms。原因不是驱动：Vulkan 的录制、顶点/常量拷贝、描述符查找全在执行器线程上，每个 draw 约 50 µs（合成测试里驱动本身只要 7-10 µs），而 GL 后端把这些交给了单独的 GL 线程，执行器和 GL 线程并行。下一步：在设备上 perf 执行器线程，找出 nv2a_vk 自己每 draw 的开销；再考虑像 GL 一样把录制挪到单独线程。
- 一次运行在开机约 40 s 后游戏停住（不是整机死机）：最后一帧之后主线程在 D3D 的 BlockOnFence 里等，GPU 信号量 0x3F17 落后 D3D 写序号 0x3F19，推送缓冲已全部执行完（GET == PUT）。像是等一个永远不会来的围栏事件（陷阱/事件丢失），原因未查清；GL 版是否也会发生未知。r36s-guard 改为按"[fps] 行是否增加"判断游戏卡住（只看 log.txt 修改时间不够：卡住的游戏仍在写 [wake] 行），这次 186 s 后自动杀掉。
- 测试工具的修正：gptokeyb 退出游戏用 `pkill -f <二进制名>`，命令行里带 `NFSU2_BIN=nfsu2_recomp_vk` 的 autotest 驱动也被它杀了 —— 启动脚本改成 `NFSU2_VULKAN=1` 选 Vulkan 版；autotest 认 `nfsu2_recomp_vk` 进程名（procps 的模式不能超过 15 字符）；按 A 后若 ES 启动的不是 NFSU2 立刻报错。

### 16.7 游戏里 Vulkan 驱动开销确实更小（perf，比赛中 8 s，~51 ms/帧，~720 draw/帧）
| 每帧 CPU 时间 | GL（§10.1，GL 线程 ~100% 一个核） | Vulkan 1.0（执行器线程 80% 一个核） |
|---|---|---|
| Mali 驱动（libmali） | 25-29 ms（51-60%） | 12.3 ms（≈ 17 µs/draw） |
| libc（多为驱动的 memcpy）+ 内核 | ~8 ms | 4.7 + 2.6 ms |
| 我们的代码 | ~10 ms（gl_draw_raw 等，GL 线程上） | 20 ms（推送缓冲解释 + nv2a_vk，同一线程） |
- 驱动部分 Vulkan 约为 GL 的一半（合成测试是 1/2-1/3，游戏里有管线绑定、描述符集更新和每帧提交）。
- 但 Vulkan 的所有渲染工作和推送缓冲解释在同一个执行器线程上串行（~40 ms/帧），GL 版是执行器（~40% 核）和 GL 线程（~100%）两条线并行，所以整帧没有变快。
- 上限：游戏主线程本身每帧在 CPU 上 ~40 ms（Vulkan 版 6231 个样本 / 8 s；GL 版 75-81% 一个核），渲染再快，帧时间也在 ~40 ms（~25 fps）附近，目前是 ~50 ms（20 fps）。

## 18. 主线程计算链路（用 GC 符号给 Xbox 函数命名）

### 18.1 方法
- GC 侧：`Speed.elf.c` 缺不少函数体（`World::DoTimestep`、`DoTimestepCollisions`、`IntegrateEuler`、`World::GetTimestep` 都没有），所以直接读 `disc/files/Speed.elf` 的符号表（15029 个函数）+ 逐字解码 `bl`，得到完整的直接调用图（`tmp/chain/gcelf.py` → `gcelf.json`，CodeWarrior 名字粗略还原成 `Class::method`）。
- Xbox 侧：§16.7 那次 perf 的主线程调用栈（Vulkan 版，比赛中，6231 个样本，`tmp/chain/main_stacks.txt.gz`）+ gen 里的 `CALL()` 静态调用图。
- 对齐：从结构确定的一对（`MainLoop` = `sub_0018A8C0`）开始，把两边**按调用顺序**排列的直接被调函数并排比较（函数大小、被调数、重复次数）；虚函数调用在静态图里看不到，靠 perf 栈里的父子关系补。`tmp/chain/side.py X=GCADDR` 并排打印，`ntree.py` 打印带名字的动态树。
- 表里只列结构对得上的（被调序列逐项对上，或大小/被调数/唯一调用关系都一致）；带 ≈ 的是只有一两条证据。`prop.py` 的自动传播结果有误配（比如所有空函数 `sub_00011000` 被配成同一个名字），没有直接采用。

### 18.2 主线程一帧（% 为占主线程样本，含子函数；主线程 ~40 ms/帧 CPU）
| Xbox | GC 名字 | 含子 | 自身 |
|---|---|---|---|
| `sub_0018A8C0` | `MainLoop` | 100 | 0 |
| ├ `sub_001F9E40` | `World::DoTimestep`（Xbox 41 个被调、GC 46 个，顺序对得上） | 53.6 | 0.2 |
| │ ├ `sub_001E1130` | `World::DoTimestepMove`（遍历 CollisionBodyList，虚调 Move） | 29.9 | 0 |
| │ │ └ `sub_001B4730` | `CarCollisionBody::Move`（两条路径合计 36.2） | 29.9 | 0.1 |
| │ │ &nbsp;&nbsp;└ `sub_001F1260` | `Car::Move` | 29.6 | 0.1 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;├ `sub_001C1510` | `PhysicsMover::Move`（玩家/对手车，见 18.3） | 21.1 | 0.1 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ └ `sub_001B4CD0` | `PhysicsSystem::IntegrateEuler`（两条路径合计 31.3） | 20.1 | 1.1 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ &nbsp;&nbsp;├ `sub_001BAA90` | 组件遍历（各组件 ResolveForces / ComputeAuxiliaries 的虚调） | 17.4 | 0.3 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ &nbsp;&nbsp;│ ├ `sub_001BAB00` | `PhysicsMover::ResolveForces`（9 个被调逐项对上） | 9.1 | 0.3 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ &nbsp;&nbsp;│ │ └ `sub_001B6700` | `PhysicsMover::IterateForces`（`Wheel::GuessGroundForce` `001B2B10`、`Wheel::FinalizeGroundForce` `001A8250`、`DriveTrain::*Clutch*`） | 6.0 | 1.1 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ &nbsp;&nbsp;│ ├ `sub_001B2FF0` | ≈`Suspension::ComputeAuxiliaries`（3×bSin、3×矩阵乘；下面 `001A7E90` → `001D7890` `TopologyCoordinate::NewGetElevation`） | 3.2 | 0.7 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ &nbsp;&nbsp;│ └ `sub_001B6400` | `PhysicsMover::BottomOut`（→ `001AB710` `PhysicsSystem::ComputeAuxiliaries`） | 2.7 | 0.1 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;│ &nbsp;&nbsp;└ `sub_001AB6B0` | `PhysicsSystem::GetDerivative` | 1.8 | 0.4 |
| │ │ &nbsp;&nbsp;&nbsp;&nbsp;└ `sub_001B55A0` | `MellowMover::Move`（远处/交通车的简化物理，同样走 IntegrateEuler） | 6.9 | 0.1 |
| │ ├ `sub_001EF9A0` | `World::DoTimestepCollisions`（17 个被调逐项对上） | 9.7 | 0 |
| │ │ ├ `sub_001BCB00` | `Contact::Resolve` → 再调一次 `CarCollisionBody::Move`（碰撞后重算物理） | 7.3 | 0 |
| │ │ └ `sub_001BF400` | `CollisionBodyWitness::TestForContact` | 1.7 | 0 |
| │ ├ `sub_0003B2D0` | `RaceCarController::Think`（对手 AI） | 5.7 | 0 |
| │ │ └ `sub_00039CC0` → `0002B490` → … → `00037EF0` | `AICarController::Think` → `ExecuteCurrentAction` → ≈`PsychoDriver::Drive`（转向/油门 PID、查表） | 5.6 | 0 |
| │ ├ `sub_00038E90` | ≈`TrafficController::Think` | 1.1 | 0 |
| │ └ `sub_00049EF0` | `CAnimPlayer::UpdateTime` | 1.4 | 0 |
| ├ `sub_000AEA90` | `eDisplayFrame`（Xbox 版，结构和 GC 差别大） | 39.1 | 0 |
| │ ├ `sub_000ADF60` | 主视图的场景/车绘制（GC 里是 `eDisplayFrame` 内联的一段） | 30.4 | 0 |
| │ │ ├ `sub_000AD7D0` | 贴图 bucket flush（≈`eFlushTextureBucketList` 的 Xbox 实现，下面是 D3D 推送缓冲） | 10.3 | 1.3 |
| │ │ ├ `sub_002106F0` | `RenderCars`（Xbox 13 个被调、GC 14 个，顺序对得上） | 8.4 | 0.1 |
| │ │ │ ├ `sub_0020BC50` | `CarRenderInfo::Render`（Xbox 301 个被调，GC 307） | 6.3 | 0.9 |
| │ │ │ └ `sub_00204D60` | `CarRenderInfo::RenderFast` | 1.6 | 0.3 |
| │ │ ├ `sub_001D9E30` → `001D9DB0` → `001C6DC0` | `GrandSceneryCullInfo::DoCulling` → `CullView` → `ScenerySectionHeader::TreeCull` | 4.8 | 0.7 |
| │ │ │ └ `sub_001C69C0` | `ScenerySectionHeader::DrawAScenery`（§2，`001C6900` = `eView::GetPixelSize`） | 3.4 | 2.0 |
| │ │ ├ `sub_001C6EF0` | `GrandSceneryCullInfo::StuffScenery`（→ `000ACB30` `eViewPlatInterface::Render`） | 1.6 | 0.1 |
| │ │ └ `sub_00206330` | ≈`RenderCarReflections` | 1.1 | 0 |
| │ ├ `sub_000AE590` | HUD/前端界面（`0014xxxx` 一带 ≈ `cFEngRender::PackageFinished` / `RenderObject`） | 4.1 | 0 |
| │ └ `sub_000AC6F0` | 光晕/反射一类的附加 pass（未细分） | 3.8 | 0 |
| └ `sub_00091760` | `EAXSound::Update`（音频逻辑，混音在别的线程） | 4.3 | 0 |

运行时自身（`recomp_lookup` + `recomp_lookup_manual`）1.5%。最高的单个函数自身时间是 `DrawAScenery` 2.0%，其余都在 1.5% 以下：没有可以单独改写的热点。

### 18.3 物理是按固定步长子步的，帧越慢每帧子步越多
GC `PhysicsMover::Move`（Xbox `sub_001C1510`，同样的 `fdiv [0x3A55B0]` → `_ftol` → `+1`）：
```
n = (int)(dt / MaximumTimeStep) + 1;          // MaximumTimeStep = 0.0044 s
for (i = 0; i < n; i++) IntegrateEuler(dt / n);
```
- Xbox 的两个常量都在 `.data`（可写）：`MaximumTimeStep` = 0.0044 @ `0x3A55B0`，`MaxiMellowTimeStep`（`MellowMover::Move`）= 1/70 @ `0x3A5580`。GC 里还有 `SmokeableMaximumTimeStep` = 1/60。
- `dt` 由 `PrepareRealTimestep` 按 1/60 s 的 tick 量化，最多 `MaxTicksPerTimestep` = 3 个 tick（GC 的值；Xbox 的对应变量没找）。所以 R36S 上 ~50 ms/帧时 dt 被截在 50 ms：每辆 PhysicsMover 车**每帧 12 个子步**（60 fps 时是 4 个），MellowMover 车 4 个。再慢游戏就变成慢动作。
- 子步频率约 1/0.0044 ≈ 230-240 Hz，物理每秒的总开销与帧率无关；帧率低时它占每帧的比例就高。上表里车辆物理合计 36.2%（~14 ms/帧），其中玩家/对手车的 IntegrateEuler ~25%。
- 子步数没有在设备上直接计数，以上来自 GC 代码 + Xbox 的同一常量；"12 步"是推算。

### 18.4 能动的地方（按预计收益）
1. 物理子步：把 `0x3A55B0` 从 0.0044 提到 0.0066 → 12 步变 8 步，物理约 -1/3（主线程 ~-4-5 ms/帧）；提到 0.0088 → 6 步（~-7 ms）。代价是显式欧拉对硬悬挂的稳定性和手感会变，必须实车试（颠簸路面、落地、撞墙）。是一个启动时写一个 float 的 knob，改动最小。
2. 碰撞后重算：`Contact::Resolve` 再跑一遍 `CarCollisionBody::Move` 占 6.2%，随 1 一起下降。
3. AI（5.7%）、音频逻辑（4.3%）、HUD（4.1%）各自都不大。
4. 绘制侧的 guest 代码（39%）里 bucket flush 10.3% 和 `CarRenderInfo::Render` 6.3% 最大，但它们的大头是生成推送缓冲，减少它们需要减少 draw（§2 的 PX_PCT 已经在做）。
5. 单函数原生改写：最热的也只有 2% 自身时间，收益小。

注意主线程降下来不等于帧率升：GL 版的 GL 线程、Vulkan 版的执行器线程每帧也在 40-50 ms，两边都要降。

## 19. Vulkan 录制挪到单独线程（`RECOMP_VK_THREAD`，Vulkan 版启动脚本默认 1）

### 19.1 做法
- §16.7：Vulkan 版的推送缓冲解释和 draw 录制都在执行器线程上（~80% 一个核），GL 版是执行器 + GL 线程两条线。
- `nv2a_vk.c` 末尾照搬 nv2a_gl 的线程队列（§10、`RECOMP_GL_THREAD`）：执行器只解释并入队（寄存器影子按 64 字节块、程序/常量按 16 字节行发差异；顶点数据按 draw 读的字节区间、转换后的属性、索引都拷进 48 MB 环形队列）；录制线程 `vkrec` 重建 batch 后调原来的 `vk_draw_raw` / `vk_clear` / `vk_flip`。最多落后执行器两帧，纹理在录制时才从 guest 内存读（和 GL 线程一样）。
- Vulkan 专有的两处：
  - `regs_changed`（实例化判定）在线程模式下读队列记录带来的变化块（`s_t_dirty`），不再读执行器的 dirty 位（执行器那边被入队消费掉了）。
  - 实例化比较顶点指针时用 title 给的原地址（记录里另存一份 `TqKeys`），否则每个 draw 的数据都在队列里的新位置，实例化全部失效。PC 上线程开/关的实例合并数一致（~875 / ~915 每帧）。
- `RECOMP_GL_SKIP` 的跳过放在执行器入队前。
- 顺带：`hz_now()` 在 aarch64 上直接读 `cntvct_el0`（每个 draw 原来 3 次 clock_gettime）；入队时常量/程序行的差异原来算两遍（先数后写，占执行器 ~14%），改为一遍。
- `[vk] recording thread: idle N%, executor held M%` 每 10 s 一行。`RECOMP_VK_THREAD=0` 回到原来的单线程路径。

### 19.2 设备上（比赛中，冷却后单次启动，PX_PCT=10 的 3 个窗口；交替跑）
| | 帧时间中位数 ms |
|---|---|
| 线程开 | 51.0、49.3、50.2（另两次带 perf：~52） |
| 线程关 | 53.5、58.6、54.2 |
- 约快 4-6 ms（~18 → ~20 fps），和 GL 版（47.6-52）持平或略好。
- perf（8 s，线程开，样本数 / 7992 = 占一个核）：主线程 80%，`vkrec` 60%，执行器 32%（原来 ~80%），apu 40%。执行器 + 录制线程合计比原来多 ~2% 样本，是拷贝的代价。
- `[frame]` 行的 `gl=` 现在是录制线程每帧在后端里的时间（~42 ms，含驱动里的阻塞；CPU 只占 60%）。

### 19.3 现在卡在哪
- 渲染两条线都没满（执行器 32%，录制 60%），**主线程 80% 一个核、~41 ms/帧 CPU，是帧率上限**。下一步就是 §18.4 的物理子步（`MaximumTimeStep` @ `0x3A55B0`）。
- §16.6 那次的游戏停住（BlockOnFence 等不到信号量）在这几轮（8 次启动）里没有再出现，未证明已解决。

## 20. 物理子步加长（knob `PHYS_PCT` / `MELLOW_PCT`）：主线程降了，帧时间不动

### 20.1 做法
- `PHYS_PCT`：`MaximumTimeStep`（0x3A55B0，0.0044 s，只有 `PhysicsMover::Move` 读）按百分比放大；`MELLOW_PCT`：`MaxiMellowTimeStep`（0x3A5580，1/70 s）同理。默认 100（不写内存）；改回 100 时写回原值。
- 由 knobs 线程每秒调游戏侧的 `knobs_game_apply()`（`src/recomp_manual.c`，knobs.h 里的弱符号），可以在运行中的比赛里实时切换。
- 顺带修了 `r36s/build.sh`：不带 `VULKAN=1` 时 `$([ … ] && echo -vk)` 返回 1，`set -e` 让脚本一声不响地退出，GL 版设备构建从 Vulkan 那次提交起其实一直没在编（`build-r36s/nfsu2_recomp` 停在 10-20 11:52）。

### 20.2 设备上（比赛中一次启动，sweep 每个设置前后各一个基线窗口）
主线程（perf 只采主线程 + 每秒读 `/proc/<pid>/task/<pid>/stat`；Vulkan 版）：

| 窗口 | 主线程占一个核 | `PhysicsMover::Move` 子树 | `MellowMover::Move` |
|---|---|---|---|
| 基线 | 70-72% | 11-14% | 2-9% |
| PHYS_PCT=150 | 73% | 8.0% | 9.1% |
| PHYS_PCT=200 | 72% | 6.5% | 11.3% |
| PHYS_PCT=200 + MELLOW_PCT=150 | 68% | 6.8% | 1.7% |
| PHYS_PCT=300 | 62% | 5.9% | 0% |

帧时间（d_ms = 设置减去两边基线的均值）：

| | 150 | 200 | 200+M150 | 300 |
|---|---|---|---|---|
| GL | -0.5 | -1.4 | -2.5 | -0.3 |
| Vulkan（第 2 次 sweep） | -11.7（第一个基线是 72 ms 的离群值） | -0.7 | -2.6 | -0.3 |
| Vulkan（第 3 次 sweep） | -7.2（同上） | +1.2 | +1.2 | -0.8 |

- 子步确实变少了（物理子树按比例下降，PHYS_PCT=300 时主线程从 ~72% 降到 ~62%），但帧时间都在噪声里：**主线程不是瓶颈**。GL 版是 GL 线程（ms ≈ gl），Vulkan 版是录制线程（见下）。§18.4 的推断（主线程 ~80% 就是上限）不对：主线程剩下的时间是在等渲染那边。
- 手感没有测（设备上只有自动驾驶）。knob 保持默认 100。
- 第 1 次 Vulkan sweep 跑完最后一个窗口后整机挂死，硬件看门狗约 1 分钟后重启（§17 的恢复正常工作）。ramoops 里最后只有 r36s-guard 的心跳，没有内核报错；之后同样的 sweep 又跑了 4 次都正常。原因未查。

### 20.3 Vulkan 录制线程才是上限
`[frame]` 行加了 `cpu=`（录制线程这一帧的 CPU 时间，`CLOCK_THREAD_CPUTIME_ID`）、`flip=`、`submit=`。比赛中 300 帧的中位数：ms 49.3，gl 43.4，**cpu 41.8**，flip 2.1，submit 0.9，wait 0.0，~740 draw → 每个 draw ~57 µs 的 CPU。

只采录制线程的 perf（8 s，6225 个样本 = 78% 一个核）：

| | 占录制线程 |
|---|---|
| Mali 驱动 | 39%，其中 `vkCmdDrawIndexed`（包括实例化攒下的那次）33%：每个 draw call ~22 µs |
| 我们的代码 | 36%（`vk_draw_raw` 自身 13.6%，`bind_stage` 3.2%，`bytes_hash` 2.9%，`regs_changed` 2.8%，`uniform_upload` 2%，`tex_get` 1.1% …，很平） |
| libc（几乎都是往 GPU 环形缓冲 memcpy：顶点、192 行常量、uniform） | 16% |
| 内核（主要是 Mali ioctl） | 8% |

- `RECOMP_GL_DUMP`（autotest 每 100 帧存一张 BMP）占 ~3%，正常游戏没有。
- 所以下一步要降的是录制线程：
  1. 减少 draw call（Mali 每次 ~22 µs，无法绕开）；
  2. 我们这边每个 draw 的准备工作：索引 u32→u16 的逐个转换是标量循环、写的是 GPU 内存，FsBlock 的 16 个颜色做了 64 次除法；
  3. 把能挪的挪到执行器（它只用了 ~32% 一个核）：索引转换、属性转换；或者让队列本身就是 GPU 可见内存，省掉第二次拷贝（改动大）。

## 21. xtrace：Vulkan 版一帧的时间线（Perfetto）

### 21.1 埋点和工具
- `nv2a_vk.c` 现在写 xtrace（原来只有 GL 后端有）。轨道：
  - `Vulkan`（录制线程）：`draw` / `clear` / `flip` / `queue empty`；`flip` 里有 `vkQueueSubmit`、`frame fence`（等帧槽）、`dump BMP (RECOMP_GL_DUMP)`；`tex upload`、`prog compile`；计数器 `VK frames queued`。
  - 每 `RECOMP_TRACE_DRAW`（默认 16）个 draw 抽一个拆阶段（`d:regs`、`d:held draw`、`d:prim+target`、`d:prog_get`、`d:textures`、`d:pipeline`、`d:pass+dynamic`、`d:uniforms`、`d:vertices`、`d:indices`、`d:constants`、`d:descriptors`、`d:hold` / `d:vkCmdDrawIndexed`、`d:instance append`），每个阶段是从上一个标记到这里的时间。
  - `executor`：`pb exec`、`idle`，被队列挡住时 `wait VK (queue full)` / `wait VK (2 frames queued)`，`flip queued`。
  - `KMS`：`frame fence`（从这一帧提交到 GPU 画完）、`page flip`、`KMS copy`。
- 抓法：`r36s/autotest.py --trace --env NFSU2_VULKAN=1 --set <名字>:<KNOB=V>`，同一次比赛里录基线、各设置、结尾基线各 6 s。
- `tools/trace_vk_frames.py`：以录制线程的 `flip` 分帧，打印每帧各线程的忙/等中位数、谁是瓶颈的比例、抽样 draw 的阶段。`trace_summary.py` 把 `wait VK` 算作等待。
- 设备上的 trace 在 `tmp/perfetto/`（拖进 ui.perfetto.dev）：`r36s_vk_race_base`、`_PHYS_PCT300`、`_gpu520locked`、`_scale0.5`。
- `autotest_driver.py` 存 trace 后 `sync`：这几轮里设备两次在结束时挂死重启，exFAT 上没落盘的 trace 都成了 0 字节。

### 21.2 比赛中一帧（中位数 ms，~750 draw / ~620 draw call）
| | 基线 | PHYS_PCT=300 | GPU 锁 520 MHz | 渲染 0.5 倍 |
|---|---|---|---|---|
| 帧间隔 | 54.6 | 54.7 | 55.1 | 48.2 |
| 录制线程 `draw` | 44.9 | 46.8 | 47.8 | 40.3 |
| 录制线程 `queue empty`（等执行器） | 0.0 | 0.0 | 0.0 | 1.5 |
| 执行器 `pb exec` / `idle` | 39.8 / 18.4 | 45.9 / 0.0 | 26.7 / 26.8 | 20.8 / 25.6 |
| **GPU：提交到画完（KMS `frame fence`）** | **43.8** | 43.6 | 44.4 | **18.7** |
| 瓶颈是录制线程的帧 | 84% | 97% | 87% | 70% |

- 录制线程是关键路径，但 GPU 只差一点：一帧提交后 GPU 要 ~44 ms 才画完，下一次提交前 GPU 只空闲 ~10 ms（16% 的帧是排在上一帧后面的）。比赛中 GPU 本来就在 520 MHz（devfreq 最高档），锁频没有变化。
- 渲染 0.5 倍时 GPU 时间 44 → 19 ms：**GPU 的大半是填充 / 带宽**，不是顶点或 draw 数。
- 执行器和主线程都不是瓶颈：执行器经常空闲（在等游戏写推送缓冲），从来没被队列挡住（`wait VK` = 0）。
- 所以录制线程最多能省 ~10 ms（55 → ~45 ms，到 GPU 的上限），再往下要减 GPU 的填充。

### 21.3 一个 draw 在录制线程上的时间（抽样 ~4300 个 draw，µs，含每个标记 ~1 µs 的打点开销）
| 阶段 | µs | 占 | 内容 |
|---|---|---|---|
| `d:held draw` | 20 | 26% | 上一个攒着的 draw 的 `vkCmdDrawIndexed`：Mali 每个 draw call 的固定开销 |
| `d:descriptors` | 12-15 | 17-20% | Vulkan 1.0 的描述符集：按 4 张纹理查本帧的缓存，没命中就 `vkAllocateDescriptorSets` + 7 项 `vkUpdateDescriptorSets`；`vkCmdBindDescriptorSets` |
| `d:textures` | 8-10 | 11-12% | `bind_stage` ×4（纹理查找、哈希、采样器） |
| `d:vertices` | 6-7 | 8-9% | 顶点区间拷进环形缓冲 + 绑定 |
| `d:uniforms` / `d:pass+dynamic` / `d:constants` / `d:pipeline` / `d:regs` / `d:prog_get` / 其他 | 2-5 各 | ~35% 合计 | |

### 21.4 下一步（按预计收益）
1. 描述符集跨帧复用：现在每帧清空缓存和池（`desc_reset`），而一帧里用到的纹理组合几乎不变。按帧槽保留缓存（纹理/采样器销毁时作废），大部分 draw 只剩 `vkCmdBindDescriptorSets`。估计省 ~5-8 ms/帧。
2. `bind_stage`：上一 draw 同一 stage 的纹理状态没变就跳过（GL 后端的 `TEX_MEMO` 同理）。
3. 减 draw call（每个 ~20 µs 的 Mali 开销）：实例化合并已经在做，同 mesh 不同 draw 的批量还有空间。
4. GPU 侧（录制省下来以后才会显出来）：填充占大半，可以从后处理 pass、半透明叠加、渲染目标的 load/store（Mali 是分块渲染，每次切换渲染目标都要读写整张图）入手。

### 21.5 设备挂死
目前 Vulkan 版共 3 次整机挂死（08:05、11:27、11:43），都在 sweep / trace 跑完、游戏退出前后；看门狗都在约 1 分钟内重启了设备。ramoops 里只有 r36s-guard 的心跳，没有内核报错。GL 版这几轮没有发生。原因未查，怀疑退出时 Mali 还有未完成的工作或 KMS 状态。

### 21.6 draw call 确认是最大的一项，而且在游戏里贵了一倍（`tmp/drawcall.py`、`tmp/vkbench`）
按 1/16 抽样折算回每帧（基线 trace，793 draw/帧，录制线程 `draw` 47.7 ms/帧）：

| 阶段 | µs/draw | ms/帧 |
|---|---|---|
| `d:held draw`（`vkCmdDrawIndexed`） | 17.1 | **13.6**（28%） |
| `d:descriptors` | 13.5 | 10.7 |
| `d:textures` | 7.3 | 5.8 |
| `d:vertices` | 5.5 | 4.3 |
| 其余 10 项 | 1-4 各 | ~13 |

抽样 draw 的阶段合计 67.7 µs，整段 `draw` 平均 60.2 µs，差的 ~7 µs 是 14 个打点的开销。

同一台机器上的合成测试（`vkbench`，800 draw/帧，36 索引，每个 draw 换 UBO 动态偏移 + 重绑顶点）里一次 `vkCmdDrawIndexed` 只要 6-9 µs：

| 变化 | `vkCmdDrawIndexed` µs |
|---|---|
| 3 个属性 / 16 个属性（同游戏，13 个 stride 0） | 7.0 / 8.3 |
| UBO 范围 3 KB / 48 KB（同游戏的 c[192×16]） | 9.1 / 7.8（无差别） |
| CPU 1512 / 1200 MHz | 8.4 / 8.3 |
| 另一个核做 memcpy、或三个核空转 | 8.5 |
| **两个核做 memcpy** | 10.3 |
| **三个核做 memcpy** | **14.8-25.8**（整个 draw 24-30 µs） |

- 所以游戏里 draw call 贵一倍主要是**内存带宽 / 共享 L2 被其他核抢**（主线程、执行器、apu 同时在跑），不是我们传给驱动的状态（属性数、UBO 大小影响 ≤1 µs）。
- 渲染 0.5 倍（GPU 带宽少一大半）时，`d:held draw` 17.1 → 15.6 µs，整个 draw 60 → 56 µs：GPU 的带宽占用只是小头。
- 推论：减少 draw call 数量仍然是最直接的收益（每个 ~17 µs 驱动 + 我们的 ~40 µs 准备）；降低其他线程的内存流量（执行器往队列的拷贝、录制线程往环形缓冲的拷贝）也会让每个 draw 变便宜。

## 22. 16 bpp 渲染（`RECOMP_VK_RT16` / `RECOMP_VK_TEX16`，默认关）：GPU 只省 1-4 ms

### 22.1 做法
- `RECOMP_VK_RT16=1`：颜色渲染目标（surface、render pass、pipeline 的颜色格式）从 B8G8R8A8 改成 R5G6B5。没有目标 alpha（读出来是 1）；`RECOMP_GL_DUMP` 读回时转成 32 位存 BMP。设备不支持该格式的渲染/混合/采样/blit 时自动退回。
- `RECOMP_VK_TEX16=1`：解码后的纹理（设备没有 BC，DXT 也在这里解）按 alpha 存 16 位：全不透明 R5G6B5，alpha 只有 0/255 用 A1R5G5B5，其余 R4G4B4A4。立方体贴图和线性 A8R8G8B8（影片帧）不变。lavapipe 不支持 16 位格式的 blit，PC 上自动退回 32 位。
- `[vk] per frame` 统计（`RECOMP_DRAW_STATS=1`）多了一行：每帧 render pass 数、带深度的数量、读写的像素量。设备上还没收这个数。

### 22.2 设备上（每种配置单独启动一次，各 3 段 6 s trace，中位数 ms）
| 配置 | GPU：提交到画完 | 录制线程 draw | 帧间隔 |
|---|---|---|---|
| 32 位（原来） | 44.3-44.8 | 45-46 | 52-54 |
| RT16 | 42.7-43.2（-1.6） | 45-51 | 57-59 |
| TEX16 | 43.0-43.6（-1.0） | 39-44 | 52-54 |
| 两个都开 | 40.5-41.4（**-3.7**） | 42-45 | 49-53 |

- 每种配置第一段都有离群（65、76 ms），没算。RT16 那轮帧间隔高是录制线程慢（启动时 59 ℃），GPU 并没有变慢；没有交替重复验证。
- 画面：PC 上 RT16 和 32 位看不出差别，设备上两个都开的截图正常。渐变（天空）可能有轻微色带，没有逐像素比。
- 结论：颜色缓冲和纹理带宽都不是 GPU 时间的大头。§21.2 里半分辨率能省 25 ms，说明是和像素数成正比的东西，但不在颜色格式/纹理大小上。剩下的嫌疑（都未验证）：片元着色器的 ALU（通用的 register combiner 模拟）、每个 render pass 对颜色 + D24S8 深度的整张 LOAD/STORE（Mali 分块渲染每次切换渲染目标都要付）。
- 开关保留、默认关：录制线程降下来、GPU 成为瓶颈以后，两个都开能拿回 ~3-4 ms。trace 在 `tmp/xtrace16/`。
