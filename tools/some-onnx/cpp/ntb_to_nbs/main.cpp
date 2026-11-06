#include "inflate_gzip.h"
#include "nbt.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct Pos {
    int x = 0;
    int y = 0;
    int z = 0;
};

bool operator==(const Pos& a, const Pos& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

struct PosHash {
    size_t operator()(const Pos& p) const {
        size_t h = static_cast<size_t>(p.x) * 73856093u;
        h ^= static_cast<size_t>(p.y) * 19349663u;
        h ^= static_cast<size_t>(p.z) * 83492791u;
        return h;
    }
};

struct BlockInfo {
    std::string name;
    std::map<std::string, std::string> props;
};

using BlockMap = std::unordered_map<Pos, BlockInfo, PosHash>;

struct Event {
    int tick = 0;
    std::string instrument;
    int note = 0;
};

const std::map<std::string, std::pair<int, int>> DIRS = {
    {"east", {1, 0}},
    {"west", {-1, 0}},
    {"south", {0, 1}},
    {"north", {0, -1}},
};

const std::map<std::string, int> NBS_INSTRUMENT = {
    {"harp", 0},
    {"bass", 1},
    {"basedrum", 2},
    {"snare", 3},
    {"hat", 4},
    {"guitar", 5},
    {"flute", 6},
    {"bell", 7},
    {"chime", 8},
    {"xylophone", 9},
    {"iron_xylophone", 10},
    {"cow_bell", 11},
    {"didgeridoo", 12},
    {"bit", 13},
    {"banjo", 14},
    {"pling", 15},
};

// NBS key 0 = A0 (MIDI 21). The key is an absolute pitch; the instrument's
// base pitch determines which part of 0..87 is actually usable.
//   bass / didgeridoo : F#1 -> MIDI 30 -> key 9
//   harp / guitar / flute / bit / banjo : F#3 -> MIDI 54 -> key 33
//   bell / chime / xylophone / iron_xylophone / cow_bell / pling : F#5 -> MIDI 78 -> key 57
// Unpitched instruments (basedrum/snare/hat) keep key 33; NBS ignores their pitch.
int nbsBaseKey(const std::string& instrument) {
    if (instrument == "bass" || instrument == "didgeridoo") return 9;
    if (instrument == "bell" || instrument == "chime" ||
        instrument == "xylophone" || instrument == "iron_xylophone" ||
        instrument == "cow_bell" || instrument == "pling") {
        return 57;
    }
    return 33;
}

bool loadStructure(const std::vector<uint8_t>& bytes, BlockMap& blocks,
                   std::vector<int>& size, std::string& error) {
    std::vector<uint8_t> raw;
    if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B) {
        std::string gzError;
        if (!gzipDecompress(bytes.data(), bytes.size(), raw, gzError)) {
            error = "gzip: " + gzError;
            return false;
        }
    } else {
        raw = bytes;
    }

    NbtTag root;
    if (!parseNbt(raw, root, error)) return false;

    const NbtTag* palette = nbtFind(root, "palette");
    const NbtTag* blockList = nbtFind(root, "blocks");
    const NbtTag* sizeTag = nbtFind(root, "size");
    if (!palette || palette->type != 9 || !blockList || blockList->type != 9 ||
        !sizeTag || sizeTag->type != 9 || sizeTag->list.size() != 3) {
        error = "missing palette/blocks/size in structure NBT";
        return false;
    }

    for (const NbtTag& v : sizeTag->list) {
        if (v.type != 3) {
            error = "size entries are not TAG_Int";
            return false;
        }
        size.push_back(v.i32);
    }

    blocks.clear();
    for (const NbtTag& entry : blockList->list) {
        if (entry.type != 10) {
            error = "block entry is not a compound";
            return false;
        }
        const NbtTag* posTag = nbtFind(entry, "pos");
        const NbtTag* stateTag = nbtFind(entry, "state");
        if (!posTag || posTag->type != 9 || posTag->list.size() != 3 ||
            !stateTag || stateTag->type != 3) {
            error = "invalid block entry";
            return false;
        }
        Pos p;
        for (int i = 0; i < 3; ++i) {
            if (posTag->list[static_cast<size_t>(i)].type != 3) {
                error = "block pos is not TAG_Int";
                return false;
            }
        }
        p.x = posTag->list[0].i32;
        p.y = posTag->list[1].i32;
        p.z = posTag->list[2].i32;

        int state = stateTag->i32;
        if (state < 0 || state >= static_cast<int>(palette->list.size())) {
            error = "block state index out of range";
            return false;
        }
        const NbtTag& paletteEntry = palette->list[static_cast<size_t>(state)];
        const NbtTag* nameTag = nbtFind(paletteEntry, "Name");
        if (!nameTag || nameTag->type != 8) {
            error = "palette entry has no Name";
            return false;
        }
        BlockInfo info;
        info.name = nameTag->str;
        const NbtTag* propsTag = nbtFind(paletteEntry, "Properties");
        if (propsTag && propsTag->type == 10) {
            for (const auto& kv : propsTag->compound) {
                if (kv.second.type == 8) {
                    info.props[kv.first] = kv.second.str;
                }
            }
        }
        blocks[p] = std::move(info);
    }
    return true;
}

