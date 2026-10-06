# AutoRedStoneMusic

A Forge mod and local toolchain that converts ordinary audio into playable Minecraft redstone music structures.

AutoRedStoneMusic takes an MP3 or WAV file and turns it into a Minecraft Structure NBT file containing note blocks and redstone timing. The generated structure can be saved, shared, loaded again, and placed directly in front of the player from inside Minecraft.

## What It Does

- Converts MP3/WAV audio into Standard MIDI.
- Uses a Basic Pitch ONNX model to detect notes, onsets, polyphony, and note persistence.
- Runs audio inference in C++ through the ONNX Runtime C++ API, without requiring Python or JNI at runtime.
- Quantizes MIDI events into Minecraft redstone timing.
- Generates note blocks, redstone dust, repeaters, note lines, and instrument bases.
- Exports a Minecraft Structure NBT file that the Forge mod can load and place.
- Supports both `simple` linear layout and `folded` layout.
- Supports resolution settings, chords, simultaneous notes, sustain handling, and redstone validation.
- Extracts the tools to a system temporary directory and gives each conversion job its own workspace for concurrent use.
- Keeps the tools available when a player leaves a world; cleanup happens when the Minecraft process actually exits.
- Provides commands for listing files, converting only, converting and placing, and placing an existing NBT structure.

## Pipeline

```text
MP3/WAV
   |
   v
FFmpeg audio decoding
   |
   v
C++ audio_to_midi.exe + Basic Pitch nmp.onnx
   |
   v
Standard MIDI
   |
   v
midi_to_nbt.exe + redstone structure generator
   |
   v
Minecraft Structure NBT
   |
   v
Forge places the structure in front of the player
```

The project is intentionally split into two independent Windows tools:

1. `audio_to_midi.exe`
   - Implemented in C++17.
   - Loads `nmp.onnx` through the ONNX Runtime C++ API.
   - Uses FFmpeg to decode MP3/WAV into 22050 Hz mono PCM.
   - Writes Standard MIDI.

2. `midi_to_nbt.exe`
   - Packages the Python generator into a standalone executable with PyInstaller.
   - Reads MIDI and writes Minecraft Structure NBT.
   - Preserves note quantization, layout, sustain, and structure validation logic.

The mod calls both executables with `ProcessBuilder`, without JNI. Audio inference and NBT generation are therefore decoupled from the Minecraft JVM, making the pipeline easier to test, replace, and extend.

## In-Game Directories

After the first launch, the mod creates:

```text
config/RedStoneMusic/
├── mp3/    # Put .mp3 or .wav files here
└── ntb/    # Generated .ntb/.nbt structures are saved here
```

The mod extracts runtime resources to a system temporary directory:

```text
redstonemusic/tools/
├── audio_to_midi.exe
├── midi_to_nbt.exe
├── nmp.onnx
├── onnxruntime.dll
├── onnxruntime_providers_shared.dll
└── ffmpeg.exe
```

This `redstonemusic/tools/` path is created inside the temporary runtime workspace. Users do not need to create it manually.

## In-Game Commands

The commands require permission level 2. Lowercase compatibility commands are also registered.

```text
/RedStoneMusic list mp3
/RedStoneMusic list ntb
/RedStoneMusic Convert song.mp3
/RedStoneMusic Convert song.mp3 to ntb
/RedStoneMusic place song.ntb
```

Commands:

- `list mp3`: Lists MP3/WAV files in the configuration directory.
- `list ntb`: Lists generated structure files.
- `Convert song.mp3`: Converts the song, saves the NBT, and places it in front of the player.
- `Convert song.mp3 to ntb`: Converts and saves the NBT without placing it.
- `place song.ntb`: Loads and places an existing structure.

Only a single file name inside the configured directory is accepted. The mod validates extensions and rejects path traversal attempts.

## Test Results

The test videos show the converted songs and the resulting Minecraft redstone music. Video playback duration is not conversion time. The conversion time below means the complete pipeline from input audio through decoding, ONNX inference, MIDI writing, NBT generation, and final output completion.

| Test song | Full conversion time | Test coverage | Video |
| --- | ---: | --- | --- |
| Senbonzakura | About 34 seconds | MP4 audio extracted, then MP3/WAV → MIDI → NBT, followed by in-game redstone structure playback | [Watch test video](./千本樱.mp4) |
| March of the Volunteers | About 4 seconds | MP4 audio extracted, then MP3/WAV → MIDI → NBT, followed by in-game redstone structure playback | [Watch test video](./义勇军进行曲.mp4) |

