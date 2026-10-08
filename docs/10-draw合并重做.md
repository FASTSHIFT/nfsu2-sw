# 10 - draw 合并重做：基线与方案

> 日期：2026-10-08。前置：[09 碎片 draw 合并尝试](09-碎片draw合并尝试.md)（旧方案，已回退）、[07 RenderDoc 帧捕获分析](07-RenderDoc帧捕获分析.md)。

## 1. 为什么回退旧方案

旧方案（提交 6dbb731）在顶点着色器里按 `gl_VertexID` 选常量块：每个顶点跑一个循环，再动态索引 `c[blk*192+i]`。PC 上 draw 减 76%、画面正确，R36S 上从 7-13 fps 掉到 0.2-2.5 fps。问题在方案本身：

- 改的是 shader，代价在 Mali 上完全不同于 llvmpipe，PC 验证不能推断真机；
- Mali 的 uniform 预算只够 5 块，合并率 1.45（PC 4.2），省不下多少；
- 每个 draw 都做 `mg_match`/`vertex_runs` 扫描，合并不成功时纯亏 CPU。

代码已 revert，`docs/09` 留作失败分析。默认行为不变（回退前后基线一致，见下）。

## 2. 基线（PC，llvmpipe，回退后）

工具：`RECOMP_PERF_LOG=2` 每帧一行 `[frame] n ms gl wait swap dc frag calls`，与 `[AUTOTEST] HUD live` 在同一日志流里，可以并排看比赛开始前后。`frag` = 索引数 < 50 的 draw。

```bash
BIN=$PWD/build/linux-hud/nfsu2_recomp HUD_STOP=12 RECOMP_SKIP_INTRO=1 \
  RECOMP_PERF_LOG=2 bash tools/pc/run.sh base 150
```

HUD 首次更新后各阶段（`build/base0`，每帧中位数）：

| 阶段 | 帧 | dc | frag | calls | ms |
|---|---|---|---|---|---|
| HUD 已创建，仍是加载/菜单画面 | 0-59 | 161 | 161 | 1542 | 38.7 |
| 赛前场景 | 80-209 | 1067 | 557 | 11729 | 40.2 |
| **比赛** | 225+ | **2531** | **1475** | **25621** | 44.1 |

要点：

- HUD 在 3D 场景出现前约 70 帧就开始更新（Player 构造时创建 HUD），所以 `HUD live` 是"进比赛流程"，稳态基线要从它之后约 220 帧起取。
- 比赛里 **58% 的 draw 是碎片**，平均每个 draw 约 10 次 GL 调用。
- `wait=0.0`：llvmpipe 下 GL 线程一直满载，PC 上看不出游戏线程是不是瓶颈。

## 3. 下一步：先定瓶颈，再选方案

docs/09 的方向 D 没有排除：真机瓶颈可能是 guest 主线程，draw 合并碰不到它。新 HUD 的 `GL` / `WAIT` 就是答案：

- `WAIT` 接近 0、`GL` 高 → GL 线程是瓶颈，合并 draw 有意义；
- `WAIT` 高 → GL 线程在等游戏线程，合并 draw 没用。

所以 **第一步是在 R36S 上拿同样的基线**（`RECOMP_HUD=1` 看屏幕，`RECOMP_PERF_LOG=2` 看日志）。这需要 gen 带 `sub_00100360_gen`（regen，或 `build/gen-hud` 副本）。

## 4. 方案候选（真机 GL 线程是瓶颈时）

按"不改 shader 或只做最小改动"排序：

| # | 方案 | 说明 |
|---|---|---|
| A | 只合并 `c[]` 完全相同的相邻 draw | 不改 shader；docs/07 里相邻对只有约 11% 相同，收益小 |
| B | 合并时在 CPU 上预变换顶点 | 把每个子 draw 的顶点乘它的模型矩阵烘成统一空间，合并后常量一致，shader 不动。要看 A35 上的顶点数 × 矩阵乘开销 |
| C | 减 GL 调用而不是减 draw | 比赛里 25k 调用 / 2.5k draw ≈ 10 次/draw：状态、纹理、uniform 上传。去掉冗余调用不动 shader，风险最低 |

先做 C 的统计（哪些调用占大头），再决定要不要 B。任何改 shader 的方案必须先在真机小规模验证。
