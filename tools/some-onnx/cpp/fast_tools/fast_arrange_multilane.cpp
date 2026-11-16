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
    int bus_delay_cols = 0;   // 起始总线到本轨累积的中继器延迟（列）
    int pad_cols = 0;         // 为对齐起播额外补的空列
};

// ---------------------------------------------------------------------------
// 原版 16 种音符盒音色：垫块 + 基准音（note=0 时的绝对音高）。
// 每个音色的有效音域固定为 25 个半音（两个八度 + 1），超出按 ±12 折回。
// 数组顺序即轨道排布顺序（低音在前，打击乐在后）。
// ---------------------------------------------------------------------------
struct McInstrument {
    const char* name;
    const char* material;
    int base;
};

const McInstrument MC_INSTRUMENTS[] = {
    {"bass", "minecraft:bamboo_planks", 30},
    {"didgeridoo", "minecraft:pumpkin", 30},
    {"guitar", "minecraft:white_wool", 42},
    {"banjo", "minecraft:hay_block", 54},
    {"harp", "minecraft:dirt", 54},
    {"bit", "minecraft:emerald_block", 54},
    {"flute", "minecraft:clay", 66},
    {"xylophone", "minecraft:bone_block", 66},
    {"iron_xylophone", "minecraft:iron_block", 66},
    {"cow_bell", "minecraft:soul_sand", 66},
    {"bell", "minecraft:gold_block", 78},
    {"chime", "minecraft:packed_ice", 78},
    {"pling", "minecraft:glowstone", 78},
    {"basedrum", "minecraft:stone", 0},
    {"snare", "minecraft:sand", 0},
    {"hat", "minecraft:glass", 0},
};

const McInstrument* findInstrument(const std::string& name) {
    for (const auto& mc : MC_INSTRUMENTS) {
        if (name == mc.name) return &mc;
    }
    return nullptr;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// GM 打击乐键位 → 三种打击音色
std::string drumFamily(int pitch) {
    if (pitch == 35 || pitch == 36) return "basedrum";
    if (pitch >= 37 && pitch <= 40) return "snare";
    if (pitch == 41 || pitch == 43 || pitch == 45 || pitch == 47 ||
        pitch == 48 || pitch == 50) return "basedrum";   // 桶鼓并入底鼓
    if (pitch == 42 || pitch == 44 || pitch == 46 || pitch == 49 ||
        pitch == 51 || pitch == 52 || pitch == 53 || pitch == 55 ||
        pitch == 54 || pitch == 56 || pitch == 57 || pitch == 59) return "hat";
    return "hat";
}

// GM program → MC 音色；同家族共用一个音色（如钢琴/弦乐/人声都落 harp）
std::string mcForProgram(int program) {
    if (program < 0 || program > 127) return "harp";
    if (program < 8) return "harp";                    // 钢琴
    if (program < 16) return "xylophone";              // 色彩打击
    if (program < 24) return "harp";                   // 风琴
    if (program < 32) return "guitar";                 // 吉他
    if (program < 40) return "bass";                   // 贝斯
    if (program < 48) return program == 43 ? "bass" : "harp";  // 弦乐，低音提琴落 bass
    if (program < 56) {
        if (program == 47) return "basedrum";          // 定音鼓
        if (program == 55) return "snare";             // 管弦乐齐奏
        return "harp";                                 // 合奏/人声
    }
    if (program < 64) return "didgeridoo";             // 铜管
    if (program < 72) return "flute";                  // 簧管
    if (program < 80) return "flute";                  // 管乐
    if (program < 88) return "bit";                    // 合成主音
    if (program < 104) return "pling";                 // 合成铺底/音效
    if (program < 112) return "banjo";                 // 民族乐器
    if (program < 120) return "xylophone";             // 打击乐
    return "pling";
}

std::string classifyRole(int channel, int program, const std::string& track_name, int pitch) {
    if (channel == 9 || toLower(track_name).find("drum") != std::string::npos) {
        return drumFamily(pitch);
    }
    return mcForProgram(program);
}

MappedNote mapRoleNote(const RoleNote& n, int shift) {
    const McInstrument* mc = findInstrument(n.role);
    if (mc == nullptr) mc = findInstrument("harp");
    if (mc->base == 0) {
        // 打击乐没有音高概念：底鼓固定 note 0，军鼓/踩镲也统一 0
        return MappedNote{mc->name, mc->material, 0};
    }
    int v = n.pitch + shift - mc->base;
    while (v < 0) v += 12;
    while (v > 24) v -= 12;
    return MappedNote{mc->name, mc->material, v};
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
                                               classifyRole(ev.channel, prog, track.name,
                                                            ev.note)});
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

