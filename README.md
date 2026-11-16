# AutoRedStoneMusic

将音频自动转换为 Minecraft 红石音乐结构的 Forge 模组与本地转换工具链。

AutoRedStoneMusic 的目标，是把一首歌曲从普通音频文件直接变成可以在 Minecraft 世界中放置、播放和分享的红石音乐结构。用户只需要把 MP3 或 WAV 放入指定目录，即可在游戏内调用转换工具，生成 `.nbt` 结构并放置到玩家面前。

> **本次更新（2026-11-16）**：在线链路新增**可选乐器提示**（`Convert <歌名> online inst`），按音符盒音色家族强制分离。垫块从 4 种扩到**16 种原版音色**（bass / didgeridoo / guitar / banjo / harp / bit / flute / xylophone / iron_xylophone / cow_bell / bell / chime / pling + basedrum / snare / hat），GM program 与鼓键位自动映射；起始总线改为**分段中继器 + 按轨补列**，6~8 条轨也能在同一 tick 起播；转换结果放置后会在结构末端自动生成**返回按钮**（命令方块 + 石按钮，一键传回起点）。

> **本次更新（2026-11-14）**：**分离算法重写**。改为 HPSS → REPET-SIM(kNN) → STFT 域平滑交叉分频，修掉 WOLA 边界尖峰和红石粉长度误报；`midi_to_nbt` 新增 `--poly-cap`，单轨移调改为整八度。曾试验过“三角色分离 + 音符级融合”，量化指标更好但实际听感变薄，**已回滚**，传统链路仍为 6 轨逐轨转写后合并。

> **本次更新（2026-11-08）**：新增**在线多乐器转写**（可选联网）。`Convert <歌名> online` 会把音频交给公开部署的在线模型转写，得到带乐器信息的 MIDI，再直接生成**按乐器分轨的多轨结构**（轨道数 = 乐器数）。执行命令后聊天栏会先询问是否允许上传，点「是」才开始；该分支固定参数、不接受其它命令参数，转换完成自动放置。

> **本次更新（2026-11-06）**：`midi_to_nbt.exe`、`arrange_multilane.exe`、`merge_midis.exe` 已由 Python/PyInstaller **重写为 C++17**，逐方块/逐音符输出与旧版一致；实测提速约 **108× / 53× / 145×**，模组 JAR 因此缩小约 **54 MB**。新增独立的 **NTB → NBS（Note Block Studio）** 导出工具。README 补充了从克隆仓库到构建模组的完整步骤（含 Git LFS）。

> **本次更新（2026-10-31）**：新增**多轨直排结构**——在“最终 MIDI → NBT”阶段按音高/音色拆成 1~3 条并行红石轨道，所有轨道共享同一条时间轴骨架与单一拉杆起始总线，脉冲同步前进；`Convert` 新增 `lanes <N>`、`split pitch|bank`、`poly 1..128` 参数，默认仍走原版单轨 NBT；所有结构支撑方块统一替换为**平滑石英**（保留音符盒正下方的乐器垫块）；新增独立工具 `arrange_multilane.py`、`arrange_straight.py`、`eval_ntb_loss.py` 与两个调试结构。

> **本次更新（2026-10-11）**：新增**音源分离多音轨转换**，提供 AI（Demucs）与传统算法（C++ REPET/HPSS）双引擎；支持按能量门限丢弃弱轨、逐轨转 MIDI 后合并、再复用原本三档音域扩展生成单条结构。同时修复非整秒总时长断言、24bit WAV 读取、分离写盘缩放导致还原度偏低等问题。

## 核心能力

> **本次更新**：在原有“音频 → MIDI → NBT”单链路之外，新增音源分离多音轨链路与双分离引擎，并新增能量门限丢轨、多轨 MIDI 合并、单档三档映射、内置结构调试放置等能力。

