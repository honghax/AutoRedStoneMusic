# -*- coding: utf-8 -*-
"""Minecraft Note Studio 检测算法的 Python 复刻（不含游戏内逻辑）。

原模组是纯 DSP 方案：22050Hz 单声道 → 25ms 一帧 / 2048 点 FFT → 25 个半音候选
（F#3~F#5）的谐波打分 → 起音状态机 → 8 种原版乐器判定。本脚本按 Java 源码逐行
移植，用于离线跑通并量化它的转换效果。

和原版一致的部分：
  · hop = 22050*25/1000 = 551 采样、Hann 窗、RMS 门限 0.012、tonality 门限 0.01
  · 4 次谐波权重 1.0/0.45/0.24/0.14、±1 bin 取峰并做 1/(1+|Δbin|) 惩罚
  · 局部极大（左右 ±1 半音）筛选、复音上限、释放帧计数
  · 打击乐三分（谱质心 >4500 HAT / >1250 SNARE / <220 BASEDRUM）
  · 乐器判定阈值与原版 chooseInstrument 完全一致
  · AUTO 模式：40 帧区间自适应 + 4 档回声阈值候选 + 余弦相似度择优

与原版不同的部分（仅为离线分析方便）：
  · FFT 用 numpy 实现（数学等价，不做逐点蝶形循环）
  · 解码直接交给 ffmpeg 做单声道/22050 重采样（原版是 JavaSound + 线性插值）
  · 额外输出 MIDI / WAV / NBT，便于试听与对比

用法:
  python notestudio_port.py --audio song.mp3 \
      [--mode balanced|smooth|sensitive|auto] \
      [--prefix out] [--wav] [--mp3] [--midi] [--nbt] [--compare] [--json]
"""

import argparse
import collections
import json
import math
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass

import numpy as np
import scipy.io.wavfile as wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SR = 22050                    # 分析采样率（与原版一致）
FFT_SIZE = 2048
HOP = SR * 25 // 1000         # 551 采样 = 25ms
FRAME_SECONDS = 25 / 1000.0
MIN_AUDIBLE_RMS = 0.012
MIN_TONAL_ENERGY_RATIO = 0.01
AUTO_ZONE_TICKS = 40
MAX_EVENTS_PER_SECOND = 100
RENDER_SR = 44100

INSTRUMENTS = ('HARP', 'BASS', 'BASEDRUM', 'SNARE', 'HAT', 'BELL', 'CHIME', 'XYLOPHONE')
PERCUSSION = ('BASEDRUM', 'SNARE', 'HAT')

# 原版音符盒各音色的基准频率（note=0 时的实际发声）
INSTRUMENT_BASE_HZ = {
    'HARP': 185.0,        # F#3
    'BASS': 46.25,        # F#1
    'BELL': 740.0,        # F#5
    'CHIME': 740.0,       # F#5
    'XYLOPHONE': 740.0,   # F#5
}


@dataclass(frozen=True)
class Mode:
    name: str
    max_polyphony: int
    relative_pitch_threshold: float
    release_frames: int
    attack_flux_threshold: float
    attack_rise_ratio: float


MODES = {
    'balanced': Mode('balanced', 4, 0.18, 2, 0.34, 1.07),
    'smooth': Mode('smooth', 2, 0.24, 3, 0.50, 1.14),
    'sensitive': Mode('sensitive', 6, 0.11, 1, 0.18, 1.02),
}


@dataclass(frozen=True)
class Event:
    tick: int
    pitch: int
    strength: float
    instrument: str

    def seconds(self):
        return self.tick * FRAME_SECONDS

    def midi_pitch(self):
        """还原成原版音符盒实际发出的绝对音高（BASS 会比检测低两个八度）。"""
        if self.instrument in PERCUSSION:
            return {'BASEDRUM': 36, 'SNARE': 38, 'HAT': 42}[self.instrument]
        freq = INSTRUMENT_BASE_HZ[self.instrument] * (2.0 ** (self.pitch / 12.0))
        return int(round(69 + 12 * math.log2(freq / 440.0)))


# --------------------------------------------------------------------------- #
# 解码
# --------------------------------------------------------------------------- #
def find_ffmpeg():
    path = shutil.which('ffmpeg')
    if path:
        return path
    local = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         '..', 'runtime', 'ffmpeg.exe')
    local = os.path.abspath(local)
    if os.path.isfile(local):
        return local
    raise SystemExit('找不到 ffmpeg：请加入 PATH，或放到 tools/some-onnx/runtime/')


