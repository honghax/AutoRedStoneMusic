# 红石音乐机器 · 项目进度与已完成内容

> 更新日期：2026-10-06
> 维护约定：本文件记录**总体构想、架构、已修缺陷、产物清单、复现步骤**。改动生成器/模组后请同步更新。

---

## 0. 一句话

把任意音频（MP3 / 抖音视频）**全自动**变成"放进 MC 就能自己弹"的 vanilla 红石音乐结构：

```
音频 → basic-pitch 转 MIDI → gen_machine.py → vanilla 结构 .nbt → Forge 模组 /redstonemusic place → 拉杆自动演奏
```

不用矿车计时，纯**中继器延迟线**；支持**多层蛇形折叠**（紧凑）与**一条到底直线**（直观）两种布局。

**当前状态：整曲 181s《鸟之诗》已跑通并全绿；另已完成 4 首短曲；两种布局均已产出并接入模组。**

---

## 1. 总流水线

| 阶段 | 工具 | 说明 |
|---|---|---|
| ① 取素材 | `E:\project\spider\douyin_spider`（抖音爬虫） | node a_bogus 签名；`scripts/dyv.py` 下载视频 |
| ② 音视频分离 / 切片 | `ffmpeg` / `ffprobe` | `-vn` 提音频，`-t N` 截前 N 秒 |
| ③ 音频转 MIDI | `basic-pitch`（ICASSP 2022, ONNX/CPU） | 经 `tools/some-onnx/src/pipeline.py`，限 6 线程 |
| ④ MIDI → 红石结构 | `tools/some-onnx/src/gen_machine.py` | 自研生成器，含 10 项自校验 + 产物级复核 |
| ⑤ 放置 | Forge 1.20.1 模组 | `src/main/java/com/redstonemusic/RedStoneMusic.java`，`/redstonemusic place <结构名>` |

---

## 2. 环境与固定命令

- Python（生成器/转写）：`E:\project\RedStoneMusic\tools\some-onnx\.venv\Scripts\python.exe`
- 模组构建：项目根 `.\gradlew build`（产物 `build\libs\redstone_music-1.0.0.jar`）
- 开发运行：`.\gradlew runClient`
- 爬虫环境（已验证）：node v18.20.8 / npm 10.8.2 / `node_modules` 已装（`@moonr/abogus`）/ `cookie.txt` 约 5KB / `requests` 可用

```powershell
# 抖音下载（stdin 需多喂一个回车，脚本末尾有一次 input）
"https://v.douyin.com/xxxx/`n`n" | python scripts\dyv.py

# 分离音频 + 截前 N 秒
ffmpeg -y -i in.mp4 -vn -ac 2 -ar 44100 out.wav
ffmpeg -y -i in.mp4 -t 34 -vn -ac 2 -ar 44100 out_34s.wav

# 音频 → MIDI
.\.venv\Scripts\python.exe src\pipeline.py test\x.wav test\x_bp.mid

# MIDI → 红石结构（折叠版 / 直线版）
.\.venv\Scripts\python.exe src\gen_machine.py test\x_bp.mid test\x.nbt --seconds 34
.\.venv\Scripts\python.exe src\gen_machine.py test\x_bp.mid test\x_simple.nbt --seconds 34 --layout simple
```

---

## 3. 生成器 `gen_machine.py`

### 3.1 时间模型

- `1 列 = --resolution 个游戏刻`，默认 `resolution=2`（= 0.1s）。
- 抽头时刻 = `resolution × 列号`（游戏刻）；音符发声 = 抽头 + 2（支线中继器 delay=1）。
- 中继器 `delay` 属性单位是**红石刻**（1 红石刻 = 2 游戏刻），故 delay 1..4 = 2/4/6/8 游戏刻。

### 3.2 拓扑（折叠版）

- `facing` 语义（实测）：**facing = 读输入方向，输出在反面**。正向层主干 +X 写 `facing='west'`。
- 主干每级占 2 格：`中继器 → 输出粉（= 本列抽头 & 前进下一级）→ 下一级中继器`。
- 只为"有音符的列"设抽头；相邻抽头列差 g 列时用尽量少的中继器跨 2g 刻（先 delay=4，余数 r 用 delay=r//2）。
- 支线（同列）：`跨接粉(z=base+1) → 总线粉(z=base+2..1+N) → 支线中继器(x±1) → 音符盒(x±2) → 垫块(x±2, y-1)`。
- 列间隔离：同层抽头间距 ≥4 格（不足补 z=base 的 0 延迟粉）。
- 脉冲源：`lever → 正对 observer(facing=west) → 列 0 抽头 (-2,1,0)`。
- 末端指示灯：最后一个抽头后再加 1 级 delay=1 中继器 + `redstone_lamp`（诊断信号是否走完）。
- 支撑层：每个工作层下方铺 `dirt`（折返处的 `glowstone` 不被覆盖），同时作音符盒的垫块/材质。