- MP3/WAV 自动识别并转换为 Standard MIDI。
- 使用 Basic Pitch ONNX 模型进行多音符、起音和音符持续性分析。
- 使用 C++ 与 ONNX Runtime 实现音频推理，避免运行时依赖 Python 或 JNI。
- 将 MIDI 音符量化为 Minecraft 红石时序。
- 自动生成音符盒、红石粉、红石中继器、音符线路和乐器基座。
- 生成 Minecraft Structure NBT，可直接由 Forge 模组加载和放置。
- 支持 `simple` 直线布局和 `folded` 折叠布局。
- 支持不同 resolution、和弦、多音符同列以及延音处理。
- 转换工具释放到系统临时目录，转换任务使用独立工作目录，支持多任务并发。
- 游戏退出存档时不删除工具；Minecraft 进程真正退出时才清理临时工具目录。
- 支持 MP3/NTB 列表、只转换、转换并自动放置、读取已有 NBT 放置等命令。
- **新增：音源分离多音轨转换（双引擎）**——AI 引擎（Demucs htdemucs，C++ 多线程推理）与传统算法引擎（C++ REPET-SIM + HPSS，固定 6 类：drums/bass/other/vocals/guitar/piano）。
- **新增：能量门限丢轨**——按能量占比（默认 1.5%）自动丢弃低能量音轨，避免噪声轨污染结果。
- **新增：多轨 MIDI 合并**——逐轨转写 MIDI 后合并为单一 MIDI，复用三档音域扩展（高音 bell / 中音 harp / 低音 bass）生成同一条结构。
- **新增：内置结构调试放置**——`debug place <structure>` 直接放置模组自带结构，便于试听对比与内部调试。
- **新增：多轨直排结构（可选）**——在“最终 MIDI → NBT”阶段按音高/音色拆成 1~3 条并行轨道；每条轨道有独立红石骨架，全部共享同一条时间轴列集合与单一拉杆起始总线，脉冲同步前进。
- **新增：`lanes` / `split` / `poly` 命令参数**——默认仍生成原版单轨 NBT；`lanes` 启用多轨，`split` 选择按音高均衡或按音色档位分轨，`poly` 限制每轨单列复音上限（1~128，默认 20）。
- **新增：统一建筑材质**——所有结构支撑/条带间隔方块改为 `minecraft:smooth_quartz`，仅保留音符盒正下方的乐器垫块（`dirt` / `bamboo_planks` / `gold_block`）以及折叠布局中功能性必需的 `glowstone`。
- **新增：C++17 快速工具链**——`midi_to_nbt` / `arrange_multilane` / `merge_midis` 已从 Python/PyInstaller 重写为 C++17，输出与原版一致，速度提升约 53×~145×，模组 JAR 缩小约 54 MB。
- **新增：NTB → NBS 导出**——独立 C++ 工具 `ntb_to_nbs.exe` 可把已生成的红石结构转换为 Note Block Studio 的 `.nbs`（v4，20 TPS），便于在 NBS 中查看、编辑和二次分享。
- **新增：在线多乐器转写（可选联网）**——`Convert <歌名> online` 使用公开部署的在线模型完成多乐器转写，不需要本机运行推理模型；结果直接生成按乐器分轨的多轨结构（轨道数 = 实际出现的乐器数）。上传前需在聊天栏点「是」确认，点「否」不会上传任何文件。
- **新增：可选乐器提示（`online inst`）**——默认由在线模型自行判断乐器；加 `inst` 才发送乐器提示，让模型按音符盒可用的音色家族强制分离。民族乐器、独奏等素材建议不加，避免被硬塞进不匹配的音色。
- **新增：16 种原版音符盒音色**——除原有的 bass / harp / bell / basedrum 外，新增 didgeridoo / guitar / banjo / bit / flute / xylophone / iron_xylophone / cow_bell / chime / pling / snare / hat，每种音色自动使用对应的垫块与基准音，GM program 与鼓键位自动映射到最接近的音色。
- **新增：分段起始总线 + 按轨补列**——红石粉 0 延迟上限 15 格，超出后每 14 格插入一个中继器（延迟 = 1 列），靠前的轨补空列，所有轨道仍在同一 tick 起播；按乐器严格分轨出现 6~8 条轨时不再需要把 bell 并进 harp。
- **新增：末端返回按钮**——放置结构后自动在终点灯后补一个命令方块 + 石按钮，点击即用相对坐标传送回拉杆起点；不写入 NBT，结构文件保持原样。
- **新增：传统分离算法重写**——HPSS 先行做打击/谐波拆分，vocals 用 kNN REPET-SIM（96 对数频带相似帧平均）软掩蔽，分频改到 STFT 域做平滑交叉并补零消除边界尖峰；6 轨输出保持不变，逐轨转写后合并。

## 工作流程

> **本次更新**：流水线新增“音源分离 → 逐轨 MIDI → 合并 MIDI → 单档三档生成”分支；传统分离工具由 Python 改写为 C++（固定 7 线程），新增 MIDI 合并工具，并修复非整秒总时长断言与音频格式兼容问题。

单音轨（原有）链路：

```text
MP3/WAV
   |
   v
FFmpeg 音频解码
   |
   v
C++ audio_to_midi.exe + Basic Pitch nmp.onnx
   |
   v
Standard MIDI
   |
   v
midi_to_nbt.exe + 红石结构生成器
   |
   v
Minecraft Structure NBT
   |
   v
Forge 模组读取并放置到玩家前方
```

多音轨（传统引擎，2026-11-14 分离算法重写）：

```text
MP3/WAV
   |
   v
FFmpeg 规整为 44.1kHz 立体声 16-bit PCM WAV
   |
   v
traditional_sep.exe
   ├─ HPSS（时间/频率轴中值 17/17）：drums / harmonic
   ├─ REPET-SIM（96 对数频带 + kNN 相似帧平均）：vocals / instrumental
   └─ STFT 域平滑交叉分频：bass / guitar / piano / other
   |
   v
6 条 stem：drums / bass / other / vocals / guitar / piano
   |
   v
能量门限：丢弃能量占比 < 1.5% 的音轨
   |
   v
逐轨 audio_to_midi.exe（Basic Pitch）
   |
   v
merge_midis.exe 合并为单一 MIDI
   |
   v
midi_to_nbt.exe --poly-cap 13 → Minecraft Structure NBT
```

AI 引擎（Demucs）为 4 轨（drums / bass / other / vocals），后续步骤相同。

多轨结构（可选，2026-10-31 新增）：

```text
最终 MIDI（单轨直转结果，或分离后合并的 MIDI）
   |
   v
arrange_multilane.exe
   ├─ split pitch：按音高排序均衡分轨
   ├─ split bank ：按 bass / harp / bell 音色分轨
   └─ poly <N>   ：限制每轨单列复音（超出保留最高音）
   |
   v
N 条平行直排红石轨道（各自 repeater + 支线 + 音符盒）
   |
   v
共享起始总线（一个拉杆 + observer + 0 延迟红石粉）
   |
   v
Minecraft Structure NBT
```

在线多乐器（2026-11-08 新增，可选，需要联网；2026-11-16 支持 `inst` 乐器提示）：

```text
MP3/WAV
   |
   v
玩家确认允许上传
   |
   v
公开部署的在线模型（多乐器转写；`online inst` 时附带乐器提示）
   |
   v
多乐器 MIDI（每件乐器保留独立的轨道 / program 信息）
   |
   v
arrange_multilane.exe --split instrument
   ├─ GM program / 鼓键位 → 16 种音符盒音色
   ├─ 轨道数 = 实际出现的音色数（常见 6~8 条）
   └─ 起始总线每 14 格插一个中继器，靠前的轨补空列对齐起播
   |
   v
共享起始总线 + Minecraft Structure NBT（自动放置，末端附返回按钮）
```

项目由以下独立的 Windows 工具组成：

1. `audio_to_midi.exe`
   - Python 转换管线（basic_pitch + onnxruntime）通过 PyInstaller 打包为独立 EXE。
   - 调用 FFmpeg 将 MP3/WAV 转成 22050 Hz、单声道 PCM，输出 Standard MIDI。
   - 仓库另含 `tools/some-onnx/cpp/` 的 C++ ONNX 参考实现，可用 `build_win.ps1` 构建。

