#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class EvKind {
    NoteOn,
    NoteOff,
    SetTempo,
    ProgramChange,
    Other
};

struct MidiEvent {
    int delta = 0;
    EvKind kind = EvKind::Other;
    int channel = 0;
    int note = 0;
    int velocity = 0;
    int tempo = 0;
    int program = 0;
};

struct MidiTrackData {
    std::string name;
    std::vector<MidiEvent> events;
};

struct MidiFileData {
    int format = 1;
    int tpq = 480;
    std::vector<MidiTrackData> tracks;
};

bool readMidiFile(const std::string& path, MidiFileData& out, std::string& error);

// Playback length in seconds using mido's merged-track semantics.
double midiLengthSeconds(const MidiFileData& midi);

struct MergeNoteEvent {
    int tick = 0;
    bool on = false;
    int note = 0;
    int channel = 0;
};

bool writeMergedMidi(const std::string& path, int tpq, int tempo,
                     const std::vector<MergeNoteEvent>& events, std::string& error);