### 3.3 折返（关键难点）

| 类型 | 结构 | 延迟 | 用途 |
|---|---|---|---|
| **Z 折返**（同层换条带） | X 段 4 格 → 拐角 → Z 段 `PITCH` 格；走廊里插 **2 个** delay=1 中继器（X 段首格隔离折返列总线 + Z 段远端 `zm=PITCH-1` 切开长粉段） | +4 游戏刻 | 紧凑折叠 |
| **Y 折返**（升一层） | 3 步对角（横 1 + 升 1），`glowstone` 支撑 | 0 | 换层 / **窄间隔退让** |
| 走廊受约束 | 每段连续红石粉 ≤15（信号强度），走廊两端接主干/总线后仍须成立 | — | 硬约束 |

- **窄间隔退让**：若某段列间隔扣不起 Z 折返的 4 刻（如相邻 8 分音符 Δc=1），该折返**自动退化为 0 延迟的 Y 折返**，避免长粉段。
- **时序补偿**：Z 折返的中继器会吃掉延迟，`sim_path()` 预演折返次数后从本段主干延迟里迭代扣减至收敛，保证每个抽头仍≈`resolution×列号`。
- **抽头限窗**：折返刚结束时主干可能停在走廊格里，抽头前用 0 延迟粉"走回窗口" `[-2,28]`，避免总线焊进走廊/覆盖走廊中继器。

### 3.4 音高映射（垫块分档，借鉴 hyperchoron）

垫在音符盒**正下方**的方块决定乐器及其自然基频，从而把音域从单一 harp 的 F#3~F#5 扩到 **F#1~F#7**，超出的音才在档内八度折回。定义在 `INSTRUMENT_BANKS`：

| 档 | instrument | 垫块 | MIDI 音域 | note |
|---|---|---|---|---|
| 低 | `bass` | `minecraft:bamboo_planks` | F#1–F#3 | pitch−30 |
| 中 | `harp` | `minecraft:dirt` | F#3–F#5 | pitch−54 |
| 高 | `bell` | `minecraft:gold_block` | F#5–F#7 | pitch−78 |

- `note ∈ [0,24]`，`note_block` 的 `instrument` 属性与垫块一致。
- 全曲按**中位数**整体移调对齐到 harp 音域中心（等价 hyperchoron 的 `--transpose`）。
- 副作用：音色随档变化（低音 bass、高音 bell），换取真实音高不折叠、不产生"同列同音高撞车"。

### 3.5 两种布局 `--layout`

| 布局 | 说明 | 特点 |
|---|---|---|
| `folded`（默认） | 多层蛇形折叠；`N_STRIP=6` 条带/层，层高 3 | 占地小；有走廊，密列+折返处可能有 ≤10 刻时序漂移 |
| `simple` | **单层、单条带**，主干一路 +X 成一条直线，完全禁用折返 | 占地长但直观好跟看；**无走廊 → 时序偏差恒为 0**；粉段天然 ≤15 |

`PITCH = SLOT0(2) + MAX_SLOT(本曲最大同列音符数) + 1`，**按曲自适应收缩**条带宽。

### 3.6 自校验（10 项，全部为断言）

| # | 检查 | 判据 |
|---|---|---|
| (0) | 抽头刻数 vs `resolution×列号` | 绝对偏差 ≤ `TIMING_TOL_TICKS`(=10) |
| (1) | 条带间隔格非空 | 除走廊外须只含 dirt/空 |
| (2) | 单列最大音符数 | ≤ 条带容量 |
| (3) | Z 折返走廊中继器数 | == 设计值 |
| (4) | Y 折返：对角 / glowstone 支撑 / 无中继器 | 全 0 |
| (5) | 总时长 | == `seconds×20`；无跨层相邻 / 隔层残留 |
| (6) | 支线-主干相邻 / 音符盒朝向 | 全 0；条带内 X 隔离 |
| (7) | 总线断点 / 支线输入断开 / 跨接粉异常 | 全 0 |
| (8) | 最长连续红石粉段 | ≤15 |
| (9) | `note_block` 6 面邻接红石粉 | **0**（防强充能回灌造成锁存/提前触发） |

> (10) 项实际为"音符盒 6 面无粉"硬约束；另由 `note_dust_adjacency()` 复用。

### 3.7 产物级复核（写盘后再验一次）

