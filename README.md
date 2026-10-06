# AutoRedStoneMusic

将音频自动转换为 Minecraft 红石音乐结构的 Forge 模组与本地转换工具链。

AutoRedStoneMusic 的目标，是把一首歌曲从普通音频文件直接变成可以在 Minecraft 世界中放置、播放和分享的红石音乐结构。用户只需要把 MP3 或 WAV 放入指定目录，即可在游戏内调用转换工具，生成 `.nbt` 结构并放置到玩家面前。

## 核心能力

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

## 工作流程

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

项目由两个独立的 Windows 工具组成：

1. `audio_to_midi.exe`
   - C++17 实现。
   - 使用 ONNX Runtime C++ API 加载 `nmp.onnx`。
   - 调用 FFmpeg 将 MP3/WAV 转成 22050 Hz、单声道 PCM。
   - 输出 Standard MIDI。

2. `midi_to_nbt.exe`
   - Python 生成器通过 PyInstaller 打包为独立 EXE。
   - 读取 MIDI 并生成 Minecraft Structure NBT。
   - 保留音符量化、线路布局、延音和结构校验逻辑。

模组通过 `ProcessBuilder` 调用这两个 EXE，不使用 JNI，因此音频推理和 NBT 生成与 Minecraft JVM 解耦，便于调试、替换和独立测试。

## 游戏内目录

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
└── ffmpeg.exe
```

上面的 `redstonemusic/tools/` 是临时工作目录中的资源目录，不是用户需要手动创建的配置目录。

## 游戏内命令

需要权限等级 2。命令同时提供大写主命令和小写兼容命令。

```text
/RedStoneMusic list mp3
/RedStoneMusic list ntb
/RedStoneMusic Convert song.mp3
/RedStoneMusic Convert song.mp3 to ntb
/RedStoneMusic place song.ntb
```

命令作用：

- `list mp3`：列出配置目录中的 MP3/WAV 文件。
- `list ntb`：列出已经生成的结构文件。
- `Convert song.mp3`：转换歌曲，保存 NBT，并自动在玩家前方放置。
- `Convert song.mp3 to ntb`：只转换并保存 NBT，不自动放置。
- `place song.ntb`：读取已有 NBT 并放置结构。

输入文件只允许使用配置目录下的单个文件名，模组会检查扩展名并阻止路径穿越。

## 测试结果

测试视频是转换结果和歌曲展示，视频播放时长不等于转换耗时。下面的“转换耗时”指从输入音频开始，经由音频解码、ONNX 推理、MIDI 生成、NBT 生成到结果文件完成的全流程耗时。

| 测试歌曲 | 全流程转换耗时 | 测试内容 | 视频 |
| --- | ---: | --- | --- |
| 千本樱 | 约 34 秒 | MP4 音频提取后完成 MP3/WAV → MIDI → NBT，并在 Minecraft 中展示红石结构 | [观看测试视频](./千本樱.mp4) |
| 义勇军进行曲 | 约 4 秒 | MP4 音频提取后完成 MP3/WAV → MIDI → NBT，并在 Minecraft 中展示红石结构 | [观看测试视频](./义勇军进行曲.mp4) |

测试覆盖：

- 音频解码与模型推理。
- MIDI 文件写出和读取。
- NBT 结构生成与压缩写出。
- 红石线路时序校验。
- Minecraft StructureTemplate 回读。
- Forge 模组内转换、保存和放置。
- 退出存档后重新进入时工具仍然可用。

## 与传统方法的对比

传统的 Minecraft 音乐制作通常需要人工扒谱、手动输入音符、逐段调整红石延迟，再通过地图编辑器或结构工具反复测试。这种方式适合少量手工创作，但面对完整歌曲时，工作量会随着音符数量和歌曲长度快速增加，难以保持稳定节拍，也很难把普通用户的音频直接变成可放置结构。

| 对比项目 | 传统手工方法 | AutoRedStoneMusic |
| --- | --- | --- |
| 输入 | 乐谱、音符列表或人工扒谱 | MP3/WAV |
| 音符获取 | 人工识别和录入 | ONNX 模型自动分析 |
| 节奏处理 | 手动设置延迟 | 自动量化为 Minecraft tick |
| 和弦处理 | 需要逐列安排 | 自动处理同一时刻的多个音符 |
| 延音 | 手工设计线路 | 生成器统一处理并校验 |
| 结构生成 | 手动搭建或多次编辑 | 自动输出 Structure NBT |
| 验证 | 游戏内反复试听 | 生成阶段进行线路和时序校验 |
| 扩展歌曲 | 成本高，容易出错 | 更换输入音频即可重复转换 |

这不是简单地把已有播放器搬进游戏，而是改变了红石音乐结构的生产方式：输入从“人工整理后的音符数据”前移为“普通音频”，编谱、量化、布局、线路生成和结构导出全部被串成自动化流水线。红石音乐从逐个音符搭建，转变为可重复、可验证、可部署的内容生成过程。

## 为什么这是一次革命式改变

- **降低创作门槛**：不要求用户先掌握乐理、扒谱和红石延迟设计。
- **扩大可转换内容**：理论上任何可解码的音乐音频都能作为输入，而不是局限于已经整理好的 MIDI 或 NBS。
- **把声音直接映射到空间结构**：模型识别的是音频中的音符事件，生成器再把这些事件转换成 Minecraft 中可观察、可编辑、可分享的红石实体。
- **让结构成为可复用产物**：生成的 NBT 可以保存、复制、分享和再次放置，不依赖当次转换过程。
- **形成可扩展工具链**：音频识别、MIDI 处理和 NBT 生成相互独立，未来可以替换模型、加入更多乐器映射或接入游戏内面板，而不必重写整个系统。
- **兼顾自动化与可验证性**：不是只追求生成文件，而是在生成过程中检查时序、线路和 NBT 回读，减少进入游戏后才发现结构损坏的情况。

## 构建要求

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

```text
src/main/java/com/redstonemusic/   Forge 模组与命令实现
src/main/resources/                模组元数据和运行时工具资源
 tools/some-onnx/cpp/              C++ 音频推理源码与构建脚本
 tools/some-onnx/src/              MIDI、NBT 和 Python 辅助源码
 tools/some-onnx/packaging/        PyInstaller 构建规格
 tools/some-onnx/runtime/          EXE、DLL、FFmpeg 和模型权重
```

## 许可证与第三方组件

本项目包含 Minecraft Forge 模组代码、Basic Pitch 推理流程和红石结构生成器。仓库中的第三方工具保留其原有许可证和作者信息，使用时请同时遵守对应项目的许可条款。

运行时包含 FFmpeg、ONNX Runtime 和 `nmp.onnx` 模型权重；这些组件分别受其各自许可证约束。大型运行时文件通过 Git LFS 管理。