def decode_audio(path):
    """单声道 22050Hz，等价于原版 AudioDecoder 的降混 + 重采样结果。"""
    ffmpeg = find_ffmpeg()
    command = [ffmpeg, '-v', 'error', '-i', path,
               '-map', '0:a:0', '-vn', '-sn', '-dn',
               '-f', 's16le', '-ac', '1', '-ar', str(SR), 'pipe:1']
    proc = subprocess.run(command, capture_output=True)
    if proc.returncode != 0:
        raise SystemExit('解码失败: ' + proc.stderr.decode('utf-8', 'ignore')[:300])
    samples = np.frombuffer(proc.stdout, dtype='<i2').astype(np.float32) / 32768.0
    if samples.size == 0:
        raise SystemExit('音频里没有可解码的采样')
    return samples


# --------------------------------------------------------------------------- #
# 谐波打分（对应 AudioAnalyzer.harmonicScore）
# --------------------------------------------------------------------------- #
def harmonic_score(magnitude, fundamental):
    weights = (1.0, 0.45, 0.24, 0.14)
    score = 0.0
    for harmonic, weight in enumerate(weights, start=1):
        exact_bin = fundamental * harmonic * FFT_SIZE / SR
        if exact_bin >= 1024.0:
            break
        center = int(math.floor(exact_bin + 0.5))     # Java Math.round
        peak = 0.0
        for b in range(max(1, center - 1), min(1023, center + 1) + 1):
            value = magnitude[b] / (1.0 + abs(b - exact_bin))
            if value > peak:
                peak = value
        score += peak * weight
    return score


def choose_instrument(pitch, centroid, tonality):
    if pitch <= 5 and centroid < 700.0:
        return 'BASS'
    if centroid > 4200.0 and tonality > 0.025:
        return 'CHIME'
    if centroid > 2300.0 and tonality > 0.02:
        return 'BELL'
    if pitch >= 15 and centroid > 1500.0 and tonality > 0.016:
        return 'XYLOPHONE'
    return 'HARP'


def audible_strength(rms):
    return float(max(0.15, min(1.0, (rms - MIN_AUDIBLE_RMS) / 0.16)))


def add_rate_limited(events, recent_ticks, event):
    one_second_ago = event.tick - MAX_EVENTS_PER_SECOND + 1
    while recent_ticks and recent_ticks[0] < one_second_ago:
        recent_ticks.popleft()
    if len(recent_ticks) >= MAX_EVENTS_PER_SECOND:
        return False
    events.append(event)
    recent_ticks.append(event.tick)
    return True


