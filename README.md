# AutoRedStoneMusic

将音频自动转换为 Minecraft 红石音乐结构的 Forge 模组与本地转换工具链。

AutoRedStoneMusic 的目标，是把一首歌曲从普通音频文件直接变成可以在 Minecraft 世界中放置、播放和分享的红石音乐结构。用户只需要把 MP3 或 WAV 放入指定目录，即可在游戏内调用转换工具，生成 `.nbt` 结构并放置到玩家面前。

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

多音轨（新增）链路：

```text
MP3/WAV
   |
   v
FFmpeg 规整为 44.1kHz 立体声 16-bit PCM WAV
   |
   v
音源分离（二选一）
   ├─ ai  ：demucs_mt.cpp.main.exe + htdemucs 模型（4 轨）
   └─ trad：traditional_sep.exe（REPET-SIM + HPSS，6 轨）
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
midi_to_nbt.exe 单轨三档映射 → Minecraft Structure NBT
```

项目由以下独立的 Windows 工具组成：

1. `audio_to_midi.exe`
   - C++17 实现。
   - 使用 ONNX Runtime C++ API 加载 `nmp.onnx`。
   - 调用 FFmpeg 将 MP3/WAV 转成 22050 Hz、单声道 PCM。
   - 输出 Standard MIDI。

2. `midi_to_nbt.exe`
   - Python 生成器通过 PyInstaller 打包为独立 EXE。
   - 读取 MIDI 并生成 Minecraft Structure NBT。
   - 保留音符量化、线路布局、延音和结构校验逻辑。

3. `demucs_mt.cpp.main.exe`（新增）
   - AI 音源分离引擎（Demucs htdemucs，4 轨：drums / bass / other / vocals）。
   - C++ 多线程纯 CPU 推理，配套 `ggml-model-htdemucs-4s-f16.bin` 模型权重。

4. `traditional_sep.exe`（新增）
   - 传统算法音源分离（C++17 + Eigen，纯 CPU）。
   - 流程：STFT → REPET-SIM（人声/伴奏，非局部中值滤波 + Wiener 软掩蔽）→ HPSS（鼓/谐波）→ 谐波频段切分（贝斯 <250Hz / 吉他 250~1000Hz / 钢琴 >1kHz / other 为剩余）。
   - 固定输出 6 类：drums / bass / other / vocals / guitar / piano；硬编码 7 物理线程。

5. `merge_midis.exe`（新增）
   - 把多轨 MIDI 合并为单一 MIDI（每轨分配独立 channel，避免同音高跨轨覆盖与首尾相接配对错误）。

模组通过 `ProcessBuilder` 调用这些 EXE，不使用 JNI，因此音频推理、音源分离和 NBT 生成与 Minecraft JVM 解耦，便于调试、替换和独立测试。

## 游戏内目录

> **本次更新**：临时工具资源目录新增多音轨相关 EXE 与 Demucs 模型权重。

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
└── merge_midis.exe
```

上面的 `redstonemusic/tools/` 是临时工作目录中的资源目录，不是用户需要手动创建的配置目录。

## 游戏内命令

> **本次更新**：`Convert` 增加引擎与多轨语法节点（`ai`/`trad`、`true`/`false`），`true`/`false` 作为命令语法组成而非字符串参数；新增 `debug place <structure>` 内置结构放置；生成文件名加入引擎与多轨签名。

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

生成文件名为 `<原名>_<引擎>_<是否多轨>.ntb`，例如单轨 AI 转换 `a_ai_false.ntb`、传统多轨转换 `a_trad_true.ntb`。输入文件只允许使用配置目录下的单个文件名，模组会检查扩展名并阻止路径穿越。

## 测试结果

> **本次更新**：新增多音轨（AI / 传统分离 → 能量门限 → 逐轨 MIDI → 合并 → 单轨三档）结构测试；补充 C++ 传统分离还原度核验——把分离出的音轨合成回原曲，与原始音频的相似度（相关系数）约 0.98 量级，确认分离与写盘环节保真。

测试视频是转换结果和歌曲展示，视频播放时长不等于转换耗时。下面的“转换耗时”指从输入音频开始，经由音频解码、ONNX 推理、MIDI 生成、NBT 生成到结果文件完成的全流程耗时。

| 测试歌曲 | 全流程转换耗时 | 测试内容 | 视频 |
| --- | ---: | --- | --- |
| 千本樱 | 约 34 秒 | MP4 音频提取后完成 MP3/WAV → MIDI → NBT，并在 Minecraft 中展示红石结构 | [观看测试视频](./千本樱.mp4) |
| 义勇军进行曲 | 约 4 秒 | MP4 音频提取后完成 MP3/WAV → MIDI → NBT，并在 Minecraft 中展示红石结构 | [观看测试视频](./义勇军进行曲.mp4) |

多音轨额外验证：

- 传统 C++ 分离单曲（约 56s 素材）分离阶段约 4.6 秒，明显快于等价的纯 Python 实现。
- 分离音轨重新合成回原曲，与源音频相似度约 0.98，残差归一化后约 0.03，说明分离与写盘未引入可听失真。
- 生成的测试结构：`merged.nbt`（AI 多轨）、`trad_merged.nbt`（传统多轨）、`ap4_merged.nbt`。

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

> **本次更新**：新增传统分离工具的 C++ 构建（MSVC + CMake/Ninja，Eigen 为 header-only，已随仓库提供）；传统分离不再使用 PyInstaller 打包。

- Windows 10/11
- Java 17
- Minecraft 1.20.1
- Forge 47.4.16
- Visual Studio 2022 C++ 工具链
- CMake 与 Ninja
- Python 3.x
- PyInstaller
- ONNX Runtime Windows x64 C++ SDK 1.29.0

### 构建 C++ 音频工具

```powershell
powershell -ExecutionPolicy Bypass -File tools\some-onnx\cpp\build_win.ps1 `
  -OrtRoot "E:\path\to\onnxruntime-win-x64-1.29.0"
```

