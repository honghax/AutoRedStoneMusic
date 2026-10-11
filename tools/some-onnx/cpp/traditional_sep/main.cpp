// traditional_sep.cpp — 传统算法音源分离（纯 CPU、固定 6 类、能量门限）
//
// 与 Python 版 tools/some-onnx/src/traditional_separate.py 算法对齐：
//   STFT -> REPET-SIM(人声/伴奏，非局部中值滤波 + Wiener 软掩蔽)
//        -> HPSS(鼓/谐波，时间轴/频率轴中值滤波)
//        -> 谐波频段切分(贝斯<250Hz / 吉他250~1000Hz / 钢琴>1kHz / other=剩余)
// 能量占比 < 1.5% 的轨道丢弃。硬编码 7 物理线程。
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
static const int REPET_WIDTH = 200; // 非局部滤波窗口（帧）
static const int MED_KERNEL = 31;   // HPSS 中值滤波核（帧/频点）
static const double MIN_ENERGY = 0.015;
static const double BASS_HZ = 250.0, GUITAR_HZ = 1000.0, SR = 44100.0;

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
    y.assign(ns, 0.0);
    for (int n = 0; n < ns; ++n)
        y[n] = wsum[n] > 1e-9 ? acc[n] / wsum[n] : 0.0;   // Eigen FFT inv 归一化与否待确认，先不改
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

// ---------------- biquad butterworth（4 阶 = 2 级联，零相位 forward-backward） ----------------
struct Biquad { double b0, b1, b2, a1, a2; };

// cookbook 低通/高通节；fc 为归一化频率(0~0.5)；Q 分配 = 4 阶 butterworth 的两节 Q（0.5412, 1.3066）
static Biquad lowpass_section(double fc, double Q) {
    double w0 = 2.0 * M_PI * fc, c = std::cos(w0), s = std::sin(w0);
    double alpha = s / (2.0 * Q), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 - c) / 2.0 / a0; b.b1 = (1.0 - c) / a0; b.b2 = (1.0 - c) / 2.0 / a0;
    b.a1 = -2.0 * c / a0; b.a2 = (1.0 - alpha) / a0;
    return b;
}
static Biquad highpass_section(double fc, double Q) {
    double w0 = 2.0 * M_PI * fc, c = std::cos(w0), s = std::sin(w0);
    double alpha = s / (2.0 * Q), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 + c) / 2.0 / a0; b.b1 = -(1.0 + c) / a0; b.b2 = (1.0 + c) / 2.0 / a0;
    b.a1 = -2.0 * c / a0; b.a2 = (1.0 - alpha) / a0;
    return b;
}
// RBJ 带通（中心归一化频率 fo、Q），4 阶带通由两节不同 Q 级联近似
static Biquad bandpass_section(double fo, double Q) {
    double w0 = 2.0 * M_PI * fo, c = std::cos(w0), s = std::sin(w0);
    double alpha = s / (2.0 * Q), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = alpha / a0; b.b1 = 0.0; b.b2 = -alpha / a0;
    b.a1 = -2.0 * c / a0; b.a2 = (1.0 - alpha) / a0;
    return b;
}

static void butter_sos(double f1, double f2, int order, std::vector<Biquad> &sos) {
    // f1/f2: 截止 Hz；f2<=0 → 高通(f1)；f1<=0 → 低通(f2)；都>0 → 带通
    static const double QS[2] = {0.5412, 1.3066};
    sos.clear();
    if (f1 > 0 && f2 > 0) {
        double fo = std::sqrt(f1 * f2);
        for (int i = 0; i < order / 2; ++i) sos.push_back(bandpass_section(fo, QS[i]));
    } else if (f2 > 0) {
        for (int i = 0; i < order / 2; ++i) sos.push_back(lowpass_section(f2, QS[i]));
    } else {
        for (int i = 0; i < order / 2; ++i) sos.push_back(highpass_section(f1, QS[i]));
    }
}

static void biquad_apply(const std::vector<Biquad> &sos, const std::vector<double> &x,
                         std::vector<double> &y) {
    int n = (int)x.size();
    y.assign(n, 0.0);
    std::vector<double> cur = x;
    for (const auto &b : sos) {
        std::vector<double> out(n);
        double z1 = 0, z2 = 0;
        for (int i = 0; i < n; ++i) {
            double v = b.b0 * cur[i] + z1;
            z1 = b.b1 * cur[i] - b.a1 * v + z2;
            z2 = b.b2 * cur[i] - b.a2 * v;
            out[i] = v;
        }
        // backward（零相位）
        double z1r = 0, z2r = 0;
        for (int i = n - 1; i >= 0; --i) {
            double v = b.b0 * out[i] + z1r;
            z1r = b.b1 * out[i] - b.a1 * v + z2r;
            z2r = b.b2 * out[i] - b.a2 * v;
            out[i] = v;
        }
        cur.swap(out);
    }
    y = cur;
}

