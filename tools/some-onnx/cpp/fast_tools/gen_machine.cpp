#include "gen_machine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <set>
#include <stdexcept>

namespace {

constexpr int NOTE_BASE_MIDI = 54;
constexpr int NOTE_COUNT = 25;
constexpr int NOTE_CENTER = 12;
constexpr int Z_CAP = 128;
constexpr int Z_FOLD_REPS = 2;
constexpr int TIMING_TOL_TICKS = 10;
constexpr int N_STRIP = 6;
constexpr int SLOT0 = 2;
constexpr int X_MIN = -2;
constexpr int X_MAX = 28;

const Bank INSTRUMENT_BANKS[3] = {
    {"bass", "minecraft:bamboo_planks", 30},
    {"harp", "minecraft:dirt", 54},
    {"bell", "minecraft:gold_block", 78},
};

const char* trackMaterial(const std::string& track) {
    if (track == "drums") return "minecraft:stone";
    if (track == "bass") return "minecraft:bamboo_planks";
    if (track == "other") return "minecraft:dirt";
    if (track == "vocals") return "minecraft:dirt";
    return nullptr;
}

const char* trackInstrument(const std::string& track) {
    if (track == "drums") return "basedrum";
    if (track == "bass") return "bass";
    if (track == "other") return "harp";
    if (track == "vocals") return "harp";
    return nullptr;
}

const Bank& trackBank(const std::string& track) {
    if (track == "bass") return INSTRUMENT_BANKS[0];
    if (track == "drums") return INSTRUMENT_BANKS[1];
    return INSTRUMENT_BANKS[1];  // other / vocals
}

const int DR[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

bool isWire(const BlockMap& blocks, const Pos& p) {
    auto it = blocks.find(p);
    return it != blocks.end() && it->second.name == "minecraft:redstone_wire";
}

bool wireConnectable(const std::string& name) {
    return name == "minecraft:redstone_wire" || name == "minecraft:repeater" ||
           name == "minecraft:observer" || name == "minecraft:lever";
}

struct RepOut {
    Pos out;
    int weight;
};

} // namespace

MappedNote PitchMap::conv(int pitch) const {
    int q = pitch + shift;
    const Bank* b = nullptr;
    if (has_bank) {
        b = &bank;
    } else if (q < 54) {
        b = &INSTRUMENT_BANKS[0];
    } else if (q > 78) {
        b = &INSTRUMENT_BANKS[2];
    } else {
        b = &INSTRUMENT_BANKS[1];
    }
    int n = q - b->base;
    while (n < 0) n += 12;
    while (n > NOTE_COUNT - 1) n -= 12;
    return MappedNote{b->inst, b->mat, n};
}

std::vector<Note> loadNotes(const MidiFileData& midi) {
    std::vector<Note> out;
    double tpq = static_cast<double>(midi.tpq);
    for (const auto& track : midi.tracks) {
        int tempo = 500000;
        double now = 0.0;
        std::map<std::pair<int, int>, double> pending;
        for (const auto& ev : track.events) {
            double scale = static_cast<double>(tempo) * 1e-6 / tpq;
            now += static_cast<double>(ev.delta) * scale;
            if (ev.kind == EvKind::SetTempo) {
                tempo = ev.tempo;
            } else if (ev.kind == EvKind::NoteOn) {
                pending[std::make_pair(ev.channel, ev.note)] = now;
            } else if (ev.kind == EvKind::NoteOff) {
                auto it = pending.find(std::make_pair(ev.channel, ev.note));
                if (it != pending.end()) {
                    double start = it->second;
                    pending.erase(it);
                    if (now > start) out.push_back(Note{ev.note, start, now});
                }
            }
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Note& a, const Note& b) { return a.start < b.start; });
    return out;
}

PitchMap buildPitchMap(const std::vector<int>& pitches, const std::string* track) {
    PitchMap pm;
    std::vector<int> sorted = pitches;
    std::sort(sorted.begin(), sorted.end());
    double med = 0.0;
    if (!sorted.empty()) {
        std::size_t n = sorted.size();
        if (n % 2 == 1) {
            med = static_cast<double>(sorted[n / 2]);
        } else {
            med = (static_cast<double>(sorted[n / 2 - 1]) +
                   static_cast<double>(sorted[n / 2])) / 2.0;
        }
    }
    pm.has_bank = track != nullptr;
    if (pm.has_bank) pm.bank = trackBank(*track);
    int center = pm.has_bank ? (pm.bank.base + NOTE_CENTER)
                            : (NOTE_BASE_MIDI + NOTE_CENTER);
    pm.shift = pyRound(static_cast<double>(center) - med);
    return pm;
}

std::vector<int> segmentDelays(int ticks) {
    std::vector<int> ds(static_cast<std::size_t>(std::max(0, ticks / 8)), 4);
    int r = ticks % 8;
    if (r != 0) ds.push_back(r / 2);
    return ds;
}

BuildResult buildStructure(int resolution,
                           const std::map<int, std::vector<MappedNote>>& col_notes,
                           int max_per_col, bool simple, const std::string* track,
                           const std::vector<int>* timeline) {
    BuildResult result;
    BlockMap& blocks = result.blocks;

    auto put = [&](int x, int y, int z, const std::string& name,
                   const std::map<std::string, std::string>& props) {
        Block b;
        b.name = name;
        b.props = props;
        blocks[Pos{x, y, z}] = std::move(b);
    };
    auto putSimple = [&](int x, int y, int z, const std::string& name) {
        put(x, y, z, name, std::map<std::string, std::string>{});
    };
    auto makeRep = [](int d, const std::string& facing) {
        std::map<std::string, std::string> p;
        p["facing"] = facing;
        p["delay"] = std::to_string(d);
        p["locked"] = "false";
        p["powered"] = "false";
        return p;
    };

    int MAX_SLOT = std::max(1, max_per_col);
    int PITCH = SLOT0 + MAX_SLOT + 1;

    struct State {
        int x = X_MIN;
        int y = 1;
        int dir = 1;
        int base_z = 0;
        int zdir = 1;
        int in_layer = 0;
        int fold_reps = Z_FOLD_REPS;
    } st;

    std::set<int> layer_ys;
    layer_ys.insert(1);

    auto facing = [&]() -> std::string { return st.dir > 0 ? "west" : "east"; };

    auto atStripEnd = [&]() -> bool {
        if (simple) return false;
        return (st.dir > 0 && st.x >= X_MAX) || (st.dir < 0 && st.x <= X_MIN);
    };

    auto nearLayerEnd = [&]() -> bool {
        if (simple) return false;
        return st.in_layer >= N_STRIP - 1 &&
               ((st.dir > 0 && st.x >= X_MAX - 4) || (st.dir < 0 && st.x <= X_MIN + 4));
    };

    auto doZfold = [&]() {
        int x = st.x, y = st.y, bz = st.base_z, d = st.dir, zd = st.zdir;
        int reps = st.fold_reps;
        int xd = (d > 0) ? (X_MAX + 4) : (X_MIN - 4);
        int xc = x;
        if (reps >= 2) {
            xc = x + d;
            putSimple(xc, y, bz, "minecraft:redstone_wire");
            xc = x + 2 * d;
            put(xc, y, bz, "minecraft:repeater", makeRep(1, d > 0 ? "west" : "east"));
        }
        while (xc != xd) {
            xc += d;
            putSimple(xc, y, bz, "minecraft:redstone_wire");
        }
        int zm = std::max(1, PITCH - 1);
        std::string zfacing = zd > 0 ? "north" : "south";
        for (int i = 1; i <= PITCH; ++i) {
            if (i == zm) {
                put(xd, y, bz + i * zd, "minecraft:repeater", makeRep(1, zfacing));
            } else {
                putSimple(xd, y, bz + i * zd, "minecraft:redstone_wire");
            }
        }
        st.x = xd;
        st.base_z = bz + PITCH * zd;
        st.dir = -d;
        st.in_layer += 1;
    };

    auto simPath = [&](int x, int y, int bz, int d, int zd, int il, int pad,
                       const std::vector<int>& ds, int reps, bool is_simple) {
        std::pair<int, int> counts{0, 0};
        if (is_simple) return counts;
        auto chk = [&]() -> bool {
            if (il >= N_STRIP - 1 &&
                ((d > 0 && x >= X_MAX - 4) || (d < 0 && x <= X_MIN + 4))) {
                x += d;
                y += 3;
                d = -d;
                zd = -zd;
                il = 0;
                counts.second += 1;
                return true;
            }
            if ((d > 0 && x >= X_MAX) || (d < 0 && x <= X_MIN)) {
                if (reps >= Z_FOLD_REPS) {
                    x = (d > 0) ? (X_MAX + 4) : (X_MIN - 4);
                    bz += PITCH * zd;
                    d = -d;
                    il += 1;
                    counts.first += 1;
                } else {
                    x += d;
                    y += 3;
                    d = -d;
                    zd = -zd;
                    il = 0;
                    counts.second += 1;
                }
                return true;
            }
            return false;
        };
        auto chkZ = [&]() {
            if ((d > 0 && x >= X_MAX) || (d < 0 && x <= X_MIN)) {
                x = (d > 0) ? (X_MAX + 4) : (X_MIN - 4);
                bz += PITCH * zd;
                d = -d;
                il += 1;
                counts.first += 1;
            }
        };
        (void)y;
        (void)bz;
        for (int i = 0; i < pad; ++i) {
            if (!chk()) x += d;
        }
        for (std::size_t i = 0; i < ds.size(); ++i) {
            chk();
            x += d;
            chkZ();
            x += d;
        }
        return counts;
    };

    auto doYfold = [&]() {
        int x = st.x, y = st.y, d = st.dir, bz = st.base_z;
        Pos s1{x + d, y + 1, bz};
        Pos s2{x + d, y + 2, bz + 1};
        Pos end{x + d, y + 3, bz};
        putSimple(s1[0], y, bz, "minecraft:glowstone");
        putSimple(s1[0], s1[1], s1[2], "minecraft:redstone_wire");
        putSimple(s2[0], y + 1, bz + 1, "minecraft:glowstone");
        putSimple(s2[0], s2[1], s2[2], "minecraft:redstone_wire");
        putSimple(end[0], y + 2, bz, "minecraft:glowstone");
        putSimple(end[0], end[1], end[2], "minecraft:redstone_wire");
        st.x = end[0];
        st.y = end[1];
        st.dir = -d;
        st.zdir = -st.zdir;
        st.in_layer = 0;
        layer_ys.insert(st.y);
    };

    auto foldIfNeeded = [&]() -> bool {
        if (nearLayerEnd()) {
            doYfold();
            return true;
        }
        if (atStripEnd()) {
            if (st.fold_reps < Z_FOLD_REPS) {
                doYfold();
            } else {
                doZfold();
            }
            return true;
        }
        return false;
    };

    auto stepDust = [&]() {
        if (!foldIfNeeded()) {
            st.x += st.dir;
            putSimple(st.x, st.y, st.base_z, "minecraft:redstone_wire");
        }
    };

    auto stepRepeater = [&](int d) {
        foldIfNeeded();
        st.x += st.dir;
        put(st.x, st.y, st.base_z, "minecraft:repeater", makeRep(d, facing()));
        if (atStripEnd()) doZfold();
        st.x += st.dir;
        putSimple(st.x, st.y, st.base_z, "minecraft:redstone_wire");
    };

    auto columnAt = [&](int c, int x, int y, int bz, int d) {
        putSimple(x, y, bz + 1, "minecraft:redstone_wire");
        result.bus_dust[c] = {};
        const std::vector<MappedNote>& notes = col_notes.at(c);
        for (std::size_t i = 0; i < notes.size(); ++i) {
            int z = bz + SLOT0 + static_cast<int>(i);
            putSimple(x, y, z, "minecraft:redstone_wire");
            result.bus_dust[c].push_back(Pos{x, y, z});
            put(x + d, y, z, "minecraft:repeater", makeRep(1, d > 0 ? "west" : "east"));
            result.branch_reps[c].push_back(Pos{x + d, y, z});
            std::string pad_material =
                track ? std::string(trackMaterial(*track)) : notes[i].mat;
            std::string note_instrument =
                track ? std::string(trackInstrument(*track)) : notes[i].inst;
            putSimple(x + 2 * d, y - 1, z, pad_material);
            std::map<std::string, std::string> np;
            np["instrument"] = note_instrument;
            np["note"] = std::to_string(notes[i].note);
            np["powered"] = "false";
            put(x + 2 * d, y, z, "minecraft:note_block", np);
            result.note_blocks[c].push_back(Pos{x + 2 * d, y, z});
        }
        result.col_layer[c] = y;
    };

    // Pulse source at the start of the first strip.
    std::map<std::string, std::string> lever_props;
    lever_props["face"] = "floor";
    lever_props["facing"] = "west";
    lever_props["powered"] = "false";
    put(-4, 1, 0, "minecraft:lever", lever_props);
    std::map<std::string, std::string> obs_props;
    obs_props["facing"] = "west";
    obs_props["powered"] = "false";
    put(-3, 1, 0, "minecraft:observer", obs_props);
    putSimple(-2, 1, 0, "minecraft:redstone_wire");
    result.tap_of_col[0] = Pos{-2, 1, 0};
    result.col_layer[0] = 1;

    std::vector<int> timeline_cols;
    if (timeline != nullptr) {
        timeline_cols = *timeline;
        std::sort(timeline_cols.begin(), timeline_cols.end());
    } else {
        for (const auto& kv : col_notes) timeline_cols.push_back(kv.first);
    }

    int prev = 0;
    for (int c : timeline_cols) {
        int gap = resolution * (c - prev);
        std::vector<int> ds = segmentDelays(gap);
        int reps = Z_FOLD_REPS;
        for (int iter = 0; iter < 12; ++iter) {
            int pad = std::max(0, 4 - 2 * static_cast<int>(ds.size()));
            auto counts = simPath(st.x, st.y, st.base_z, st.dir, st.zdir,
                                  st.in_layer, pad, ds, reps, simple);
            int need = gap - 2 * reps * counts.first;
            if (need < 0 && reps > 1) {
                reps = 1;
                counts = simPath(st.x, st.y, st.base_z, st.dir, st.zdir,
                                 st.in_layer, pad, ds, reps, simple);
                need = gap - 2 * reps * counts.first;
            }
            if (need < 0) {
                throw std::runtime_error("column " + std::to_string(c) +
                                         ": fold delay exceeds column gap");
            }
            std::vector<int> newds = segmentDelays(need);
            if (newds == ds) break;
            ds = newds;
        }
        st.fold_reps = reps;
        for (int i = 0; i < std::max(0, 4 - 2 * static_cast<int>(ds.size())); ++i) {
            stepDust();
        }
        for (int d : ds) stepRepeater(d);
        if (!simple) {
            while (!(st.x >= X_MIN && st.x <= X_MAX)) stepDust();
        }
        result.tap_of_col[c] = Pos{st.x, st.y, st.base_z};
        auto it = col_notes.find(c);
        if (it != col_notes.end()) {
            columnAt(c, st.x, st.y, st.base_z, st.dir);
        }
        prev = c;
    }

    // End-of-line lamp.
    foldIfNeeded();
    st.x += st.dir;
    put(st.x, st.y, st.base_z, "minecraft:repeater", makeRep(1, facing()));
    st.x += st.dir;
    std::map<std::string, std::string> lamp_props;
    lamp_props["lit"] = "false";
    put(st.x, st.y, st.base_z, "minecraft:redstone_lamp", lamp_props);

    // Support layer beneath every working layer.
    std::vector<Pos> snapshot;
    snapshot.reserve(blocks.size());
    for (const auto& kv : blocks) snapshot.push_back(kv.first);
    for (const Pos& p : snapshot) {
        if (layer_ys.count(p[1])) {
            blocks.emplace(Pos{p[0], p[1] - 1, p[2]},
                           Block{"minecraft:smooth_quartz", {}});
        }
    }

    // Redstone wire connection properties.
    refreshWireConnections(blocks);

    result.max_per_col = max_per_col;
    return result;
}

void refreshWireConnections(BlockMap& blocks) {
    for (auto& kv : blocks) {
        if (kv.second.name != "minecraft:redstone_wire") continue;
        int x = kv.first[0], y = kv.first[1], z = kv.first[2];
        std::map<std::string, std::string> p;
        p["power"] = "0";
        const char* keys[4] = {"east", "west", "south", "north"};
        for (int i = 0; i < 4; ++i) {
            int dx = DR[i][0], dz = DR[i][1];
            auto nb = blocks.find(Pos{x + dx, y, z + dz});
            if (nb != blocks.end() && wireConnectable(nb->second.name)) {
                p[keys[i]] = "side";
            } else if (isWire(blocks, Pos{x + dx, y + 1, z + dz})) {
                p[keys[i]] = "up";
            } else {
                p[keys[i]] = "none";
            }
        }
        kv.second.props = std::move(p);
    }
}

std::map<Pos, int> simulateRedstone(const BlockMap& blocks) {
    std::set<Pos> dust;
    std::map<Pos, std::vector<RepOut>> repeaters_by_input;
    for (const auto& kv : blocks) {
        const Pos& pos = kv.first;
        const Block& b = kv.second;
        if (b.name == "minecraft:redstone_wire") {
            dust.insert(pos);
        } else if (b.name == "minecraft:repeater") {
            auto fit = b.props.find("facing");
            auto dit = b.props.find("delay");
            if (fit == b.props.end() || dit == b.props.end()) continue;
            int dx = 0, dz = 0;
            if (fit->second == "east") { dx = 1; dz = 0; }
            else if (fit->second == "west") { dx = -1; dz = 0; }
            else if (fit->second == "south") { dx = 0; dz = 1; }
            else if (fit->second == "north") { dx = 0; dz = -1; }
            RepOut out;
            out.out = Pos{pos[0] - dx, pos[1], pos[2] - dz};
            out.weight = std::stoi(dit->second) * 2;
            Pos input{pos[0] + dx, pos[1], pos[2] + dz};
            repeaters_by_input[input].push_back(out);
        }
    }

    std::map<Pos, int> dist;
    using Item = std::pair<int, Pos>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    Pos src{-2, 1, 0};
    dist[src] = 0;
    pq.push({0, src});
    while (!pq.empty()) {
        Item cur = pq.top();
        pq.pop();
        int d = cur.first;
        const Pos u = cur.second;
        auto dit = dist.find(u);
        if (dit != dist.end() && d > dit->second) continue;
        if (dust.count(u)) {
            for (int i = 0; i < 4; ++i) {
                for (int dy = -1; dy <= 1; ++dy) {
                    Pos v{u[0] + DR[i][0], u[1] + dy, u[2] + DR[i][1]};
                    if (dust.count(v)) {
                        auto it = dist.find(v);
                        if (it == dist.end() || d < it->second) {
                            dist[v] = d;
                            pq.push({d, v});
                        }
                    }
                }
            }
        }
        auto rit = repeaters_by_input.find(u);
        if (rit != repeaters_by_input.end()) {
            for (const auto& rep : rit->second) {
                if (blocks.find(rep.out) == blocks.end()) continue;
                int nd = d + rep.weight;
                auto it = dist.find(rep.out);
                if (it == dist.end() || nd < it->second) {
                    dist[rep.out] = nd;
                    pq.push({nd, rep.out});
                }
            }
        }
    }
    return dist;
}

std::vector<std::pair<Pos, Pos>> noteDustAdjacency(const BlockMap& blocks) {
    std::vector<std::pair<Pos, Pos>> bad;
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                            {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& kv : blocks) {
        if (kv.second.name != "minecraft:note_block") continue;
        const Pos& pos = kv.first;
        for (int i = 0; i < 6; ++i) {
            Pos q{pos[0] + dirs[i][0], pos[1] + dirs[i][1], pos[2] + dirs[i][2]};
            if (isWire(blocks, q)) bad.emplace_back(pos, q);
        }
    }
    return bad;
}

bool validateStructure(const BuildResult& result, int resolution,
                       std::vector<std::string>& problems) {
    problems.clear();
    if (result.max_per_col > Z_CAP) {
        problems.push_back("max notes per column " + std::to_string(result.max_per_col) +
                           " exceeds Z cap " + std::to_string(Z_CAP));
    }

    auto dist = simulateRedstone(result.blocks);

    for (const auto& kv : result.tap_of_col) {
        int c = kv.first;
        Pos tap = kv.second;
        auto it = dist.find(tap);
        if (it == dist.end()) {
            problems.push_back("column " + std::to_string(c) + " tap unreachable");
            continue;
        }
        int expected = resolution * c;
        if (std::abs(it->second - expected) > TIMING_TOL_TICKS) {
            problems.push_back("column " + std::to_string(c) + " tap tick=" +
                               std::to_string(it->second) + " expected=" +
                               std::to_string(expected));
        }
    }

    for (const auto& kv : result.note_blocks) {
        int c = kv.first;
        for (const Pos& nb : kv.second) {
            auto it = dist.find(nb);
            int expected = resolution * c + 2;
            if (it == dist.end() ||
                std::abs(it->second - expected) > TIMING_TOL_TICKS) {
                problems.push_back("column " + std::to_string(c) + " note tick invalid");
            }
        }
    }

    bool lamp_found = false;
    for (const auto& kv : result.blocks) {
        if (kv.second.name != "minecraft:redstone_lamp") continue;
        lamp_found = true;
        if (dist.find(kv.first) == dist.end()) problems.push_back("lamp unreachable");
    }
    if (!lamp_found) problems.push_back("no redstone lamp");

    auto adj = noteDustAdjacency(result.blocks);
    if (!adj.empty()) {
        problems.push_back("note blocks adjacent to redstone dust: " +
                           std::to_string(adj.size()));
    }

    // Longest continuous dust run must be <= 15.
    std::set<Pos> dust;
    for (const auto& kv : result.blocks) {
        if (kv.second.name == "minecraft:redstone_wire") dust.insert(kv.first);
    }
    std::set<Pos> seen;
    for (const Pos& start : dust) {
        if (seen.count(start)) continue;
        std::vector<Pos> stack{start};
        seen.insert(start);
        std::size_t count = 0;
        while (!stack.empty()) {
            Pos u = stack.back();
            stack.pop_back();
            ++count;
            for (int i = 0; i < 4; ++i) {
                for (int dy = -1; dy <= 1; ++dy) {
                    Pos v{u[0] + DR[i][0], u[1] + dy, u[2] + DR[i][1]};
                    if (dust.count(v) && !seen.count(v)) {
                        seen.insert(v);
                        stack.push_back(v);
                    }
                }
            }
        }
        if (count > 15) {
            problems.push_back("redstone dust run of " + std::to_string(count) +
                               " cells exceeds 15");
            break;
        }
    }

    return problems.empty();
}
