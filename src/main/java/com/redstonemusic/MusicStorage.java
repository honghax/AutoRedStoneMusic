package com.redstonemusic;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.FileTime;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.stream.Stream;

public final class MusicStorage {
    private final Path root;
    private final Path mp3;
    private final Path ntb;

    public MusicStorage(Path configDirectory) throws IOException {
        root = configDirectory.resolve("RedStoneMusic").toAbsolutePath().normalize();
        mp3 = root.resolve("mp3");
        ntb = root.resolve("ntb");
        Files.createDirectories(mp3);
        Files.createDirectories(ntb);
    }

    public Path root() { return root; }
    public Path mp3Directory() { return mp3; }
    public Path ntbDirectory() { return ntb; }

    public Path resolveMp3(String name) throws IOException {
        return resolveFile(mp3, name, ".mp3", ".wav");
    }

    public Path resolveNtb(String name) throws IOException {
        return resolveFile(ntb, name, ".ntb", ".nbt");
    }

    public List<FileEntry> list(String type) throws IOException {
        Path directory = switch (type.toLowerCase(Locale.ROOT)) {
            case "mp3" -> mp3;
            case "ntb" -> ntb;
            default -> throw new IOException("仅支持 mp3 或 ntb");
        };
        try (Stream<Path> files = Files.list(directory)) {
            return files.filter(Files::isRegularFile)
                .sorted(Comparator.comparing(path -> path.getFileName().toString().toLowerCase(Locale.ROOT)))
                .map(path -> {
                    try {
                        return new FileEntry(path.getFileName().toString(), Files.size(path), Files.getLastModifiedTime(path));
                    } catch (IOException e) {
                        throw new StorageException(e);
                    }
                }).toList();
        } catch (StorageException e) {
            throw e;
        }
    }

    private static Path resolveFile(Path directory, String input, String... extensions) throws IOException {
        Path candidate = Path.of(input);
        if (candidate.getNameCount() != 1 || input.contains("/") || input.contains("\\") || input.equals(".") || input.equals("..")) {
            throw new IOException("文件名必须是单个文件名，不能包含路径");
        }
        String lower = input.toLowerCase(Locale.ROOT);
        boolean valid = false;
        for (String extension : extensions) {
            if (lower.endsWith(extension)) {
                valid = true;
                break;
            }
        }
        if (!valid) {
            throw new IOException("文件格式不支持: " + input);
        }
        Path result = directory.resolve(input).normalize();
        if (!result.getParent().equals(directory)) {
            throw new IOException("非法文件路径");
        }
        return result;
    }

    public record FileEntry(String name, long size, FileTime modified) {
        public String modifiedText() {
            return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss").format(new Date(modified.toMillis()));
        }
    }

    public static final class StorageException extends RuntimeException {
        public StorageException(Throwable cause) { super(cause); }
    }
}
