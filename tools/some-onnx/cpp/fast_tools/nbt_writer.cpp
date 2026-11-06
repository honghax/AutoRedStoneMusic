#include "nbt_writer.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

namespace {

void putU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void putI16(std::vector<std::uint8_t>& out, int v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void putI32(std::vector<std::uint8_t>& out, int v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void putString(std::vector<std::uint8_t>& out, const std::string& s) {
    putI16(out, static_cast<int>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void putNamed(std::vector<std::uint8_t>& out, std::uint8_t type, const std::string& name) {
    putU8(out, type);
    putString(out, name);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> gzipStored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    if (data.empty()) {
        out.insert(out.end(), {0x01, 0x00, 0x00, 0xFF, 0xFF});
    } else {
        std::size_t offset = 0;
        while (offset < data.size()) {
            std::size_t chunk = data.size() - offset;
            if (chunk > 65535) chunk = 65535;
            bool last = (offset + chunk) >= data.size();
            out.push_back(last ? 0x01 : 0x00);
            out.push_back(static_cast<std::uint8_t>(chunk & 0xFF));
            out.push_back(static_cast<std::uint8_t>((chunk >> 8) & 0xFF));
            std::uint16_t nlen = static_cast<std::uint16_t>(~chunk);
            out.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
            out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                       data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            offset += chunk;
        }
    }
    std::uint32_t crc = crc32(data.data(), data.size());
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((crc >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((crc >> 24) & 0xFF));
    std::uint32_t isize = static_cast<std::uint32_t>(data.size());
    out.push_back(static_cast<std::uint8_t>(isize & 0xFF));
    out.push_back(static_cast<std::uint8_t>((isize >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((isize >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((isize >> 24) & 0xFF));
    return out;
}

} // namespace

bool writeStructureNbt(const std::string& path, const BlockMap& blocks,
                       int& size_x, int& size_y, int& size_z, std::string& error) {
    error.clear();
    if (blocks.empty()) {
        error = "no blocks to write";
        return false;
    }

    int minx = blocks.begin()->first[0];
    int miny = blocks.begin()->first[1];
    int minz = blocks.begin()->first[2];
    int maxx = minx, maxy = miny, maxz = minz;
    for (const auto& kv : blocks) {
        minx = std::min(minx, kv.first[0]);
        miny = std::min(miny, kv.first[1]);
        minz = std::min(minz, kv.first[2]);
        maxx = std::max(maxx, kv.first[0]);
        maxy = std::max(maxy, kv.first[1]);
        maxz = std::max(maxz, kv.first[2]);
    }
    size_x = maxx - minx + 1;
    size_y = maxy - miny + 1;
    size_z = maxz - minz + 1;

    using PaletteKey = std::pair<std::string, std::map<std::string, std::string>>;
    std::map<PaletteKey, int> index;
    std::vector<PaletteKey> palette;
    std::vector<std::pair<Pos, int>> block_entries;
    block_entries.reserve(blocks.size());
    for (const auto& kv : blocks) {
        PaletteKey key{kv.second.name, kv.second.props};
        auto it = index.find(key);
        int id = 0;
        if (it == index.end()) {
            id = static_cast<int>(palette.size());
            index.emplace(key, id);
            palette.push_back(key);
        } else {
            id = it->second;
        }
        block_entries.emplace_back(kv.first, id);
    }

    std::vector<std::uint8_t> data;
    // Root compound with empty name.
    putNamed(data, 10, "");
    putNamed(data, 3, "DataVersion");
    putI32(data, 3465);
    putNamed(data, 9, "size");
    putU8(data, 3);
    putI32(data, 3);
    putI32(data, size_x);
    putI32(data, size_y);
    putI32(data, size_z);
    putNamed(data, 9, "palette");
    putU8(data, 10);
    putI32(data, static_cast<int>(palette.size()));
    for (const auto& entry : palette) {
        putNamed(data, 8, "Name");
        putString(data, entry.first);
        if (!entry.second.empty()) {
            putNamed(data, 10, "Properties");
            for (const auto& prop : entry.second) {
                putNamed(data, 8, prop.first);
                putString(data, prop.second);
            }
            putU8(data, 0);
        }
        putU8(data, 0);  // end palette compound
    }
    putNamed(data, 9, "blocks");
    putU8(data, 10);
    putI32(data, static_cast<int>(block_entries.size()));
    for (const auto& entry : block_entries) {
        putNamed(data, 9, "pos");
        putU8(data, 3);
        putI32(data, 3);
        putI32(data, entry.first[0] - minx);
        putI32(data, entry.first[1] - miny);
        putI32(data, entry.first[2] - minz);
        putNamed(data, 3, "state");
        putI32(data, entry.second);
        putU8(data, 0);  // end block compound
    }
    putNamed(data, 9, "entities");
    putU8(data, 10);
    putI32(data, 0);
    putU8(data, 0);  // end root compound

    std::vector<std::uint8_t> gz = gzipStored(data);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write NBT file: " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(gz.data()),
              static_cast<std::streamsize>(gz.size()));
    if (!out) {
        error = "failed writing NBT file: " + path;
        return false;
    }
    return true;
}
