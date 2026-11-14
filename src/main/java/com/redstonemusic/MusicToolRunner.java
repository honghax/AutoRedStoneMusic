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
import java.util.function.Consumer;

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
    private static final String FUSE_EXE = "/redstonemusic/tools/fuse_midis.exe";
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
        release(FUSE_EXE);
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
                            midi.toString(), output.toString(), "--resolution", "2",
                            "--layout", "simple", "--poly-cap", "13"));
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
                    // --roles：只输出 drums/bass/harmonic 三条，
                    // 避免同一段旋律被 vocals/guitar/piano 各转写一次产生八度重复
                    run(jobDirectory, List.of(trad.toString(), wav.toString(), stems.toString(), "--roles"));
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
                Path mergedMidi = jobDirectory.resolve("merged.mid");
                if (!ai) {
                    // 传统引擎：全混音转写一次（召回）+ harmonic/bass 各转写一次（精度），
                    // 再做音符级交集融合，避免"分轨转写把分离误差放大成伪音符"
                    Path mixMidi = jobDirectory.resolve("mix.mid");
                    run(jobDirectory, List.of(workDirectory.resolve("audio_to_midi.exe").toString(),
                            wav.toString(), mixMidi.toString()));
                    List<Path> pool = new java.util.ArrayList<>();
                    for (Path stem : kept) {
                        String name = stem.getFileName().toString();
                        if (!name.contains("harmonic") && !name.contains("bass")) continue;
                        Path midi = jobDirectory.resolve("pool_" + pool.size() + ".mid");
                        run(jobDirectory, List.of(workDirectory.resolve("audio_to_midi.exe").toString(),
                                stem.toString(), midi.toString()));
                        pool.add(midi);
                    }
                    if (pool.isEmpty()) {
                        // 没有可用的 harmonic/bass 轨时退回直接使用混音转写
                        Files.copy(mixMidi, mergedMidi, StandardCopyOption.REPLACE_EXISTING);
                    } else {
                        List<String> fuseCmd = new java.util.ArrayList<>();
                        fuseCmd.add(workDirectory.resolve("fuse_midis.exe").toString());
                        fuseCmd.add("--primary");
                        fuseCmd.add(mixMidi.toString());
                        for (Path p : pool) {
                            fuseCmd.add("--pool");
                            fuseCmd.add(p.toString());
                        }
                        fuseCmd.add("--out");
                        fuseCmd.add(mergedMidi.toString());
                        fuseCmd.add("--tol");
                        fuseCmd.add("0.12");
                        fuseCmd.add("--semis");
                        fuseCmd.add("0");
                        run(jobDirectory, fuseCmd);
                    }
                } else {
                    List<Path> midis = new java.util.ArrayList<>();
                    for (Path stem : kept) {
                        Path stemWav = normalize(stem, jobDirectory, "stem");
                        Path midi = jobDirectory.resolve("stem_" + midis.size() + ".mid");
                        run(jobDirectory, List.of(workDirectory.resolve("audio_to_midi.exe").toString(),
                                stemWav.toString(), midi.toString()));
                        midis.add(midi);
                    }
                    List<String> mergeCmd = new java.util.ArrayList<>();
                    mergeCmd.add(workDirectory.resolve("merge_midis.exe").toString());
                    midis.forEach(m -> mergeCmd.add(m.toString()));
                    mergeCmd.add(mergedMidi.toString());
                    run(jobDirectory, mergeCmd);
                }
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
                            "--resolution", "2", "--layout", "simple", "--poly-cap", "13"));
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

    /**
     * 在线转换：MuScriptor（多乐器转写）→ MIDI → 按乐器拆分的多轨 NBT。
     *
     * <p>该链路只使用固定参数（instrument 分轨、resolution 2、lane-gap 2），
     * 不额外接收命令参数；轨道数由转写结果中的乐器数决定。</p>
     */
    public CompletableFuture<Path> convertOnlineAsync(Path input, Path output,
                                                      Consumer<String> progress) {
        return CompletableFuture.supplyAsync(() -> {
            Path jobDirectory;
            try {
                jobDirectory = Files.createTempDirectory(workDirectory, "online-");
            } catch (IOException e) {
                throw new IllegalStateException("无法创建在线转换临时目录", e);
            }
            try {
                Path midi = jobDirectory.resolve("online.mid");
                new MuscriptorClient().transcribe(input, midi, progress);
                // 在线模型会在开头补静音/补齐小节，这里把首个音符对齐到原曲实际起音
                double lead = detectOnsetSeconds(input, jobDirectory,
                    workDirectory.resolve("ffmpeg.exe"));
                if (lead > 0.0) {
                    progress.accept(String.format(Locale.ROOT,
                        "对齐起始位置：首个音符 %.2fs", lead));
                }
                run(jobDirectory, List.of(workDirectory.resolve("arrange_multilane.exe").toString(),
                        "--midi", midi.toString(), "--out", output.toString(),
                        "--split", "instrument", "--lanes", "1", "--resolution", "2",
                        "--poly-cap", "128", "--lane-gap", "2",
                        "--align-lead", String.format(Locale.ROOT, "%.3f", lead)));
                RedStoneMusic.LOGGER.info("在线多乐器转换完成: {}", output);
                return output;
            } catch (IOException e) {
                throw new IllegalStateException("在线转换失败", e);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("在线转换被中断", e);
            } finally {
                deleteTree(jobDirectory);
            }
        }, executor);
    }

    /**
     * 探测音频的首个有效起音时刻（秒）：解码为 22050Hz 单声道，按 25ms 帧算 RMS。
     * 先用 0.08（约 -22 dBFS）定位真正的起音，找不到时退回 0.012 的弱门限，
     * 避免淡入前奏被当成起音、也避免整体静音的素材得到 0。失败时返回 0。
     */
    static double detectOnsetSeconds(Path input, Path jobDirectory, Path ffmpeg) {
        final int sampleRate = 22050;
        final int hop = sampleRate * 25 / 1000;   // 551 采样 = 25ms
        try {
            Path mono = jobDirectory.resolve("onset.wav");
            ProcessBuilder builder = new ProcessBuilder(List.of(ffmpeg.toString(),
                    "-y", "-v", "error", "-i", input.toString(),
                    "-ac", "1", "-ar", Integer.toString(sampleRate),
                    "-c:a", "pcm_s16le", mono.toString()));
            builder.directory(jobDirectory.toFile());
            builder.redirectErrorStream(true);
            Process process = builder.start();
            process.getInputStream().readAllBytes();
            if (process.waitFor() != 0) return 0.0;

            byte[] buf = Files.readAllBytes(mono);
            int dataPos = pcmDataOffset(buf);
            if (dataPos < 0) return 0.0;
            int frame = firstFrameAbove(buf, dataPos, hop, 0.08);
            if (frame < 0) frame = firstFrameAbove(buf, dataPos, hop, 0.012);
            return frame < 0 ? 0.0 : (double) frame * hop / sampleRate;
        } catch (Exception e) {
            RedStoneMusic.LOGGER.warn("起音探测失败，按 0 处理: {}", input, e);
        }
        return 0.0;
    }

    // 返回第一个 RMS 越过阈值的 25ms 帧序号，找不到返回 -1
    private static int firstFrameAbove(byte[] buf, int dataPos, int hop, double threshold) {
        int frame = 0;
        for (int start = dataPos; start + hop * 2 <= buf.length; start += hop * 2, frame++) {
            double energy = 0;
            for (int i = 0; i < hop; i++) {
                short v = (short) ((buf[start + i * 2] & 0xff) | ((buf[start + i * 2 + 1] & 0xff) << 8));
                double d = v / 32768.0;
                energy += d * d;
            }
            if (Math.sqrt(energy / hop) >= threshold) return frame;
        }
        return -1;
    }

    // 定位 WAV 的 data 块起点（跳过 fmt 等其它块）
    private static int pcmDataOffset(byte[] buf) {
        if (buf.length < 44 || buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'F' || buf[3] != 'F') return -1;
        int pos = 12;
        while (pos + 8 <= buf.length) {
            if (buf[pos] == 'd' && buf[pos + 1] == 'a' && buf[pos + 2] == 't' && buf[pos + 3] == 'a') {
                return pos + 8;
            }
            int size = (buf[pos + 4] & 0xff) | ((buf[pos + 5] & 0xff) << 8)
                    | ((buf[pos + 6] & 0xff) << 16) | ((buf[pos + 7] & 0xff) << 24);
            pos += 8 + size + (size & 1);
        }
        return -1;
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
