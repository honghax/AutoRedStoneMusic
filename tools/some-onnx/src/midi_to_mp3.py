# -*- coding: utf-8 -*-
"""把 MIDI 渲染成音频（WAV/MP3），用于试听 SOME 提取出的音符。

环境里没有 fluidsynth / 音色库，所以这里用谐波叠加 + 包络自己做一个小合成器，
对单声部旋律来说足够听清音高走势。

用法：
  python midi_to_mp3.py <input.mid> [output.mp3]
"""
import pathlib
import subprocess
import sys

import mido
import numpy as np
import soundfile as sf

SR = 44100
HARMONICS = [(1, 1.0), (2, 0.45), (3, 0.22), (4, 0.12), (5, 0.06)]


def midi_to_notes(path):
    """解析 MIDI，返回 [(音高, 起始秒, 时长秒), ...]

    遍历所有轨道：pretty_midi 等写出的是 format 1（第 1 轨只有速度信息，
    音符在后续轨）；我们自己写的单轨文件也能正常处理。
    """
    mf = mido.MidiFile(str(path))
    ticks_per_beat = mf.ticks_per_beat

    notes = []
    for track in mf.tracks:
        tempo = 500000  # 默认 120bpm，每轨各自维护
        now = 0.0
        pending = {}
        for msg in track:
            now += mido.tick2second(msg.time, ticks_per_beat, tempo)
            if msg.type == "set_tempo":
                tempo = msg.tempo
            elif msg.type == "note_on" and msg.velocity > 0:
                pending[msg.note] = now
            elif msg.type in ("note_off", "note_on"):
                if msg.note in pending:
                    start = pending.pop(msg.note)
                    if now > start:
                        notes.append((msg.note, start, now - start))
    notes.sort(key=lambda x: x[1])
    return notes


def render(notes, total_sec):
    buf = np.zeros(int((total_sec + 1.0) * SR), dtype=np.float64)
    for note, start, dur in notes:
        freq = 440.0 * 2 ** ((note - 69) / 12)
        n = int(dur * SR)
        if n <= 0:
            continue
        t = np.arange(n) / SR
        sig = np.zeros(n)
        for k, amp in HARMONICS:
            sig += amp * np.sin(2 * np.pi * freq * k * t)

        # 包络：15ms 起音 + 轻微衰减 + 60ms 收尾
        env = np.exp(-t * 0.8)
        a, r = min(int(0.015 * SR), n), min(int(0.06 * SR), n)
        env[:a] *= np.linspace(0.0, 1.0, a)
        env[-r:] *= np.linspace(1.0, 0.0, r)

        i = int(start * SR)
        seg = sig * env
        buf[i:i + n] += seg[:len(buf) - i]
    peak = np.max(np.abs(buf))
    if peak > 0:
        buf *= 0.9 / peak
    return buf.astype(np.float32)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    mid_path = pathlib.Path(sys.argv[1])
    mp3_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else mid_path.with_suffix(".mp3")
    wav_path = mp3_path.with_suffix(".wav")

    notes = midi_to_notes(mid_path)
    if not notes:
        raise SystemExit("MIDI 里没有音符")
    total = max(s + d for _, s, d in notes)
    print(f"音符数: {len(notes)}  总时长: {total:.2f}s")
    print(f"音高范围: {min(n for n, _, _ in notes)} ~ {max(n for n, _, _ in notes)}")

    audio = render(notes, total)
    sf.write(str(wav_path), audio, SR)
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(wav_path),
                    "-b:a", "192k", str(mp3_path)], check=True)
    wav_path.unlink()  # 中间 WAV 不保留
    print(f"已生成: {mp3_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())