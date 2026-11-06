#include "inflate_gzip.h"

#include <algorithm>
#include <cstring>

namespace {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint32_t buf = 0;
    int bits = 0;

    // Fill the buffer until at least `count` bits are available (strict).
    bool peek(int count, uint32_t& value) {
        while (bits < count && pos < size) {
            buf |= static_cast<uint32_t>(data[pos++]) << bits;
            bits += 8;
        }
        if (bits < count) return false;
        value = buf & ((1u << count) - 1);
        return true;
    }

    // Fill as much as possible; missing high bits are zero-filled for Huffman
    // table lookup. Returns false only when no bits are available at all.
    bool peekPartial(int count, uint32_t& value) {
        while (bits < count && pos < size) {
            buf |= static_cast<uint32_t>(data[pos++]) << bits;
            bits += 8;
        }
        if (bits == 0) return false;
        int take = std::min(bits, count);
        value = buf & ((1u << take) - 1);
        return true;
    }

    bool read(int count, uint32_t& value) {
        if (!peek(count, value)) return false;
        buf >>= count;
        bits -= count;
        return true;
    }

    void alignByte() {
        // Only used immediately after a 3-bit block header, so `bits` is < 8.
        // Dropping the buffered remainder aligns `pos` to the next byte.
        buf = 0;
        bits = 0;
    }
};

struct Huffman {
    int maxBits = 0;
    std::vector<uint16_t> symbols;
    std::vector<uint8_t> lengths;

    bool build(const std::vector<int>& lens, std::string& error) {
        maxBits = 0;
        for (int l : lens) maxBits = std::max(maxBits, l);
        if (maxBits == 0) {
            symbols.clear();
            lengths.clear();
            return true;
        }
        if (maxBits > 15) {
            error = "invalid Huffman code length";
            return false;
        }
        int tableSize = 1 << maxBits;
        symbols.assign(tableSize, 0);
        lengths.assign(tableSize, 0);

        std::vector<int> counts(maxBits + 1, 0);
        for (int l : lens) if (l > 0) counts[l]++;

        std::vector<int> next(maxBits + 1, 0);
        int code = 0;
        for (int l = 1; l <= maxBits; ++l) {
            code = (code + counts[l - 1]) << 1;
            next[l] = code;
        }

        for (int sym = 0; sym < static_cast<int>(lens.size()); ++sym) {
            int l = lens[sym];
            if (l == 0) continue;
            int c = next[l]++;
            int rev = 0;
            for (int i = 0; i < l; ++i) {
                rev = (rev << 1) | ((c >> i) & 1);
            }
            for (int i = rev; i < tableSize; i += (1 << l)) {
                symbols[i] = static_cast<uint16_t>(sym);
                lengths[i] = static_cast<uint8_t>(l);
            }
        }
        return true;
    }

    bool decode(BitReader& br, int& sym, std::string& error) const {
        if (maxBits == 0) {
            error = "empty Huffman table";
            return false;
        }
        uint32_t index = 0;
        if (!br.peekPartial(maxBits, index)) {
            error = "unexpected end of deflate stream";
            return false;
        }
        uint8_t l = lengths[index];
        if (l == 0) {
            error = "invalid Huffman code";
            return false;
        }
        if (l > br.bits) {
            error = "truncated Huffman code";
            return false;
        }
        br.buf >>= l;
        br.bits -= l;
        sym = symbols[index];
        return true;
    }
};

const int LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const int LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const int DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
const int DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

bool buildFixedTables(Huffman& lit, Huffman& dist, std::string& error) {
    std::vector<int> litLens(288, 0);
    for (int i = 0; i <= 143; ++i) litLens[i] = 8;
    for (int i = 144; i <= 255; ++i) litLens[i] = 9;
    for (int i = 256; i <= 279; ++i) litLens[i] = 7;
    for (int i = 280; i <= 287; ++i) litLens[i] = 8;
    std::vector<int> distLens(32, 5);
    if (!lit.build(litLens, error)) return false;
    return dist.build(distLens, error);
}

bool decodeDynamicTables(BitReader& br, Huffman& lit, Huffman& dist,
                         int version, std::string& error) {
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!br.read(5, hlit) || !br.read(5, hdist) || !br.read(4, hclen)) {
        error = "truncated dynamic Huffman header";
        return false;
    }
    int nlit = static_cast<int>(hlit) + 257;
    int ndist = static_cast<int>(hdist) + 1;
    int nclen = static_cast<int>(hclen) + 4;
    (void)version;

    static const int ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    std::vector<int> clens(19, 0);
    for (int i = 0; i < nclen; ++i) {
        uint32_t v = 0;
        if (!br.read(3, v)) {
            error = "truncated code-length table";
            return false;
        }
        clens[ORDER[i]] = static_cast<int>(v);
    }

    Huffman clHuff;
    if (!clHuff.build(clens, error)) return false;

    std::vector<int> lens(nlit + ndist, 0);
    int index = 0;
    while (index < static_cast<int>(lens.size())) {
        int sym = 0;
        if (!clHuff.decode(br, sym, error)) return false;
        if (sym >= 0 && sym <= 15) {
            lens[index++] = sym;
        } else if (sym == 16) {
            if (index == 0) {
                error = "repeat code without previous length";
                return false;
            }
            uint32_t extra = 0;
            if (!br.read(2, extra)) {
                error = "truncated repeat code";
                return false;
            }
            int repeat = 3 + static_cast<int>(extra);
            int prev = lens[index - 1];
            if (index + repeat > static_cast<int>(lens.size())) {
                error = "code-length repeat overflow";
                return false;
            }
            for (int i = 0; i < repeat; ++i) lens[index++] = prev;
        } else if (sym == 17) {
            uint32_t extra = 0;
            if (!br.read(3, extra)) {
                error = "truncated zero repeat";
                return false;
            }
            int repeat = 3 + static_cast<int>(extra);
            if (index + repeat > static_cast<int>(lens.size())) {
                error = "zero repeat overflow";
                return false;
            }
            for (int i = 0; i < repeat; ++i) lens[index++] = 0;
        } else if (sym == 18) {
            uint32_t extra = 0;
            if (!br.read(7, extra)) {
                error = "truncated long zero repeat";
                return false;
            }
            int repeat = 11 + static_cast<int>(extra);
            if (index + repeat > static_cast<int>(lens.size())) {
                error = "long zero repeat overflow";
                return false;
            }
            for (int i = 0; i < repeat; ++i) lens[index++] = 0;
        } else {
            error = "invalid code-length symbol";
            return false;
        }
    }

    std::vector<int> litLens(lens.begin(), lens.begin() + nlit);
    std::vector<int> distLens(lens.begin() + nlit, lens.end());
    if (!lit.build(litLens, error)) return false;
    return dist.build(distLens, error);
}

