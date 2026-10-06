# -*- coding: utf-8 -*-
"""MP3 → MIDI 完整推理端（Spotify basic-pitch，ONNX 后端，纯 CPU）。

链路：
  MP3 ──ffmpeg──► 单声道 22050Hz PCM ──basic-pitch(nmp.onnx)──► MIDI

说明：
  - 模型就是官方 basicpitch.spotify.com 网页版用的同一个 ICASSP 2022 模型
    （basic_pitch 包内自带的 nmp.onnx，仅 230KB），本地与网页版输出一致。
  - 直接写出 predict() 返回的 PrettyMIDI 对象，因此**保留弯音(pitch bend)**；
    若自己拆成 (start,end,pitch,velocity) 重建音符会丢掉弯音。
  - 用 ffmpeg 解码而不是直接喂给 librosa：libsndfile 对部分 MP3（大文件/带
    ID3）会报 "Unspecified internal error"。

用法：
  python pipeline.py <input.mp3> [output.mid]
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

import numpy as np
import pretty_midi
import soundfile as sf

from basic_pitch import ICASSP_2022_MODEL_PATH
from basic_pitch.inference import predict

TARGET_SR = 22050          # basic-pitch 的工作采样率


def decode_to_wav(src, sr=TARGET_SR):
    """用 ffmpeg 解码为单声道 PCM，返回 (wav路径, 临时目录对象)"""
    tmpdir = tempfile.TemporaryDirectory()
    wav = pathlib.Path(tmpdir.name) / "decoded.wav"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(src),
                    "-ar", str(sr), "-ac", "1", "-c:a", "pcm_f32le", str(wav)],
                   check=True)
    return wav, tmpdir


def polyphony(midi):
    """返回 (最大同时发声, 平均同时发声)"""
    notes = [(n.start, n.end) for i in midi.instruments for n in i.notes]
    if not notes:
        return 0, 0.0
    ev = []
    for s, e in notes:
        ev.append((s, 1))
        ev.append((e, -1))
    ev.sort()
    cur = mx = 0
    area = 0.0
    last = ev[0][0]
    for t, d in ev:
        if cur > 0:
            area += (t - last) * cur
        cur += d
        mx = max(mx, cur)
        last = t
    dur = max(e for _, e in notes) - min(s for s, _ in notes)
    return mx, area / max(dur, 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output", nargs="?", help="输出 MIDI 路径（默认与输入同名的 .mid）")
    args = ap.parse_args()

    src = pathlib.Path(args.input)
    if not src.exists():
        raise SystemExit(f"输入不存在: {src}")
    out = pathlib.Path(args.output) if args.output else src.with_suffix(".mid")

    print(f"[1/2] 解码 {src.name}")
    wav, _tmp = decode_to_wav(src)
    y, sr = sf.read(str(wav), dtype="float32")
    print(f"      {len(y)} 采样 @ {sr}Hz ({len(y) / sr:.2f}s)")

    print("[2/2] basic-pitch 转写（ONNX / CPU）")
    _, midi, _ = predict(str(wav), ICASSP_2022_MODEL_PATH)
    midi.write(str(out))

    notes = [n for i in midi.instruments for n in i.notes]
    # pretty_midi 1.x 把弯音挂在 Instrument 上（旧版才在 Note 上）
    bends = sum(len(i.pitch_bends) for i in midi.instruments)
    mx, avg = polyphony(midi)
    if notes:
        print(f"      音符={len(notes)}  最大同时={mx}  平均同时={avg:.2f}  "
              f"音域={min(n.pitch for n in notes)}-{max(n.pitch for n in notes)}  "
              f"弯音点={bends}")
    else:
        print("      未检测到音符")
    print(f"已写出: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())