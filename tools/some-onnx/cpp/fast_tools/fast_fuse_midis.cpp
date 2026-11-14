// fuse_midis.cpp — 音符级投票融合：保留主转写中能在分离 stem 转写里找到匹配的音符。
//
// 动机：单次全混音转写召回高但含伪音符；分轨转写精度高但漏检。
// 取主转写(mix) ∩ 各 stem 转写(harmonic/bass) 的交集，可在音符级同时提高精度。
//
// 用法:
//   fuse_midis --primary mix.mid --pool harm.mid [--pool bass.mid]
//              --out fused.mid [--tol 0.12] [--semis 0]
//
// 匹配规则：|起始时间差| <= tol 秒 且 |音高差| <= semis 半音。
#include "gen_machine.h"
#include "midi_io.h"
#include "model.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

constexpr int TPQ = 480;
constexpr int TEMPO = 500000;              // 120 BPM
inline int secondsToTicks(double s) {
    return static_cast<int>(std::lround(s * 2.0 * TPQ));
}

} // namespace

int main(int argc, char** argv) {
    std::string primary, out_path;
    std::vector<std::string> pools;
    double tol = 0.12;
    int semis = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto take = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (a == "--primary") {
            if (!take(primary)) { std::fprintf(stderr, "missing --primary\n"); return 2; }
        } else if (a == "--pool") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --pool\n"); return 2; }
            pools.push_back(v);
        } else if (a == "--out") {
            if (!take(out_path)) { std::fprintf(stderr, "missing --out\n"); return 2; }
        } else if (a == "--tol") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --tol\n"); return 2; }
            tol = std::stod(v);
        } else if (a == "--semis") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --semis\n"); return 2; }
            semis = std::stoi(v);
        } else if (startsWith(a, "--")) {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }
    if (primary.empty() || pools.empty() || out_path.empty()) {
        std::fprintf(stderr,
            "usage: fuse_midis --primary mix.mid --pool stem.mid [--pool ...] "
            "--out fused.mid [--tol 0.12] [--semis 0]\n");
        return 2;
    }

    try {
        std::string error;
        MidiFileData pm;
        if (!readMidiFile(primary, pm, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::vector<Note> primary_notes = loadNotes(pm);

        std::vector<Note> pool_notes;
        for (const std::string& p : pools) {
            MidiFileData mm;
            if (!readMidiFile(p, mm, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::vector<Note> n = loadNotes(mm);
            pool_notes.insert(pool_notes.end(), n.begin(), n.end());
        }
        std::sort(pool_notes.begin(), pool_notes.end(),
                  [](const Note& a, const Note& b) { return a.start < b.start; });

        std::vector<double> starts;
        starts.reserve(pool_notes.size());
        for (const Note& n : pool_notes) starts.push_back(n.start);

        auto matched = [&](const Note& n) {
            auto lo = std::lower_bound(starts.begin(), starts.end(), n.start - tol);
            for (auto it = lo; it != starts.end() && *it <= n.start + tol; ++it) {
                const Note& cand = pool_notes[static_cast<std::size_t>(it - starts.begin())];
                if (std::abs(cand.pitch - n.pitch) <= semis) return true;
            }
            return false;
        };

        std::vector<MergeNoteEvent> events;
        int matched_notes = 0;
        for (const Note& n : primary_notes) {
            if (!matched(n)) continue;
            ++matched_notes;
            int on = secondsToTicks(n.start);
            int off = std::max(on + 6, secondsToTicks(n.end));
            events.push_back(MergeNoteEvent{on, true, n.pitch, 0});
            events.push_back(MergeNoteEvent{off, false, n.pitch, 0});
        }
        std::stable_sort(events.begin(), events.end(),
                         [](const MergeNoteEvent& a, const MergeNoteEvent& b) {
                             if (a.tick != b.tick) return a.tick < b.tick;
                             return a.on && !b.on;      // 同一刻先 off 后 on
                         });

        if (!writeMergedMidi(out_path, TPQ, TEMPO, events, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("fuse: primary=%zu pool=%zu kept=%d -> %s\n",
                    primary_notes.size(), pool_notes.size(), matched_notes, out_path.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