2. `midi_to_nbt.exe`（C++17，2026-11-06 重写）
   - 读取 MIDI 并生成 Minecraft Structure NBT；支持 folded/simple、延音与音轨音色。
   - 源码在 `tools/some-onnx/cpp/fast_tools/`，运行时已替换为 C++ 版。
   - 与旧 Python 版逐方块一致，实测约 108× 提速。

3. `merge_midis.exe`（C++17，2026-11-06 重写）
   - 把多轨 MIDI 合并为单一 MIDI（每源独立 channel，同 tick 先 off 后 on）。
   - 与旧版音符事件一致，实测约 145× 提速。

4. `arrange_multilane.exe`（C++17，2026-11-06 重写）
   - 可选多轨结构生成器：按 `split` 分轨、按 `poly` 限复音，生成 1~3 条平行直排轨道。
   - 所有轨道共享时间轴骨架与单一拉杆起始总线；与旧版逐方块一致，实测约 53× 提速。

5. `demucs_mt.cpp.main.exe`
   - AI 音源分离（Demucs htdemucs，4 轨），C++ 多线程 CPU 推理 + 模型权重。

6. `traditional_sep.exe`
   - 传统算法分离（C++17 + Eigen，7 线程）。
   - `--roles` 模式输出 `drums / bass / harmonic` 三条；否则输出 6 条细分类。

7. `ntb_to_nbs.exe`（独立工具，2026-11-06 新增，未打进模组）
   - NTB → Note Block Studio `.nbs`（v4，20 TPS）。
   - 源码在 `tools/some-onnx/cpp/ntb_to_nbs/`；按乐器基准音高映射 NBS key（0=A0，harp 33~57、bass 9~33、bell 57~81）。

8. 在线转写客户端（2026-11-08 新增，模组内置 Java 实现）
   - 直接向公开部署的在线模型提交音频，读取转写完成的多乐器 MIDI，再交给 `arrange_multilane.exe --split instrument` 生成多轨结构。
   - 不占用本机推理 CPU / 显存，但需要联网；不含本地回退，在线服务不可用时转换失败。
   - 服务地址集中在源码的在线转写客户端中，可按需替换为其它公开部署的在线服务。

9. `fuse_midis.exe`（C++17，2026-11-14 新增，未接入默认链路）
   - 音符级投票融合实验工具：`--primary` 为主转写，`--pool` 为分离轨转写（可多次）；保留 primary 中能在 pool 找到匹配（`--tol` 秒、`--semis` 半音）的音符。
   - 量化指标更好但实际听感变薄，**默认链路不使用**；仅保留为独立工具。
   - 源码在 `tools/some-onnx/cpp/fast_tools/fast_fuse_midis.cpp`。

模组通过 `ProcessBuilder` 调用这些 EXE，不使用 JNI，因此音频推理、音源分离和 NBT 生成与 Minecraft JVM 解耦，便于调试、替换和独立测试。在线转写是唯一的联网链路，且必须先经玩家在聊天栏确认。

## 游戏内目录

> **本次更新（2026-10-31）**：临时工具资源目录新增 `arrange_multilane.exe`（多轨结构生成器）。
>
> **本次更新（2026-10-11）**：临时工具资源目录新增多音轨相关 EXE 与 Demucs 模型权重。

首次启动后，模组会在 Minecraft 配置目录创建：

```text
config/RedStoneMusic/
├── mp3/    # 放入待转换的 .mp3 或 .wav
└── ntb/    # 保存生成的 .ntb/.nbt 结构
```

工具资源会从模组资源中释放到系统临时目录，包括：

```text
redstonemusic/tools/
├── audio_to_midi.exe
├── midi_to_nbt.exe
├── nmp.onnx
├── onnxruntime.dll
├── onnxruntime_providers_shared.dll
├── ffmpeg.exe
├── demucs_mt.cpp.main.exe
├── ggml-model-htdemucs-4s-f16.bin
├── traditional_sep.exe
├── merge_midis.exe
├── fuse_midis.exe
└── arrange_multilane.exe
```

上面的 `redstonemusic/tools/` 是临时工作目录中的资源目录，不是用户需要手动创建的配置目录。

## 游戏内命令

> **本次更新（2026-11-16）**：`online` 分支新增可选子节点 `inst`：`Convert <歌名> online inst` 会附带乐器提示，按 16 种音符盒音色强制分轨；不写 `inst` 时仍由在线模型自行判断。所有转换结果放置后都会在结构末端生成命令方块 + 石按钮的返回点（需开启命令方块/作弊）。

> **本次更新（2026-11-14）**：命令语法未变。`trad true` 使用重写后的分离算法（仍是 6 轨逐轨转写后合并）；单轨/多轨 NBT 生成统一带 `--poly-cap 13`（单列超限保留高音，避免支线总线超过 15 格报错）；单轨移调改为整八度。

> **本次更新（2026-11-08）**：`Convert` 新增 `online` 分支：`Convert <歌名> online`。该分支不接受其它命令参数；执行后先在聊天栏询问是否允许上传音频，点「是」才联网转写，随后按乐器分轨并自动放置。生成文件名为 `<原名>_online_multi.ntb`。

> **本次更新（2026-10-31）**：`Convert` 新增 `lanes <N>`、`split pitch|bank`、`poly 1..128` 语法节点；默认不写 `lanes` 时仍走原版单轨 NBT；生成文件名追加 `_L<N>[_bank][_P<cap>]` 签名。
>
> **本次更新（2026-10-11）**：`Convert` 增加引擎与多轨语法节点（`ai`/`trad`、`true`/`false`），`true`/`false` 作为命令语法组成而非字符串参数；新增 `debug place <structure>` 内置结构放置；生成文件名加入引擎与多轨签名。

需要权限等级 2。命令同时提供大写主命令和小写兼容命令。

