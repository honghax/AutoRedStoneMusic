# -*- coding: utf-8 -*-
"""传统算法音源分离（纯 CPU、固定 6 类、能量门限丢弃弱轨）。

方法全部来自开源库 librosa 的标准算法，不引入任何机器学习模型：
  1. 人声/伴奏：REPET-SIM（Rafii & Pardo 2012，librosa 官方实现：nn_filter 非局部
     中值滤波 + Wiener 软掩蔽），人声=前景、伴奏=背景。
  2. 鼓：伴奏谱做 HPSS（谐波-打击乐分离，librosa.effects.hpss）取打击乐成分。
  3. 贝斯/吉他/钢琴：伴奏谐波成分按频段切分（低频 250Hz 以下=贝斯；
     中频 250~1000Hz=吉他；1kHz 以上=钢琴）。频段切分是近似，串音高于 AI 模型。
  4. other：谐波成分减去上述三类后的剩余。

能量门限：每轨能量占比 < MIN_ENERGY_RATIO(1.5%) 的轨道直接丢弃（不写盘），
避免把近静音/噪声轨转写成一堆假音符。

用法：
  python traditional_separate.py <input.wav> <outdir>
输出（命名与 demucs_mt.cpp 一致，可直接接现有转写流程）：
  <outdir>/target_0_drums.wav  target_1_bass.wav  target_2_other.wav
  <outdir>/target_3_vocals.wav target_4_guitar.wav target_5_piano.wav
"""
import sys
import time
import pathlib
import numpy as np
import soundfile as sf
import librosa

SR = 44100
MIN_ENERGY_RATIO = 0.015          # 能量占比低于 1.5% 的轨道丢弃
BAND_BASS_HZ = 250.0              # 贝斯低通
BAND_GUITAR_HZ = 1000.0           # 吉他/钢琴频段分界


def band_filter(harm, sr, lo, hi=None, order=4):
    """对谐波成分做带通/低通/高通（零相位 sosfiltfilt）。"""
    from scipy.signal import butter, sosfiltfilt
    ny = sr / 2.0
    if lo and hi:
        sos = butter(order, [max(lo, 1.0) / ny, min(hi, ny - 1.0) / ny], btype='band', output='sos')
    elif hi:
        sos = butter(order, min(hi, ny - 1.0) / ny, btype='low', output='sos')
    else:
        sos = butter(order, max(lo, 1.0) / ny, btype='high', output='sos')
    return sosfiltfilt(sos, harm)


def separate(y, sr):
    """y: 单声道 float；返回 dict(6 轨) 波形。"""
    S_full, phase = librosa.magphase(librosa.stft(y))
    # --- REPET-SIM：伴奏估计 = 非局部中值滤波 ---
    S_filter = librosa.decompose.nn_filter(S_full, aggregate=np.median,
                                           metric='cosine', width=200)
    S_filter = np.minimum(S_full, S_filter)
    # --- Wiener 软掩蔽：掩蔽更接近伴奏的部分 → 背景 ---
    mask_bg = librosa.util.softmask(S_filter, 1.0 * (S_full - S_filter), power=2)
    S_background = mask_bg * S_full
    S_foreground = (1 - mask_bg) * S_full
    vocals = librosa.istft(S_foreground * phase)
    accomp = librosa.istft(S_background * phase)

    # --- HPSS：伴奏 → 打击乐（鼓）/ 谐波（贝斯+吉他+钢琴+other） ---
    harm, perc = librosa.effects.hpss(accomp)
    drums = perc

    # --- 谐波成分按频段切分为 贝斯/吉他/钢琴，剩余 = other ---
    bass = band_filter(harm, sr, None, BAND_BASS_HZ)
    guitar = band_filter(harm, sr, BAND_BASS_HZ, BAND_GUITAR_HZ)
    piano = band_filter(harm, sr, BAND_GUITAR_HZ)
    other = harm - bass - guitar - piano
    return {'vocals': vocals, 'drums': drums, 'bass': bass,
            'other': other, 'guitar': guitar, 'piano': piano}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    input_wav, outdir = sys.argv[1], sys.argv[2]
    y, sr = sf.read(input_wav)
    if y.ndim == 2:
        y = y.mean(axis=1)
    t0 = time.time()
    stems = separate(y, sr)
    dt = time.time() - t0

    order = {'drums': 0, 'bass': 1, 'other': 2, 'vocals': 3, 'guitar': 4, 'piano': 5}
    tot = sum(float(np.sum(stems[k] ** 2)) for k in order)
    p = pathlib.Path(outdir)
    p.mkdir(parents=True, exist_ok=True)
    kept, dropped = [], []
    for name, idx in order.items():
        ratio = np.sum(stems[name] ** 2) / (tot + 1e-12)
        if ratio < MIN_ENERGY_RATIO:
            dropped.append((name, ratio))
            print(f'  stem {name:<6} 能量占比 {ratio:.3f}  < {MIN_ENERGY_RATIO:.3f} → 丢弃')
            continue
        kept.append(name)
        mono = stems[name]
        stereo = np.stack([mono, mono], axis=1)
        peak = np.max(np.abs(stereo)) + 1e-12
        sf.write(str(p / f'target_{idx}_{name}.wav'), (stereo * 0.95 / peak).astype(np.float32), sr)
        print(f'  stem {name:<6} 能量占比 {ratio:.3f}  → 保留')
    print(f'完成: {dt:.1f}s 处理 {len(y)/sr:.1f}s 音频 → {p}（保留 {kept}，丢弃 {[d[0] for d in dropped]}）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
