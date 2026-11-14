// traditional_sep.cpp — 传统算法音源分离（纯 CPU、固定 6 类、能量门限）
//
// 链路（v2）：
//   STFT -> HPSS(时间/频率轴中值，17/17 核) 拆 drums / harmonic
//        -> 对 harmonic 做 REPET-SIM(对数频带特征 + kNN 相似帧平均) 拆 vocals / instrumental
//        -> instrumental 在 STFT 域按平滑交叉权重切 bass / guitar / piano / other（权重和恒为 1）
// 掩蔽做 3x3 平滑并夹到 [MASK_FLOOR, 1-MASK_FLOOR]，抑制音乐噪声；
// 写盘时按所有轨道的统一峰值缩放，避免单轨归一化打乱相对音量。
//
// 用法: traditional_sep <input.wav> <outdir>
// 输出: <outdir>/target_0_drums.wav ... target_5_piano.wav（float32 立体声）
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <unsupported/Eigen/FFT>

namespace fs = std::filesystem;
using cpx = std::complex<double>;

static const int NUM_THREADS = 7;   // 硬编码物理线程数
static const int N_FFT = 2048;
static const int HOP = 512;
static const int HPSS_TIME_KERNEL = 17;   // 谐波方向（沿时间）中值核
static const int HPSS_FREQ_KERNEL = 17;   // 打击方向（沿频率）中值核
static const int REPET_STRIDE = 4;        // REPET 特征降采样步长（帧）
static const int REPET_BANDS = 96;        // 对数频带数
static const int REPET_NEIGHBORS = 6;     // 相似帧个数
static const int REPET_EXCLUDE = 3;       // 排除的邻近降采样帧（避免把自己当背景）
static const double MASK_FLOOR = 0.02;    // 掩蔽下限，避免频点被完全杀死
static const double MIN_ENERGY = 0.015;
static const double BASS_HZ = 250.0, GUITAR_HZ = 1500.0, PIANO_HZ = 4000.0, SR = 44100.0;

// ---------------- WAV IO（PCM 16bit / float32） ----------------
struct Audio { std::vector<double> l, r; double sr; };

static bool read_wav(const std::string &path, Audio &a) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char hdr[64];
    f.read(hdr, 64);
    if (std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) return false;
    uint16_t audio_fmt = *(uint16_t *)(hdr + 20);
    uint16_t channels = *(uint16_t *)(hdr + 22);
    uint32_t rate = *(uint32_t *)(hdr + 24);
    uint16_t bits = *(uint16_t *)(hdr + 34);
    // WAVEFORMATEXTENSIBLE（fmt=0xFFFE）：真实格式在 SubFormat GUID 前 2 字节（fmt chunk offset 24 = 文件 offset 44）
    if (audio_fmt == 0xFFFE) audio_fmt = *(uint16_t *)(hdr + 44);
    if (audio_fmt != 1 && audio_fmt != 3) return false;
    // 跳过到 data 块（找到时指针已越过 size 字段 = data 数据开头，data_bytes 取自 chunk 头）
    f.clear(); f.seekg(12, std::ios::beg);
    bool found = false;
    uint32_t data_bytes = 0;
    while (f) {
        char chunk[8];
        f.read(chunk, 8);
        if (!f) break;
        uint32_t sz = *(uint32_t *)(chunk + 4);
        if (!std::memcmp(chunk, "data", 4)) { found = true; data_bytes = sz; break; }
        f.seekg(sz + (sz & 1), std::ios::cur);
    }
    if (!found) return false;
    long nsamp = data_bytes / (bits / 8);
    if (nsamp <= 0) return false;
    a.l.resize(nsamp / channels);
    a.r = a.l;
    a.sr = rate;
    std::vector<uint8_t> buf(data_bytes);
    f.read((char *)buf.data(), data_bytes);
    for (long i = 0; i < nsamp; i += channels) {
        double s;
        if (bits == 16) {
            int16_t v; std::memcpy(&v, &buf[i * 2], 2); s = v / 32768.0;
        } else if (bits == 24) {
            // 3 字节小端，符号扩展
            int32_t v = (buf[i * 3] & 0xff) | ((buf[i * 3 + 1] & 0xff) << 8) | ((buf[i * 3 + 2] & 0xff) << 16);
            if (v & 0x800000) v |= ~0xffffff;
            s = v / 8388608.0;
        } else if (bits == 32 && audio_fmt == 3) {
            float v; std::memcpy(&v, &buf[i * 4], 4); s = v;
        } else {
            std::cerr << "不支持的 WAV 格式: fmt=" << audio_fmt << " bits=" << bits << std::endl;
            return false;
        }
        a.l[i / channels] = s;
        if (channels > 1) {
            if (bits == 16) { int16_t v; std::memcpy(&v, &buf[(i + 1) * 2], 2); s = v / 32768.0; }
            else if (bits == 24) {
                int32_t v = (buf[(i + 1) * 3] & 0xff) | ((buf[(i + 1) * 3 + 1] & 0xff) << 8) | ((buf[(i + 1) * 3 + 2] & 0xff) << 16);
                if (v & 0x800000) v |= ~0xffffff;
                s = v / 8388608.0;
            }
            else if (bits == 32 && audio_fmt == 3) { float v; std::memcpy(&v, &buf[(i + 1) * 4], 4); s = v; }
            a.r[i / channels] = s;
        } else a.r[i / channels] = s;
    }
    return true;
}

