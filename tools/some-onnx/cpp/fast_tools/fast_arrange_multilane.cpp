#include "gen_machine.h"
#include "midi_io.h"
#include "model.h"
#include "nbt_writer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct LaneInfo {
    std::map<int, std::vector<MappedNote>> col_notes;
    LaneStats stats;
    std::vector<std::size_t> indices;
    int z_offset = 0;
    int pitch_min = 0;
    int pitch_max = 0;
};

// ---------------------------------------------------------------------------
// instrument 模式：按 Morpheus/MuScriptor 的轨道跟踪逻辑（channel 9 → drum，
// 否则 GM program // 8 → family），把每个音符固定到它所属的乐器轨。
// 轨道数 = 乐器数；各轨共享同一条时间轴与同一个拉杆起始总线。
// ---------------------------------------------------------------------------

struct RoleNote {
    int pitch = 0;
    double start = 0.0;
    double end = 0.0;
    std::string role;
};

struct InstrumentLane {
    std::string inst;
    std::map<int, std::vector<MappedNote>> cols;
    int peak = 0;
    int z = 0;
};

const char* const FAMILY_MAP[16] = {
    "pno", "pno", "pad", "lead", "bass", "pad", "pad", "lead",
    "lead", "lead", "lead", "pad", "pad", "lead", "drum", "drum"
};

const Bank LANE_BANKS[3] = {
    {"bass", "minecraft:bamboo_planks", 30},
    {"harp", "minecraft:dirt", 54},
    {"bell", "minecraft:gold_block", 78},
};

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string classifyRole(int channel, int program, const std::string& track_name) {
    if (channel == 9 || toLower(track_name).find("drum") != std::string::npos) return "drum";
    int family = program / 8;
    if (family < 0 || family > 15) family = 3;
    return FAMILY_MAP[family];
}

MappedNote mapRoleNote(const RoleNote& n, int shift) {
    if (n.role == "drum") return MappedNote{"basedrum", "minecraft:stone", 0};
    int q = n.pitch + shift;
    const Bank* b = nullptr;
    if (n.role == "bass") {
        b = &LANE_BANKS[0];
    } else if (q < 54) {
        b = &LANE_BANKS[0];
    } else if (q > 78) {
        b = &LANE_BANKS[2];
    } else {
        b = &LANE_BANKS[1];
    }
    int v = q - b->base;
    while (v < 0) v += 12;
    while (v > 24) v -= 12;
    return MappedNote{b->inst, b->mat, v};
}