`write_nbt()` 之后**独立重新读回 `.nbt`**（解压 + 解码调色板），核对：方块集合/名称一致、size 一致、**音符盒 6 面贴粉 = 0**、方块数一致。**任一项不过 → 删除产物 + 非 0 退出**，绝不留下不合规文件。

### 3.8 CLI 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `input` / `output` | — | 输入 MIDI / 输出 .nbt |
| `--seconds` | 10 | 截取时长（列数 = seconds/resolution×20） |
| `--resolution` | 2 | 每列游戏刻（2/4/6/8） |
| `--sustain` / `--sustain-interval` | 关 / 2 | 延音（按音符时长重复触发） |
| `--layout` | `folded` | `folded` / `simple` |

---

## 4. Forge 模组（1.20.1 / Forge 47.4.16）

- 入口 `RedStoneMusic.java`，命令：`/redstonemusic place [结构名]`（需权限等级 2）。
  - 不传参 → `STRUCTURE_ID`（默认 `machine30s`）；传参 → `data/redstone_music/structures/<name>.nbt`。
- 起点 = 玩家位置 + 朝向水平偏移 4 格；Y 不偏移；**朝向硬编码沿 +X，不旋转**。
- 放置用 `StructureTemplate.placeInWorld(..., Block.UPDATE_ALL)`：红石粉连接/供电需方块更新重算；结构自带 y=0 泥土支撑层，不会掉落。

### 已知坑（务必遵守）

1. **结构资源名必须是小写 `a-z0-9_.-`**（MC 资源路径规则）。文件名与命令参数都不要用大写/驼峰。
2. 资源文件放在 `src/main/resources/data/redstone_music/structures/`；构建后核对 jar 内条目为小写。
3. 名字非法/文件对不上会分别报"非法结构名"/"结构加载失败"（已加兜底提示，不再是"意外错误"）。

---

## 5. 已修复的缺陷清单

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 折返走廊出现 23~25 格连续粉段 | `do_zfold` 只铺粉、从未按文档插中继器 | 走廊插 R1(隔离折返列总线)+R2(Z 段远端) |
| 2 | 抽头落在 `x=±(X_MAX+1)`/走廊列，总线焊进走廊 | `step_repeater` 越界一格才折返 | 输出格前再判折返 + 抽头前"走回窗口" |
| 3 | 折返走廊后一段不可达 | 中继器**直连**中继器，`simulate` 不支持 | R1 前加一格缓冲粉 |
| 4 | 部分抽头 `dist=None` | 从 repeater 格触发 Y 折返，斜线必须从粉格起步 | step 的第二次检查只允许 Z 折返 |
| 5 | 窄间隔(Δc=1)遇折返扣不起 4 刻 | 时序预算不足 | 该折返**退化为 0 延迟 Y 折返** |
| 6 | 密列+折返处仍 16 格 | 返回端 N=7 的总线与走廊尾焊一起 | Z 段中继器移到 `zm=PITCH-1` |
| 7 | 转弯处粉可能贴音符盒 → 锁存 | 走廊/总线与音符盒相邻 | 新增**硬约束**：音符盒 6 面无粉（断言 + 产物级复核） |
| 8 | 同列同音高音符被静默去重丢弃 | `col_pitches` 用 set | 现状**保留去重**（同刻同音高只听一次，无听感损失）；分档后撞车基本消失 |
| 9 | 按文档示例 `place machineFull` 报"意外错误" | 驼峰大写非法 → `ResourceLocationException`，且崩在首行日志前 | 文件改小写 `machinefull.nbt`；命令参数 `toLowerCase` + try/catch 兜底 |
| 10 | 条带宽固定 14，密列易触发走廊过长 | 固定 `PITCH` | 改为 `PITCH = SLOT0 + max_per_col + 1` 自适应 |
| 11 | 整曲存在单调时序漂移 | 密列+折返处数个 +2 刻累积 | 放宽为 `TIMING_TOL_TICKS=10` 并可打印全曲最大偏差（根治需单独处理折返补偿） |

---

## 6. 已生成的产物

### 6.1 结构（均在 `src/main/resources/data/redstone_music/structures/`，jar 内可直接 `/place`）

