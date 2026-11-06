#include "midi_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    bool need(std::size_t n) const { return pos <= size && n <= size - pos; }

    bool u8(std::uint8_t& v) {
        if (!need(1)) return false;
        v = data[pos++];
        return true;
    }

    bool u16(std::uint16_t& v) {
        if (!need(2)) return false;
        v = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[pos]) << 8) |
                                       static_cast<std::uint16_t>(data[pos + 1]));
        pos += 2;
        return true;
    }

    bool u32(std::uint32_t& v) {
        if (!need(4)) return false;
        v = (static_cast<std::uint32_t>(data[pos]) << 24) |
            (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
            (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
            static_cast<std::uint32_t>(data[pos + 3]);
        pos += 4;
        return true;
    }

    bool vlq(int& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = 0;
            if (!u8(b)) return false;
            value = (value << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) return true;
        }
        return false;
    }
};

struct RawEvent {
    int abs_tick = 0;
    const MidiEvent* ev = nullptr;
};

} // namespace

bool readMidiFile(const std::string& path, MidiFileData& out, std::string& error) {
    error.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open MIDI file: " + path;
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    Reader r{bytes.data(), bytes.size(), 0};

    std::uint32_t magic = 0;
    if (!r.need(4)) {
        error = "empty MIDI file";
        return false;
    }
    std::memcpy(&magic, bytes.data(), 4);
    r.pos = 4;
    std::uint32_t header_len = 0;
    std::uint16_t format = 0, ntracks = 0, division = 0;
    if (!r.u32(header_len) || !r.u16(format) || !r.u16(ntracks) || !r.u16(division)) {
        error = "truncated MIDI header";
        return false;
    }
    (void)magic;
    if (header_len < 6) {
        error = "invalid MIDI header length";
        return false;
    }
    if (r.pos + (header_len - 6) > bytes.size()) {
        error = "truncated MIDI header";
        return false;
    }
    r.pos += header_len - 6;
    if ((division & 0x8000u) != 0) {
        error = "SMPTE time division is not supported";
        return false;
    }

    out.format = static_cast<int>(format);
    out.tpq = static_cast<int>(division);
    out.tracks.clear();

    while (out.tracks.size() < ntracks && r.need(8)) {
        std::uint8_t id[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) r.u8(id[i]);
        std::uint32_t len = 0;
        if (!r.u32(len)) {
            error = "truncated MIDI chunk header";
            return false;
        }
        if (!r.need(len)) {
            error = "truncated MIDI chunk";
            return false;
        }
        std::size_t chunk_end = r.pos + len;
        if (std::memcmp(id, "MTrk", 4) != 0) {
            r.pos = chunk_end;
            continue;
        }

        MidiTrackData track;
        int status = -1;
        int abs_tick = 0;
        while (r.pos < chunk_end) {
            int delta = 0;
            if (!r.vlq(delta)) {
                error = "invalid delta time";
                return false;
            }
            abs_tick += delta;
            std::uint8_t b = 0;
            if (!r.u8(b)) {
                error = "truncated track event";
                return false;
            }
            MidiEvent ev;
            ev.delta = delta;

            if (b < 0x80) {
                if (status < 0) {
                    error = "running status without status byte";
                    return false;
                }
                int hi = (status >> 4) & 0x0F;
                int needed = (hi == 0xC || hi == 0xD) ? 0 : 1;
                int data1 = static_cast<int>(b);
                int data2 = 0;
                if (needed > 0 && !r.u8(reinterpret_cast<std::uint8_t&>(data2))) {
                    error = "truncated running status event";
                    return false;
                }
                ev.channel = status & 0x0F;
                if (hi == 0x8) {
                    ev.kind = EvKind::NoteOff;
                    ev.note = data1;
                    ev.velocity = data2;
                } else if (hi == 0x9) {
                    if (data2 > 0) {
                        ev.kind = EvKind::NoteOn;
                        ev.note = data1;
                        ev.velocity = data2;
                    } else {
                        ev.kind = EvKind::NoteOff;
                        ev.note = data1;
                        ev.velocity = 0;
                    }
                } else {
                    ev.kind = EvKind::Other;
                }
                track.events.push_back(ev);
                continue;
            }

            if (b == 0xFF) {
                std::uint8_t meta_type = 0;
                int meta_len = 0;
                if (!r.u8(meta_type) || !r.vlq(meta_len)) {
                    error = "truncated meta event";
                    return false;
                }
                if (!r.need(static_cast<std::size_t>(meta_len))) {
                    error = "truncated meta payload";
                    return false;
                }
                if (meta_type == 0x51 && meta_len == 3) {
                    ev.kind = EvKind::SetTempo;
                    ev.tempo = (static_cast<int>(bytes[r.pos]) << 16) |
                               (static_cast<int>(bytes[r.pos + 1]) << 8) |
                               static_cast<int>(bytes[r.pos + 2]);
                } else {
                    ev.kind = EvKind::Other;
                }
                r.pos += static_cast<std::size_t>(meta_len);
                track.events.push_back(ev);
                if (meta_type == 0x2F) break;
                status = -1;
                continue;
            }

            if (b == 0xF0 || b == 0xF7) {
                int sysex_len = 0;
                if (!r.vlq(sysex_len)) {
                    error = "truncated sysex length";
                    return false;
                }
                if (!r.need(static_cast<std::size_t>(sysex_len))) {
                    error = "truncated sysex payload";
                    return false;
                }
                r.pos += static_cast<std::size_t>(sysex_len);
                ev.kind = EvKind::Other;
                track.events.push_back(ev);
                status = -1;
                continue;
            }

            if (b >= 0x80 && b <= 0xEF) {
                status = b;
                int hi = (b >> 4) & 0x0F;
                int nbytes = (hi == 0xC || hi == 0xD) ? 1 : 2;
                std::uint8_t d1 = 0, d2 = 0;
                if (!r.u8(d1)) {
                    error = "truncated channel event";
                    return false;
                }
                if (nbytes == 2 && !r.u8(d2)) {
                    error = "truncated channel event";
                    return false;
                }
                ev.channel = b & 0x0F;
                if (hi == 0x8) {
                    ev.kind = EvKind::NoteOff;
                    ev.note = d1;
                    ev.velocity = d2;
                } else if (hi == 0x9) {
                    if (d2 > 0) {
                        ev.kind = EvKind::NoteOn;
                        ev.note = d1;
                        ev.velocity = d2;
                    } else {
                        ev.kind = EvKind::NoteOff;
                        ev.note = d1;
                        ev.velocity = 0;
                    }
                } else {
                    ev.kind = EvKind::Other;
                }
                track.events.push_back(ev);
                continue;
            }

            // Other system common messages: skip their data bytes.
            if (b == 0xF2) {
                if (!r.need(2)) {
                    error = "truncated system message";
                    return false;
                }
                r.pos += 2;
            } else if (b == 0xF1 || b == 0xF3) {
                if (!r.need(1)) {
                    error = "truncated system message";
                    return false;
                }
                r.pos += 1;
            }
            ev.kind = EvKind::Other;
            track.events.push_back(ev);
            status = -1;
        }
        r.pos = chunk_end;
        out.tracks.push_back(std::move(track));
    }

    if (out.tracks.empty()) {
        error = "MIDI file has no tracks";
        return false;
    }
    if (out.tpq <= 0) {
        error = "invalid ticks per beat";
        return false;
    }
    return true;
}

