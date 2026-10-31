# -*- coding: utf-8 -*-
"""量化「MIDI → NBT（红石音符盒结构）」的转录损失。

用法:
    python eval_ntb_loss.py --nbt out.ntb [--midi ref.mid] [--audio src.mp3]
                            [--resolution 2] [--json report.json]

原理:
    1) 反向解码 .nbt：定位 observer → 反推脉冲源 → 用 gen_machine.simulate 求出每个
       音符盒的触发刻数；instrument/note 属性 → 绝对 MIDI 音高。
    2) --midi（生成该 NBT 的输入 MIDI）存在时，逐音符对比，量化五类损失：
       时序量化 / 音高移调与八度折回 / 同列去重 / 超时长丢弃 / 力度·时值·音色缺失。
    3) --audio 存在时，用全局 chroma 与逐音符音级命中率，量化端到端谐波保真度。

依赖: numpy, scipy, mido, nbtlib（与生成器一致），ffmpeg 在 PATH 中可用。
"""
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict

import numpy as np
import scipy.io.wavfile as wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_machine as g

BANK_BASE = {'bass': 30, 'harp': 54, 'bell': 78, 'basedrum': 54}
AUDIO_SR = 22050
N_FFT, HOP = 2048, 512


def decode_nbt(path):
    """从 .nbt 反解出音符事件：[(触发秒, 绝对MIDI音高, 乐器), ...]。"""
    blocks, size = g.load_blocks_from_nbt(path)
    observers = [(p, v) for p, v in blocks.items() if v[0] == 'minecraft:observer']
    if not observers:
        raise SystemExit('NBT 中找不到 observer，无法定位脉冲源')
    pos, props = observers[0]
    dx, dz = g.DIRS[props[1]['facing']]
    src = (pos[0] - dx, pos[1], pos[2] - dz)
    # 平移回 gen_machine.simulate 约定的脉冲源 (-2, 1, 0)
    shift = (src[0] + 2, src[1] - 1, src[2])
    shifted = {(x - shift[0], y - shift[1], z - shift[2]): v for (x, y, z), v in blocks.items()}
    dist = g.simulate(shifted)
    events, unreachable = [], 0
    for p, v in shifted.items():
        if v[0] != 'minecraft:note_block':
            continue
        tick = dist.get(p)
        if tick is None:
            unreachable += 1
            continue
        inst = v[1]['instrument']
        events.append((tick / 20.0, BANK_BASE[inst] + int(v[1]['note']), inst))
    events.sort()
    lamp = [p for p, v in shifted.items() if v[0] == 'minecraft:redstone_lamp']
    return {
        'events': events,
        'size': tuple(int(s) for s in size),
        'blocks': len(blocks),
        'unreachable': unreachable,
        'lamp_tick': dist.get(lamp[0]) if lamp else None,
    }