# --------------------------------------------------------------------------- #
# 主检测循环（对应 AudioAnalyzer.analyze）
# --------------------------------------------------------------------------- #
def analyze(samples, mode_name='balanced', verbose=True):
    automatic = mode_name == 'auto'
    mode = MODES['balanced'] if automatic else MODES[mode_name]

    ticks = max(1, (len(samples) + HOP - 1) // HOP)
    window = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(FFT_SIZE) / 2047.0)
    reference = np.zeros((ticks, 25)) if automatic else None

    events = []
    recent_ticks = collections.deque()
    previous = np.zeros(25)
    active = np.zeros(25, dtype=bool)
    missing_frames = np.zeros(25, dtype=int)
    last_onset_tick = np.full(25, -100)
    last_onset_score = np.zeros(25)
    last_onset_rms = np.zeros(25)
    pitch_valley = np.full(25, np.inf)
    previous_rms = 0.0
    envelope_valley = math.inf
    zone_mode = MODES['balanced']
    zone_flux = zone_tonality = zone_decay = 0.0
    zone_frames = 0
    peaks = []

    started = time.perf_counter()
    for tick in range(ticks):
        if verbose and (tick == 0 or tick == ticks - 1 or tick % 400 == 0):
            print(f'[分析] {tick + 1}/{ticks} 帧', flush=True)

        offset = tick * HOP
        frame = np.zeros(FFT_SIZE, dtype=np.float64)
        available = min(FFT_SIZE, len(samples) - offset)
        if available > 0:
            frame[:available] = samples[offset:offset + available]

        rms = math.sqrt(float(np.dot(frame, frame)) / FFT_SIZE)
        if rms < MIN_AUDIBLE_RMS:
            previous[:] = 0.0
            active[:] = False
            missing_frames[:] = 0
            pitch_valley[:] = np.inf
            envelope_valley = math.inf
            previous_rms = 0.0
            continue

        spectrum = np.fft.fft(frame * window)
        magnitude = spectrum.real ** 2 + spectrum.imag ** 2
        bins = np.arange(1, 1024)
        spectrum_energy = float(magnitude[1:1024].sum())
        frequencies = bins * SR / FFT_SIZE
        centroid = (float((frequencies * magnitude[1:1024]).sum() / spectrum_energy)
                    if spectrum_energy > 0 else 0.0)
        transient_frame = previous_rms > 0.0 and rms > previous_rms * 1.45

        scores = np.array([harmonic_score(magnitude, 185.0 * 2.0 ** (p / 12.0))
                           for p in range(25)])
        strongest = float(scores.max())

        if automatic and strongest > 0.0:
            amplitude = min(1.0, rms / 0.16)
            reference[tick] = np.sqrt(scores / strongest) * amplitude

        tonality = strongest / spectrum_energy if spectrum_energy > 0 else 0.0
        score_energy = float(scores.sum())
        positive_flux = float(np.maximum(0.0, scores - previous).sum())
        normalized_flux = positive_flux / score_energy if score_energy > 0 else 0.0
        envelope_valley = min(envelope_valley, rms)

        energy_reattack = (previous_rms > 0.0 and rms > previous_rms * 1.08
                           and rms > envelope_valley * 1.22)
        decaying_section = (previous_rms > 0.0 and rms < previous_rms * 0.98
                            and normalized_flux < 0.6)

        local_mode = mode
        if automatic:
            zone_flux += normalized_flux
            zone_tonality += tonality
            if decaying_section:
                zone_decay += 1
            zone_frames += 1
            if zone_frames >= AUTO_ZONE_TICKS:
                average_flux = zone_flux / zone_frames
                average_tonality = zone_tonality / zone_frames
                decay_ratio = zone_decay / zone_frames
                if decay_ratio > 0.52 or average_tonality < 0.016:
                    zone_mode = MODES['smooth']
                elif average_flux > 0.48:
                    zone_mode = MODES['sensitive']
                else:
                    zone_mode = MODES['balanced']
                zone_flux = zone_tonality = zone_decay = 0.0
                zone_frames = 0
            local_mode = zone_mode

        flux_threshold = 0.6 if rms < previous_rms * 0.98 else 0.42
        spectral_reattack = normalized_flux > flux_threshold
        if automatic:
            clear_attack = (previous_rms <= 0.0 or energy_reattack
                            or spectral_reattack)
        else:
            clear_attack = (previous_rms <= 0.0
                            or rms > previous_rms * mode.attack_rise_ratio
                            or normalized_flux > mode.attack_flux_threshold)

        # 打击乐：能量突增 + 调性低 → 按谱质心分三类
        if transient_frame and tonality < 0.025:
            if centroid > 4500.0:
                percussion = 'HAT'
            elif centroid > 1250.0:
                percussion = 'SNARE'
            elif centroid < 220.0:
                percussion = 'BASEDRUM'
            else:
                percussion = None
            if percussion:
                pitch = 6 if percussion == 'BASEDRUM' else 12
                add_rate_limited(events, recent_ticks,
                                 Event(tick, pitch, audible_strength(rms), percussion))

        if tonality < MIN_TONAL_ENERGY_RATIO:
            for p in range(25):
                if active[p]:
                    missing_frames[p] += 1
                    if missing_frames[p] >= 2:
                        active[p] = False
                        missing_frames[p] = 0
                        pitch_valley[p] = np.inf
            previous = scores.copy()
            previous_rms = rms
            continue

        for p in range(25):
            if active[p]:
                pitch_valley[p] = min(pitch_valley[p], scores[p])

        order = sorted(range(25), key=lambda p: scores[p], reverse=True)
        selected = 0
        present = np.zeros(25, dtype=bool)
        for p in order:
            score = float(scores[p])
            if selected >= local_mode.max_polyphony:
                break
            if score < strongest * local_mode.relative_pitch_threshold:
                break
            separated = ((p == 0 or scores[p - 1] <= score)
                         and (p == 24 or scores[p + 1] <= score))
            if not separated:
                continue

            selected += 1
            present[p] = True
            missing_frames[p] = 0

            was_active = bool(active[p])
            onset_gap = tick - last_onset_tick[p]
            valley = max(1.0e-12, float(pitch_valley[p]))
            onset_reference = max(1.0e-12, float(last_onset_score[p]))
            strong_rebound = score > valley * 1.38 and score > previous[p] * 1.1
            renewed_energy = (rms >= last_onset_rms[p] * 0.68
                              and (energy_reattack or normalized_flux > 0.48))
            rapid_articulation = (onset_gap >= 1 and normalized_flux > 0.72
                                  and score >= onset_reference * 0.42)
            repeated_attack = (automatic and was_active and onset_gap >= 1
                               and ((strong_rebound and renewed_energy)
                                    or rapid_articulation))
            initial_attack = (not was_active and clear_attack
                              and (previous[p] <= 0.0 or score > previous[p] * 1.28))
            active[p] = True

            if repeated_attack or initial_attack:
                instrument = choose_instrument(p, centroid, tonality)
                add_rate_limited(events, recent_ticks,
                                 Event(tick, p, audible_strength(rms), instrument))
                last_onset_tick[p] = tick
                last_onset_score[p] = score
                last_onset_rms[p] = rms
                pitch_valley[p] = score
                envelope_valley = rms

        for p in range(25):
            if not present[p] and active[p]:
                missing_frames[p] += 1
                if missing_frames[p] >= local_mode.release_frames:
                    active[p] = False
                    missing_frames[p] = 0
                    pitch_valley[p] = np.inf

        peaks.append(int(present.sum()))
        previous = scores.copy()
        previous_rms = rms

    elapsed = time.perf_counter() - started
    if automatic and events:
        events = select_best_auto_candidate(events, reference, ticks, verbose)

    events.sort(key=lambda e: e.tick)
    return {
        'events': events,
        'ticks': ticks,
        'duration': ticks * FRAME_SECONDS,
        'reference': reference,
        'analysis_seconds': elapsed,
        'peak_polyphony': max(peaks) if peaks else 0,
    }