构建结果位于被忽略的 `tools/some-onnx/cpp-build/`。构建脚本会自动查找 CMake、Ninja 和 MSVC，并清理旧的 CMake 配置缓存。

### 构建传统分离工具（C++）

`tools/some-onnx/cpp/traditional_sep/` 是独立的 CMake 工程，依赖同目录下的 header-only Eigen（`tools/some-onnx/cpp/eigen`）：

```powershell
cmake -S tools\some-onnx\cpp\traditional_sep -B tools\some-onnx\cpp-build\traditional_sep -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools\some-onnx\cpp-build\traditional_sep
```

### 构建 Python 工具

使用包含 `basic_pitch`、`pretty_midi`、`mido`、`nbtlib` 和 PyInstaller 的 Python 环境：

```powershell
python -m PyInstaller --clean --noconfirm `
  tools\some-onnx\packaging\audio_to_midi.spec

python -m PyInstaller --clean --noconfirm `
  tools\some-onnx\packaging\midi_to_nbt.spec
```

### 构建 Forge 模组

```powershell
.\gradlew.bat build --no-daemon
```

最终模组 JAR 位于 `build/libs/`。构建资源会被复制到 JAR 内的 `redstonemusic/tools/`。

## 项目结构

> **本次更新**：新增 `cpp/traditional_sep/`（C++ 传统分离）、`cpp/eigen/`（Eigen 头文件）、`src/merge_midis.py`（MIDI 合并）、`src/traditional_separate.py`（Python 参考实现）与 `MultiTrackNbtMerger.java`。

```text
src/main/java/com/redstonemusic/   Forge 模组与命令实现
src/main/resources/                模组元数据和运行时工具资源
 tools/some-onnx/cpp/              C++ 音频推理源码与构建脚本
 tools/some-onnx/cpp/traditional_sep/   C++ 传统音源分离
 tools/some-onnx/cpp/eigen/         Eigen 头文件库
 tools/some-onnx/src/              MIDI、NBT 和 Python 辅助源码
 tools/some-onnx/packaging/        PyInstaller 构建规格
 tools/some-onnx/runtime/          EXE、DLL、FFmpeg 和模型权重
```

## 安全说明与服务器管理员须知

> **本次更新**：资源释放清单与并发说明补充多音轨工具（`demucs_mt.cpp.main.exe`、`traditional_sep.exe`、`merge_midis.exe` 与 Demucs 模型）。

这是一个会在服务器主机上启动外部原生程序的模组，不应仅因它是 Forge JAR 就视为没有主机权限影响。集成服务器进程通常能够访问其操作系统账户可读写的文件；本模组启动的 EXE 会继承该账户权限。当前版本没有使用操作系统沙箱、容器隔离或独立低权限账户，也没有对转换任务数或 CPU 使用率设置硬性上限。请只在你信任模组来源、已审阅发行版本并理解其运行方式时安装。

### 为什么会释放并启动 EXE/DLL

模组 JAR 内包含固定版本的 `audio_to_midi.exe`、`midi_to_nbt.exe`、`demucs_mt.cpp.main.exe`、`traditional_sep.exe`、`merge_midis.exe`、Demucs 模型权重、FFmpeg、ONNX Runtime DLL 和 `nmp.onnx` 权重。初始化时，模组把这些资源复制到系统临时目录下新建的 `RedStoneMusic-*` 工作目录。这样做是为了让 Windows 能从普通文件路径加载可执行文件和 DLL；它们不是从网络下载的，也不会从歌曲文件中提取或安装程序。转换命令通过 `ProcessBuilder` 以参数列表启动这些指定 EXE，而不是经由 shell 拼接命令。

### 文件访问和清理范围

- 输入仅从 `config/RedStoneMusic/mp3/` 选择，支持 `.mp3` 和 `.wav`；文件名解析拒绝目录分隔符和路径穿越。
- `audio_to_midi.exe` 读取所选音频，调用随包 FFmpeg 解码，并使用随包 ONNX 模型推理，生成中间 Standard MIDI 文件。
- 多音轨流程通过 `demucs_mt.cpp.main.exe` 或 `traditional_sep.exe` 分离音轨，再用 `merge_midis.exe` 合并中间 MIDI；这些中间文件都位于该转换任务的独立临时目录内。
- `midi_to_nbt.exe` 读取该 MIDI，生成压缩的 Minecraft Structure NBT，写到 `config/RedStoneMusic/ntb/`。这是用户要求保留的最终转换结果。
- 中间 MIDI、分离音轨和任务目录在该转换任务结束时删除；这不是对最终 NBT 的删除。临时工具、模型和 DLL 留在工具工作目录中，以便退出当前存档后仍可继续使用；Minecraft/JVM 正常关闭时由 shutdown hook 尝试清理。
- 清理是尽力而为：若进程崩溃、系统断电或文件被占用，临时文件可能残留。管理员可在确认 Minecraft 已关闭后检查并删除名称以 `RedStoneMusic-` 开头的本模组临时目录。
- 模组记录命令和工具输出日志。不要把含有敏感内容的私人音频放入服务器；工具日志也应按服务器日志策略保护。

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
