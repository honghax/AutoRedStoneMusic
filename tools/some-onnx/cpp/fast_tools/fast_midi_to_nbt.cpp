#include "gen_machine.h"
#include "midi_io.h"
#include "model.h"
#include "nbt_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string input, output;
    std::vector<std::string> positional;
    double seconds = 10.0;
    bool has_seconds = false;
    int resolution = 2;
    bool sustain = false;
    int sustain_interval = 2;
    std::string layout = "folded";
    std::string track;
    bool has_track = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto take_value = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (a == "--seconds") {
            std::string v;
            if (!take_value(v)) { std::fprintf(stderr, "missing value for --seconds\n"); return 2; }
            seconds = std::stod(v);
            has_seconds = true;
        } else if (a == "--resolution") {
            std::string v;
            if (!take_value(v)) { std::fprintf(stderr, "missing value for --resolution\n"); return 2; }
            resolution = std::stoi(v);
        } else if (a == "--sustain") {
            sustain = true;
        } else if (a == "--sustain-interval") {
            std::string v;
            if (!take_value(v)) { std::fprintf(stderr, "missing value for --sustain-interval\n"); return 2; }
            sustain_interval = std::stoi(v);
        } else if (a == "--layout") {
            if (!take_value(layout)) { std::fprintf(stderr, "missing value for --layout\n"); return 2; }
        } else if (a == "--track") {
            if (!take_value(track)) { std::fprintf(stderr, "missing value for --track\n"); return 2; }
            has_track = true;
        } else if (startsWith(a, "--")) {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() < 2) {
        std::fprintf(stderr, "usage: fast_midi_to_nbt <input.mid> <output.nbt> [options]\n");
        return 2;
    }
    input = positional[0];
    output = positional[1];

    if (resolution != 2 && resolution != 4 && resolution != 6 && resolution != 8) {
        std::fprintf(stderr, "resolution must be 2/4/6/8\n");
        return 2;
    }
    if (layout != "folded" && layout != "simple") {
        std::fprintf(stderr, "layout must be folded or simple\n");
        return 2;
    }
    if (has_track && track != "drums" && track != "bass" &&
        track != "other" && track != "vocals") {
        std::fprintf(stderr, "track must be drums/bass/other/vocals\n");
        return 2;
    }

    try {
        MidiFileData midi;
        std::string error;
        if (!readMidiFile(input, midi, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        double col_seconds = static_cast<double>(resolution) / 20.0;
        if (!has_seconds) {
            double length = midiLengthSeconds(midi);
            int columns0 = std::max(1, pyRound(std::max(length, 0.1) / col_seconds));
            double raw = static_cast<double>(columns0) * col_seconds;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6f", raw);
            seconds = std::stod(buf);
            has_seconds = true;
        }

        int columns = pyRound(seconds / col_seconds);
        bool simple = layout == "simple";
        std::string* track_ptr = has_track ? &track : nullptr;

        std::vector<Note> all_notes = loadNotes(midi);
        if (all_notes.empty()) {
            std::fprintf(stderr, "MIDI has no notes\n");
            return 1;
        }

        struct Quantized {
            int pitch;
            int c0;
            int duration;
        };
        std::vector<Quantized> notes_q;
        for (const Note& n : all_notes) {
            if (n.start >= seconds) continue;
            int c0 = pyRound(n.start / col_seconds);
            if (!(c0 >= 0 && c0 < columns)) continue;
            int D = std::max(1, pyRound((n.end - n.start) / col_seconds));
            notes_q.push_back(Quantized{n.pitch, c0, D});
        }

        std::vector<int> pitches;
        pitches.reserve(all_notes.size());
        for (const Note& n : all_notes) pitches.push_back(n.pitch);
        PitchMap conv = buildPitchMap(pitches, track_ptr);

        std::map<int, std::set<NoteKey>> col_key;
        for (const Quantized& q : notes_q) {
            MappedNote m = conv.conv(q.pitch);
            NoteKey key{m.inst, m.mat, m.note};
            std::vector<int> cols{q.c0};
            if (sustain && q.duration > 1) {
                int K = std::max(1, sustain_interval);
                for (int c = q.c0 + K; c < q.c0 + q.duration; c += K) cols.push_back(c);
            }
            for (int c : cols) {
                if (c >= columns) continue;
                col_key[c].insert(key);
            }
        }

        std::map<int, std::vector<MappedNote>> col_notes;
        for (auto& kv : col_key) {
            std::vector<MappedNote> arr;
            arr.reserve(kv.second.size());
            for (const auto& key : kv.second) {
                arr.push_back(MappedNote{std::get<0>(key), std::get<1>(key), std::get<2>(key)});
            }
            std::stable_sort(arr.begin(), arr.end(), [](const MappedNote& a, const MappedNote& b) {
                return std::make_pair(a.note, a.inst) < std::make_pair(b.note, b.inst);
            });
            col_notes[kv.first] = std::move(arr);
        }

        int max_per_col = 0;
        int total_notes = 0;
        for (const auto& kv : col_notes) {
            max_per_col = std::max(max_per_col, static_cast<int>(kv.second.size()));
            total_notes += static_cast<int>(kv.second.size());
        }

        BuildResult result = buildStructure(resolution, col_notes, max_per_col,
                                            simple, track_ptr, nullptr);
        std::vector<std::string> problems;
        if (!validateStructure(result, resolution, problems)) {
            for (const auto& p : problems) std::fprintf(stderr, "validation: %s\n", p.c_str());
            return 1;
        }
        if (max_per_col > 128) {
            std::fprintf(stderr, "max notes per column exceeds Z cap\n");
            return 1;
        }

        int sx = 0, sy = 0, sz = 0;
        if (!writeStructureNbt(output, result.blocks, sx, sy, sz, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        std::printf("notes=%d columns=%d blocks=%zu note_blocks=%zu size=%d,%d,%d\n",
                    total_notes, columns, result.blocks.size(), result.note_blocks.size(),
                    sx, sy, sz);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