double midiLengthSeconds(const MidiFileData& midi) {
    std::vector<RawEvent> merged;
    for (const auto& track : midi.tracks) {
        int abs = 0;
        for (const auto& ev : track.events) {
            abs += ev.delta;
            merged.push_back(RawEvent{abs, &ev});
        }
    }
    std::stable_sort(merged.begin(), merged.end(),
                     [](const RawEvent& a, const RawEvent& b) { return a.abs_tick < b.abs_tick; });

    const double tpq = static_cast<double>(midi.tpq);
    int tempo = 500000;
    int prev = 0;
    double now = 0.0;
    for (const auto& m : merged) {
        int delta = m.abs_tick - prev;
        if (delta > 0) {
            double scale = static_cast<double>(tempo) * 1e-6 / tpq;
            now += static_cast<double>(delta) * scale;
        }
        if (m.ev->kind == EvKind::SetTempo) tempo = m.ev->tempo;
        prev = m.abs_tick;
    }
    return now;
}

namespace {

void appendVlq(std::vector<std::uint8_t>& out, int value) {
    if (value < 0) value = 0;
    std::uint8_t buf[4];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
    while (value > 0) {
        buf[n++] = static_cast<std::uint8_t>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

bool writeChunk(std::vector<std::uint8_t>& file, const char* id,
                const std::vector<std::uint8_t>& payload) {
    file.insert(file.end(), id, id + 4);
    appendU32(file, static_cast<std::uint32_t>(payload.size()));
    file.insert(file.end(), payload.begin(), payload.end());
    return true;
}

} // namespace

bool writeMergedMidi(const std::string& path, int tpq, int tempo,
                     const std::vector<MergeNoteEvent>& events, std::string& error) {
    error.clear();
    std::vector<std::uint8_t> file;
    file.insert(file.end(), {'M', 'T', 'h', 'd'});
    appendU32(file, 6);
    appendU16(file, 1);
    appendU16(file, 2);
    appendU16(file, static_cast<std::uint16_t>(tpq));

    std::vector<std::uint8_t> tempo_track;
    appendVlq(tempo_track, 0);
    tempo_track.push_back(0xFF);
    tempo_track.push_back(0x51);
    tempo_track.push_back(0x03);
    tempo_track.push_back(static_cast<std::uint8_t>((tempo >> 16) & 0xFF));
    tempo_track.push_back(static_cast<std::uint8_t>((tempo >> 8) & 0xFF));
    tempo_track.push_back(static_cast<std::uint8_t>(tempo & 0xFF));
    appendVlq(tempo_track, 0);
    tempo_track.push_back(0xFF);
    tempo_track.push_back(0x2F);
    tempo_track.push_back(0x00);
    writeChunk(file, "MTrk", tempo_track);

    std::vector<std::uint8_t> note_track;
    int now = 0;
    for (const auto& ev : events) {
        int delta = ev.tick - now;
        if (delta < 0) delta = 0;
        appendVlq(note_track, delta);
        now = ev.tick;
        std::uint8_t status = static_cast<std::uint8_t>(
            (ev.on ? 0x90 : 0x80) | (ev.channel & 0x0F));
        note_track.push_back(status);
        note_track.push_back(static_cast<std::uint8_t>(ev.note & 0x7F));
        note_track.push_back(static_cast<std::uint8_t>(ev.on ? 80 : 0));
    }
    appendVlq(note_track, 0);
    note_track.push_back(0xFF);
    note_track.push_back(0x2F);
    note_track.push_back(0x00);
    writeChunk(file, "MTrk", note_track);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write MIDI file: " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
    if (!out) {
        error = "failed writing MIDI file: " + path;
        return false;
    }
    return true;
}