bool simulate(const BlockMap& blocks, std::unordered_map<Pos, int, PosHash>& dist) {
    Pos source{-2, 1, 0};

    std::unordered_set<Pos, PosHash> dust;
    struct Link {
        Pos out;
        int delay = 1;
    };
    std::unordered_map<Pos, std::vector<Link>, PosHash> repeatersByInput;

    for (const auto& kv : blocks) {
        const Pos& p = kv.first;
        const BlockInfo& info = kv.second;
        if (info.name == "minecraft:redstone_wire") {
            dust.insert(p);
        } else if (info.name == "minecraft:repeater") {
            auto facingIt = info.props.find("facing");
            if (facingIt == info.props.end()) continue;
            auto dirIt = DIRS.find(facingIt->second);
            if (dirIt == DIRS.end()) continue;
            int dx = dirIt->second.first;
            int dz = dirIt->second.second;
            Pos input{p.x + dx, p.y, p.z + dz};
            Pos output{p.x - dx, p.y, p.z - dz};
            int delay = 1;
            auto delayIt = info.props.find("delay");
            if (delayIt != info.props.end()) {
                try {
                    delay = std::stoi(delayIt->second);
                } catch (...) {
                    delay = 1;
                }
            }
            repeatersByInput[input].push_back(Link{output, delay});
        }
    }

    using Item = std::pair<int, Pos>;
    struct Greater {
        bool operator()(const Item& a, const Item& b) const {
            return a.first > b.first;
        }
    };
    std::priority_queue<Item, std::vector<Item>, Greater> pq;

    auto relax = [&](const Pos& p, int nd) {
        auto it = dist.find(p);
        if (it == dist.end() || nd < it->second) {
            dist[p] = nd;
            pq.push(Item{nd, p});
        }
    };

    dist.clear();
    dist[source] = 0;
    pq.push(Item{0, source});

    while (!pq.empty()) {
        Item item = pq.top();
        pq.pop();
        int d = item.first;
        Pos u = item.second;
        auto distIt = dist.find(u);
        if (distIt == dist.end() || d > distIt->second) continue;

        if (dust.find(u) != dust.end()) {
            for (const auto& dir : DIRS) {
                int dx = dir.second.first;
                int dz = dir.second.second;
                for (int vy : {u.y, u.y + 1, u.y - 1}) {
                    Pos v{u.x + dx, vy, u.z + dz};
                    if (dust.find(v) != dust.end()) relax(v, d);
                }
            }
        }

        auto repIt = repeatersByInput.find(u);
        if (repIt != repeatersByInput.end()) {
            for (const Link& link : repIt->second) {
                if (blocks.find(link.out) == blocks.end()) continue;
                int nd = d + link.delay * 2;
                relax(link.out, nd);
            }
        }
    }
    return true;
}

void writeU8(std::ostream& out, int v) {
    out.put(static_cast<char>(v & 0xFF));
}

void writeLE16(std::ostream& out, int v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
}

void writeLE32(std::ostream& out, int v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
    out.put(static_cast<char>((v >> 16) & 0xFF));
    out.put(static_cast<char>((v >> 24) & 0xFF));
}