def midi_metrics(path, decoded, resolution):
    """逐音符对比参考 MIDI 与 NBT 内容。"""
    notes = g.load_notes(path)
    if not notes:
        raise SystemExit('参考 MIDI 中没有音符')
    pitches = [p for p, _, _ in notes]
    conv, median, shift = g.build_pitch_map(pitches, None)
    col_s = resolution / 20.0

    rows = []
    for pitch, start, end in notes:
        c0 = int(round(start / col_s))
        inst, _mat, note_value = conv(pitch)
        mapped = BANK_BASE[inst] + note_value
        rows.append({
            'pitch': pitch, 'start': start, 'dur': end - start, 'col': c0,
            'key': (inst, note_value), 'mapped': mapped,
            'dev': mapped - pitch,
            'timing_err_ticks': abs(resolution * c0 - 20.0 * start),
        })

    columns = int(round(max(e for _, _, e in notes) / col_s))
    dropped = sum(1 for r in rows if not (0 <= r['col'] < columns))
    placed = defaultdict(set)
    for r in rows:
        if 0 <= r['col'] < columns:
            placed[r['col']].add(r['key'])
    placed_count = sum(len(v) for v in placed.values())

    predicted = {(resolution * c + 2, k[0], k[1]) for c, ks in placed.items() for k in ks}
    decoded_set = {(int(round(t * 20)), inst, abs_pitch - BANK_BASE[inst])
                   for t, abs_pitch, inst in decoded['events']}
    exact = len(predicted & decoded_set)

    devs = [r['dev'] for r in rows]
    folded = [r for r in rows if abs(r['dev'] - shift) >= 12]
    durs = [r['dur'] for r in rows]
    t_err = [r['timing_err_ticks'] for r in rows]

    # 相邻音符音程保持率
    seq = sorted(rows, key=lambda r: r['start'])
    interval_ok = sum(1 for a, b in zip(seq, seq[1:])
                      if (b['mapped'] - a['mapped']) == (b['pitch'] - a['pitch']))

    vel = []
    import mido
    for track in mido.MidiFile(str(path)).tracks:
        for msg in track:
            if msg.type == 'note_on' and msg.velocity > 0:
                vel.append(msg.velocity)

    per_col = Counter(r['col'] for r in rows)
    return {
        'ref_notes': len(rows),
        'nbt_notes': len(decoded['events']),
        'matched': exact,
        'dropped_out_of_range': dropped,
        'merged_same_key': len(rows) - placed_count,
        'placed': placed_count,
        'global_shift_semitones': shift,
        'global_shift_cents': shift * 100,
        'pitch_dev_min': min(devs), 'pitch_dev_max': max(devs),
        'pitch_dev_median': statistics.median(devs),
        'octave_folded': len(folded),
        'octave_folded_pct': len(folded) / len(rows) * 100,
        'interval_preserved': interval_ok,
        'interval_total': len(seq) - 1,
        'interval_pct': interval_ok / max(1, len(seq) - 1) * 100,
        'timing_err_mean_ms': statistics.mean(t_err) * 50,
        'timing_err_max_ms': max(t_err) * 50,
        'timing_err_mean_ticks': statistics.mean(t_err),
        'duration_mean_s': statistics.mean(durs),
        'duration_median_s': statistics.median(durs),
        'duration_max_s': max(durs),
        'duration_gt_0_9s': sum(1 for d in durs if d > 0.9),
        'duration_gt_0_9s_pct': sum(1 for d in durs if d > 0.9) / len(durs) * 100,
        'velocity_values': len(set(vel)),
        'velocity_min': min(vel) if vel else None,
        'velocity_max': max(vel) if vel else None,
        'max_polyphony_per_column': max(per_col.values()) if per_col else 0,
        'columns_used': len(per_col),
    }


def _decode_audio_mono(path, sr=AUDIO_SR):
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        local = os.path.join(os.path.dirname(__file__), '..', 'runtime', 'ffmpeg.exe')
        ffmpeg = local if os.path.isfile(local) else None
    if not ffmpeg:
        raise SystemExit('找不到 ffmpeg；请把它放进 PATH 或 tools/some-onnx/runtime/')
    with tempfile.TemporaryDirectory() as td:
        wav = os.path.join(td, 'mono.wav')
        subprocess.run([ffmpeg, '-y', '-v', 'error', '-i', path,
                        '-ar', str(sr), '-ac', '1', '-c:a', 'pcm_s16le', wav], check=True)
        rate, data = wavfile.read(wav)
    y = data.astype(np.float32)
    if y.max() > 1.5:
        y /= 32768.0
    return y


def _chroma_frames(y, sr=AUDIO_SR):
    window = np.hanning(N_FFT)
    freqs = np.fft.rfftfreq(N_FFT, 1 / sr)
    valid = (freqs >= 40) & (freqs <= 2000)
    pcs = np.round(69 + 12 * np.log2(np.maximum(freqs, 1) / 440)).astype(int) % 12
    frames = []
    for i in range(0, len(y) - N_FFT, HOP):
        mag = np.abs(np.fft.rfft(y[i:i + N_FFT] * window))
        c = np.zeros(12)
        np.add.at(c, pcs[valid], mag[valid])
        norm = np.linalg.norm(c)
        frames.append(c / norm if norm > 0 else c)
    return np.array(frames)