# --------------------------------------------------------------------------- #
# AUTO 模式：回声阈值候选 + 自洽性打分（对应 selectBestAutoCandidate）
# --------------------------------------------------------------------------- #
def select_best_auto_candidate(detected, reference, ticks, verbose=True):
    thresholds = (0.035, 0.07, 0.11, 0.16)
    candidates = [list(detected)]
    for threshold in thresholds:
        candidates.append(build_auto_candidate(detected, reference, ticks, threshold))

    best = candidates[0]
    best_score = candidate_similarity(best, reference, ticks)
    for index, candidate in enumerate(candidates[1:], start=1):
        score = candidate_similarity(candidate, reference, ticks)
        if verbose:
            print(f'[AUTO] 候选 {index + 1}/{len(candidates)} '
                  f'阈值={thresholds[index - 1]:.3f} 得分={score:.4f}', flush=True)
        if score > best_score:
            best_score = score
            best = candidate
    if verbose:
        print(f'[AUTO] 原始候选 得分={candidate_similarity(candidates[0], reference, ticks):.4f}')
        print(f'[AUTO] 选中得分 {best_score:.4f}，事件 {len(best)}/{len(detected)}')
    return best


def build_auto_candidate(detected, reference, ticks, echo_threshold):
    voice_count = len(INSTRUMENTS) * 25
    last_tick = np.full(voice_count, -1000)
    last_peak = np.zeros(voice_count)
    result = []

    for event in detected:
        if event.instrument not in PERCUSSION:
            aligned = strongest_nearby_tick(reference, event.pitch, event.tick, ticks)
            current = reference[aligned][event.pitch]
            earlier = 0.0 if aligned == 0 else reference[aligned - 1][event.pitch]
            two_back = 0.0 if aligned < 2 else reference[aligned - 2][event.pitch]
            rebound = current - min(earlier, two_back)
            voice = INSTRUMENTS.index(event.instrument) * 25 + event.pitch
            gap = aligned - last_tick[voice]
            decaying_echo = (gap > 0 and gap <= 12
                             and current < last_peak[voice] * 0.9
                             and rebound < echo_threshold)
            if not decaying_echo:
                result.append(Event(aligned, event.pitch, event.strength, event.instrument))
                last_tick[voice] = aligned
                last_peak[voice] = max(current, 1.0e-9)
        else:
            result.append(event)

    result.sort(key=lambda e: e.tick)
    return result