bool inflateBlocks(const uint8_t* data, size_t size,
                   std::vector<uint8_t>& out, std::string& error) {
    BitReader br{data, size, 0, 0, 0};

    bool finalBlock = false;
    while (!finalBlock) {
        uint32_t header = 0;
        if (!br.read(3, header)) {
            error = "truncated block header";
            return false;
        }
        finalBlock = (header & 1) != 0;
        int type = static_cast<int>((header >> 1) & 3);

        if (type == 0) {
            br.alignByte();
            uint32_t len = 0, nlen = 0;
            if (!br.read(16, len) || !br.read(16, nlen)) {
                error = "truncated stored block";
                return false;
            }
            if ((len ^ 0xFFFFu) != nlen) {
                error = "stored block length check failed";
                return false;
            }
            if (br.pos + len > br.size) {
                error = "stored block exceeds input";
                return false;
            }
            for (uint32_t i = 0; i < len; ++i) {
                out.push_back(br.data[br.pos++]);
            }
            continue;
        }

        Huffman lit, dist;
        if (type == 1) {
            if (!buildFixedTables(lit, dist, error)) return false;
        } else if (type == 2) {
            if (!decodeDynamicTables(br, lit, dist, 0, error)) return false;
        } else {
            error = "invalid deflate block type";
            return false;
        }

        while (true) {
            int sym = 0;
            if (!lit.decode(br, sym, error)) return false;
            if (sym < 256) {
                out.push_back(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                break;
            } else if (sym >= 257 && sym <= 285) {
                int li = sym - 257;
                uint32_t extra = 0;
                if (LEN_EXTRA[li] > 0 && !br.read(LEN_EXTRA[li], extra)) {
                    error = "truncated length code";
                    return false;
                }
                int length = LEN_BASE[li] + static_cast<int>(extra);

                int dsym = 0;
                if (!dist.decode(br, dsym, error)) return false;
                if (dsym < 0 || dsym >= 30) {
                    error = "invalid distance symbol";
                    return false;
                }
                uint32_t dextra = 0;
                if (DIST_EXTRA[dsym] > 0 && !br.read(DIST_EXTRA[dsym], dextra)) {
                    error = "truncated distance code";
                    return false;
                }
                int distance = DIST_BASE[dsym] + static_cast<int>(dextra);
                if (distance <= 0 || distance > static_cast<int>(out.size())) {
                    error = "invalid back-reference distance";
                    return false;
                }
                for (int i = 0; i < length; ++i) {
                    out.push_back(out[out.size() - distance]);
                }
            } else {
                error = "invalid literal/length symbol";
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool gzipDecompress(const uint8_t* data, size_t size,
                    std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    error.clear();

    if (size < 10) {
        error = "gzip input too short";
        return false;
    }
    if (!(data[0] == 0x1F && data[1] == 0x8B)) {
        error = "not a gzip stream";
        return false;
    }
    if (data[2] != 0x08) {
        error = "unsupported gzip compression method";
        return false;
    }
    uint8_t flags = data[3];
    size_t pos = 10; // magic(2) + method(1) + flags(1) + mtime(4) + xfl(1) + os(1)

    if (flags & 0x04) { // FEXTRA
        if (pos + 2 > size) {
            error = "truncated gzip extra field";
            return false;
        }
        uint16_t xlen = static_cast<uint16_t>(data[pos]) |
                        static_cast<uint16_t>(data[pos + 1] << 8);
        pos += 2;
        if (pos + xlen > size) {
            error = "truncated gzip extra data";
            return false;
        }
        pos += xlen;
    }
    if (flags & 0x08) { // FNAME
        while (pos < size && data[pos] != 0) pos++;
        if (pos >= size) {
            error = "truncated gzip filename";
            return false;
        }
        pos++;
    }
    if (flags & 0x10) { // FCOMMENT
        while (pos < size && data[pos] != 0) pos++;
        if (pos >= size) {
            error = "truncated gzip comment";
            return false;
        }
        pos++;
    }
    if (flags & 0x02) { // FHCRC
        if (pos + 2 > size) {
            error = "truncated gzip header CRC";
            return false;
        }
        pos += 2;
    }

    if (pos >= size) {
        error = "gzip stream has no deflate data";
        return false;
    }

    return inflateBlocks(data + pos, size - pos, out, error);
}