```text
/RedStoneMusic list mp3
/RedStoneMusic list ntb
/RedStoneMusic Convert song.mp3
/RedStoneMusic Convert song.mp3 to ntb
/RedStoneMusic Convert song.mp3 true
/RedStoneMusic Convert song.mp3 false
/RedStoneMusic Convert song.mp3 ai true
/RedStoneMusic Convert song.mp3 trad true
/RedStoneMusic Convert song.mp3 trad false
/RedStoneMusic Convert song.mp3 lanes 3
/RedStoneMusic Convert song.mp3 lanes 3 poly 8
/RedStoneMusic Convert song.mp3 lanes 3 split bank
/RedStoneMusic Convert song.mp3 lanes 3 split bank poly 8
/RedStoneMusic Convert song.mp3 true lanes 3 poly 8
/RedStoneMusic Convert song.mp3 trad true lanes 3 split bank poly 12
/RedStoneMusic Convert song.mp3 online
/RedStoneMusic Convert song.mp3 online inst
/RedStoneMusic place song.ntb
/RedStoneMusic debug place <structure>
```

命令作用：

- `list mp3`：列出配置目录中的 MP3/WAV 文件。
- `list ntb`：列出已经生成的结构文件。
- `Convert song.mp3`：单轨转换歌曲，保存 NBT，并自动在玩家前方放置。
- `Convert song.mp3 to ntb`：单轨转换并保存 NBT，不自动放置。
- `Convert song.mp3 true`：启用**多音轨分离**（默认 `ai` 引擎），保存 NBT 并自动放置。
- `Convert song.mp3 false`：不启用多音轨分离（等价于单轨转换）。
- `Convert song.mp3 ai true`：使用 **AI（Demucs）引擎**做多音轨转换。
- `Convert song.mp3 trad true`：使用**传统 C++ 算法**做多音轨转换。
- `Convert song.mp3 trad false`：显式指定传统引擎但不启用多音轨分离。
- `place song.ntb`：读取已有 NBT 并放置结构。
- `debug place <structure>`：放置模组内置结构资源（`data/redstone_music/structures/<structure>.nbt`），无需文件位于 `ntb` 目录，便于内部试听对比。
- `Convert song.mp3 lanes 3`：把最终 MIDI 生成 **3 条并行直排轨道**（默认 `split=pitch`、`poly=20`）。
- `Convert song.mp3 lanes 3 poly 8`：每轨单列最多保留 8 个音符，超出保留最高音；`poly` 范围 1~128。
- `Convert song.mp3 lanes 3 split bank`：按 `bass / harp / bell` 音色分轨，而不是按音高均衡分轨。
- `Convert song.mp3 true lanes 3 ...`：先做 AI 多音轨分离，再把合并后的最终 MIDI 生成 3 轨结构。
- `Convert song.mp3 trad true lanes 3 ...`：传统分离 + 3 轨结构。
- `Convert song.mp3 online`：使用公开部署的在线模型做**多乐器转写**，生成**按乐器分轨的多轨结构**（轨道数 = 乐器数），完成后自动放置。该命令不接受其它参数；执行后聊天栏会弹出「是 / 否」确认，只有点「是」才会上传音频。需要联网，且不含本地回退。
- `Convert song.mp3 online inst`：同上，但附带**乐器提示**，让模型按音符盒可用的音色家族强制分离，便于吃满 16 种垫块；民族乐器等素材建议不加。

生成文件名为 `<原名>_<引擎>_<是否多轨>[_L<N>][_bank][_P<cap>].ntb`，例如：单轨 AI 转换 `a_ai_false.ntb`、传统多轨转换 `a_trad_true.ntb`、3 轨 pitch/poly20 `a_ai_false_L3.ntb`、3 轨 bank/poly8 `a_ai_false_L3_bank_P8.ntb`。在线转换输出 `<原名>_online_multi.ntb`，带乐器提示时为 `<原名>_online_multi_inst.ntb`。输入文件只允许使用配置目录下的单个文件名，模组会检查扩展名并阻止路径穿越。

放置完成后，结构末端（终点灯后）会多出一个命令方块和石按钮，点击按钮即可传送回拉杆起点；命令方块未启用（单人未开作弊、服务器 `enable-command-block=false`）时按钮不会生效。

## 音符盒音色与垫块（2026-11-16 扩充）

每种音色的可用音域固定为 25 个半音（两个八度 + 1），超出部分按 ±12 折回。

| 音色 | 垫块 | 基准音（note 0） |
| --- | --- | --- |
| bass | `bamboo_planks` | F#1 |
| didgeridoo | `pumpkin` | F#1 |
| guitar | `white_wool` | F#2 |
| banjo | `hay_block` | F#3 |
| harp | `dirt` | F#3 |
| bit | `emerald_block` | F#3 |
| flute | `clay` | F#4 |
| xylophone | `bone_block` | F#4 |
| iron_xylophone | `iron_block` | F#4 |
| cow_bell | `soul_sand` | F#4 |
| bell | `gold_block` | F#5 |
| chime | `packed_ice` | F#5 |
| pling | `glowstone` | F#5 |
| basedrum | `stone` | 无音高（note 固定 0） |
| snare | `sand` | 无音高（note 固定 0） |
| hat | `glass` | 无音高（note 固定 0） |

GM program 与鼓键位按家族映射到上表音色：钢琴/弦乐/人声 → harp，吉他 → guitar，贝斯 → bass，铜管 → didgeridoo，簧管/管乐 → flute，色彩打击 → xylophone，合成主音 → bit，合成铺底 → pling，民族乐器 → banjo，定音鼓 → basedrum。

## 测试结果

> **本次更新（2026-11-16）**：补充 8 轨乐器分轨实测与分段总线同步校验。

> **本次更新（2026-11-14）**：新增欢乐颂前 30 秒的传统链路重构对比（6 轨 / 3 角色 / 音符级融合），以及分离环节的客观指标。

> **本次更新（2026-10-31）**：补充卡农（无 vocals）前 30 秒的单轨 vs 传统分离对比，以及 3 轨直排结构实测；新增 MIDI/NBT 损失量化工具与结果。结构支撑材质统一为平滑石英。
>
> **本次更新（2026-10-11）**：新增多音轨（AI / 传统分离 → 能量门限 → 逐轨 MIDI → 合并 → 单轨三档）结构测试；补充 C++ 传统分离还原度核验——把分离出的音轨合成回原曲，与原始音频的相似度（相关系数）约 0.98 量级，确认分离与写盘环节保真。