def strongest_nearby_tick(reference, pitch, tick, ticks):
    best = max(0, min(ticks - 1, tick))
    best_rise = -math.inf
    for candidate in range(max(0, tick - 1), min(ticks - 1, tick + 1) + 1):
        before = 0.0 if candidate == 0 else reference[candidate - 1][pitch]
        rise = reference[candidate][pitch] - before
        if rise > best_rise:
            best_rise = rise
            best = candidate
    return best


def candidate_similarity(candidate, reference, ticks):
    synthesized = np.zeros((ticks, 25))
    onset_quality = 0.0
    tonal_events = 0

    for event in candidate:
        if event.instrument in PERCUSSION:
            continue
        start = max(0, min(ticks - 1, event.tick))
        for age in range(0, 10):
            if start + age >= ticks:
                break
            value = event.strength * (0.72 ** age)
            synthesized[start + age][event.pitch] = max(
                synthesized[start + age][event.pitch], value)
        before = 0.0 if start == 0 else reference[start - 1][event.pitch]
        onset_quality += max(0.0, reference[start][event.pitch] - before)
        tonal_events += 1

    dot = float((reference * synthesized).sum())
    reference_power = float((reference * reference).sum())
    synthesized_power = float((synthesized * synthesized).sum())
    spectral_match = dot / math.sqrt(max(1.0e-12, reference_power * synthesized_power))
    attack_match = onset_quality / tonal_events if tonal_events else 0.0
    clutter_penalty = len(candidate) / max(1, ticks) * 0.015
    return spectral_match * 0.82 + attack_match * 0.18 - clutter_penalty


# --------------------------------------------------------------------------- #
# 输出：MIDI / WAV / NBT
# --------------------------------------------------------------------------- #
def write_midi(events, path):
    import mido

    midi = mido.MidiFile(ticks_per_beat=480)
    tempo_track = mido.MidiTrack()
    tempo_track.append(mido.MetaMessage('set_tempo', tempo=500000, time=0))
    tempo_track.append(mido.MetaMessage('end_of_track', time=0))
    midi.tracks.append(tempo_track)

    # 25ms 一帧，120BPM 下 1 拍=500ms=20 帧 → 1 帧 = 24 MIDI tick
    for index, instrument in enumerate(INSTRUMENTS):
        subset = [e for e in events if e.instrument == instrument]
        if not subset:
            continue
        track = mido.MidiTrack()
        track.append(mido.MetaMessage('track_name', name=instrument, time=0))
        channel = 9 if instrument in PERCUSSION else index
        messages = []
        for event in subset:
            on = event.tick * 24
            velocity = max(1, min(127, int(round(event.strength * 127))))
            messages.append((on, 1, 'note_on', event.midi_pitch(), velocity))
            messages.append((on + 6, 0, 'note_off', event.midi_pitch(), 0))
        messages.sort(key=lambda m: (m[0], m[1]))
        now = 0
        for absolute, _order, kind, note, velocity in messages:
            track.append(mido.Message(kind, note=note, velocity=velocity,
                                      channel=channel, time=absolute - now))
            now = absolute
        track.append(mido.MetaMessage('end_of_track', time=0))
        midi.tracks.append(track)

    midi.save(path)
    return path


def _tone(frequency, duration, harmonics, decay, rng=None):
    n = max(1, int(duration * RENDER_SR))
    t = np.arange(n) / RENDER_SR
    signal = np.zeros(n)
    for k, amplitude in harmonics:
        signal += amplitude * np.sin(2 * np.pi * frequency * k * t)
    envelope = np.exp(-t * decay)
    attack = min(int(0.006 * RENDER_SR), n)
    envelope[:attack] *= np.linspace(0.0, 1.0, attack)
    release = min(int(0.04 * RENDER_SR), n)
    envelope[-release:] *= np.linspace(1.0, 0.0, release)
    return signal * envelope


def _noise_burst(duration, decay, rng, highpass=False):
    n = max(1, int(duration * RENDER_SR))
    noise = rng.standard_normal(n)
    if highpass:
        noise = np.diff(noise, prepend=0.0)
        noise /= max(1e-9, np.max(np.abs(noise)))
    t = np.arange(n) / RENDER_SR
    return noise * np.exp(-t * decay)