static double energy(const std::vector<double> &x) {
    double e = 0;
    for (double v : x) e += v * v;
    return e;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "用法: traditional_sep <input.wav> <outdir>" << std::endl;
        return 1;
    }
    std::string in = argv[1], outdir = argv[2];
    Audio a;
    if (!read_wav(in, a)) { std::cerr << "无法读取 WAV: " << in << std::endl; return 1; }
    long ns = (long)a.l.size();
    std::vector<double> mix(ns);
    for (long i = 0; i < ns; ++i) mix[i] = (a.l[i] + a.r[i]) * 0.5;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<double> win;
    make_hann(win, N_FFT);

    // 1) STFT
    std::vector<std::vector<cpx>> S;
    stft(mix, win, S);
    int T = (int)S.size(), F = N_FFT / 2 + 1;
    std::vector<std::vector<double>> Smag(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) Smag[t][k] = std::abs(S[t][k]);

    // 2) REPET-SIM：非局部中值（宽度 REPET_WIDTH）→ 伴奏掩蔽
    std::vector<std::vector<double>> Sfilter(T, std::vector<double>(F));
    {
        int half = REPET_WIDTH / 2;
        std::vector<std::thread> pool;
        int per = std::max(1, (T + NUM_THREADS - 1) / NUM_THREADS);
        for (int t0 = 0; t0 < T; t0 += per)
            pool.emplace_back([&, t0]() {
                std::vector<double> winv;
                for (int t = t0; t < std::min(T, t0 + per); ++t) {
                    int lo = std::max(0, t - half), hi = std::min(T - 1, t + half);
                    for (int k = 0; k < F; ++k) {
                        winv.clear();
                        for (int tt = lo; tt <= hi; ++tt) winv.push_back(Smag[tt][k]);
                        std::nth_element(winv.begin(), winv.begin() + winv.size() / 2, winv.end());
                        Sfilter[t][k] = std::min(Smag[t][k], winv[winv.size() / 2]);
                    }
                }
            });
        for (auto &th : pool) th.join();
    }
    std::vector<std::vector<double>> resi(T, std::vector<double>(F)), mask(T, std::vector<double>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) resi[t][k] = Smag[t][k] - Sfilter[t][k];
    softmask2(Sfilter, resi, mask);
    std::vector<std::vector<cpx>> Sv(T, std::vector<cpx>(F)), Sa(T, std::vector<cpx>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) {
            Sv[t][k] = S[t][k] * (1.0 - mask[t][k]);
            Sa[t][k] = S[t][k] * mask[t][k];
        }
    std::vector<double> vocals(ns), accomp(ns);
    istft(Sv, win, (int)ns, vocals);
    istft(Sa, win, (int)ns, accomp);

    // 3) HPSS：伴奏 → 谐波/打击乐
    std::vector<std::vector<double>> A(T, std::vector<double>(F)), Ah, Ap;
    for (int t = 0; t < T; ++t) for (int k = 0; k < F; ++k) A[t][k] = std::abs(Sa[t][k]);
    median_filter(A, 0, MED_KERNEL, Ah);   // 时间轴中值 → 谐波侧
    median_filter(A, 1, MED_KERNEL, Ap);   // 频率轴中值 → 打击侧
    std::vector<std::vector<double>> mh, mp;
    softmask2(Ah, Ap, mh);
    std::vector<std::vector<cpx>> Sh(T, std::vector<cpx>(F)), Sp(T, std::vector<cpx>(F));
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < F; ++k) {
            Sh[t][k] = Sa[t][k] * mh[t][k];
            Sp[t][k] = Sa[t][k] * (1.0 - mh[t][k]);
        }
    std::vector<double> drums(ns), harm(ns);
    istft(Sp, win, (int)ns, drums);
    istft(Sh, win, (int)ns, harm);

    // 4) 谐波频段切分
    std::vector<Biquad> sos_lp, sos_bp, sos_hp;
    butter_sos(0, BASS_HZ / (SR / 2), 4, sos_lp);
    butter_sos(BASS_HZ / (SR / 2), GUITAR_HZ / (SR / 2), 4, sos_bp);
    butter_sos(GUITAR_HZ / (SR / 2), 0, 4, sos_hp);
    std::vector<double> bass, guitar, piano, other;
    biquad_apply(sos_lp, harm, bass);
    biquad_apply(sos_bp, harm, guitar);
    biquad_apply(sos_hp, harm, piano);
    other.resize(ns);
    for (long i = 0; i < ns; ++i) other[i] = harm[i] - bass[i] - guitar[i] - piano[i];

    // 5) 能量门限 + 写盘
    const char *names[6] = {"drums", "bass", "other", "vocals", "guitar", "piano"};
    std::vector<std::vector<double>> stems = {drums, bass, other, vocals, guitar, piano};
    std::vector<double> en(6);
    double tot = 0;
    for (int i = 0; i < 6; ++i) { en[i] = energy(stems[i]); tot += en[i]; }
    fs::create_directories(outdir);
    for (int i = 0; i < 6; ++i) {
        double ratio = en[i] / (tot + 1e-12);
        if (ratio < MIN_ENERGY) {
            std::printf("  stem %-6s 能量占比 %.3f < %.3f → 丢弃\n", names[i], ratio, MIN_ENERGY);
            continue;
        }
        // 掩蔽稀疏谱会产生个别巨大尖峰（相位叠加），统一 clip 到 [-1,1] 再缩放，
        // 避免尖峰污染整体音量（这是"还原不像"的根因之一）
        std::vector<double> outl = stems[i], outr = stems[i];
        for (auto &v : outl) v = std::clamp(v, -1.0, 1.0) * 0.9;
        for (auto &v : outr) v = std::clamp(v, -1.0, 1.0) * 0.9;
        fs::path p = fs::path(outdir) / ("target_" + std::to_string(i) + "_" + names[i] + ".wav");
        write_wav_f32(p.string(), outl, outr, SR);
        std::printf("  stem %-6s 能量占比 %.3f → 保留 (%s)\n", names[i], ratio, p.string().c_str());
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("完成: %.1fs 处理 %.1fs 音频（%d 线程）\n", secs, ns / SR, NUM_THREADS);
    return 0;
}