// ---------------------------------------------------------------------------
// 起始总线分段：红石粉 0 延迟只能传 15 格，超过后每段接一个中继器。
// 中继器延迟设为 resolution/2（1..4 档），正好等于 1 列，便于按列补齐。
// 注意：中继器不能放在某条轨自己的 z 上（那会顶掉该轨的接入点），
// 也不能让两段之间超过 15 格红石粉。
// ---------------------------------------------------------------------------
constexpr int BUS_SEG = 14;                    // 一段红石粉的目标长度

std::vector<int> planBusRepeaters(int span, const std::set<int>& lane_z) {
    std::vector<int> reps;
    for (int dir : {1, -1}) {
        int run = 0;
        for (int d = 1; d <= span; ++d) {
            int z = dir * d;
            bool is_lane = lane_z.count(z) > 0;
            bool next_lane = lane_z.count(z + dir) > 0;
            bool need = run >= BUS_SEG || (run >= BUS_SEG - 1 && next_lane);
            if (!is_lane && need && d < span) {
                reps.push_back(z);
                run = 0;
            } else {
                ++run;
            }
        }
    }
    std::sort(reps.begin(), reps.end());
    return reps;
}

int busDelayBefore(const std::vector<int>& reps, int z) {
    int a = std::abs(z);
    int n = 0;
    for (int p : reps) {
        if (std::abs(p) < a) ++n;
    }
    return n;
}

void buildStartBus(BlockMap& merged, int z_min, int z_max, const std::vector<int>& reps,
                   int repeaterDelay) {
    const std::set<int> rep_set(reps.begin(), reps.end());
    for (int z = z_min; z <= z_max; ++z) {
        if (rep_set.count(z)) {
            std::map<std::string, std::string> props;
            // 模拟器约定：facing 指向输入侧（与 gen_machine 的折叠布线一致）
            props["facing"] = z > 0 ? "north" : "south";
            props["delay"] = std::to_string(repeaterDelay);
            props["locked"] = "false";
            props["powered"] = "false";
            merged[Pos{-2, 1, z}] = Block{"minecraft:repeater", props};
        } else {
            std::map<std::string, std::string> wire_props;
            wire_props["power"] = "0";
            merged[Pos{-2, 1, z}] = Block{"minecraft:redstone_wire", wire_props};
        }
        merged.emplace(Pos{-2, 0, z}, Block{"minecraft:smooth_quartz", {}});
    }
}