static void write_wav_f32(const std::string &path, const std::vector<double> &l,
                          const std::vector<double> &r, double sr) {
    uint32_t ns = (uint32_t)l.size();
    uint32_t data_bytes = ns * 8;
    std::ofstream f(path, std::ios::binary);
    f.write("RIFF", 4);
    uint32_t riff = 36 + data_bytes; f.write((char *)&riff, 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    uint32_t fmt_sz = 16; f.write((char *)&fmt_sz, 4);
    uint16_t fmt = 3, ch = 2, bits = 32;
    uint32_t rate = (uint32_t)sr, brate = rate * ch * bits / 8;
    uint16_t align = ch * bits / 8;
    f.write((char *)&fmt, 2); f.write((char *)&ch, 2);
    f.write((char *)&rate, 4); f.write((char *)&brate, 4);
    f.write((char *)&align, 2); f.write((char *)&bits, 2);
    f.write("data", 4); f.write((char *)&data_bytes, 4);
    for (uint32_t i = 0; i < ns; ++i) {
        float lf = (float)l[i], rf = (float)r[i];
        f.write((char *)&lf, 4); f.write((char *)&rf, 4);
    }
}

// ---------------- STFT / ISTFT（wOLA，hann 窗） ----------------
static void make_hann(std::vector<double> &w, int n) {
    w.resize(n);
    for (int i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));
}

static int n_frames(long ns) {
    return std::max(1L, (long)std::ceil((ns - N_FFT) / (double)HOP) + 1);
}

static void stft(const std::vector<double> &x, const std::vector<double> &win,
                 std::vector<std::vector<cpx>> &S) {
    long ns = (long)x.size();
    int T = n_frames(ns);
    S.assign(T, std::vector<cpx>(N_FFT / 2 + 1));
    std::vector<std::thread> pool;
    int per = std::max(1, (T + NUM_THREADS - 1) / NUM_THREADS);
    for (int t0 = 0; t0 < T; t0 += per) {
        pool.emplace_back([&, t0]() {
            Eigen::FFT<double> fft;
            std::vector<double> seg(N_FFT);
            std::vector<cpx> out(N_FFT);
            for (int t = t0; t < std::min(T, t0 + per); ++t) {
                long off = (long)t * HOP;
                for (int n = 0; n < N_FFT; ++n)
                    seg[n] = (off + n < ns) ? x[off + n] * win[n] : 0.0;
                fft.fwd(out, seg);
                for (int k = 0; k <= N_FFT / 2; ++k) S[t][k] = out[k];
            }
        });
    }
    for (auto &th : pool) th.join();
}

