#pragma once

#include "midi_io.h"
#include "model.h"

#include <map>
#include <string>
#include <vector>

struct Bank {
    std::string inst;
    std::string mat;
    int base = 0;
};

struct PitchMap {
    bool has_bank = false;
    Bank bank;
    int shift = 0;
    MappedNote conv(int pitch) const;
};

std::vector<Note> loadNotes(const MidiFileData& midi);
PitchMap buildPitchMap(const std::vector<int>& pitches, const std::string* track);
std::vector<int> segmentDelays(int ticks);

BuildResult buildStructure(int resolution,
                           const std::map<int, std::vector<MappedNote>>& col_notes,
                           int max_per_col, bool simple, const std::string* track,
                           const std::vector<int>* timeline);

std::map<Pos, int> simulateRedstone(const BlockMap& blocks);
std::vector<std::pair<Pos, Pos>> noteDustAdjacency(const BlockMap& blocks);
void refreshWireConnections(BlockMap& blocks);

// Returns true when all hard constraints pass; problems are appended otherwise.
bool validateStructure(const BuildResult& result, int resolution,
                       std::vector<std::string>& problems);