def audio_metrics(path, decoded, offset_s=0.1, align_shift=0):
    """align_shift: 已知的整曲移调（NBT 生成器会按中位数移调），按 -shift 校正后再比对。"""
    y = _decode_audio_mono(path)
    frames = _chroma_frames(y)
    if len(frames) == 0:
        raise SystemExit('音频过短，无法分析')

    histogram = np.zeros(12)
    for _t, pitch, _inst in decoded['events']:
        histogram[pitch % 12] += 1
    audio_global = frames.sum(axis=0)
    norm = np.linalg.norm(audio_global) * np.linalg.norm(histogram)
    cos_by_shift = [float(audio_global @ np.roll(histogram, r) / norm) for r in range(12)]
    best_shift = int(np.argmax(cos_by_shift))
    align_shift %= 12

    values, ranks = [], []
    for t, pitch, _inst in decoded['events']:
        src_t = max(0.0, t - offset_s)
        k = min(len(frames) - 1, int(round(src_t * AUDIO_SR / HOP)))
        c = frames[k]
        pc = (pitch + align_shift) % 12
        values.append(float(c[pc]))
        ranks.append(int((c > c[pc]).sum()) + 1)

    values = np.array(values)
    ranks = np.array(ranks)
    return {
        'audio_duration_s': len(y) / AUDIO_SR,
        'global_chroma_cosine_raw': cos_by_shift[0],
        'global_chroma_cosine': cos_by_shift[align_shift],
        'align_shift': align_shift,
        'best_chroma_shift': best_shift,
        'global_chroma_cosine_best': cos_by_shift[best_shift],
        'note_pitch_presence_mean': float(values.mean()),
        'note_pitch_presence_median': float(np.median(values)),
        'pitch_class_top1_pct': float((ranks == 1).mean() * 100),
        'pitch_class_top3_pct': float((ranks <= 3).mean() * 100),
        'pitch_class_median_rank': int(np.median(ranks)),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nbt', help='要评估的 .ntb（省略时按 --midi 直接对音频评估）')
    ap.add_argument('--midi', help='参考 MIDI；无 --nbt 时为待评估的最终 MIDI')
    ap.add_argument('--audio', help='原始音频（用于端到端谐波保真度）')
    ap.add_argument('--resolution', type=int, default=2, help='每列游戏刻数（默认 2）')
    ap.add_argument('--json', help='把结果另存为 JSON')
    args = ap.parse_args()

    if not args.nbt and not args.midi:
        raise SystemExit('至少需要 --nbt 或 --midi')

    align_shift = 0
    if args.nbt:
        decoded = decode_nbt(args.nbt)
        print(f'[NBT] {args.nbt}')
        print(f'      size={decoded["size"]}  方块={decoded["blocks"]}  '
              f'音符盒={len(decoded["events"])}  不可达={decoded["unreachable"]}  '
              f'末端灯={decoded["lamp_tick"]} 刻')
        if decoded['events']:
            inst = Counter(e[2] for e in decoded['events'])
            span = (decoded['events'][0][0], decoded['events'][-1][0])
            print(f'      触发时间 {span[0]:.2f}~{span[1]:.2f}s  乐器分布 {dict(inst)}')
        report = {'nbt': args.nbt, 'nbt_stats': {
            'size': decoded['size'], 'blocks': decoded['blocks'],
            'note_blocks': len(decoded['events']), 'unreachable': decoded['unreachable'],
        }}
        if args.midi:
            m = midi_metrics(args.midi, decoded, args.resolution)
            report['nbt_stage'] = m
            print(f'\n[MIDI→NBT 损失]  参考 MIDI: {args.midi}')
            print(f'  音符保留:      {m["nbt_notes"]}/{m["ref_notes"]}  '
                  f'(参考-MIDI 精确对齐 {m["matched"]}，超时长丢弃 {m["dropped_out_of_range"]})')
            print(f'  同列去重合并:  {m["merged_same_key"]} 个音符'
                  f'（单列最大复音 {m["max_polyphony_per_column"]}）')
            print(f'  整体移调:      {m["global_shift_semitones"]:+d} 半音 '
                  f'= {m["global_shift_cents"]:+d} cents')
            print(f'  音高偏差:      min={m["pitch_dev_min"]} max={m["pitch_dev_max"]} '
                  f'median={m["pitch_dev_median"]} 半音')
            print(f'  八度折回音符:  {m["octave_folded"]} ({m["octave_folded_pct"]:.1f}%)')
            print(f'  相邻音程保持:  {m["interval_preserved"]}/{m["interval_total"]} '
                  f'({m["interval_pct"]:.1f}%)')
            print(f'  时序量化误差:  mean={m["timing_err_mean_ticks"]:.2f} 刻 '
                  f'({m["timing_err_mean_ms"]:.1f}ms)，max={m["timing_err_max_ms"]:.1f}ms'
                  f'（另有恒定 2 刻/100ms 支线延迟）')
            print(f'  时值:          mean={m["duration_mean_s"]:.2f}s '
                  f'median={m["duration_median_s"]:.2f}s max={m["duration_max_s"]:.2f}s；'
                  f'>0.9s 仅 {m["duration_gt_0_9s"]} ({m["duration_gt_0_9s_pct"]:.1f}%)')
            print(f'  力度:          MIDI {m["velocity_values"]} 档 '
                  f'({m["velocity_min"]}~{m["velocity_max"]}) → NBT 无力度维度（100% 丢失）')
            align_shift = (-m['global_shift_semitones']) % 12
    else:
        notes = g.load_notes(args.midi)
        if not notes:
            raise SystemExit('MIDI 中没有音符')
        pitches = [p for p, _, _ in notes]
        duration = max(e for _, _, e in notes)
        decoded = {'events': [(s, p, 'midi') for p, s, e in notes],
                   'size': None, 'blocks': None, 'unreachable': 0, 'lamp_tick': None}
        print(f'[MIDI] {args.midi}')
        print(f'      音符={len(notes)}  音域={min(pitches)}~{max(pitches)}  '
              f'覆盖时长={duration:.2f}s')
        report = {'midi': args.midi, 'midi_stats': {
            'notes': len(notes), 'pitch_min': min(pitches), 'pitch_max': max(pitches),
            'duration_s': duration,
        }}

    if args.audio:
        a = audio_metrics(args.audio, decoded, 0.1 if args.nbt else 0.0, align_shift)
        report['audio_vs_source'] = a
        label = '音频→NBT 端到端' if args.nbt else 'MIDI vs 原曲'
        print(f'\n[{label}]  原始音频: {args.audio}')
        print(f'  音频时长:      {a["audio_duration_s"]:.2f}s')
        print(f'  全局 chroma:   原始={a["global_chroma_cosine_raw"]:.4f}  '
              f'移调校正={a["global_chroma_cosine"]:.4f}'
              f'（校正 {a["align_shift"]} 半音，最佳旋转参考 {a["best_chroma_shift"]} 半音）')
        print(f'  逐音符音级强度: mean={a["note_pitch_presence_mean"]:.3f} '
              f'median={a["note_pitch_presence_median"]:.3f}')
        print(f'  音级命中:      top1={a["pitch_class_top1_pct"]:.1f}% '
              f'top3={a["pitch_class_top3_pct"]:.1f}% '
              f'中位排名={a["pitch_class_median_rank"]}')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        print(f'\n已写出: {args.json}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
