#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct NbtTag {
    int type = 0;
    int8_t i8 = 0;
    int16_t i16 = 0;
    int32_t i32 = 0;
    int64_t i64 = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    std::string str;
    std::vector<int8_t> byteArray;
    std::vector<int32_t> intArray;
    std::vector<int64_t> longArray;
    std::vector<NbtTag> list;
    std::vector<std::pair<std::string, NbtTag>> compound;
};

const NbtTag* nbtFind(const NbtTag& tag, const std::string& key);
bool parseNbt(const std::vector<uint8_t>& data, NbtTag& root, std::string& error);