static void istft(const std::vector<std::vector<cpx>> &S, const std::vector<double> &win,
                  int ns, std::vector<double> &y) {
    int T = (int)S.size();
    int nout = (T - 1) * HOP + N_FFT;
    std::vector<double> acc(nout, 0.0), wsum(nout, 0.0);
    Eigen::FFT<double> fft;
    std::vector<cpx> in(N_FFT), out(N_FFT);
    for (int t = 0; t < T; ++t) {
        for (int k = 0; k < N_FFT / 2; ++k) in[k] = S[t][k];
        in[N_FFT / 2] = S[t][N_FFT / 2].real();
        for (int k = N_FFT / 2 + 1; k < N_FFT; ++k) in[k] = std::conj(S[t][N_FFT - k]);
        fft.inv(out, in);
        long off = (long)t * HOP;
        for (int n = 0; n < N_FFT; ++n) {
            if (off + n >= nout) break;
            acc[off + n] += out[n].real() * win[n];
            wsum[off + n] += win[n] * win[n];
        }
    }
    // 输入在 STFT 前两端各补了 N_FFT 个零，这里按同样偏移裁掉；
    // 边界处 wsum 仍有完整重叠，不会再出现 1e-3 量级的分母放大。
    y.assign(ns, 0.0);
    for (int n = 0; n < ns; ++n) {
        long idx = (long)n + N_FFT;
        if (idx >= nout) break;
        y[n] = wsum[idx] > 1e-6 ? acc[idx] / wsum[idx] : 0.0;
    }
}

// ---------------- 中值滤波（沿时间轴或频率轴） ----------------
static void median_filter(const std::vector<std::vector<double>> &X, int axis, int kern,
                          std::vector<std::vector<double>> &Y) {
    int T = (int)X.size(), F = (int)X[0].size();
    Y.assign(T, std::vector<double>(F));
    int half = kern / 2;
    std::vector<std::thread> pool;
    int per = std::max(1, (T + NUM_THREADS - 1) / NUM_THREADS);
    for (int t0 = 0; t0 < T; t0 += per) {
        pool.emplace_back([&, t0]() {
            std::vector<double> win;
            for (int t = t0; t < std::min(T, t0 + per); ++t) {
                if (axis == 0) { // 时间轴：对每个频点取邻域帧中值
                    for (int f = 0; f < F; ++f) {
                        win.clear();
                        for (int tt = std::max(0, t - half); tt <= std::min(T - 1, t + half); ++tt)
                            win.push_back(X[tt][f]);
                        std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
                        Y[t][f] = win[win.size() / 2];
                    }
                } else { // 频率轴：对每帧取邻域频点中值
                    for (int f = 0; f < F; ++f) {
                        win.clear();
                        for (int ff = std::max(0, f - half); ff <= std::min(F - 1, f + half); ++ff)
                            win.push_back(X[t][ff]);
                        std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
                        Y[t][f] = win[win.size() / 2];
                    }
                }
            }
        });
    }
    for (auto &th : pool) th.join();
}

// softmask(X, Y, power=2): X^2/(X^2+Y^2)
static void softmask2(const std::vector<std::vector<double>> &X,
                      const std::vector<std::vector<double>> &Y,
                      std::vector<std::vector<double>> &M) {
    int T = (int)X.size(), F = (int)X[0].size();
    M.assign(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t)
        for (int f = 0; f < F; ++f) {
            double x = X[t][f], y = Y[t][f];
            M[t][f] = x * x / (x * x + y * y + 1e-12);
        }
}

// 3x3 时频平滑 + 夹到 [floor, 1-floor]：抑制孤立频点造成的音乐噪声
static void smooth_and_clamp(std::vector<std::vector<double>> &M) {
    int T = (int)M.size(), F = (int)M[0].size();
    std::vector<std::vector<double>> out(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t) {
        int t0 = std::max(0, t - 1), t1 = std::min(T - 1, t + 1);
        for (int f = 0; f < F; ++f) {
            int f0 = std::max(0, f - 1), f1 = std::min(F - 1, f + 1);
            double acc = 0.0;
            for (int tt = t0; tt <= t1; ++tt)
                for (int ff = f0; ff <= f1; ++ff) acc += M[tt][ff];
            double v = acc / ((t1 - t0 + 1) * (f1 - f0 + 1));
            out[t][f] = std::min(1.0 - MASK_FLOOR, std::max(MASK_FLOOR, v));
        }
    }
    M.swap(out);
}