> **本次更新（2026-11-14）**：补充“量化指标的局限”说明——指标存在系统性偏好，只能作为参考，取舍以试听为准。

### 量化指标的局限

本仓库使用的 chroma / top1 / top3 等指标**存在系统性偏好，只具备部分参考价值**：

- **不惩罚漏音**：top1/top3 只判断该参考帧里这个音级有没有出现，删掉把握不大的音符就能直接刷高分数。
- **忽略音乐信息**：chroma 只看整段音级分布，不区分八度、时值、力度和音色。
- **对时间对齐敏感**：默认 0.1s 对齐窗口，不同链路必须用同一种对齐方式才可比；对齐偏 0.1s 就能让 top1 从十几%跳到四十几%。
- **实证**：三角色 + 音符融合方案在指标上最高（top1 42.9%），但实际试听比 6 轨合并明显更薄。

结论：指标只能用于**同素材、同对齐条件下的纵向对比**，判断“好不好听”必须以实际试听为准。

### 乐器分轨与总线分段（2026-11-16）

| 项目 | 结果 |
| --- | --- |
| 在线多乐器转写（带乐器提示，8 轨素材） | 8 条轨、2342 个音符盒 |
| 音色分布 | bass 186 / guitar 973 / bit 35 / flute 80 / xylophone 311 / basedrum 115 / snare 90 / hat 552 |
| 结构自校验 | 尺寸 2703×2×53，不可达 0、音符盒贴粉 0 |
| 分段总线（4 轨，z=±71） | 中继器 8 个，延迟 8/2/2/8，靠前轨补列 0/6/6/0，不可达 0 |
| 分段总线（3 轨，z=±28） | 每侧 1 个中继器，不可达 0 |
| 无中继器回归（4 轨，z=±14） | 方块与音符盒数量与改动前完全一致 |

以上结构均在游戏内实机试听确认同步正常。

下面的“转换耗时”指从输入音频开始，经由音频解码、ONNX 推理、MIDI 生成、NBT 生成到结果文件完成的全流程耗时。

| 测试歌曲 | 全流程转换耗时 | 测试内容 |
| --- | ---: | --- |
| 千本樱 | 约 34 秒 | MP4 音频提取后完成 MP3/WAV → MIDI → NBT，并在 Minecraft 中展示红石结构 |
| 义勇军进行曲 | 约 4 秒 | MP4 音频提取后完成 MP3/WAV → MIDI → NBT，并在 Minecraft 中展示红石结构 |

多音轨额外验证：

- 传统 C++ 分离单曲（约 56s 素材）分离阶段约 4.6 秒，明显快于等价的纯 Python 实现。
- 分离音轨重新合成回原曲，与源音频相似度约 0.98，残差归一化后约 0.03，说明分离与写盘未引入可听失真。
- 生成的测试结构：`merged.nbt`（AI 多轨）、`trad_merged.nbt`（传统多轨）、`ap4_merged.nbt`。

卡农 30s / 多轨结构验证（2026-10-31）：

| 指标 | 单轨直转 | 传统分离 |
| --- | ---: | ---: |
| 音符数 | 187 | 589 |
| 全局 chroma 余弦 | 0.9297 | 0.9310 |
| 谐波覆盖率 | 56.6% | 63.3% |
| 逐音符音级 top1 / top3 | 54.5% / 89.8% | 49.7% / 82.0% |

- 3 轨直排结构（`split=pitch`，`poly=6`）：197 / 196 / 196 音符，size 699×2×27，音符盒 445，共享起始总线最远 10 格；自校验不可达 0、音符盒贴粉 0。
- 平滑石英验证：支撑方块 3324 个 `smooth_quartz`，`dirt` 仅 320 个且全部位于音符盒正下方；bass/bell 垫块 `bamboo_planks` / `gold_block` 保持不变。
- 调试结构：`canon_roles_straight.nbt`（按角色分声部单轨）、`canon_multilane.nbt`（3 轨）。

C++ 重写等价性与提速（2026-11-06）：

| 用例 | 旧版（Python） | C++ 版 | 结果 |
| --- | --- | --- | --- |
| `midi_to_nbt` folded | 2345 方块 / 187 音符 | 同 | PASS |
| `midi_to_nbt` simple | 2044 方块 / 187 音符 | 同 | PASS |
| `--sustain` | 6147 方块 / 584 音符 | 同 | PASS |
| `arrange_multilane` 3 轨 | 7534 方块 / 445 音符 | 同 | PASS |
| `merge_midis` 4 stems | 589 音符事件 | 同 | PASS |
| 完整链路 merge → midi_to_nbt | 4406 方块 | 同 | PASS |

| 工具 | 旧版中位数 | C++ 版中位数 | 提速 |
| --- | ---: | ---: | ---: |
| `midi_to_nbt` | 1413.1 ms | 13.1 ms | **107.9×** |
| `arrange_multilane` | 1948.8 ms | 36.7 ms | **53.1×** |
| `merge_midis` | 823.8 ms | 5.7 ms | **144.5×** |

计时包含旧版 PyInstaller 启动开销；不同机器上会有波动，复测 `midi_to_nbt` 约 1.49~1.70 s → 17.1~17.6 ms。

传统链路重构（2026-11-14，欢乐颂前 30 秒，44.1kHz 立体声）：

| 链路 | 音符盒 | 结构尺寸 | 全局 chroma | 逐音符 top1 | top3 |
|---|---:|---|---:|---:|---:|
| 单次全混音转写 | 325 | 609×2×9 | 0.8570 | 37.5% | 76.3% |
| 旧传统（6 轨 + 旧分离） | 754 | 993×2×13 | 0.9175 | 30.8% | 71.4% |
| **传统 6 轨 + 新分离（当前默认）** | **752** | 969×2×15 | 0.9114 | 34.4% | 72.6% |
| 传统 3 角色（已回滚） | 479 | 761×2×12 | 0.8800 | 33.4% | 70.8% |
| 3 角色 + 音符融合（已回滚） | 198 | 455×2×8 | 0.8919 | 42.9% | 78.8% |

