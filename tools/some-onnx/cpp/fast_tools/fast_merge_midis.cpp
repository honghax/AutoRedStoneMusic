#include "midi_io.h"
#include "model.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int TICKS = 480;
constexpr int TEMPO = 500000;

struct SourceNote {
    double start = 0.0;
    double duration = 0.0;
    int note = 0;
    int channel = 0;
};

std::vector<SourceNote> notesFrom(const MidiFileData& midi, int channel) {
    std::vector<SourceNote> out;
    double tpq = static_cast<double>(midi.tpq);
    for (const auto& track : midi.tracks) {
        int tempo = TEMPO;
        double now = 0.0;
        std::map<int, double> pending;
        for (const auto& ev : track.events) {
            double scale = static_cast<double>(tempo) * 1e-6 / tpq;
            now += static_cast<double>(ev.delta) * scale;
            if (ev.kind == EvKind::SetTempo) {
                tempo = ev.tempo;
            } else if (ev.kind == EvKind::NoteOn) {
                pending[ev.note] = now;
            } else if (ev.kind == EvKind::NoteOff) {
                auto it = pending.find(ev.note);
                if (it != pending.end()) {
                    double start = it->second;
                    pending.erase(it);
                    if (now > start) {
                        out.push_back(SourceNote{start, now - start, ev.note, channel});
                    }
                }
            }
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: fast_merge_midis input1.mid [input2.mid ...] output.mid\n");
        return 1;
    }
    std::string output = argv[argc - 1];

    std::vector<SourceNote> notes;
    for (int i = 1; i < argc - 1; ++i) {
        MidiFileData midi;
        std::string error;
        if (!readMidiFile(argv[i], midi, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::vector<SourceNote> part = notesFrom(midi, i - 1);
        notes.insert(notes.end(), part.begin(), part.end());
    }

    std::stable_sort(notes.begin(), notes.end(),
                     [](const SourceNote& a, const SourceNote& b) {
                         return a.start < b.start;
                     });

    double scale = static_cast<double>(TICKS) * (1e6 / static_cast<double>(TEMPO));
    std::vector<MergeNoteEvent> events;
    events.reserve(notes.size() * 2);
    for (const SourceNote& n : notes) {
        int t_on = pyRound(n.start * scale);
        int t_off = std::max(t_on + 1,
                             pyRound((n.start + n.duration) * scale));
        events.push_back(MergeNoteEvent{t_on, true, n.note, n.channel});
        events.push_back(MergeNoteEvent{t_off, false, n.note, n.channel});
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const MergeNoteEvent& a, const MergeNoteEvent& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return (a.on ? 1 : 0) < (b.on ? 1 : 0);
                     });

    std::string error;
    if (!writeMergedMidi(output, TICKS, TEMPO, events, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    int pmin = notes.empty() ? 0 : notes.front().note;
    int pmax = pmin;
    for (const SourceNote& n : notes) {
        pmin = std::min(pmin, n.note);
        pmax = std::max(pmax, n.note);
    }
    std::printf("merged %zu notes (pitch %d~%d, %d sources) -> %s\n",
                notes.size(), pmin, pmax, argc - 2, output.c_str());
    return 0;
}
