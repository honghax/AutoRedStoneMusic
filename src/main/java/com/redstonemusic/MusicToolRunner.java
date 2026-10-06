package com.redstonemusic;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
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
        releaseOptional(FFMPEG);
        Runtime.getRuntime().addShutdownHook(new Thread(this::close, "RedStoneMusic-tool-cleanup"));
    }

    public Path convert(Path input, Path output) {
        return convertAsync(input, output).join();
    }

    public CompletableFuture<Path> convertAsync(Path input, Path output) {
        return CompletableFuture.supplyAsync(() -> {
            Path jobDirectory;
            try {
                jobDirectory = Files.createTempDirectory(workDirectory, "job-");
            } catch (IOException e) {
                throw new IllegalStateException("无法创建转换临时目录", e);
            }
            try {
                Path midi = jobDirectory.resolve(input.getFileName() + ".mid");
                run(jobDirectory, List.of(workDirectory.resolve("audio_to_midi.exe").toString(),
                        input.toString(), midi.toString()));
                run(jobDirectory, List.of(workDirectory.resolve("midi_to_nbt.exe").toString(),
                        midi.toString(), output.toString(), "--resolution", "2", "--layout", "simple"));
                return output;
            } finally {
                deleteTree(jobDirectory);
            }
        }, executor);
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

    private void releaseOptional(String resource) throws IOException {
        try (InputStream stream = MusicToolRunner.class.getResourceAsStream(resource)) {
            if (stream == null) {
                RedStoneMusic.LOGGER.warn("未找到可选资源 {}，将使用系统 PATH 中的 ffmpeg", resource);
                return;
            }
            Files.copy(stream, workDirectory.resolve("ffmpeg.exe"), StandardCopyOption.REPLACE_EXISTING);
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