> 三角色与音符融合在客观指标上更高，但实际试听明显偏薄（大量被过滤的音符其实是有效内容），因此没有保留在默认链路中。这类“逐音符命中率”指标会奖励少而稳的音符，不能单独作为听感依据。

分离环节指标（同素材）：

| 版本 | vocals 能量占比 | 重建 corr / 残差 | bass 谱质心 |
|---|---:|---|---:|
| 旧（中值 REPET + 滤波器分频） | 56.8% | 0.9948 / 0.0095 | 171Hz |
| 新（kNN REPET + STFT 分频 + WOLA 补零） | 13.5% | 0.9995 / 0.0022 | 103Hz |

融合规则消融（MIDI 层，offset=0）：并集 37.8% < 单次转写 38.5% < harmonic 单轨 43.5% < **交集（harmonic∪bass）44.9%**。

测试覆盖：

- 音频解码与模型推理。
- MIDI 文件写出和读取。
- NBT 结构生成与压缩写出。
- 红石线路时序校验。
- Minecraft StructureTemplate 回读。
- Forge 模组内转换、保存和放置。
- 退出存档后重新进入时工具仍然可用。
- 多音轨链路：音源分离、能量门限丢轨、逐轨转写、MIDI 合并、单轨三档映射。

## 与传统方法的对比

> **本次更新**：对比维度新增“自动音源分离与分轨重组”能力。

传统的 Minecraft 音乐制作通常需要人工扒谱、手动输入音符、逐段调整红石延迟，再通过地图编辑器或结构工具反复测试。这种方式适合少量手工创作，但面对完整歌曲时，工作量会随着音符数量和歌曲长度快速增加，难以保持稳定节拍，也很难把普通用户的音频直接变成可放置结构。

| 对比项目 | 传统手工方法 | AutoRedStoneMusic |
| --- | --- | --- |
| 输入 | 乐谱、音符列表或人工扒谱 | MP3/WAV |
| 音符获取 | 人工识别和录入 | ONNX 模型自动分析 |
| 节奏处理 | 手动设置延迟 | 自动量化为 Minecraft tick |
| 和弦处理 | 需要逐列安排 | 自动处理同一时刻的多个音符 |
| 音源分离 | 需人工分轨或直接混音转写 | 自动分离为 vocals/伴奏/乐器多轨后重组 |
| 延音 | 手工设计线路 | 生成器统一处理并校验 |
| 结构生成 | 手动搭建或多次编辑 | 自动输出 Structure NBT |
| 验证 | 游戏内反复试听 | 生成阶段进行线路和时序校验 |
| 扩展歌曲 | 成本高，容易出错 | 更换输入音频即可重复转换 |

这不是简单地把已有播放器搬进游戏，而是改变了红石音乐结构的生产方式：输入从“人工整理后的音符数据”前移为“普通音频”，编谱、量化、布局、线路生成和结构导出全部被串成自动化流水线。红石音乐从逐个音符搭建，转变为可重复、可验证、可部署的内容生成过程。

## 为什么这是一次革命式改变

> **本次更新**：新增一条“把混音拆成声部、再按声部映射回结构”的能力，使复杂混音歌曲也能分轨参与结构生成。

- **降低创作门槛**：不要求用户先掌握乐理、扒谱和红石延迟设计。
- **扩大可转换内容**：理论上任何可解码的音乐音频都能作为输入，而不是局限于已经整理好的 MIDI 或 NBS。
- **从混音中还原声部**：内置 AI 与传统两套音源分离，把整首歌拆成 vocals/伴奏/乐器轨，再按声部映射回结构，减弱不同声部互相干扰导致的音准流失。
- **把声音直接映射到空间结构**：模型识别的是音频中的音符事件，生成器再把这些事件转换成 Minecraft 中可观察、可编辑、可分享的红石实体。
- **让结构成为可复用产物**：生成的 NBT 可以保存、复制、分享和再次放置，不依赖当次转换过程。
- **形成可扩展工具链**：音频识别、音源分离、MIDI 处理和 NBT 生成相互独立，未来可以替换模型、加入更多乐器映射或接入游戏内面板，而不必重写整个系统。
- **兼顾自动化与可验证性**：不是只追求生成文件，而是在生成过程中检查时序、线路和 NBT 回读，减少进入游戏后才发现结构损坏的情况。

## 构建要求

> **本次更新（2026-11-06）**：补充从克隆到构建的完整步骤；`midi_to_nbt` / `arrange_multilane` / `merge_midis` 改为 C++17 构建，不再使用 PyInstaller；运行时二进制通过 Git LFS 管理。

### 前置环境

| 依赖 | 用途 | 要求 |
| --- | --- | --- |
| Windows 10/11 x64 | 运行与构建 | — |
| JDK 17 | Forge 模组 | 17 |
| Git + Git LFS | 拉取运行时 EXE/DLL | 较新版本 |
| Visual Studio 2022 C++ 工具链 | C++ 工具 | MSVC v143 / x64 |
| CMake + Ninja | C++ 工具 | CMake 3.16+ |
| Python 3.x + PyInstaller | 仅重建 `audio_to_midi.exe` | 3.9+ |
| ONNX Runtime Windows x64 C++ SDK 1.29.0 | 仅重建 C++ `audio_to_midi` 参考实现 | 1.29.0 |

目标运行时：Minecraft 1.20.1 + Forge 47.4.16（Forge 依赖由 Gradle 自动解析）。

### 从克隆到构建（模组）

```powershell
git clone https://github.com/honghax/AutoRedStoneMusic.git RedStoneMusic-local
cd RedStoneMusic-local
git lfs install
git lfs pull
.\gradlew.bat build --no-daemon
```

- `tools/some-onnx/runtime/` 已包含运行时 EXE/DLL（Git LFS）；`processResources` 会把它们复制到 JAR 内的 `redstonemusic/tools/`。
- **只要 LFS 拉取成功，无需重建任何工具即可构建模组。**
- 构建产物：`build/libs/redstone_music-*.jar`。
- 若没执行 `git lfs pull`，克隆下来的 EXE/DLL 只是指针文件：模组能构建，但运行时会失败。

