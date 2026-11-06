#pragma once

#include "model.h"

#include <string>

// Writes a gzipped Minecraft structure NBT. Returns false on I/O failure.
bool writeStructureNbt(const std::string& path, const BlockMap& blocks,
                       int& size_x, int& size_y, int& size_z, std::string& error);
