package com.redstonemusic;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Base64;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.function.Consumer;

/**
 * MuScriptor 在线转写客户端。
 *
 * <p>访问官网网页版使用的内部接口 POST /transcribe（SSE 流）。它不是官方公开 API，
 * 只在玩家显式执行 online 命令（即表示同意上传音频）时调用；单任务、失败重试有限。</p>
 *
 * <p>事件流：progress 事件报告分片进度；transcription_complete 事件携带 base64 MIDI。</p>
 */
public final class MuscriptorClient {
    private static final String DEFAULT_ENDPOINT = "https://muscriptor.kyutai.org/transcribe";
    private static final int MAX_ATTEMPTS = 12;
    private static final long RETRY_DELAY_MS = 5000L;

    /**
     * 乐器提示词表：全部落在音符盒已有的音色家族上（弦乐/贝斯/竖琴/色彩打击/管乐/
     * 铜管/齐奏/合成主音/鼓）。仅作为可选选项使用，民族乐器等素材不指定更稳妥。
     */
    public static final List<String> INSTRUMENT_HINTS = List.of(
        "orchestral_harp", "electric_bass", "chromatic_percussion", "flutes",
        "acoustic_guitar", "brass_section", "orchestra_hit", "synth_lead", "drums");

    private final HttpClient http = HttpClient.newBuilder()
        .connectTimeout(Duration.ofSeconds(30))
        .followRedirects(HttpClient.Redirect.NORMAL)
        .build();

    /** 上传音频并等待在线转写完成的 MIDI（不指定乐器，由模型自行判断）。 */
    public Path transcribe(Path audio, Path midiOut, Consumer<String> progress)
            throws IOException, InterruptedException {
        return transcribe(audio, midiOut, progress, List.of());
    }

    /** 上传音频并等待在线转写完成的 MIDI；instruments 非空时按提示乐器做条件化分离。 */
    public Path transcribe(Path audio, Path midiOut, Consumer<String> progress,
                           List<String> instruments)
            throws IOException, InterruptedException {
        String endpoint = System.getProperty("redstonemusic.muscriptor.endpoint", DEFAULT_ENDPOINT);
        String boundary = "----RedStoneMusic" + UUID.randomUUID().toString().replace("-", "");
        byte[] body = multipart(audio, boundary, instruments);
        String clientId = UUID.randomUUID().toString();
        if (!instruments.isEmpty()) {
            progress.accept("已按乐器提示上传：" + String.join(", ", instruments));
        }

        for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
            progress.accept("在线转写尝试 " + attempt + "/" + MAX_ATTEMPTS + "...");
            HttpRequest request = HttpRequest.newBuilder(URI.create(endpoint))
                .timeout(Duration.ofMinutes(10))
                .header("Content-Type", "multipart/form-data; boundary=" + boundary)
                .header("Origin", "https://muscriptor.kyutai.org")
                .header("Referer", "https://muscriptor.kyutai.org/")
                .header("X-Client-Id", clientId)
                .POST(HttpRequest.BodyPublishers.ofByteArray(body))
                .build();

            HttpResponse<InputStream> response =
                http.send(request, HttpResponse.BodyHandlers.ofInputStream());
            int status = response.statusCode();
            if (status == 503) {
                closeQuietly(response.body());
                progress.accept("服务端忙，5 秒后重试（" + attempt + "/" + MAX_ATTEMPTS + "）");
                Thread.sleep(RETRY_DELAY_MS);
                continue;
            }
            if (status != 200) {
                String text = new String(response.body().readAllBytes(), StandardCharsets.UTF_8);
                throw new IOException("MuScriptor 返回 HTTP " + status + ": " + snippet(text));
            }
            return readEvents(response.body(), midiOut, progress);
        }
        throw new IOException("MuScriptor 连续 " + MAX_ATTEMPTS + " 次 503，请稍后再试");
    }

    private Path readEvents(InputStream stream, Path midiOut, Consumer<String> progress)
            throws IOException {
        try (BufferedReader reader =
                 new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.startsWith("data: ")) continue;
                JsonObject event;
                try {
                    event = JsonParser.parseString(line.substring(6)).getAsJsonObject();
                } catch (Exception e) {
                    RedStoneMusic.LOGGER.debug("忽略无法解析的 SSE 事件: {}", line, e);
                    continue;
                }
                String type = stringField(event, "type");
                if ("progress".equals(type)) {
                    progress.accept("在线转写进度 " + intField(event, "completed")
                        + "/" + intField(event, "total"));
                } else if ("transcription_complete".equals(type)) {
                    byte[] midi = Base64.getDecoder().decode(stringField(event, "data"));
                    Files.createDirectories(midiOut.toAbsolutePath().getParent());
                    Files.write(midiOut, midi);
                    progress.accept("在线转写完成，MIDI " + midi.length + " bytes");
                    return midiOut;
                }
            }
        }
        throw new IOException("MuScriptor 事件流结束但没有返回 MIDI");
    }

    private static byte[] multipart(Path audio, String boundary, List<String> instruments)
            throws IOException {
        String name = audio.getFileName().toString();
        byte[] file = Files.readAllBytes(audio);
        ByteArrayOutputStream out = new ByteArrayOutputStream(file.length + 512);
        // 乐器提示是重复的同名字段，必须在文件之前；为空时保持原样的纯文件上传
        for (String instrument : instruments) {
            String field = "--" + boundary + "\r\n"
                + "Content-Disposition: form-data; name=\"instruments\"\r\n\r\n"
                + instrument + "\r\n";
            out.write(field.getBytes(StandardCharsets.UTF_8));
        }
        String head = "--" + boundary + "\r\n"
            + "Content-Disposition: form-data; name=\"file\"; filename=\"" + name + "\"\r\n"
            + "Content-Type: " + mimeFor(name) + "\r\n\r\n";
        out.write(head.getBytes(StandardCharsets.UTF_8));
        out.write(file);
        String tail = "\r\n--" + boundary + "--\r\n";
        out.write(tail.getBytes(StandardCharsets.UTF_8));
        return out.toByteArray();
    }

    private static String mimeFor(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        if (lower.endsWith(".mp3")) return "audio/mpeg";
        if (lower.endsWith(".wav")) return "audio/wav";
        if (lower.endsWith(".flac")) return "audio/flac";
        if (lower.endsWith(".ogg")) return "audio/ogg";
        if (lower.endsWith(".m4a")) return "audio/mp4";
        if (lower.endsWith(".opus")) return "audio/opus";
        return "application/octet-stream";
    }

    private static String stringField(JsonObject object, String key) {
        return object.has(key) && !object.get(key).isJsonNull()
            ? object.get(key).getAsString() : "";
    }

    private static int intField(JsonObject object, String key) {
        try {
            return object.get(key).getAsInt();
        } catch (Exception e) {
            return 0;
        }
    }

    private static String snippet(String text) {
        String flat = text.replace('\n', ' ').replace('\r', ' ');
        return flat.length() > 200 ? flat.substring(0, 200) : flat;
    }

    private static void closeQuietly(InputStream stream) {
        try {
            stream.close();
        } catch (IOException ignored) {
        }
    }
}