### 重建 C++ 快速工具（可选）

`midi_to_nbt`、`arrange_multilane`、`merge_midis` 的源码在 `tools/some-onnx/cpp/fast_tools/`：

```powershell
powershell -ExecutionPolicy Bypass -File tools\some-onnx\cpp\fast_tools\build.ps1
```

脚本会自动查找 CMake/Ninja/MSVC，产物在 `tools/some-onnx/cpp/fast_tools/build/`。替换运行时：

```powershell
Copy-Item tools\some-onnx\cpp\fast_tools\build\fast_midi_to_nbt.exe tools\some-onnx\runtime\midi_to_nbt.exe -Force
Copy-Item tools\some-onnx\cpp\fast_tools\build\fast_arrange_multilane.exe tools\some-onnx\runtime\arrange_multilane.exe -Force
Copy-Item tools\some-onnx\cpp\fast_tools\build\fast_merge_midis.exe tools\some-onnx\runtime\merge_midis.exe -Force
```

### 重建传统分离工具（C++）

`tools/some-onnx/cpp/traditional_sep/` 是独立 CMake 工程，依赖 header-only Eigen：

```powershell
cmake -S tools\some-onnx\cpp\traditional_sep -B tools\some-onnx\cpp-build\traditional_sep -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools\some-onnx\cpp-build\traditional_sep
```

### 构建 NTB → NBS（C++，可选）

```powershell
cmake -S tools\some-onnx\cpp\ntb_to_nbs -B tools\some-onnx\cpp-build\ntb_to_nbs -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools\some-onnx\cpp-build\ntb_to_nbs
```

产物：`tools/some-onnx/cpp-build/ntb_to_nbs/ntb_to_nbs.exe`，独立使用，不复制进运行时。

> CMake/Ninja 不在 PATH 时，请在 Visual Studio 的 Developer PowerShell 中运行；`fast_tools/build.ps1` 会自动查找。

### 重建 audio_to_midi（PyInstaller）

使用包含 `basic_pitch`、`pretty_midi`、`mido`、`soundfile`、`librosa`、`resampy`、`soxr`、`onnxruntime`、PyInstaller 的 Python 环境：

```powershell
python -m PyInstaller --clean --noconfirm tools\some-onnx\packaging\audio_to_midi.spec
```

产物在 `dist/audio_to_midi.exe`，复制到 `tools/some-onnx/runtime/audio_to_midi.exe` 即可。

> 仓库中的 C++ ONNX 参考实现可用 `powershell -ExecutionPolicy Bypass -File tools\some-onnx\cpp\build_win.ps1 -OrtRoot "E:\path\to\onnxruntime-win-x64-1.29.0"` 构建；当前运行时使用上面的 PyInstaller 版本。
>
> `midi_to_nbt.spec`、`arrange_multilane.spec`、`merge_midis.spec` 是旧的 PyInstaller 规格，已被 C++ 版取代；不要用它们覆盖当前运行时。

### 构建 Forge 模组

```powershell
.\gradlew.bat build --no-daemon
```

最终 JAR 在 `build/libs/`，运行时资源位于 JAR 内的 `redstonemusic/tools/`。

## 项目结构

> **本次更新（2026-11-06）**：新增 `cpp/fast_tools/`（C++17 版 `midi_to_nbt` / `arrange_multilane` / `merge_midis`）与 `cpp/ntb_to_nbs/`（NTB → NBS）。
>
> **本次更新（2026-10-31）**：新增 `src/arrange_multilane.py`（最终 MIDI → 多轨直排）、`src/arrange_straight.py`（多角色 → 单条直排）、`src/eval_ntb_loss.py`（损失量化）、`packaging/arrange_multilane.spec` 与调试结构 `canon_roles_straight.nbt` / `canon_multilane.nbt`。
>
> **本次更新（2026-10-11）**：新增 `cpp/traditional_sep/`（C++ 传统分离）、`cpp/eigen/`（Eigen 头文件）、`src/merge_midis.py`（MIDI 合并）、`src/traditional_separate.py`（Python 参考实现）与 `MultiTrackNbtMerger.java`。

```text
src/main/java/com/redstonemusic/   Forge 模组与命令实现
src/main/resources/                模组元数据和运行时工具资源
 tools/some-onnx/cpp/              C++ 音频推理源码与构建脚本
 tools/some-onnx/cpp/traditional_sep/   C++ 传统音源分离
 tools/some-onnx/cpp/eigen/         Eigen 头文件库
 tools/some-onnx/cpp/fast_tools/    C++17 快速工具（midi_to_nbt / arrange_multilane / merge_midis / fuse_midis）
 tools/some-onnx/cpp/fast_tools/fast_fuse_midis.cpp   音符级投票融合（2026-11-14）
 tools/some-onnx/cpp/ntb_to_nbs/    NTB → NBS 转换器
 tools/some-onnx/src/              MIDI、NBT 和 Python 辅助源码
 tools/some-onnx/src/arrange_multilane.py   最终 MIDI → 多轨直排结构
 tools/some-onnx/src/arrange_straight.py    多角色 MIDI → 单条直排结构
 tools/some-onnx/src/eval_ntb_loss.py       MIDI/NBT 损失量化
 tools/some-onnx/packaging/        PyInstaller 构建规格
 tools/some-onnx/packaging/arrange_multilane.spec   多轨工具打包规格
 tools/some-onnx/runtime/          EXE、DLL、FFmpeg 和模型权重
 src/main/resources/data/redstone_music/structures/  内置调试结构（含 canon_*.nbt）
```

## 安全说明与服务器管理员须知

> **本次更新**：资源释放清单与并发说明补充多音轨工具（`demucs_mt.cpp.main.exe`、`traditional_sep.exe`、`merge_midis.exe` 与 Demucs 模型）。

