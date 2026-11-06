#pragma once

#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using Pos = std::array<int, 3>;

struct Block {
    std::string name;
    std::map<std::string, std::string> props;
};

using BlockMap = std::map<Pos, Block>;

struct Note {
    int pitch = 0;
    double start = 0.0;
    double end = 0.0;
};

struct MappedNote {
    std::string inst;
    std::string mat;
    int note = 0;
};

using NoteKey = std::tuple<std::string, std::string, int>;  // (instrument, pad material, note value)

struct BuildResult {
    BlockMap blocks;
    std::map<int, Pos> tap_of_col;
    std::map<int, std::vector<Pos>> bus_dust;
    std::map<int, std::vector<Pos>> branch_reps;
    std::map<int, std::vector<Pos>> note_blocks;
    std::map<int, int> col_layer;
    std::set<int> layer_ys;
    int max_per_col = 0;
};

struct LaneStats {
    int notes = 0;
    int out_of_range = 0;
    int merged_duplicates = 0;
    int capped = 0;
    int max_per_col = 0;
    int used_columns = 0;
};

// Python 3 round() semantics: round half to even, then int().
inline int pyRound(double v) {
    double f = std::floor(v);
    double diff = v - f;
    if (diff < 0.5) return static_cast<int>(f);
    if (diff > 0.5) return static_cast<int>(f) + 1;
    int i = static_cast<int>(f);
    return (i % 2 == 0) ? i : i + 1;
}
