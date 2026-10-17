# 11 - OpenUG2 移植到 R36S：可行性评估
> 日期：2026-10-17。前置：[10 draw 合并重做](10-draw合并重做.md)。
> 结论先行：OpenUG2 能在 R36S 上跑起来并进入场景，但实测稳态约 **2.9 fps**（中位帧 344 ms），低于本仓库的 Xbox 重编译版（比赛约 8 fps，114~125 ms）。一半以上的帧时间是 CPU 物理而不是渲染；修掉物理后渲染本身约 140 ms，大致只能追平 Xbox 版。

## 1. 为什么看它
[whoismept/OpenUG2](https://github.com/whoismept/OpenUG2)（MIT，`development` 分支，commit `921e015`）是从零写的 NFSU2 引擎，读 PC 零售版数据，SDL2 + OpenGL/GLES2，洁净室实现（不含 EA 代码/资源）。问题是：它的场景是"加载时把摆放矩阵烘焙进顶点再按格/贴图合批"，能不能借此绕开 Xbox 版 ~2050 draw/帧的瓶颈。

### 1.1 资源侧不能降 draw call
- `STREAM*.BUN` 里是原型网格 + 64 字节摆放记录（AABB、矩阵、模型 key）；没有预合并几何。原版引擎每个摆放、每个网格单独画，这和 RenderDoc 里 469/470/471 三个网格各重复 84 次一致。
- reVC 同理：上游从来没有降过 draw call（217~440 draw/帧）。
- OpenUG2 的合批靠接管整条管线：加载时按 400 m 空间格 + 贴图 + 混合模式排序，在 u16 顶点上限处拆批（1400 m 半径约 2.38 万网格→4528 批）。我们的场景摆放/裁剪/bucket flush 都在 guest 代码里，不能照搬。
- 重编译框架里可行的是 GL 层烘焙：用 `sub_000AD7D0`（bucket flush）里 `node+0x2C` 的世界矩阵识别静态物体。**未验证**。

## 2. 资源与构建
### 2.1 PC 数据
`PortMaster/nfsu2-pc` 是 3DM 安装版：`CD1/compressed.zip`（663 MB）+ `CD2/compressed.zip`（141 MB）+ mdf 镜像 + 破解 `SPEED2.EXE`。zip 内路径用反斜杠，Linux `unzip` 会得到扁平文件名，用 Python `zipfile` 把 `\` 换成 `/` 后合并解压两个 zip 到 `PortMaster/nfsu2-pc-data/`（1.2 GB，1222 个文件）：`CARS/ TRACKS/ GLOBAL/ FRONTEND/ LANGUAGES/ SOUND/ NIS/ ...`。`MOVIES` 不需要。PC 上用该数据渲染 L4RA 场景正常（Hummer、立交桥、路牌、贴图都对）。设备 `/roms/ports/openug2/nfsu2-pc-data` 与本地 1222 个文件的路径和大小逐一比对一致（exFAT，大小写不敏感）。

### 2.2 PC 构建
`make` 即可（SDL2、zlib、libpng、libcrypto、xxd）。只有警告。上游 Makefile 把全部 `.c` 放在**一条 cc 命令**里编译，单线程，所以编译时 CPU 占用低是正常的。

### 2.3 交叉编译（aarch64，GLES2）
文件在 `tools/ug2/`：
- `Dockerfile`：基于现有 `nfsu2-r36s-cross:focal`（x86_64 原生交叉 gcc 9 / glibc 2.31，**不是 qemu**），叠加 arm64 的 zlib、libpng、libssl、GLESv2 以及 `libgl-dev`（提供 `KHR/khrplatform.h`）。
- `fetch-debs.sh`：在宿主机下载 deb 到 `/tmp/ug2debs`，再复制到构建目录的 `debs/`。容器里的 `apt-get update` 下载索引极慢甚至卡住（宿主机直连同一镜像 0.04 s），所以改为宿主机下载。
- `build.sh`：逐文件并行编译再链接（约 50 s），参数与上游 `make gles` 一致，另加 `-mcpu=cortex-a35`。
  环境变量 `UG2_SRC` 指向 OpenUG2 源码目录；构建目录里要有 `src/generated_*.h`（容器里没有 xxd，先在 PC 上 `make` 一次）。

坑：
1. 镜像是 usr-merge（`/lib -> usr/lib`），deb 里有 `./lib/`，`dpkg-deb -x ... /` 会把 `/lib` 软链接换成真目录，x86 的 libc 丢失（表现为 `rm: not found`）。解法：先解到暂存目录，再 `tar --keep-directory-symlink` 合并。
2. 设备是 Debian 13 / glibc 2.41，只有 `libcrypto.so.3`，没有 focal 的 `libcrypto.so.1.1`。只用到 SHA-256，所以静态链接 `libcrypto.a`（`-Wl,-Bstatic -lcrypto -Wl,-Bdynamic -ldl -lpthread`）。`libpng16.so.16`、`libz.so.1`、`libGLESv2.so.2`、`libSDL2` 设备上都有。

### 2.4 设备端
- `OpenUG2.sh` 放 `/roms/ports/`，仿 nfs8 的 PortMaster 启动脚本（KMSDRM + 厂商 EGL/GLES、gptokeyb 热键退出、同样的温控/IO 设置）。默认参数 `--track STREAML4RA --resolution 640x480 --view-dist ${VIEW_DIST:-400}`；`openug2/args.txt` 追加参数，`openug2/env.txt` 追加环境变量；日志过滤 ALSA underrun 刷屏。
- `device_test.sh`：SSH 里测试用，会 `systemctl stop emulationstation` 抢 DRM，退出必恢复。**不要用 `--shot`**：截图模式建隐藏窗口，设备屏幕是黑的，而且我的第一次测试因此看起来像"黑屏"。
- 注意 stdout 接管道后是块缓冲，设备日志会滞后很久，不能据此判断"没加载完"。

## 3. 对上游的改动（`tools/ug2/openug2-r36s.patch`，MIT）
1. `--view-dist M`（150~933 m，默认 933 即原行为）：把雾密度设成 `sqrt(-ln 1%)/M`，使可见裁剪距离（本来就由雾推导）和远裁剪面正好落在 M；常驻邻域半径改为 `M+467`（`world_resident_policy_valid` 允许的最小值：draw + cell 400 + safety 67）。重新居中的触发条件仍是离中心 400 m，和半径无关。
2. `OPENUG2_TEXSTAT=1`：`rhi_texture_upload` 里累计上传字节，每 32 MB 打一行到 stderr。
3. `--perf-csv` 每 60 帧 `fflush`：启动脚本退出时进程会被杀，原来的 CSV 会丢缓冲。

## 4. 测量
### 4.1 绘制距离（PC，640x480，L4RA 出生点，llvmpipe）
| 指标 | 933 m（默认） | `--view-dist 400` |
|---|---|---|
| 常驻网格 / 批 | 76763 / 8692 | 50459 / 6180 |
| 单帧绘制网格 | 9378 | 4399 |
| 单帧 draw call | 2456 | 1201 |
| 峰值 RSS（PC） | 634 MB | 566 MB |

draw call 减半，但**内存只降 11%**——绘制距离不是内存的主要成因。400 m 的画面没有明显穿帮（该点在立交桥下，视野本来就短，开阔路段未看）。

### 4.2 内存（用 `LD_PRELOAD` malloc 钩子 + 日志时间线）
- 加载 L4RA 时 `STREAML4RA.BUN`（113 MB）被整块 `fread` 读入，而且**同时读了两份**：`world_instance.c` 的 `winst_read_named` 与 `world.c:369` 的 `g->data`，峰值时 ≥2 MB 的大块合计 242 MB；其中一份要到贴图绑定后才释放。
- 贴图上传总量约 100 MB（RGB/RGBA 解码后上传，Mali 无 S3TC），不是大头。
- 设备实测峰值 RSS：绘制距离 933 m 时 816 MB，400 m 时 759 MB；设备总内存约 895 MB，可用内存最低掉到 21~48 MB，进程进入 D 状态（换页），SSH 随之超时。设备曾因此无响应并重启。
- 候选修复（**未做**）：对大文件用 `mmap(MAP_PRIVATE, PROT_READ|PROT_WRITE)` 代替 `fread`（页缓存可回收）；去掉重复读取；`malloc_trim`。

### 4.3 设备稳态帧时间（`tools/ug2/perf-r36s-view400.csv`，481 帧，去掉前 60 帧加载）
| 阶段 | 中位 ms |
|---|---|
| player_sim（玩家车物理） | 77.3 |
| ai_step（6 辆 AI 车） | 118.2 |
| cpu_swap（等 GPU） | 81.9 |
| world_draw（提交） | 38.1 |
| camera / post_draw | 10.1 / 7.5 |
| **合计** | **344.4（2.9 fps）** |

其他列：设备上 draws 中位 731（PC 同场景约 1200）；`contact_passes` 恒为 8；`wall_candidates` 恒为 578832。GPU 计时器在 GLES 上不可用（`gpu timer unavailable`），无法拆 GPU 阶段。

### 4.4 物理是主因
- 物理固定 60 Hz，`phys_clock_steps` 每帧最多补 8 个 tick（`physics.h`）。帧时间超过 133 ms 就永远卡在 8 个 tick，游戏只有约 0.4 倍速。
- 每 tick 约 24 ms（玩家 ~10 + AI ~15）。若每 tick 降到 1~3 ms，帧时间约回到"渲染本身"的 140~170 ms（6~7 fps）。
- 疑因：车对墙碰撞线性扫描约 1.9 万个障碍物（`world->nobst`，`ai.c` 里 `wall_candidates += world->nobst`）。**我只读了计数点，没读 `collide_body_walls` 内部，这是推断，未验证。**
- 渲染本身：swap 82 + 提交 38 + 其他 ≈ 140 ms，已与 Xbox 版同量级。

## 5. 与 Xbox 重编译版对比
| | Xbox 重编译版 | OpenUG2 |
|---|---|---|
| 设备稳态 | 比赛 ~8 fps（114~125 ms） | 2.9 fps（344 ms） |
| draw call / 帧 | ~2050~2300 | ~730（400 m） |
| 主要瓶颈 | guest 代码 + GL 提交/填充率 | CPU 物理（195 ms）+ GPU 等待 82 ms |
| 内存 | 正常 | 峰值 760~816 MB / 895 MB，会换页 |
| 游戏完整度 | 原版逻辑 | 半成品（AI、赛事、HUD 不完整） |
| 优势 | 原版行为 | 渲染器在我们手里，可裁灯光/雾/着色器/分辨率 |

## 6. 结论与后续
- **不建议把它作为主线**：就算物理优化完，渲染一半大概率只追平 Xbox 版，且内容缺失多。
- 若继续，按收益排序：
  1. 障碍物空间索引（验证物理是否真是主因，预期 195 ms → <30 ms）。
  2. 渲染实验（关灯光/雾、降分辨率、简化着色器）拆 GPU 的 82 ms。
  3. 大文件 mmap + 去掉重复读取，压内存峰值。
- 设备上 `openug2/args.txt`（`--perf-csv`）与 `env.txt`（`OPENUG2_TEXSTAT=1`）是测试配置，正常使用前要删掉。
- 未验证项：开阔路段的 400 m 画面质量；去重/mmap 后的真实内存；GPU 阶段拆分；`collide_body_walls` 的真实复杂度。

## 7. 复现
```bash
# 数据：见 2.1（合并解压到 nfsu2-pc-data/）
# 构建 PC 版并打补丁
git clone -b development https://github.com/whoismept/OpenUG2.git && cd OpenUG2
git checkout 921e015 && git apply ../nfsu2-sw/tools/ug2/openug2-r36s.patch && make
# 交叉编译
cd ../nfsu2-sw/tools/ug2 && bash fetch-debs.sh && mkdir -p debs && cp /tmp/ug2debs/*.deb debs/
sg docker -c 'docker build -t ug2-r36s-cross:focal -f Dockerfile .'
UG2_SRC=../../../OpenUG2 sg docker -c 'bash build.sh'
# 部署：build/nfsu2 -> /roms/ports/openug2/nfsu2，OpenUG2.sh -> /roms/ports/
```