def render_audio(events, ticks, path):
    total = ticks * FRAME_SECONDS + 1.2
    buffer = np.zeros(int(total * RENDER_SR), dtype=np.float64)
    rng = np.random.default_rng(20261114)

    for event in events:
        frequency = INSTRUMENT_BASE_HZ.get(event.instrument, 185.0) * (
            2.0 ** (event.pitch / 12.0))
        if event.instrument == 'HARP':
            signal = _tone(frequency, 1.1, ((1, 1.0), (2, 0.45), (3, 0.22), (4, 0.12)), 3.2)
        elif event.instrument == 'BASS':
            signal = _tone(frequency, 1.4, ((1, 1.0), (2, 0.5), (3, 0.28)), 2.2)
        elif event.instrument in ('BELL', 'CHIME'):
            signal = _tone(frequency, 1.6, ((1, 1.0), (2.76, 0.5), (5.4, 0.24)), 4.0)
        elif event.instrument == 'XYLOPHONE':
            signal = _tone(frequency, 0.55, ((1, 1.0), (3.0, 0.45), (6.0, 0.2)), 9.0)
        elif event.instrument == 'BASEDRUM':
            signal = _tone(58.0, 0.45, ((1, 1.0), (1.5, 0.3)), 11.0)
        elif event.instrument == 'SNARE':
            signal = (_noise_burst(0.28, 26.0, rng)
                      + 0.5 * _tone(190.0, 0.28, ((1, 1.0),), 24.0))
        else:  # HAT
            signal = _noise_burst(0.11, 70.0, rng, highpass=True)

        signal *= event.strength
        start = int(event.tick * FRAME_SECONDS * RENDER_SR)
        if start >= len(buffer):
            continue
        end = min(len(buffer), start + len(signal))
        buffer[start:end] += signal[:end - start]

    peak = float(np.max(np.abs(buffer))) if buffer.size else 0.0
    if peak > 0:
        buffer *= 0.9 / peak
    wavfile.write(path, RENDER_SR, (buffer * 32767).astype(np.int16))
    return path


BANK_OF = {
    'HARP': ('harp', 'minecraft:dirt', 54),
    'BASS': ('bass', 'minecraft:bamboo_planks', 30),
    'BELL': ('bell', 'minecraft:gold_block', 78),
    'CHIME': ('bell', 'minecraft:gold_block', 78),
    'XYLOPHONE': ('bell', 'minecraft:gold_block', 78),
}


def write_nbt(events, ticks, path, resolution=2):
    """把检测结果映射到本项目的三档结构，方便进游戏试听。"""
    import gen_machine as g

    col_seconds = resolution / 20.0
    columns = max(1, int(round(ticks * FRAME_SECONDS / col_seconds)))
    col_keys = collections.defaultdict(set)

    for event in events:
        column = int(round(event.seconds() / col_seconds))
        if not (0 <= column < columns):
            continue
        if event.instrument in PERCUSSION:
            col_keys[column].add(('basedrum', 'minecraft:stone', 0))
        else:
            instrument, material, _base = BANK_OF[event.instrument]
            col_keys[column].add((instrument, material, event.pitch))

    col_notes = {c: sorted(v, key=lambda k: (k[2], k[0])) for c, v in col_keys.items()}
    max_per_col = max((len(v) for v in col_notes.values()), default=0)
    if max_per_col > g.Z_CAP:
        raise SystemExit(f'单列复音 {max_per_col} 超过 Z 容量 {g.Z_CAP}')

    blocks, *_ = g.build(columns, resolution, col_notes, max_per_col, simple=True)
    size = g.write_nbt(path, blocks)
    return {
        'path': path,
        'size': list(size),
        'blocks': len(blocks),
        'note_blocks': sum(1 for name, _ in blocks.values()
                           if name == 'minecraft:note_block'),
        'max_per_col': max_per_col,
    }