The tests cover:

- Audio decoding and model inference.
- MIDI writing and reading.
- NBT structure generation and compressed output.
- Redstone timing and wiring validation.
- StructureTemplate loading and readback.
- In-game conversion, saving, and placement.
- Re-entering a world after leaving it while keeping the extracted tools available.

## Compared With Traditional Methods

Traditional Minecraft music construction usually requires manually transcribing a song, entering notes, tuning redstone delays, arranging the circuit, and repeatedly testing the result in a world. That approach remains useful for small hand-crafted compositions, but the effort grows rapidly with song length and note count. It also makes consistent timing difficult and gives ordinary audio files no direct path into a Minecraft structure.

| Area | Traditional manual workflow | AutoRedStoneMusic |
| --- | --- | --- |
| Input | Sheet music, note lists, or manual transcription | MP3/WAV |
| Note extraction | Manual recognition and entry | Automatic ONNX analysis |
| Rhythm | Manually configured delays | Automatic Minecraft-tick quantization |
| Chords | Notes arranged column by column | Simultaneous notes handled automatically |
| Sustain | Hand-designed circuit behavior | Generated and validated by the toolchain |
| Structure creation | Manual construction or repeated editing | Automatic Structure NBT export |
| Verification | Repeated in-game listening | Timing, wiring, and NBT checks during generation |
| Scaling to new songs | Expensive and error-prone | Re-run the pipeline with another audio file |

This is more than putting an existing player inside the game. It changes how redstone music structures are produced: the input moves from manually prepared note data to ordinary audio, while transcription, quantization, layout, wiring, and structure export become one automated pipeline. Redstone music changes from placing individual notes by hand into a repeatable, verifiable, deployable content-generation process.

## Why This Is a Revolutionary Change

- **A much lower entry barrier**: users do not need to master music theory, transcription, or redstone delay design before creating a song structure.
- **A broader input space**: any decodable music audio can become an input, instead of requiring a prepared MIDI or NBS file first.
- **A direct bridge from sound to space**: detected audio events become visible and editable Minecraft entities.
- **Reusable output**: generated NBT structures can be saved, copied, shared, and placed again without repeating the conversion.
- **A modular architecture**: the audio model, MIDI processing, and NBT generator are independent components that can evolve separately.
- **Automation with verification**: the system does not merely emit a file; it validates timing, wiring, and NBT readback during generation, reducing failures discovered only after entering the game.

## Build Requirements

- Windows 10/11
- Java 17
- Minecraft 1.20.1
- Forge 47.4.16
- Visual Studio 2022 C++ toolchain
- CMake and Ninja
- Python 3.x
- PyInstaller
- ONNX Runtime Windows x64 C++ SDK 1.29.0

### Build the C++ Audio Tool

```powershell
powershell -ExecutionPolicy Bypass -File tools\some-onnx\cpp\build_win.ps1 `
  -OrtRoot "E:\path\to\onnxruntime-win-x64-1.29.0"
```

The output is written to the ignored `tools/some-onnx/cpp-build/` directory. The build script searches for CMake, Ninja, and MSVC and removes stale CMake configuration before rebuilding.

### Build the Python Tools

Use a Python environment containing `basic_pitch`, `pretty_midi`, `mido`, `nbtlib`, and PyInstaller:

```powershell
python -m PyInstaller --clean --noconfirm `
  tools\some-onnx\packaging\audio_to_midi.spec

python -m PyInstaller --clean --noconfirm `
  tools\some-onnx\packaging\midi_to_nbt.spec
```

### Build the Forge Mod

```powershell
.\gradlew.bat build --no-daemon
```

The final mod JAR is written to `build/libs/`. Runtime tools are packaged under `redstonemusic/tools/` inside the JAR.

## Project Layout

```text
src/main/java/com/redstonemusic/   Forge mod and command implementation
src/main/resources/                Mod metadata and runtime resources
tools/some-onnx/cpp/               C++ inference source and build scripts
tools/some-onnx/src/               MIDI, NBT, and Python helper source
tools/some-onnx/packaging/         PyInstaller specifications
tools/some-onnx/runtime/           EXE, DLL, FFmpeg, and model files
```

## License and Third-Party Components

The repository contains Forge mod code, the Basic Pitch inference pipeline, and the redstone structure generator. Third-party tools retain their original licenses and author information; users must comply with the corresponding terms.

The runtime distribution includes FFmpeg, ONNX Runtime, and the `nmp.onnx` model weights. These components are governed by their respective licenses. Large runtime binaries are managed with Git LFS.