这是一个会在服务器主机上启动外部原生程序的模组，不应仅因它是 Forge JAR 就视为没有主机权限影响。集成服务器进程通常能够访问其操作系统账户可读写的文件；本模组启动的 EXE 会继承该账户权限。当前版本没有使用操作系统沙箱、容器隔离或独立低权限账户，也没有对转换任务数或 CPU 使用率设置硬性上限。请只在你信任模组来源、已审阅发行版本并理解其运行方式时安装。

### 为什么会释放并启动 EXE/DLL

模组 JAR 内包含固定版本的 `audio_to_midi.exe`、`midi_to_nbt.exe`、`arrange_multilane.exe`、`fuse_midis.exe`、`demucs_mt.cpp.main.exe`、`traditional_sep.exe`、`merge_midis.exe`、Demucs 模型权重、FFmpeg、ONNX Runtime DLL 和 `nmp.onnx` 权重。初始化时，模组把这些资源复制到系统临时目录下新建的 `RedStoneMusic-*` 工作目录。这样做是为了让 Windows 能从普通文件路径加载可执行文件和 DLL；它们不是从网络下载的，也不会从歌曲文件中提取或安装程序。转换命令通过 `ProcessBuilder` 以参数列表启动这些指定 EXE，而不是经由 shell 拼接命令。

### 文件访问和清理范围

- 输入仅从 `config/RedStoneMusic/mp3/` 选择，支持 `.mp3` 和 `.wav`；文件名解析拒绝目录分隔符和路径穿越。
- `audio_to_midi.exe` 读取所选音频，调用随包 FFmpeg 解码，并使用随包 ONNX 模型推理，生成中间 Standard MIDI 文件。
- 多音轨流程通过 `demucs_mt.cpp.main.exe` 或 `traditional_sep.exe` 分离音轨，再用 `merge_midis.exe` 合并中间 MIDI；这些中间文件都位于该转换任务的独立临时目录内。
- `midi_to_nbt.exe` 读取该 MIDI，生成压缩的 Minecraft Structure NBT，写到 `config/RedStoneMusic/ntb/`。这是用户要求保留的最终转换结果。
- 中间 MIDI、分离音轨和任务目录在该转换任务结束时删除；这不是对最终 NBT 的删除。临时工具、模型和 DLL 留在工具工作目录中，以便退出当前存档后仍可继续使用；Minecraft/JVM 正常关闭时由 shutdown hook 尝试清理。
- 清理是尽力而为：若进程崩溃、系统断电或文件被占用，临时文件可能残留。管理员可在确认 Minecraft 已关闭后检查并删除名称以 `RedStoneMusic-` 开头的本模组临时目录。
- 模组记录命令和工具输出日志。不要把含有敏感内容的私人音频放入服务器；工具日志也应按服务器日志策略保护。

### 在线转写（可选联网）

- 只有玩家执行 `Convert <文件> online` 并在聊天栏点「是」后，所选音频才会被上传到一个**不受中国境内监管的第三方在线服务**。
- 上传内容仅为所选音频文件本身，不包含世界存档、玩家信息或其它目录中的文件。
- 该链路不启动本地推理 EXE；在线服务不可用或排队时会失败并提示，重试次数有限（最多 12 次、每次间隔 5 秒）。
- 服务地址集中在源码的在线转写客户端中，便于服务器管理员或整合包作者替换为其它公开部署的在线服务；本文档不固定具体服务地址。
- 若音频涉及隐私或不宜外传，请勿使用 `online` 命令；本地 `Convert`、`true`、`trad` 链路完全在主机内完成，不产生网络上传。

### CPU 峰值和并发

Basic Pitch ONNX 推理、Demucs/传统音源分离和 FFmpeg 解码属于计算密集型操作，转换期间出现明显 CPU 峰值是预期现象，不表示它在空闲时持续运行。其中传统分离引擎按硬编码的 7 个物理线程并发执行，AI 分离与逐轨转写同样会较长时间占满多核 CPU。转换在后台线程执行，但当前使用可缓存线程池，没有配置最大并发转换数、每玩家速率限制、CPU 配额或超时。因此有权限的多个玩家同时发起转换时，可能启动多个推理/分离任务并竞争 CPU、内存和磁盘 I/O。只向可信任的玩家开放命令权限；在多人或高负载服务器上，建议先限制可使用命令的管理员人数，并在生产部署前自行设置操作系统级资源限制。权限等级 2 是 Minecraft 命令权限检查，不是操作系统沙箱。

### 建议的部署与核验

- 从项目指定的 GitHub 仓库或你自行构建的 JAR 获取文件；不要加载来源不明、被重新打包或无法核验的发行版。
- 在隔离的测试服务器上先审阅 JAR 内容、源码和依赖，再部署到生产服；对下载文件使用可信的杀毒/端点防护工具扫描。
- 以专用、低权限的操作系统账户运行 Minecraft，并仅授予该账户访问游戏目录、配置目录和临时目录所需的权限；不要用管理员/root 账户运行服务器。
- 将服务器进程的文件权限限制在游戏数据范围。需要更强隔离时使用操作系统账户隔离、容器或虚拟机，并对 CPU、内存、进程数和临时磁盘空间设置外部配额。
- 在多人服务器上，将转换权限只授予可信用户。监控转换命令日志、CPU/内存和临时目录；定期备份 `config/RedStoneMusic/ntb/` 中需要保留的结构。

以上说明描述的是当前实现，不构成安全审计或对第三方运行时组件无漏洞的保证。外部 EXE、DLL、FFmpeg、ONNX Runtime、模型和 Python 打包依赖均扩大了需要核验的供应链范围。

## 许可证与第三方组件

> **本次更新**：新增 Eigen（MPL2）等随 C++ 传统分离引入的第三方组件说明。

本项目包含 Minecraft Forge 模组代码、Basic Pitch 推理流程和红石结构生成器。仓库中的第三方工具保留其原有许可证和作者信息，使用时请同时遵守对应项目的许可条款。

运行时包含 FFmpeg、ONNX Runtime、`nmp.onnx` 模型权重，以及 Demucs（htdemucs）模型权重；这些组件分别受其各自许可证约束。C++ 传统分离工具引入的 Eigen 为 header-only 库，受其自身许可证（MPL2）约束。大型运行时文件通过 Git LFS 管理。