// ---------------- REPET-SIM：对数频带特征 + kNN 相似帧平均 ----------------
static void log_band_features(const std::vector<std::vector<double>> &X, int T, int F,
                              std::vector<std::vector<float>> &feat, int &Td) {
    const int bands = REPET_BANDS;
    std::vector<int> edge(bands + 1);
    const double loHz = 40.0, hiHz = 8000.0;
    for (int b = 0; b <= bands; ++b) {
        double hz = loHz * std::pow(hiHz / loHz, (double)b / bands);
        int bin = (int)std::llround(hz * N_FFT / SR);
        edge[b] = std::clamp(bin, 0, F - 1);
    }
    Td = std::max(1, (T + REPET_STRIDE - 1) / REPET_STRIDE);
    feat.assign(Td, std::vector<float>(bands, 0.f));
    for (int i = 0; i < Td; ++i) {
        int t = std::min(T - 1, i * REPET_STRIDE);
        double norm = 0.0;
        for (int b = 0; b < bands; ++b) {
            int a = edge[b], c = std::max(edge[b + 1], edge[b] + 1);
            double acc = 0.0;
            for (int k = a; k < c; ++k) acc += X[t][k];
            float v = (float)std::log1p(acc);
            feat[i][b] = v;
            norm += (double)v * v;
        }
        norm = std::sqrt(norm);
        if (norm > 1e-9)
            for (auto &v : feat[i]) v = (float)(v / norm);
    }
}

// 每个分析帧的"重复背景"幅度：取最相似的 k 个非邻近帧的平均幅度谱
static void repet_background(const std::vector<std::vector<double>> &X, int T, int F,
                             std::vector<std::vector<double>> &B) {
    std::vector<std::vector<float>> feat;
    int Td = 0;
    log_band_features(X, T, F, feat, Td);
    B.assign(T, std::vector<double>(F, 0.0));

    for (int i = 0; i < Td; ++i) {
        std::vector<std::pair<float, int>> scores;
        scores.reserve((size_t)Td);
        for (int j = 0; j < Td; ++j) {
            if (std::abs(i - j) <= REPET_EXCLUDE) continue;   // 邻近帧不算"重复"
            float dot = 0.f;
            for (int b = 0; b < REPET_BANDS; ++b) dot += feat[i][b] * feat[j][b];
            scores.emplace_back(dot, j);
        }
        int take = std::min(REPET_NEIGHBORS, (int)scores.size());
        if (take <= 0) continue;
        std::partial_sort(scores.begin(), scores.begin() + take, scores.end(),
                          [](const std::pair<float, int> &a, const std::pair<float, int> &b) {
                              return a.first > b.first;
                          });
        // 降采样帧 i 覆盖的原始帧共享同一组相似帧
        for (int t = i * REPET_STRIDE; t < std::min(T, (i + 1) * REPET_STRIDE); ++t) {
            std::vector<double> &dst = B[t];
            for (int n = 0; n < take; ++n) {
                const std::vector<double> &src = X[scores[n].second * REPET_STRIDE];
                for (int f = 0; f < F; ++f) dst[f] += src[f];
            }
            for (int f = 0; f < F; ++f) dst[f] /= take;
        }
    }
}

// 平滑低通权重：hz <= fc-width/2 → 1；hz >= fc+width/2 → 0；中间 raised-cosine
static double lowpass_weight(double hz, double fc, double width) {
    double edge = std::max(1.0, width * 0.5);
    if (hz <= fc - edge) return 1.0;
    if (hz >= fc + edge) return 0.0;
    double x = (hz - (fc - edge)) / (2.0 * edge);
    return 0.5 + 0.5 * std::cos(M_PI * x);
}

// 频段权重：bass/guitar/piano/other 四点，各频点权重和恒为 1
static void band_weights(int F, std::vector<double> &wb, std::vector<double> &wg,
                         std::vector<double> &wp, std::vector<double> &wo) {
    wb.resize(F); wg.resize(F); wp.resize(F); wo.resize(F);
    const double w1 = std::max(80.0, BASS_HZ * 0.5);
    const double w2 = std::max(200.0, GUITAR_HZ * 0.4);
    const double w3 = std::max(400.0, PIANO_HZ * 0.25);
    for (int k = 0; k < F; ++k) {
        double hz = (double)k * SR / N_FFT;
        double c1 = lowpass_weight(hz, BASS_HZ, w1);
        double c2 = lowpass_weight(hz, GUITAR_HZ, w2);
        double c3 = lowpass_weight(hz, PIANO_HZ, w3);
        wb[k] = c1;
        wg[k] = std::max(0.0, c2 - c1);
        wp[k] = std::max(0.0, c3 - c2);
        wo[k] = std::max(0.0, 1.0 - c3);
    }
}

