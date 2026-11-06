#include "nbt.h"

#include <cstring>
#include <limits>

namespace {

struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool need(size_t n) const { return pos + n <= size; }

    bool readU8(uint8_t& v) {
        if (!need(1)) return false;
        v = data[pos++];
        return true;
    }

    bool readI8(int8_t& v) {
        if (!need(1)) return false;
        v = static_cast<int8_t>(data[pos++]);
        return true;
    }

    bool readU16(uint16_t& v) {
        if (!need(2)) return false;
        v = static_cast<uint16_t>((static_cast<uint16_t>(data[pos]) << 8) |
                                  static_cast<uint16_t>(data[pos + 1]));
        pos += 2;
        return true;
    }

    bool readI16(int16_t& v) {
        uint16_t raw = 0;
        if (!readU16(raw)) return false;
        v = static_cast<int16_t>(raw);
        return true;
    }

    bool readU32(uint32_t& v) {
        if (!need(4)) return false;
        v = (static_cast<uint32_t>(data[pos]) << 24) |
            (static_cast<uint32_t>(data[pos + 1]) << 16) |
            (static_cast<uint32_t>(data[pos + 2]) << 8) |
            static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        return true;
    }

    bool readI32(int32_t& v) {
        uint32_t raw = 0;
        if (!readU32(raw)) return false;
        v = static_cast<int32_t>(raw);
        return true;
    }

    bool readI64(int64_t& v) {
        if (!need(8)) return false;
        uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw = (raw << 8) | static_cast<uint64_t>(data[pos + i]);
        }
        pos += 8;
        v = static_cast<int64_t>(raw);
        return true;
    }

    bool readF32(float& v) {
        uint32_t raw = 0;
        if (!readU32(raw)) return false;
        std::memcpy(&v, &raw, sizeof(v));
        return true;
    }

    bool readF64(double& v) {
        if (!need(8)) return false;
        uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw = (raw << 8) | static_cast<uint64_t>(data[pos + i]);
        }
        pos += 8;
        std::memcpy(&v, &raw, sizeof(v));
        return true;
    }

    bool readString(std::string& s) {
        int16_t len = 0;
        if (!readI16(len)) return false;
        if (len < 0) return false;
        size_t n = static_cast<size_t>(len);
        if (!need(n)) return false;
        s.assign(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return true;
    }

    bool skip(size_t n) {
        if (!need(n)) return false;
        pos += n;
        return true;
    }
};

bool parsePayload(Reader& r, int type, NbtTag& tag, int depth, std::string& error);

bool parseList(Reader& r, NbtTag& tag, int depth, std::string& error) {
    uint8_t itemType = 0;
    if (!r.readU8(itemType)) {
        error = "truncated list type";
        return false;
    }
    int32_t length = 0;
    if (!r.readI32(length) || length < 0) {
        error = "invalid list length";
        return false;
    }
    // A sane upper bound for structure files.
    if (length > 50000000) {
        error = "list too large";
        return false;
    }
    tag.list.clear();
    tag.list.reserve(static_cast<size_t>(length));
    for (int32_t i = 0; i < length; ++i) {
        NbtTag child;
        child.type = itemType;
        if (itemType == 0) {
            error = "TAG_End inside list";
            return false;
        }
        if (!parsePayload(r, itemType, child, depth + 1, error)) return false;
        tag.list.push_back(std::move(child));
    }
    return true;
}

bool parseCompound(Reader& r, NbtTag& tag, int depth, std::string& error) {
    tag.compound.clear();
    while (true) {
        uint8_t childType = 0;
        if (!r.readU8(childType)) {
            error = "truncated compound";
            return false;
        }
        if (childType == 0) break;
        std::string name;
        if (!r.readString(name)) {
            error = "truncated compound key";
            return false;
        }
        NbtTag child;
        child.type = childType;
        if (!parsePayload(r, childType, child, depth + 1, error)) return false;
        tag.compound.emplace_back(std::move(name), std::move(child));
    }
    return true;
}

bool parsePayload(Reader& r, int type, NbtTag& tag, int depth, std::string& error) {
    if (depth > 256) {
        error = "NBT nesting too deep";
        return false;
    }
    tag.type = type;
    switch (type) {
        case 1: return r.readI8(tag.i8);
        case 2: return r.readI16(tag.i16);
        case 3: return r.readI32(tag.i32);
        case 4: return r.readI64(tag.i64);
        case 5: return r.readF32(tag.f32);
        case 6: return r.readF64(tag.f64);
        case 7: {
            int32_t length = 0;
            if (!r.readI32(length) || length < 0) {
                error = "invalid byte array length";
                return false;
            }
            if (length > 50000000) {
                error = "byte array too large";
                return false;
            }
            if (!r.need(static_cast<size_t>(length))) {
                error = "truncated byte array";
                return false;
            }
            tag.byteArray.assign(
                reinterpret_cast<const int8_t*>(r.data + r.pos),
                reinterpret_cast<const int8_t*>(r.data + r.pos + length));
            r.pos += static_cast<size_t>(length);
            return true;
        }
        case 8: return r.readString(tag.str);
        case 9: return parseList(r, tag, depth, error);
        case 10: return parseCompound(r, tag, depth, error);
        case 11: {
            int32_t length = 0;
            if (!r.readI32(length) || length < 0 || length > 50000000) {
                error = "invalid int array length";
                return false;
            }
            if (!r.need(static_cast<size_t>(length) * 4)) {
                error = "truncated int array";
                return false;
            }
            tag.intArray.resize(static_cast<size_t>(length));
            for (int32_t i = 0; i < length; ++i) {
                if (!r.readI32(tag.intArray[static_cast<size_t>(i)])) return false;
            }
            return true;
        }
        case 12: {
            int32_t length = 0;
            if (!r.readI32(length) || length < 0 || length > 50000000) {
                error = "invalid long array length";
                return false;
            }
            if (!r.need(static_cast<size_t>(length) * 8)) {
                error = "truncated long array";
                return false;
            }
            tag.longArray.resize(static_cast<size_t>(length));
            for (int32_t i = 0; i < length; ++i) {
                if (!r.readI64(tag.longArray[static_cast<size_t>(i)])) return false;
            }
            return true;
        }
        default:
            error = "unsupported NBT tag type " + std::to_string(type);
            return false;
    }
}

} // namespace

const NbtTag* nbtFind(const NbtTag& tag, const std::string& key) {
    for (const auto& entry : tag.compound) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

bool parseNbt(const std::vector<uint8_t>& data, NbtTag& root, std::string& error) {
    error.clear();
    Reader r{data.data(), data.size(), 0};

    uint8_t rootType = 0;
    if (!r.readU8(rootType)) {
        error = "empty NBT data";
        return false;
    }
    if (rootType != 10) {
        error = "NBT root is not a compound";
        return false;
    }
    std::string rootName;
    if (!r.readString(rootName)) {
        error = "truncated NBT root name";
        return false;
    }
    if (!parsePayload(r, rootType, root, 0, error)) return false;
    return true;
}