// 把某条轨的内容整体后移 pad 列，使所有轨在同一 tick 起播。
template <typename ColMap>
void padColumns(ColMap& cols, int pad) {
    if (pad <= 0) return;
    ColMap shifted;
    for (auto& kv : cols) shifted[kv.first + pad] = std::move(kv.second);
    cols = std::move(shifted);
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
    for (const auto& mc : MC_INSTRUMENTS) {
        auto it = lane_cols.find(mc.name);
        if (it == lane_cols.end()) continue;
        InstrumentLane lane;
        lane.inst = mc.name;
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

    // 分段总线：每个中继器延迟 resolution/2 tick = 恰好 1 列；
    // 靠前的轨按延迟差补空列，保证所有轨从同一 tick 起播。
    const int bus_rep_delay = std::max(1, std::min(4, resolution / 2));
    std::set<int> lane_z;
    int bus_span = 0;
    for (const auto& lane : lanes) {
        lane_z.insert(lane.z);
        bus_span = std::max(bus_span, std::abs(lane.z));
    }
    std::vector<int> bus_reps = planBusRepeaters(bus_span, lane_z);
    int max_delay_cols = 0;
    for (auto& lane : lanes) {
        lane.bus_delay_cols = busDelayBefore(bus_reps, lane.z);
        max_delay_cols = std::max(max_delay_cols, lane.bus_delay_cols);
    }
    for (auto& lane : lanes) {
        lane.pad_cols = max_delay_cols - lane.bus_delay_cols;
        padColumns(lane.cols, lane.pad_cols);
    }
    columns += max_delay_cols;

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
    buildStartBus(merged, z_min, z_max, bus_reps, bus_rep_delay);
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
                "dropped=%zu unreachable=%zu adjacency=%zu size=%d,%d,%d bus_half=%d "
                "bus_rep_delay=%d bus_delay_cols=%d\n",
                notes.size(), lanes.size(), columns, merged.size(), note_count,
                dropped, unreachable, adjacency.size(), sx, sy, sz, bus_half,
                bus_rep_delay, max_delay_cols);
    for (const auto& lane : lanes) {
        int lane_notes = 0;
        for (const auto& kv : lane.cols) {
            lane_notes += static_cast<int>(kv.second.size());
        }
        std::printf("lane %s: z=%+d notes=%d columns=%d peak=%d bus_delay=%d pad=%d\n",
                    lane.inst.c_str(), lane.z, lane_notes,
                    static_cast<int>(lane.cols.size()), lane.peak,
                    lane.bus_delay_cols, lane.pad_cols);
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
            jf << "  \"bus_repeater_delay\": " << bus_rep_delay << ",\n";
            jf << "  \"bus_delay_columns\": " << max_delay_cols << ",\n";
            jf << "  \"lanes\": {\n";
            bool first = true;
            for (const auto& lane : lanes) {
                if (!first) jf << ",\n";
                first = false;
                jf << "    \"" << jsonEscape(lane.inst) << "\": {\"z_offset\": " << lane.z
                   << ", \"used_columns\": " << lane.cols.size()
                   << ", \"max_per_col\": " << lane.peak
                   << ", \"bus_delay_cols\": " << lane.bus_delay_cols
                   << ", \"pad_cols\": " << lane.pad_cols << "}";
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

        int base_pitch = 2 + std::max(1, global_max_per_col) + 1;
        int lane_width = base_pitch + lane_gap;
        std::vector<int> offsets(static_cast<std::size_t>(lanes));
        double mid = (static_cast<double>(lanes) - 1.0) / 2.0;
        for (int i = 0; i < lanes; ++i) {
            offsets[static_cast<std::size_t>(i)] =
                pyRound((static_cast<double>(i) - mid) * static_cast<double>(lane_width));
        }

        // 分段总线 + 按轨补齐，保证多轨同一 tick 起播
        const int bus_rep_delay = std::max(1, std::min(4, resolution / 2));
        std::set<int> lane_z;
        int bus_span = 0;
        for (int off : offsets) {
            lane_z.insert(off);
            bus_span = std::max(bus_span, std::abs(off));
        }
        std::vector<int> bus_reps = planBusRepeaters(bus_span, lane_z);
        int max_delay_cols = 0;
        for (const auto& kv : lane_data) {
            max_delay_cols = std::max(max_delay_cols,
                busDelayBefore(bus_reps, offsets[static_cast<std::size_t>(kv.first)]));
        }
        for (auto& kv : lane_data) {
            int pad = max_delay_cols -
                busDelayBefore(bus_reps, offsets[static_cast<std::size_t>(kv.first)]);
            padColumns(kv.second.col_notes, pad);
            kv.second.z_offset = offsets[static_cast<std::size_t>(kv.first)];
        }

        std::set<int> union_set;
        for (const auto& kv : lane_data) {
            for (const auto& c : kv.second.col_notes) union_set.insert(c.first);
        }
        std::vector<int> union_cols(union_set.begin(), union_set.end());

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