static double energy(const std::vector<double> &x) {
    double e = 0;
    for (double v : x) e += v * v;
    return e;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "用法: traditional_sep <input.wav> <outdir> [--roles]" << std::endl;
        return 1;
    }
    std::string in = argv[1], outdir = argv[2];
    bool roles = false;                 // --roles: 只输出 drums / bass / harmonic 三条
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--roles") roles = true;
    }
    Audio a;
    if (!read_wav(in, a)) { std::cerr << "无法读取 WAV: " << in << std::endl; return 1; }
    long ns = (long)a.l.size();
    std::vector<double> mix(ns);
    for (long i = 0; i < ns; ++i) mix[i] = (a.l[i] + a.r[i]) * 0.5;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<double> win;
    make_hann(win, N_FFT);

    // 1) STFT（两端各补 N_FFT 个零，保证 WOLA 边界也有完整重叠）
    std::vector<double> mix_pad(N_FFT, 0.0);
    mix_pad.insert(mix_pad.end(), mix.begin(), mix.end());
    mix_pad.insert(mix_pad.end(), N_FFT, 0.0);
    std::vector<std::vector<cpx>> S;
    stft(mix_pad, win, S);
    int T = (int)S.size(), F = N_FFT / 2 + 1;
    std::vector<std::vector<double>> Smag(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) Smag[t][k] = std::abs(S[t][k]);

    // 2) HPSS：先拆打击/谐波（时间轴中值→谐波侧，频率轴中值→打击侧）
    std::vector<std::vector<double>> Hm, Pm;
    median_filter(Smag, 0, HPSS_TIME_KERNEL, Hm);
    median_filter(Smag, 1, HPSS_FREQ_KERNEL, Pm);
    std::vector<std::vector<double>> mh;
    softmask2(Hm, Pm, mh);
    smooth_and_clamp(mh);

    std::vector<std::vector<cpx>> Sp(T, std::vector<cpx>(F)), Sh(T, std::vector<cpx>(F));
    std::vector<std::vector<double>> Hmag(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) {
            Sp[t][k] = S[t][k] * (1.0 - mh[t][k]);
            Sh[t][k] = S[t][k] * mh[t][k];
            Hmag[t][k] = std::abs(Sh[t][k]);
        }
    std::vector<double> drums(ns);
    istft(Sp, win, (int)ns, drums);

    // 诊断：HPSS 的谱能量分配
    double eh = 0, ep = 0, mh_sum = 0;
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) {
            eh += std::norm(Sh[t][k]);
            ep += std::norm(Sp[t][k]);
            mh_sum += mh[t][k];
        }
    std::printf("  HPSS: harmonic=%.3f percussive=%.3f mh_mean=%.3f\n",
                eh / (eh + ep + 1e-12), ep / (eh + ep + 1e-12),
                mh_sum / (double)T / F);
    {
        double ed = 0, em = 0;
        for (double v : drums) ed += v * v;
        for (double v : mix) em += v * v;
        std::printf("  HPSS waveform: drums_rms=%.5f mix_rms=%.5f\n",
                    std::sqrt(ed / (double)drums.size()), std::sqrt(em / (double)mix.size()));
    }

    // 3) REPET-SIM：谐波分量 → 重复背景(伴奏) / 非重复前景(人声)
    std::vector<std::vector<double>> background;
    repet_background(Hmag, T, F, background);
    std::vector<std::vector<double>> foreground(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k)
            foreground[t][k] = std::max(0.0, Hmag[t][k] - background[t][k]);
    std::vector<std::vector<double>> mfg;
    softmask2(foreground, background, mfg);   // 前景权重
    smooth_and_clamp(mfg);

    std::vector<std::vector<cpx>> Sv(T, std::vector<cpx>(F)), Si(T, std::vector<cpx>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) {
            Sv[t][k] = Sh[t][k] * mfg[t][k];
            Si[t][k] = Sh[t][k] * (1.0 - mfg[t][k]);
        }
    std::vector<double> vocals(ns);
    istft(Sv, win, (int)ns, vocals);

    // 诊断：REPET 的前景/背景谱能量分配
    double ef = 0, eb = 0;
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) { ef += std::norm(Sv[t][k]); eb += std::norm(Si[t][k]); }
    std::printf("  REPET: foreground=%.3f background=%.3f\n", ef / (ef + eb + 1e-12),
                eb / (ef + eb + 1e-12));

    // 4) 谐波频段切分：STFT 域平滑交叉权重，四段和恒等于 instrumental
    std::vector<double> wb, wg, wp, wo;
    band_weights(F, wb, wg, wp, wo);
    std::vector<std::vector<cpx>> Sb(T, std::vector<cpx>(F)), Sg(T, std::vector<cpx>(F));
    std::vector<std::vector<cpx>> Spn(T, std::vector<cpx>(F)), So(T, std::vector<cpx>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) {
            const cpx &v = Si[t][k];
            Sb[t][k] = v * wb[k];
            Sg[t][k] = v * wg[k];
            Spn[t][k] = v * wp[k];
            So[t][k] = v * wo[k];
        }
    std::vector<double> bass(ns), guitar(ns), piano(ns), other(ns);
    istft(Sb, win, (int)ns, bass);
    istft(Sg, win, (int)ns, guitar);
    istft(Spn, win, (int)ns, piano);
    istft(So, win, (int)ns, other);

    // 5) 能量门限 + 统一峰值缩放 + 写盘
    // --roles：按下游只认 drums/bass/harp/basedrum 的事实，把谐波段并成一条 harmonic，
    // 避免同一条旋律被 vocals/guitar/piano 各转写一次产生八度重复音符。
    const char *names6[6] = {"drums", "bass", "other", "vocals", "guitar", "piano"};
    std::vector<std::vector<double>> stems6 = {drums, bass, other, vocals, guitar, piano};
    std::vector<std::vector<double>> stems;
    std::vector<std::string> names;
    if (roles) {
        std::vector<std::vector<cpx>> Shr(T, std::vector<cpx>(F));
        for (int t = 0; t < T; ++t)
            for (int k = 0; k < F; ++k)
                Shr[t][k] = Sh[t][k] * (1.0 - wb[k]);   // 谐波 - bass
        std::vector<double> harmonic(ns);
        istft(Shr, win, (int)ns, harmonic);
        stems = {drums, bass, harmonic};
        names = {"drums", "bass", "harmonic"};
    } else {
        stems = stems6;
        for (const char *n : names6) names.emplace_back(n);
    }

    const int count = (int)stems.size();
    std::vector<double> en(count);
    double tot = 0;
    for (int i = 0; i < count; ++i) { en[i] = energy(stems[i]); tot += en[i]; }

    // 所有轨道共用同一个缩放系数，保持相对音量不变；避免硬 clip 引入失真
    double peak = 0.0;
    for (const auto &s : stems)
        for (double v : s) peak = std::max(peak, std::abs(v));
    double gain = peak > 0.95 ? 0.95 / peak : 1.0;

    fs::create_directories(outdir);
    for (int i = 0; i < count; ++i) {
        double ratio = en[i] / (tot + 1e-12);
        if (ratio < MIN_ENERGY) {
            std::printf("  stem %-8s 能量占比 %.3f < %.3f → 丢弃\n", names[i].c_str(), ratio, MIN_ENERGY);
            continue;
        }
        std::vector<double> outl(stems[i].size()), outr(stems[i].size());
        for (size_t n = 0; n < stems[i].size(); ++n) outl[n] = outr[n] = stems[i][n] * gain;
        fs::path p = fs::path(outdir) / ("target_" + std::to_string(i) + "_" + names[i] + ".wav");
        write_wav_f32(p.string(), outl, outr, SR);
        std::printf("  stem %-8s 能量占比 %.3f → 保留 (%s)\n", names[i].c_str(), ratio, p.string().c_str());
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("完成: %.1fs 处理 %.1fs 音频（%d 线程）\n", secs, ns / SR, NUM_THREADS);
    return 0;
}
