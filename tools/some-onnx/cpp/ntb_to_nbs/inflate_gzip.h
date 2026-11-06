#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Decompress a single-member gzip stream (as written by nbtlib) into raw bytes.
bool gzipDecompress(const uint8_t* data, size_t size,
                    std::vector<uint8_t>& out, std::string& error);