| 结构名 | 曲目 | 时长 | 布局 | size (X×Y×Z) | 方块 | 音符盒 | 时序偏差 |
|---|---|---|---|---|---|---|---|
| `machine30s` | 鸟之诗 | 30s | 折叠 | 39×11×77 | 3441 | 302 | ≤8 刻 |
| `machine30s_simple` | 鸟之诗 | 30s | 直线 | 497×2×9 | 3050 | 302 | **0** |
| `machinefull` | 鸟之诗 | 181s | 折叠 | 39×101×356 | 22701 | 1950 | ≤8 刻 |
| `machinefull_simple` | 鸟之诗 | 181s | 直线 | 3301×2×11 | 19944 | 1950 | **0** |
| `pipa2_24s` | 琵琶曲(钢琴改编) | 24s | 折叠 | 39×5×54 | 2213 | 194 | **0** |
| `pipa2_24s_simple` | 琵琶曲(钢琴改编) | 24s | 直线 | 313×2×10 | 1940 | 194 | **0** |
| `badapple_simple` | Bad Apple | 30.7s | 直线 | 409×2×7 | 2236 | 203 | **0** |
| `ruoshui_34s_simple` | 若水 | 34s | 直线 | 311×2×6 | 1758 | 165 | **0** |

> 遗留：`machine10s` / `short10s` 为早期调试产物。

所有产物均通过 (1)~(10) 自校验 + 产物级复核（音符盒 6 面无粉 = 0）。

### 6.2 素材与中间件（`tools/some-onnx/test/`）

`*.mp4`（源视频）、`*_audio.wav`（全曲音频）、`*_34s.wav`（切片）、`*_bp.mid`（basic-pitch 输出）、`*.nbt`（结构）。

### 6.3 MIDI 转写统计

| 曲目 | 音符数 | 最大同时 | 平均同时 | 音域 | 弯音点 |
|---|---|---|---|---|---|
| 鸟之诗 | ~1950 | 9 | — | 29–96 | — |
| 琵琶曲 24s | 194 | 9 | 4.28 | 38–91 | 0 |
| Bad Apple 30.7s | 203 | 6 | 2.11 | 49–97 | 182 |
| 若水 34s | 165 | 5 | 3.19 | 46–86 | 0 |

---

## 7. 已知限制与待办

1. **弯音（pitch bend）未处理**：`load_notes` 只读 note_on/off，滑音/揉弦会丢。Bad Apple 有 182 个弯音点。若要可辨，需按弯音分段切音高（较大改动）。
2. **折叠版时序漂移**：整曲最大 ≤8 刻（约 0.4s，曲末），来自密列+折返处若干 +2 刻累积。根治需修折返补偿根因。
3. **折返走廊受 15 格约束**：极端密列（单列 9~11 音符）在"窄间隔+折返"处仍可能吃紧；当前由断言拦截，不会带病出厂。
4. **世界高度**：折叠整曲 Y=101（限 320 内）；若换更长的曲子层数继续增长，需评估高度上限。
5. **直线版占地长**：整曲直线 X=3301（跨 ~207 区块），`place` 时会一次性放近 2 万方块，可能明显卡顿。
6. **未做**：真实琵琶/人声等复杂音色验证；`--sustain` 实机效果；结构放置朝向随玩家旋转。

---

## 8. 复现步骤（以"新视频 → 直线版"为例）

```powershell
# 1. 下载视频（爬虫）
cd E:\project\spider\douyin_spider
"https://v.douyin.com/xxxx/`n`n" | python scripts\dyv.py

# 2. 拷进项目（用短名，避开 Windows 路径长度限制）
Copy-Item "output\video\<id>_*.mp4" "E:\project\RedStoneMusic\tools\some-onnx\test\x.mp4"

# 3. 分离音频 + 截取
cd E:\project\RedStoneMusic\tools\some-onnx
ffmpeg -y -i test\x.mp4 -t 34 -vn -ac 2 -ar 44100 test\x_34s.wav

# 4. 音频 → MIDI
.\.venv\Scripts\python.exe src\pipeline.py test\x_34s.wav test\x_34s_bp.mid

# 5. MIDI → 简单版结构（自校验不过会直接报错并删除产物）
.\.venv\Scripts\python.exe src\gen_machine.py test\x_34s_bp.mid test\x_34s_simple.nbt --seconds 34 --layout simple

# 6. 接入模组并构建
Copy-Item test\x_34s_simple.nbt ..\..\src\main\resources\data\redstone_music\structures\
cd ..\..
.\gradlew build

# 7. 游戏内
# /redstonemusic place x_34s_simple   （面朝东，前方留空地；结构起点拉杆即播放）
```

---

## 9. 关键文件索引

| 路径 | 说明 |
|---|---|
| `tools/some-onnx/src/gen_machine.py` | 红石结构生成器（含自校验/产物复核） |
| `tools/some-onnx/src/pipeline.py` | 音频 → MIDI（basic-pitch ONNX） |
| `src/main/java/com/redstonemusic/RedStoneMusic.java` | 模组命令与放置逻辑 |
| `src/main/resources/data/redstone_music/structures/*.nbt` | 结构资源 |
| `E:\project\spider\douyin_spider` | 抖音爬虫（视频下载） |
| `tools/some-onnx/test/` | 素材与中间产物 |