# --------------------------------------------------------------------------- #
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--audio', required=True)
    parser.add_argument('--mode', default='balanced',
                        choices=('balanced', 'smooth', 'sensitive', 'auto'))
    parser.add_argument('--prefix', help='输出文件前缀，默认与音频同名')
    parser.add_argument('--wav', action='store_true', help='渲染检测结果 WAV')
    parser.add_argument('--mp3', action='store_true', help='WAV 再转 MP3')
    parser.add_argument('--midi', action='store_true', help='输出 MIDI')
    parser.add_argument('--nbt', action='store_true', help='输出可进游戏的 NBT')
    parser.add_argument('--compare', action='store_true',
                        help='和原曲做 chroma 相似度对比')
    parser.add_argument('--json', action='store_true', help='输出统计 JSON')
    args = parser.parse_args()

    prefix = args.prefix or os.path.splitext(args.audio)[0]
    started = time.perf_counter()

    print(f'[解码] {args.audio}')
    samples = decode_audio(args.audio)
    print(f'[解码] {len(samples) / SR:.2f}s，{SR}Hz 单声道')

    result = analyze(samples, args.mode)
    events = result['events']
    if not events:
        raise SystemExit('没有检测到任何音符')

    counts = collections.Counter(e.instrument for e in events)
    pitch_hist = collections.Counter(e.pitch for e in events)
    total_elapsed = time.perf_counter() - started

    print(f'[结果] 帧数={result["ticks"]} 时长={result["duration"]:.2f}s '
          f'事件={len(events)} 每秒={len(events) / result["duration"]:.2f} '
          f'单帧峰值复音={result["peak_polyphony"]}')
    print(f'[乐器] {dict(counts)}')
    print(f'[音高] {dict(sorted(pitch_hist.items()))}')
    print(f'[耗时] 分析={result["analysis_seconds"]:.2f}s 总计={total_elapsed:.2f}s')

    report = {
        'audio': os.path.abspath(args.audio),
        'mode': args.mode,
        'frames': result['ticks'],
        'duration_s': result['duration'],
        'events': len(events),
        'events_per_second': len(events) / result['duration'],
        'peak_polyphony': result['peak_polyphony'],
        'instruments': dict(counts),
        'pitch_histogram': dict(sorted(pitch_hist.items())),
        'analysis_seconds': result['analysis_seconds'],
        'total_seconds': total_elapsed,
    }

    outputs = {}
    if args.midi:
        outputs['midi'] = write_midi(events, prefix + '.mid')
        print(f'[产物] MIDI {outputs["midi"]}')
    if args.wav or args.mp3:
        wav_path = prefix + '_notestudio.wav'
        render_audio(events, result['ticks'], wav_path)
        outputs['wav'] = wav_path
        print(f'[产物] WAV {wav_path}')
        if args.mp3:
            mp3_path = prefix + '_notestudio.mp3'
            subprocess.run([find_ffmpeg(), '-y', '-v', 'error', '-i', wav_path,
                            '-b:a', '192k', mp3_path], check=True)
            outputs['mp3'] = mp3_path
            print(f'[产物] MP3 {mp3_path}')
    if args.nbt:
        outputs['nbt'] = write_nbt(events, result['ticks'], prefix + '_notestudio.nbt')
        print(f'[产物] NBT {outputs["nbt"]["path"]} '
              f'size={outputs["nbt"]["size"]} 方块={outputs["nbt"]["blocks"]} '
              f'音符盒={outputs["nbt"]["note_blocks"]}')

    if args.compare:
        try:
            from eval_ntb_loss import audio_metrics
        except Exception as exc:      # noqa: BLE001
            print(f'[对比] 跳过：{exc}')
        else:
            decoded = {'events': [(e.seconds(), e.midi_pitch(), e.instrument)
                                  for e in events]}
            # tick 本身就是起音帧，没有播放延迟，故对齐偏移为 0
            metrics = audio_metrics(args.audio, decoded, offset_s=0.0)
            report['metrics'] = metrics
            print(f'[对比] 全局 chroma 余弦={metrics["global_chroma_cosine"]:.4f} '
                  f'(原始={metrics["global_chroma_cosine_raw"]:.4f})')
            print(f'[对比] 逐音符音级 top1={metrics["pitch_class_top1_pct"]:.1f}% '
                  f'top3={metrics["pitch_class_top3_pct"]:.1f}% '
                  f'中位排名={metrics["pitch_class_median_rank"]}')

    if args.json:
        json_path = prefix + '_notestudio.json'
        with open(json_path, 'w', encoding='utf-8') as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        outputs['json'] = json_path
        print(f'[产物] JSON {json_path}')

    report['outputs'] = outputs
    return 0


if __name__ == '__main__':
    sys.exit(main())