void writeString(std::ostream& out, const std::string& s) {
    writeLE32(out, static_cast<int>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

struct Layer {
    std::string name;
    std::string instrument;
};

struct NbsNote {
    int tick = 0;
    int layer = 0;
    int instrument = 0;
    int key = 0;
};

bool writeNbs(const std::vector<Event>& events, const std::string& title,
              const std::string& path, std::string& error, int& layerCount,
              int& noteCount) {
    if (events.empty()) {
        error = "no note events to write";
        return false;
    }

    std::map<int, std::vector<const Event*>> byTick;
    for (const Event& e : events) byTick[e.tick].push_back(&e);

    std::vector<Layer> layers;
    std::map<std::string, std::vector<int>> instrumentLayers;
    std::vector<NbsNote> notes;

    for (const auto& tickGroup : byTick) {
        int tick = tickGroup.first;
        std::map<std::string, std::vector<const Event*>> byInstrument;
        for (const Event* e : tickGroup.second) byInstrument[e->instrument].push_back(e);

        for (const auto& instGroup : byInstrument) {
            const std::string& inst = instGroup.first;
            auto idIt = NBS_INSTRUMENT.find(inst);
            if (idIt == NBS_INSTRUMENT.end()) {
                error = "unsupported note block instrument: " + inst;
                return false;
            }
            int id = idIt->second;
            const std::vector<const Event*>& list = instGroup.second;
            std::vector<int>& layerVec = instrumentLayers[inst];
            for (size_t i = 0; i < list.size(); ++i) {
                if (layerVec.size() <= i) {
                    int index = static_cast<int>(layers.size());
                    layers.push_back(Layer{inst + " " + std::to_string(i + 1), inst});
                    layerVec.push_back(index);
                }
                int layer = layerVec[i];
                int key = nbsBaseKey(inst) + list[i]->note;
                if (key < 0 || key > 87) {
                    error = "NBS key out of range for " + inst + ": note " +
                            std::to_string(list[i]->note) + " -> key " +
                            std::to_string(key);
                    return false;
                }
                notes.push_back(NbsNote{tick, layer, id, key});
            }
        }
    }

    std::sort(notes.begin(), notes.end(), [](const NbsNote& a, const NbsNote& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.layer < b.layer;
    });

    int songLength = notes.back().tick + 1;
    if (songLength < 1) songLength = 1;
    if (songLength > 32767) {
        error = "song length exceeds NBS 16-bit tick range";
        return false;
    }
    if (layers.size() > 32767) {
        error = "too many NBS layers";
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot open output file: " + path;
        return false;
    }

    // Header (NBS v4).
    writeLE16(out, 0);              // marker: new format
    writeU8(out, 4);                // NBS version
    writeU8(out, 16);               // vanilla instrument count
    writeLE16(out, songLength);
    writeLE16(out, static_cast<int>(layers.size()));
    writeString(out, title);
    writeString(out, "AutoRedStoneMusic");
    writeString(out, "");
    writeString(out, "Converted from Minecraft structure");
    writeLE16(out, 2000);           // tempo: 20.0 ticks per second (x100)
    writeU8(out, 0);                // auto-saving
    writeU8(out, 0);                // auto-saving duration
    writeU8(out, 4);                // time signature
    writeLE32(out, 0);              // minutes spent
    writeLE32(out, 0);              // left clicks
    writeLE32(out, 0);              // right clicks
    writeLE32(out, 0);              // note blocks added
    writeLE32(out, 0);              // note blocks removed
    writeString(out, "");           // MIDI file name
    writeU8(out, 0);                // loop on/off
    writeU8(out, 0);                // max loop count
    writeLE16(out, 0);              // loop start tick

    // Note blocks, run-length encoded by tick and layer jumps.
    int previousTick = -1;
    size_t i = 0;
    while (i < notes.size()) {
        int tick = notes[i].tick;
        writeLE16(out, tick - previousTick);
        previousTick = tick;

        int previousLayer = -1;
        while (i < notes.size() && notes[i].tick == tick) {
            writeLE16(out, notes[i].layer - previousLayer);
            previousLayer = notes[i].layer;
            writeU8(out, notes[i].instrument);
            writeU8(out, notes[i].key);
            writeU8(out, 100);      // velocity
            writeU8(out, 100);      // panning
            writeLE16(out, 0);      // pitch
            ++i;
        }
        writeLE16(out, 0);
    }
    writeLE16(out, 0);

    // Layer metadata, in layer order.
    for (const Layer& layer : layers) {
        writeString(out, layer.name);
        writeU8(out, 0);            // locked
        writeU8(out, 100);          // volume
        writeU8(out, 100);          // stereo panning (100 = centre)
    }

    out.close();
    if (!out) {
        error = "failed while writing output file";
        return false;
    }

    layerCount = static_cast<int>(layers.size());
    noteCount = static_cast<int>(notes.size());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: ntb_to_nbs <input.nbt> <output.nbs> [--name <song name>]\n";
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];
    std::string title = std::filesystem::path(inputPath).stem().string();
    if (argc >= 5 && std::string(argv[3]) == "--name") {
        title = argv[4];
    }

    std::ifstream in(inputPath, std::ios::binary);
    if (!in) {
        std::cerr << "Cannot open input: " << inputPath << "\n";
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    in.close();

    BlockMap blocks;
    std::vector<int> size;
    std::string error;
    if (!loadStructure(bytes, blocks, size, error)) {
        std::cerr << "NBT error: " << error << "\n";
        return 1;
    }

    Pos observer;
    bool foundObserver = false;
    std::string facing;
    for (const auto& kv : blocks) {
        if (kv.second.name == "minecraft:observer") {
            observer = kv.first;
            auto it = kv.second.props.find("facing");
            if (it != kv.second.props.end()) {
                facing = it->second;
                foundObserver = true;
            }
            break;
        }
    }
    if (!foundObserver) {
        std::cerr << "No observer with a facing property found in NBT\n";
        return 1;
    }
    auto dirIt = DIRS.find(facing);
    if (dirIt == DIRS.end()) {
        std::cerr << "Unsupported observer facing: " << facing << "\n";
        return 1;
    }
    int dx = dirIt->second.first;
    int dz = dirIt->second.second;

    int shiftX = observer.x - dx + 2;
    int shiftY = observer.y - 1;
    int shiftZ = observer.z - dz;

    BlockMap shifted;
    shifted.reserve(blocks.size());
    for (const auto& kv : blocks) {
        Pos p{kv.first.x - shiftX, kv.first.y - shiftY, kv.first.z - shiftZ};
        shifted.emplace(p, kv.second);
    }

    std::unordered_map<Pos, int, PosHash> dist;
    simulate(shifted, dist);

    std::vector<Event> events;
    int unreachable = 0;
    for (const auto& kv : shifted) {
        if (kv.second.name != "minecraft:note_block") continue;
        auto distIt = dist.find(kv.first);
        if (distIt == dist.end()) {
            ++unreachable;
            continue;
        }
        auto instIt = kv.second.props.find("instrument");
        auto noteIt = kv.second.props.find("note");
        if (instIt == kv.second.props.end() || noteIt == kv.second.props.end()) {
            continue;
        }
        int note = 0;
        try {
            note = std::stoi(noteIt->second);
        } catch (...) {
            continue;
        }
        events.push_back(Event{distIt->second, instIt->second, note});
    }

    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.instrument != b.instrument) return a.instrument < b.instrument;
        return a.note < b.note;
    });

    int layerCount = 0;
    int noteCount = 0;
    if (!writeNbs(events, title, outputPath, error, layerCount, noteCount)) {
        std::cerr << "NBS error: " << error << "\n";
        return 1;
    }

    int maxTick = events.empty() ? 0 : events.back().tick;
    std::cerr << "[NBT] size=" << size[0] << "x" << size[1] << "x" << size[2]
              << " blocks=" << blocks.size()
              << " note_blocks=" << events.size()
              << " unreachable=" << unreachable
              << " max_tick=" << maxTick << "\n";
    std::cerr << "[NBS] notes=" << noteCount << " layers=" << layerCount
              << " -> " << outputPath << "\n";
    return 0;
}
