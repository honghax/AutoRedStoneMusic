package com.redstonemusic;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import javax.sound.sampled.AudioSystem;
import javax.sound.sampled.AudioInputStream;
import java.nio.file.StandardCopyOption;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MusicToolRunner implements AutoCloseable {
    private static final String AUDIO_EXE = "/redstonemusic/tools/audio_to_midi.exe";
    private static final String NBT_EXE = "/redstonemusic/tools/midi_to_nbt.exe";
    private static final String MODEL = "/redstonemusic/tools/nmp.onnx";
    private static final String ORT = "/redstonemusic/tools/onnxruntime.dll";
    private static final String ORT_PROVIDERS = "/redstonemusic/tools/onnxruntime_providers_shared.dll";
    private static final String FFMPEG = "/redstonemusic/tools/ffmpeg.exe";
    private static final String DEMUCS_EXE = "/redstonemusic/tools/demucs_mt.cpp.main.exe";
    private static final String DEMUCS_MODEL = "/redstonemusic/tools/ggml-model-htdemucs-4s-f16.bin";
    private static final String TRAD_EXE = "/redstonemusic/tools/traditional_sep.exe";
    private static final String MERGE_EXE = "/redstonemusic/tools/merge_midis.exe";
    private static final String MULTILANE_EXE = "/redstonemusic/tools/arrange_multilane.exe";

    private final Path workDirectory;
    private final ExecutorService executor = Executors.newCachedThreadPool(r -> {
        Thread thread = new Thread(r, "RedStoneMusic-convert");
        thread.setDaemon(true);
        return thread;
    });

    public MusicToolRunner() throws IOException {
        workDirectory = Files.createTempDirectory("RedStoneMusic-");
        release(AUDIO_EXE);
        release(NBT_EXE);
        release(MODEL);
        release(ORT);
        release(ORT_PROVIDERS);
        // 所有音频都要先过一遍 FFmpeg 规整，故 ffmpeg 变为必备资源
        release(FFMPEG);
        // 音轨分离作为独立 EXE 与模型一起强制释放（多音轨流程必备资源）
        release(DEMUCS_EXE);
        release(DEMUCS_MODEL);
        // 传统算法分离（C++）与 MIDI 合并（多音轨流程资源）
        release(TRAD_EXE);
        release(MERGE_EXE);
        release(MULTILANE_EXE);
        Runtime.getRuntime().addShutdownHook(new Thread(this::close, "RedStoneMusic-tool-cleanup"));
    }

    public Path convert(Path input, Path output) {
        return convertAsync(input, output).join();
    }

    public CompletableFuture<Path> convertAsync(Path input, Path output) {
        return convertAsync(input, output, 0, "pitch", 20);
    }

    public CompletableFuture<Path> convertAsync(Path input, Path output, int lanes) {
        return convertAsync(input, output, lanes, "pitch", 20);
    }

    public CompletableFuture<Path> convertAsync(Path input, Path output, int lanes, String split) {
        return convertAsync(input, output, lanes, split, 20);
    }

    public CompletableFuture<Path> convertAsync(Path input, Path output, int lanes, String split, int polyCap) {
        return CompletableFuture.supplyAsync(() -> {
            Path jobDirectory;
            try {
                jobDirectory = Files.createTempDirectory(workDirectory, "job-");
            } catch (IOException e) {
                throw new IllegalStateException("无法创建转换临时目录", e);
            }
            try {
                // 任何音频先统一过一遍 FFmpeg，规整为 44100Hz 立体声 16-bit PCM WAV
                Path wav = normalize(input, jobDirectory, "input");
                Path midi = jobDirectory.resolve(input.getFileName() + ".mid");
                run(jobDirectory, List.of(workDirectory.resolve("audio_to_midi.exe").toString(),
                        wav.toString(), midi.toString()));
                String splitMode = "bank".equalsIgnoreCase(split) ? "bank" : "pitch";
                int cap = Math.max(1, Math.min(128, polyCap));
                if (lanes > 0) {
                    run(jobDirectory, List.of(workDirectory.resolve("arrange_multilane.exe").toString(),
                            "--midi", midi.toString(), "--out", output.toString(),
                            "--lanes", Integer.toString(lanes), "--split", splitMode,
                            "--resolution", "2", "--poly-cap", Integer.toString(cap), "--lane-gap", "2"));
                } else {
                    run(jobDirectory, List.of(workDirectory.resolve("midi_to_nbt.exe").toString(),
                            midi.toString(), output.toString(), "--resolution", "2", "--layout", "simple"));
                }
                return output;
            } finally {
                deleteTree(jobDirectory);
            }
        }, executor);
    }

    /**
     * 多音轨转换：分离（ai=demucs / trad=传统C++）→ 能量门限丢弃弱轨 → 逐轨转 MIDI
     * → 合并 MIDI → 单轨三档映射生成一个 NBT。
     *
     * @param engine   "ai" 或 "trad"
     * @param minEnergy 能量占比低于该值的音轨丢弃（如 0.015 = 1.5%）
     */
    public CompletableFuture<Path> convertMultiAsync(Path input, Path output,
                                                     String engine, double minEnergy) {
        return convertMultiAsync(input, output, engine, minEnergy, 0, "pitch", 20);
    }

    public CompletableFuture<Path> convertMultiAsync(Path input, Path output,
                                                     String engine, double minEnergy, int lanes) {
        return convertMultiAsync(input, output, engine, minEnergy, lanes, "pitch", 20);
    }

    public CompletableFuture<Path> convertMultiAsync(Path input, Path output,
                                                     String engine, double minEnergy,
                                                     int lanes, String split) {
        return convertMultiAsync(input, output, engine, minEnergy, lanes, split, 20);
    }

    public CompletableFuture<Path> convertMultiAsync(Path input, Path output,
                                                     String engine, double minEnergy,
                                                     int lanes, String split, int polyCap) {
        return CompletableFuture.supplyAsync(() -> {
            Path jobDirectory;
            try {
                jobDirectory = Files.createTempDirectory(workDirectory, "multi-");
            } catch (IOException e) {
                throw new IllegalStateException("无法创建多音轨临时目录", e);
            }
            try {
                Path stems = jobDirectory.resolve("stems");
                Files.createDirectories(stems);
                // 输入音频统一过 FFmpeg 规整，再交给分离引擎
                Path wav = normalize(input, jobDirectory, "input");
                double seconds = wavSeconds(wav);
                boolean ai = "ai".equalsIgnoreCase(engine);
                if (ai) {
                    Path demucs = workDirectory.resolve("demucs_mt.cpp.main.exe");
                    Path model = workDirectory.resolve("ggml-model-htdemucs-4s-f16.bin");
                    if (!Files.isRegularFile(demucs) || !Files.isRegularFile(model)) {
                        throw new IllegalStateException("多音轨资源缺失：需要 demucs_mt.cpp.main.exe 和 htdemucs 模型");
                    }
                    run(jobDirectory, List.of(demucs.toString(), model.toString(), wav.toString(), stems.toString(), "8"));
                } else {
                    Path trad = workDirectory.resolve("traditional_sep.exe");
                    if (!Files.isRegularFile(trad)) {
                        throw new IllegalStateException("多音轨资源缺失：需要 traditional_sep.exe");
                    }
                    run(jobDirectory, List.of(trad.toString(), wav.toString(), stems.toString()));
                }

                // 收集分离产物（target_*.wav），按能量占比丢弃弱轨
                List<Path> stemWavs = new java.util.ArrayList<>();
                try (var files = Files.list(stems)) {
                    files.filter(p -> {
                        String n = p.getFileName().toString();
                        return n.startsWith("target_") && n.endsWith(".wav");
                    }).sorted().forEach(stemWavs::add);
                }
                double totalEnergy = 0;
                for (Path s : stemWavs) totalEnergy += energyOfWav(s);
                List<Path> kept = new java.util.ArrayList<>();
                for (Path s : stemWavs) {
                    double ratio = totalEnergy > 0 ? energyOfWav(s) / totalEnergy : 1.0;
                    if (ratio < minEnergy) {
                        RedStoneMusic.LOGGER.info("丢弃低能量音轨 {}（{:.1%} < {:.1%}）", s.getFileName(), ratio, minEnergy);
                        continue;
                    }
                    kept.add(s);
                }
                if (kept.isEmpty()) throw new IllegalStateException("所有音轨能量过低，无法生成结构");

                // 逐轨转写 MIDI
                List<Path> midis = new java.util.ArrayList<>();
                for (Path stem : kept) {
                    Path stemWav = normalize(stem, jobDirectory, "stem");
                    Path midi = jobDirectory.resolve("stem_" + midis.size() + ".mid");
                    run(jobDirectory, List.of(workDirectory.resolve("audio_to_midi.exe").toString(),
                            stemWav.toString(), midi.toString()));
                    midis.add(midi);
                }
                // 合并 MIDI → 单轨三档生成
                Path mergedMidi = jobDirectory.resolve("merged.mid");
                List<String> mergeCmd = new java.util.ArrayList<>();
                mergeCmd.add(workDirectory.resolve("merge_midis.exe").toString());
                midis.forEach(m -> mergeCmd.add(m.toString()));
                mergeCmd.add(mergedMidi.toString());
                run(jobDirectory, mergeCmd);
                String splitMode = "bank".equalsIgnoreCase(split) ? "bank" : "pitch";
                int cap = Math.max(1, Math.min(128, polyCap));
                if (lanes > 0) {
                    run(jobDirectory, List.of(workDirectory.resolve("arrange_multilane.exe").toString(),
                            "--midi", mergedMidi.toString(), "--out", output.toString(),
                            "--lanes", Integer.toString(lanes), "--split", splitMode,
                            "--resolution", "2", "--poly-cap", Integer.toString(cap), "--lane-gap", "2"));
                } else {
                    run(jobDirectory, List.of(workDirectory.resolve("midi_to_nbt.exe").toString(),
                            mergedMidi.toString(), output.toString(),
                            "--seconds", String.format(Locale.ROOT, "%.6f", seconds),
                            "--resolution", "2", "--layout", "simple"));
                }
                RedStoneMusic.LOGGER.info("多音轨转换完成: {}（引擎={}，保留 {} 轨，丢弃 {} 轨）",
                        output, engine, kept.size(), stemWavs.size() - kept.size());
                return output;
            } catch (IOException e) {
                throw new IllegalStateException("多音轨转换失败", e);
            } finally {
                deleteTree(jobDirectory);
            }
        }, executor);
    }

    // 读取 WAV（16bit PCM / float32）计算左声道能量（用于音轨能量门限）
    private static double energyOfWav(Path wav) {
        try {
            byte[] buf = Files.readAllBytes(wav);
            if (buf.length < 44 || buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'F' || buf[3] != 'F') return 0;
            int fmt = (buf[20] & 0xff) | ((buf[21] & 0xff) << 8);
            int ch = (buf[22] & 0xff) | ((buf[23] & 0xff) << 8);
            int bits = (buf[34] & 0xff) | ((buf[35] & 0xff) << 8);
            int pos = 12;
            int dataPos = -1;
            while (pos + 8 <= buf.length) {
                if (buf[pos] == 'd' && buf[pos + 1] == 'a' && buf[pos + 2] == 't' && buf[pos + 3] == 'a') {
                    dataPos = pos + 8;
                    break;
                }
                int sz = (buf[pos + 4] & 0xff) | ((buf[pos + 5] & 0xff) << 8)
                        | ((buf[pos + 6] & 0xff) << 16) | ((buf[pos + 7] & 0xff) << 24);
                pos += 8 + sz + (sz & 1);
            }
            if (dataPos < 0) return 0;
            double e = 0;
            if (bits == 32 && fmt == 3) {
                for (int i = 0; dataPos + i * 4 + 4 <= buf.length; i += ch) {
                    int bits32 = (buf[dataPos + i * 4] & 0xff) | ((buf[dataPos + i * 4 + 1] & 0xff) << 8)
                            | ((buf[dataPos + i * 4 + 2] & 0xff) << 16) | ((buf[dataPos + i * 4 + 3] & 0xff) << 24);
                    float v = Float.intBitsToFloat(bits32);
                    e += v * v;
                }
            } else if (bits == 16) {
                for (int i = 0; dataPos + i * 2 + 2 <= buf.length; i += ch) {
                    short v = (short) ((buf[dataPos + i * 2] & 0xff) | ((buf[dataPos + i * 2 + 1] & 0xff) << 8));
                    double d = v / 32768.0;
                    e += d * d;
                }
            }
            return e;
        } catch (IOException e) {
            RedStoneMusic.LOGGER.warn("读取音轨能量失败: {}", wav, e);
            return 0;
        }
    }

    private static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    // 统一把任意音频规整为 44100Hz 立体声 16-bit PCM WAV，保证下游工具读取格式一致
    private Path normalize(Path input, Path directory, String name) {
        Path output = directory.resolve(name + ".norm.wav");
        run(directory, List.of(workDirectory.resolve("ffmpeg.exe").toString(),
                "-y", "-hide_banner", "-loglevel", "error",
                "-i", input.toString(),
                "-ar", "44100", "-ac", "2", "-c:a", "pcm_s16le",
                output.toString()));
        return output;
    }

    private double wavSeconds(Path path) {
        try (AudioInputStream stream = AudioSystem.getAudioInputStream(path.toFile())) {
            return stream.getFrameLength() / stream.getFormat().getFrameRate();
        } catch (Exception e) {
            throw new IllegalStateException("无法读取音频时长: " + path, e);
        }
    }

    private void run(Path jobDirectory, List<String> command) {
        try {
            ProcessBuilder builder = new ProcessBuilder(command);
            builder.directory(jobDirectory.toFile());
            builder.redirectErrorStream(true);
            String existingPath = System.getenv("PATH");
            String path = workDirectory + java.io.File.pathSeparator + (existingPath == null ? "" : existingPath);
            builder.environment().put("PATH", path);
            Process process = builder.start();
            String output;
            try (InputStream stream = process.getInputStream()) {
                output = new String(stream.readAllBytes(), StandardCharsets.UTF_8);
            }
            int exit = process.waitFor();
            if (!output.isBlank()) {
                RedStoneMusic.LOGGER.info("工具输出: {}", output.replace('\n', ' ').replace('\r', ' '));
            }
            if (exit != 0) {
                throw new IllegalStateException("外部工具退出码 " + exit + ": " + output);
            }
        } catch (IOException e) {
            throw new IllegalStateException("无法启动外部工具: " + command.get(0), e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("外部工具被中断", e);
        }
    }

    private void release(String resource) throws IOException {
        try (InputStream stream = MusicToolRunner.class.getResourceAsStream(resource)) {
            if (stream == null) {
                throw new IOException("模组资源缺失: " + resource);
            }
            Path destination = workDirectory.resolve(Path.of(resource).getFileName().toString());
            Files.copy(stream, destination, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private void deleteTree(Path directory) {
        try (var files = Files.walk(directory)) {
            files.sorted(java.util.Comparator.reverseOrder()).forEach(path -> {
                try {
                    Files.deleteIfExists(path);
                } catch (IOException e) {
                    RedStoneMusic.LOGGER.debug("清理临时文件失败: {}", path, e);
                }
            });
        } catch (IOException e) {
            RedStoneMusic.LOGGER.debug("清理临时目录失败: {}", directory, e);
        }
    }

    @Override
    public void close() {
        executor.shutdownNow();
        deleteTree(workDirectory);
    }
}
