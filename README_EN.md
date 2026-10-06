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

## Security Notes for Server Administrators

This mod starts native programs on the server host. A Forge JAR should not be assumed to have no host-level security impact: the integrated server process normally has access to files permitted to its operating-system account, and child executables inherit that account's permissions. The current implementation does not use an OS sandbox, container isolation, or a separate low-privilege account. It also does not impose a hard limit on conversion concurrency or CPU use. Install it only if you trust the source, have reviewed the release you are deploying, and understand its runtime behavior.

### Why executables and DLLs are extracted

The JAR bundles fixed versions of `audio_to_midi.exe`, `midi_to_nbt.exe`, FFmpeg, ONNX Runtime DLLs, and the `nmp.onnx` weights. During initialization, the mod copies these resources into a newly created `RedStoneMusic-*` working directory under the system temporary directory. Windows tools and DLLs need ordinary filesystem paths for execution and loading. These files are bundled resources, not downloaded from the network or extracted from song input. The mod launches the two specified executables with `ProcessBuilder` argument lists rather than building a shell command string.

### File access and cleanup scope

- Input is selected from `config/RedStoneMusic/mp3/`; `.mp3` and `.wav` are accepted. Filename resolution rejects path separators and path traversal.
- `audio_to_midi.exe` reads the selected audio, invokes the bundled FFmpeg decoder, runs inference using the bundled ONNX model, and writes an intermediate Standard MIDI file.
- `midi_to_nbt.exe` reads that MIDI and writes a compressed Minecraft Structure NBT file to `config/RedStoneMusic/ntb/`. This is the final conversion result and is intentionally retained.
- The intermediate MIDI file and per-job directory are deleted when that conversion job finishes. The final NBT is not deleted. Runtime tools, model weights, and DLLs remain in the tool working directory so they can be reused after leaving a world; a JVM shutdown hook attempts to remove that directory when Minecraft exits normally.
- Cleanup is best-effort. A crash, power loss, or locked file can leave temporary files behind. After confirming Minecraft is closed, administrators can inspect and remove this mod's temporary directories whose names start with `RedStoneMusic-`.
- The mod logs commands and tool output. Do not place sensitive private audio on a server, and protect tool logs according to the server's log-handling policy.

### CPU spikes and concurrency

Basic Pitch ONNX inference and FFmpeg decoding are compute-intensive, so a noticeable CPU spike while a conversion is running is expected; it should not be interpreted as continuous idle activity. Conversions run on background threads, but the current implementation uses a cached thread pool and has no configured maximum concurrency, per-player rate limit, CPU quota, or timeout. Multiple authorized players can therefore start simultaneous inference jobs that compete for CPU, memory, and disk I/O. Grant command permission only to trusted players. For multiplayer or high-load servers, restrict access to a small set of trusted operators and apply OS-level resource limits before production deployment. Minecraft permission level 2 is a command authorization check, not an OS sandbox.

### Deployment and verification recommendations

- Obtain the JAR from the project's designated GitHub repository or build it yourself. Do not load unknown, repackaged, or unverifiable releases.
- Review the JAR contents, source, and dependencies on an isolated test server before production deployment, and scan downloaded files with trusted endpoint-protection software.
- Run Minecraft under a dedicated, low-privilege OS account. Grant only the filesystem access needed for the game, configuration, and temporary directories; do not run the server as Administrator/root.
- Restrict the server process's filesystem permissions to game data. For stronger isolation, use OS account separation, a container, or a virtual machine, and impose external quotas for CPU, memory, process count, and temporary disk space.
- On multiplayer servers, grant conversion permission only to trusted users. Monitor conversion logs, CPU/memory, and the temporary directory; back up structures in `config/RedStoneMusic/ntb/` that must be retained.

These notes describe the current implementation. They are not a security audit or a guarantee that third-party runtime components are vulnerability-free. The native executables, DLLs, FFmpeg, ONNX Runtime, model, and packaged Python dependencies all expand the software supply chain that administrators should verify.

## License and Third-Party Components

The repository contains Forge mod code, the Basic Pitch inference pipeline, and the redstone structure generator. Third-party tools retain their original licenses and author information; users must comply with the corresponding terms.

The runtime distribution includes FFmpeg, ONNX Runtime, and the `nmp.onnx` model weights. These components are governed by their respective licenses. Large runtime binaries are managed with Git LFS.