void loadRoleNotes(const MidiFileData& midi, std::vector<RoleNote>& out) {
    const double tpq = static_cast<double>(midi.tpq);
    for (const auto& track : midi.tracks) {
        int tempo = 500000;
        double now = 0.0;
        int program = 0;
        std::map<std::pair<int, int>, std::pair<double, int>> pending;
        for (const auto& ev : track.events) {
            now += static_cast<double>(ev.delta) *
                   (static_cast<double>(tempo) * 1e-6 / tpq);
            if (ev.kind == EvKind::SetTempo) {
                tempo = ev.tempo;
            } else if (ev.kind == EvKind::ProgramChange) {
                program = ev.program;
            } else if (ev.kind == EvKind::NoteOn) {
                pending[std::make_pair(ev.channel, ev.note)] =
                    std::make_pair(now, program);
            } else if (ev.kind == EvKind::NoteOff) {
                auto it = pending.find(std::make_pair(ev.channel, ev.note));
                if (it != pending.end()) {
                    double start = it->second.first;
                    int prog = it->second.second;
                    pending.erase(it);
                    if (now > start) {
                        out.push_back(RoleNote{ev.note, start, now,
                                               classifyRole(ev.channel, prog, track.name)});
                    }
                }
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const RoleNote& a, const RoleNote& b) {
        return a.start < b.start;
    });
}

void dedupeColumn(std::vector<MappedNote>& items) {
    std::set<std::pair<std::string, int>> seen;
    std::vector<MappedNote> uniq;
    for (const auto& m : items) {
        if (seen.insert(std::make_pair(m.inst, m.note)).second) uniq.push_back(m);
    }
    std::stable_sort(uniq.begin(), uniq.end(),
                     [](const MappedNote& a, const MappedNote& b) { return a.note < b.note; });
    items = std::move(uniq);
}

void refreshPeak(InstrumentLane& lane) {
    lane.peak = 0;
    for (const auto& kv : lane.cols) {
        lane.peak = std::max(lane.peak, static_cast<int>(kv.second.size()));
    }
}

void layoutInstrumentLanes(std::vector<InstrumentLane>& lanes, int gap, int& bus_half) {
    int pos = 0;
    for (auto& lane : lanes) {
        lane.z = pos;
        pos += 2 + std::max(1, lane.peak) + 1 + gap;
    }
    bus_half = 0;
    if (lanes.empty()) return;
    int shift = -pyRound(lanes.back().z / 2.0);
    for (auto& lane : lanes) {
        lane.z += shift;
        bus_half = std::max(bus_half, std::abs(lane.z));
    }
}

int runInstrumentSplit(const std::string& midi_path, const std::string& out_path,
                       const std::string& json_path, int resolution, int lane_gap,
                       double align_lead) {
    std::string error;
    MidiFileData midi;
    if (!readMidiFile(midi_path, midi, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::vector<RoleNote> notes;
    loadRoleNotes(midi, notes);
    if (notes.empty()) {
        std::fprintf(stderr, "MIDI has no notes\n");
        return 1;
    }

    // 在线模型返回的 MIDI 可能带前导静音/补齐到小节；把首个音符对齐到原曲起音。
    if (align_lead >= 0.0) {
        double first = notes.front().start;
        for (const auto& n : notes) first = std::min(first, n.start);
        double delta = align_lead - first;
        std::vector<RoleNote> shifted;
        shifted.reserve(notes.size());
        for (const auto& n : notes) {
            RoleNote m = n;
            m.start = std::max(0.0, n.start + delta);
            m.end = std::max(m.start, n.end + delta);
            shifted.push_back(m);
        }
        notes.swap(shifted);
        std::printf("align_lead: first=%.3fs target=%.3fs delta=%+.3fs\n",
                    first, align_lead, delta);
    }

    double col_seconds = static_cast<double>(resolution) / 20.0;
    double last_end = 0.0;
    for (const auto& n : notes) last_end = std::max(last_end, n.end);
    int columns = std::max(1, pyRound(last_end / col_seconds));

    std::map<std::string, std::map<int, std::vector<MappedNote>>> lane_cols;
    std::size_t dropped = 0;
    for (const auto& n : notes) {
        int c0 = pyRound(n.start / col_seconds);
        if (!(c0 >= 0 && c0 < columns)) {
            ++dropped;
            continue;
        }
        MappedNote m = mapRoleNote(n, 0);
        lane_cols[m.inst][c0].push_back(m);
    }

    std::vector<InstrumentLane> lanes;
    for (const char* name : {"bass", "harp", "bell", "basedrum"}) {
        auto it = lane_cols.find(name);
        if (it == lane_cols.end()) continue;
        InstrumentLane lane;
        lane.inst = name;
        lane.cols = std::move(it->second);
        for (auto& kv : lane.cols) dedupeColumn(kv.second);
        refreshPeak(lane);
        lanes.push_back(std::move(lane));
    }
    if (lanes.empty()) {
        std::fprintf(stderr, "all lanes are empty\n");
        return 1;
    }

    int bus_half = 0;
    layoutInstrumentLanes(lanes, lane_gap, bus_half);
    if (bus_half > 14) {
        // 总线是 0 延迟红石粉，最远轨道须 <=14 格；超限时把 bell 轨并入其它轨
        auto bell = std::find_if(lanes.begin(), lanes.end(),
                                 [](const InstrumentLane& l) { return l.inst == "bell"; });
        if (bell != lanes.end() && lanes.size() > 1) {
            auto target = std::find_if(lanes.begin(), lanes.end(),
                                       [](const InstrumentLane& l) { return l.inst == "harp"; });
            if (target == lanes.end()) {
                target = std::find_if(lanes.begin(), lanes.end(),
                    [](const InstrumentLane& l) { return l.inst != "bell"; });
            }
            for (const auto& kv : bell->cols) {
                auto& dst = target->cols[kv.first];
                dst.insert(dst.end(), kv.second.begin(), kv.second.end());
            }
            for (auto& kv : target->cols) dedupeColumn(kv.second);
            refreshPeak(*target);
            lanes.erase(bell);
            layoutInstrumentLanes(lanes, lane_gap, bus_half);
        }
    }

    std::set<int> union_set;
    for (const auto& lane : lanes) {
        for (const auto& kv : lane.cols) union_set.insert(kv.first);
    }
    std::vector<int> union_cols(union_set.begin(), union_set.end());

    BlockMap merged;
    for (auto& lane : lanes) {
        BuildResult built = buildStructure(resolution, lane.cols, lane.peak,
                                           true, nullptr, &union_cols);
        for (const auto& bkv : built.blocks) {
            if (bkv.second.name == "minecraft:lever" ||
                bkv.second.name == "minecraft:observer") {
                continue;
            }
            Pos pos{bkv.first[0], bkv.first[1], bkv.first[2] + lane.z};
            auto existing = merged.find(pos);
            if (existing != merged.end() && existing->second.name != bkv.second.name) {
                throw std::runtime_error("lane " + lane.inst + " block conflict");
            }
            merged[pos] = bkv.second;
        }
    }

    int z_min = lanes.front().z;
    int z_max = lanes.front().z;
    for (const auto& lane : lanes) {
        z_min = std::min(z_min, lane.z);
        z_max = std::max(z_max, lane.z);
    }
    std::map<std::string, std::string> lever_props;
    lever_props["face"] = "floor";
    lever_props["facing"] = "west";
    lever_props["powered"] = "false";
    merged[Pos{-4, 1, 0}] = Block{"minecraft:lever", lever_props};
    std::map<std::string, std::string> obs_props;
    obs_props["facing"] = "west";
    obs_props["powered"] = "false";
    merged[Pos{-3, 1, 0}] = Block{"minecraft:observer", obs_props};
    for (int z = z_min; z <= z_max; ++z) {
        std::map<std::string, std::string> wire_props;
        wire_props["power"] = "0";
        merged[Pos{-2, 1, z}] = Block{"minecraft:redstone_wire", wire_props};
        merged.emplace(Pos{-2, 0, z}, Block{"minecraft:smooth_quartz", {}});
    }
    for (int x : {-4, -3}) {
        merged.emplace(Pos{x, 0, 0}, Block{"minecraft:smooth_quartz", {}});
    }
    refreshWireConnections(merged);

    int sx = 0, sy = 0, sz = 0;
    if (!writeStructureNbt(out_path, merged, sx, sy, sz, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    auto dist = simulateRedstone(merged);
    std::size_t note_count = 0, unreachable = 0;
    for (const auto& kv : merged) {
        if (kv.second.name == "minecraft:note_block") {
            ++note_count;
            if (dist.find(kv.first) == dist.end()) ++unreachable;
        }
    }
    auto adjacency = noteDustAdjacency(merged);
    bus_half = std::max(std::abs(z_min), std::abs(z_max));

    std::printf("notes=%zu lanes=%zu columns=%d blocks=%zu note_blocks=%zu "
                "dropped=%zu unreachable=%zu adjacency=%zu size=%d,%d,%d bus_half=%d\n",
                notes.size(), lanes.size(), columns, merged.size(), note_count,
                dropped, unreachable, adjacency.size(), sx, sy, sz, bus_half);
    for (const auto& lane : lanes) {
        int lane_notes = 0;
        for (const auto& kv : lane.cols) {
            lane_notes += static_cast<int>(kv.second.size());
        }
        std::printf("lane %s: z=%+d notes=%d columns=%d peak=%d\n",
                    lane.inst.c_str(), lane.z, lane_notes,
                    static_cast<int>(lane.cols.size()), lane.peak);
    }

    if (!json_path.empty()) {
        std::ofstream jf(json_path, std::ios::binary);
        if (jf) {
            jf << "{\n  \"out\": \"" << jsonEscape(out_path) << "\",\n";
            jf << "  \"midi\": \"" << jsonEscape(midi_path) << "\",\n";
            jf << "  \"split\": \"instrument\",\n";
            jf << "  \"resolution\": " << resolution << ",\n";
            jf << "  \"columns\": " << columns << ",\n";
            jf << "  \"size\": [" << sx << ", " << sy << ", " << sz << "],\n";
            jf << "  \"blocks\": " << merged.size() << ",\n";
            jf << "  \"note_blocks\": " << note_count << ",\n";
            jf << "  \"unreachable_note_blocks\": " << unreachable << ",\n";
            jf << "  \"note_dust_adjacency\": " << adjacency.size() << ",\n";
            jf << "  \"bus_max_distance\": " << bus_half << ",\n";
            jf << "  \"lanes\": {\n";
            bool first = true;
            for (const auto& lane : lanes) {
                if (!first) jf << ",\n";
                first = false;
                jf << "    \"" << jsonEscape(lane.inst) << "\": {\"z_offset\": " << lane.z
                   << ", \"used_columns\": " << lane.cols.size()
                   << ", \"max_per_col\": " << lane.peak << "}";
            }
            jf << "\n  }\n}\n";
        }
    }

    if (unreachable > 0 || !adjacency.empty()) {
        std::fprintf(stderr, "self-check failed: unreachable or note/dust adjacency\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string midi_path, out_path, json_path;
    int lanes = 3;
    std::string split = "pitch";
    int resolution = 2;
    int poly_cap = 20;
    int lane_gap = 2;
    double align_lead = -1.0;      // <0：不做时间对齐

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string name = a, value;
        bool inline_value = false;
        auto eq = a.find('=');
        if (startsWith(a, "--") && eq != std::string::npos) {
            name = a.substr(0, eq);
            value = a.substr(eq + 1);
            inline_value = true;
        }
        auto take = [&](std::string& dst) -> bool {
            if (inline_value) {
                dst = value;
                return true;
            }
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (name == "--midi") {
            if (!take(midi_path)) { std::fprintf(stderr, "missing --midi\n"); return 2; }
        } else if (name == "--out") {
            if (!take(out_path)) { std::fprintf(stderr, "missing --out\n"); return 2; }
        } else if (name == "--lanes") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --lanes\n"); return 2; }
            lanes = std::stoi(v);
        } else if (name == "--split") {
            if (!take(split)) { std::fprintf(stderr, "missing --split\n"); return 2; }
        } else if (name == "--resolution") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --resolution\n"); return 2; }
            resolution = std::stoi(v);
        } else if (name == "--poly-cap") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --poly-cap\n"); return 2; }
            poly_cap = std::stoi(v);
        } else if (name == "--lane-gap") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --lane-gap\n"); return 2; }
            lane_gap = std::stoi(v);
        } else if (name == "--align-lead") {
            std::string v;
            if (!take(v)) { std::fprintf(stderr, "missing --align-lead\n"); return 2; }
            align_lead = std::stod(v);
        } else if (name == "--json") {
            if (!take(json_path)) { std::fprintf(stderr, "missing --json\n"); return 2; }
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }
    if (midi_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: fast_arrange_multilane --midi PATH --out PATH [options]\n");
        return 2;
    }
    if (lanes < 1) {
        std::fprintf(stderr, "--lanes must be >= 1\n");
        return 2;
    }
    if (split != "pitch" && split != "bank" && split != "instrument") {
        std::fprintf(stderr, "--split must be pitch, bank or instrument\n");
        return 2;
    }
    if (resolution != 2 && resolution != 4 && resolution != 6 && resolution != 8) {
        std::fprintf(stderr, "--resolution must be 2/4/6/8\n");
        return 2;
    }

    if (split == "instrument") {
        try {
            return runInstrumentSplit(midi_path, out_path, json_path, resolution, lane_gap,
                                      align_lead);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }

    try {
        std::string error;
        MidiFileData midi;
        if (!readMidiFile(midi_path, midi, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::vector<Note> notes = loadNotes(midi);
        if (notes.empty()) {
            std::fprintf(stderr, "MIDI has no notes\n");
            return 1;
        }
        std::vector<int> pitches;
        for (const Note& n : notes) pitches.push_back(n.pitch);
        PitchMap conv = buildPitchMap(pitches, nullptr);

        double col_seconds = static_cast<double>(resolution) / 20.0;
        double last_end = 0.0;
        for (const Note& n : notes) last_end = std::max(last_end, n.end);
        int columns = std::max(1, pyRound(last_end / col_seconds));

        // Lane assignment.
        std::vector<std::vector<std::size_t>> assignments(static_cast<std::size_t>(lanes));
        if (split == "pitch") {
            std::vector<std::size_t> order(notes.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                if (notes[a].pitch != notes[b].pitch) return notes[a].pitch < notes[b].pitch;
                return notes[a].start < notes[b].start;
            });
            int n = static_cast<int>(order.size());
            for (int rank = 0; rank < n; ++rank) {
                int lane = std::min(lanes - 1,
                    static_cast<int>(static_cast<long long>(rank) * lanes / std::max(1, n)));
                assignments[static_cast<std::size_t>(lane)].push_back(order[rank]);
            }
        } else {
            for (std::size_t idx = 0; idx < notes.size(); ++idx) {
                MappedNote m = conv.conv(notes[idx].pitch);
                int lane = 1;
                if (m.inst == "bass") lane = 0;
                else if (m.inst == "bell") lane = 2;
                lane = std::min(lanes - 1, lane);
                assignments[static_cast<std::size_t>(lane)].push_back(idx);
            }
        }

        std::map<int, LaneInfo> lane_data;
        int global_max_per_col = 0;
        for (int lane = 0; lane < lanes; ++lane) {
            const auto& idxs = assignments[static_cast<std::size_t>(lane)];
            if (idxs.empty()) continue;

            std::map<int, std::vector<MappedNote>> cand;
            LaneStats stats;
            stats.notes = static_cast<int>(idxs.size());
            for (std::size_t idx : idxs) {
                const Note& n = notes[idx];
                int c0 = pyRound(n.start / col_seconds);
                if (!(c0 >= 0 && c0 < columns)) {
                    stats.out_of_range += 1;
                    continue;
                }
                cand[c0].push_back(conv.conv(n.pitch));
            }

            std::map<int, std::vector<MappedNote>> col_notes;
            for (auto& kv : cand) {
                std::vector<MappedNote> uniq;
                std::map<std::pair<std::string, int>, int> seen;
                for (const MappedNote& m : kv.second) {
                    auto key = std::make_pair(m.inst, m.note);
                    if (seen.count(key)) {
                        stats.merged_duplicates += 1;
                    } else {
                        seen[key] = static_cast<int>(uniq.size());
                        uniq.push_back(m);
                    }
                }
                std::stable_sort(uniq.begin(), uniq.end(),
                                 [](const MappedNote& a, const MappedNote& b) {
                                     return a.note < b.note;
                                 });
                int len = static_cast<int>(uniq.size());
                if (poly_cap > 0 && len > poly_cap) {
                    stats.capped += len - poly_cap;
                    uniq.erase(uniq.begin(), uniq.end() - poly_cap);
                } else if (poly_cap == 0 && len > 0) {
                    stats.capped += len;
                }
                col_notes[kv.first] = std::move(uniq);
            }
            if (col_notes.empty()) continue;

            LaneInfo info;
            info.col_notes = std::move(col_notes);
            info.stats = stats;
            info.stats.used_columns = static_cast<int>(info.col_notes.size());
            info.stats.max_per_col = 0;
            for (const auto& kv : info.col_notes) {
                info.stats.max_per_col =
                    std::max(info.stats.max_per_col, static_cast<int>(kv.second.size()));
            }
            info.indices = idxs;
            info.pitch_min = notes[idxs.front()].pitch;
            info.pitch_max = notes[idxs.front()].pitch;
            for (std::size_t idx : idxs) {
                info.pitch_min = std::min(info.pitch_min, notes[idx].pitch);
                info.pitch_max = std::max(info.pitch_max, notes[idx].pitch);
            }
            global_max_per_col = std::max(global_max_per_col, info.stats.max_per_col);
            lane_data[lane] = std::move(info);
        }
        if (lane_data.empty()) {
            std::fprintf(stderr, "all lanes are empty\n");
            return 1;
        }

        std::set<int> union_set;
        for (const auto& kv : lane_data) {
            for (const auto& c : kv.second.col_notes) union_set.insert(c.first);
        }
        std::vector<int> union_cols(union_set.begin(), union_set.end());

        int base_pitch = 2 + std::max(1, global_max_per_col) + 1;
        int lane_width = base_pitch + lane_gap;
        std::vector<int> offsets(static_cast<std::size_t>(lanes));
        double mid = (static_cast<double>(lanes) - 1.0) / 2.0;
        for (int i = 0; i < lanes; ++i) {
            offsets[static_cast<std::size_t>(i)] =
                pyRound((static_cast<double>(i) - mid) * static_cast<double>(lane_width));
        }

        BlockMap merged;
        for (auto& kv : lane_data) {
            int lane = kv.first;
            LaneInfo& info = kv.second;
            info.z_offset = offsets[static_cast<std::size_t>(lane)];
            BuildResult built = buildStructure(resolution, info.col_notes,
                                               info.stats.max_per_col, true, nullptr,
                                               &union_cols);
            BlockMap lane_blocks;
            for (const auto& bkv : built.blocks) {
                if (bkv.second.name == "minecraft:lever" ||
                    bkv.second.name == "minecraft:observer") {
                    continue;
                }
                Pos pos{bkv.first[0], bkv.first[1], bkv.first[2] + info.z_offset};
                auto existing = merged.find(pos);
                if (existing != merged.end() && existing->second.name != bkv.second.name) {
                    throw std::runtime_error("lane " + std::to_string(lane) +
                                             " block conflict at shifted position");
                }
                lane_blocks[pos] = bkv.second;
            }
            for (auto& bkv : lane_blocks) {
                merged[bkv.first] = std::move(bkv.second);
            }
        }

        int z_min = offsets.front();
        int z_max = offsets.back();
        std::map<std::string, std::string> lever_props;
        lever_props["face"] = "floor";
        lever_props["facing"] = "west";
        lever_props["powered"] = "false";
        merged[Pos{-4, 1, 0}] = Block{"minecraft:lever", lever_props};
        std::map<std::string, std::string> obs_props;
        obs_props["facing"] = "west";
        obs_props["powered"] = "false";
        merged[Pos{-3, 1, 0}] = Block{"minecraft:observer", obs_props};
        for (int z = z_min; z <= z_max; ++z) {
            std::map<std::string, std::string> wire_props;
            wire_props["power"] = "0";
            merged[Pos{-2, 1, z}] = Block{"minecraft:redstone_wire", wire_props};
            merged.emplace(Pos{-2, 0, z}, Block{"minecraft:smooth_quartz", {}});
        }
        for (int x : {-4, -3}) {
            merged.emplace(Pos{x, 0, 0}, Block{"minecraft:smooth_quartz", {}});
        }
        refreshWireConnections(merged);

        int sx = 0, sy = 0, sz = 0;
        if (!writeStructureNbt(out_path, merged, sx, sy, sz, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        auto dist = simulateRedstone(merged);
        std::size_t note_count = 0, unreachable = 0;
        for (const auto& kv : merged) {
            if (kv.second.name == "minecraft:note_block") {
                ++note_count;
                if (dist.find(kv.first) == dist.end()) ++unreachable;
            }
        }
        auto adjacency = noteDustAdjacency(merged);
        int bus_half = std::max(std::abs(z_min), std::abs(z_max));

        std::printf("notes=%zu lanes=%zu columns=%d blocks=%zu note_blocks=%zu "
                    "unreachable=%zu adjacency=%zu size=%d,%d,%d bus_half=%d\n",
                    notes.size(), lane_data.size(), columns, merged.size(), note_count,
                    unreachable, adjacency.size(), sx, sy, sz, bus_half);
        for (const auto& kv : lane_data) {
            const LaneInfo& r = kv.second;
            std::printf("lane %d: z=%+d notes=%d columns=%d peak=%d dupes=%d capped=%d "
                        "pitch=%d~%d\n",
                        kv.first, r.z_offset, r.stats.notes, r.stats.used_columns,
                        r.stats.max_per_col, r.stats.merged_duplicates, r.stats.capped,
                        r.pitch_min, r.pitch_max);
        }

        if (!json_path.empty()) {
            std::ofstream jf(json_path, std::ios::binary);
            if (jf) {
                jf << "{\n";
                jf << "  \"out\": \"" << jsonEscape(out_path) << "\",\n";
                jf << "  \"midi\": \"" << jsonEscape(midi_path) << "\",\n";
                jf << "  \"lanes_requested\": " << lanes << ",\n";
                jf << "  \"lanes_used\": " << lane_data.size() << ",\n";
                jf << "  \"split\": \"" << jsonEscape(split) << "\",\n";
                jf << "  \"resolution\": " << resolution << ",\n";
                jf << "  \"cell_seconds\": " << col_seconds << ",\n";
                jf << "  \"columns\": " << columns << ",\n";
                jf << "  \"lane_width\": " << lane_width << ",\n";
                jf << "  \"lane_offsets\": [";
                for (std::size_t i = 0; i < offsets.size(); ++i) {
                    if (i) jf << ", ";
                    jf << offsets[i];
                }
                jf << "],\n";
                jf << "  \"bus_z_span\": [" << z_min << ", " << z_max << "],\n";
                jf << "  \"bus_max_distance\": " << bus_half << ",\n";
                jf << "  \"size\": [" << sx << ", " << sy << ", " << sz << "],\n";
                jf << "  \"blocks\": " << merged.size() << ",\n";
                jf << "  \"note_blocks\": " << note_count << ",\n";
                jf << "  \"unreachable_note_blocks\": " << unreachable << ",\n";
                jf << "  \"note_dust_adjacency\": " << adjacency.size() << ",\n";
                jf << "  \"lanes\": {\n";
                bool first = true;
                for (const auto& kv : lane_data) {
                    const LaneInfo& r = kv.second;
                    if (!first) jf << ",\n";
                    first = false;
                    jf << "    \"" << kv.first << "\": {\"z_offset\": " << r.z_offset
                       << ", \"notes\": " << r.stats.notes
                       << ", \"used_columns\": " << r.stats.used_columns
                       << ", \"max_per_col\": " << r.stats.max_per_col
                       << ", \"merged_duplicates\": " << r.stats.merged_duplicates
                       << ", \"capped\": " << r.stats.capped
                       << ", \"pitch_min\": " << r.pitch_min
                       << ", \"pitch_max\": " << r.pitch_max << "}";
                }
                jf << "\n  }\n}\n";
            }
        }

        if (unreachable > 0 || !adjacency.empty()) {
            std::fprintf(stderr, "self-check failed: unreachable or note/dust adjacency\n");
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
