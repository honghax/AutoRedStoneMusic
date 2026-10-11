package com.redstonemusic;

import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.nbt.Tag;

import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

final class MultiTrackNbtMerger {
    private MultiTrackNbtMerger() {
    }

    static void merge(List<Path> inputs, Path output, int gap) throws IOException {
        if (inputs.isEmpty()) {
            throw new IOException("没有可合并的音轨结构");
        }

        List<CompoundTag> sourceTags = new ArrayList<>();
        int totalY = 0;
        int maxX = 0;
        int maxZ = 0;
        for (Path input : inputs) {
            CompoundTag tag = NbtIo.readCompressed(input.toFile());
            int[] size = readSize(tag);
            if (size[0] < 1 || size[1] < 1 || size[2] < 1) {
                throw new IOException("结构尺寸无效: " + input);
            }
            sourceTags.add(tag);
            maxX = Math.max(maxX, size[0]);
            maxZ = Math.max(maxZ, size[2]);
            totalY += size[1];
        }
        totalY += Math.max(0, inputs.size() - 1) * gap;

        ListTag palette = new ListTag();
        Map<String, Integer> paletteIndex = new HashMap<>();
        ListTag blocks = new ListTag();
        int yOffset = 0;
        for (CompoundTag source : sourceTags) {
            int[] size = readSize(source);
            ListTag sourcePalette = source.getList("palette", Tag.TAG_COMPOUND);
            ListTag sourceBlocks = source.getList("blocks", Tag.TAG_COMPOUND);
            for (Tag raw : sourceBlocks) {
                CompoundTag block = ((CompoundTag) raw).copy();
                int state = block.getInt("state");
                CompoundTag paletteEntry = sourcePalette.getCompound(state).copy();
                String key = paletteEntry.toString();
                Integer mergedState = paletteIndex.get(key);
                if (mergedState == null) {
                    mergedState = palette.size();
                    paletteIndex.put(key, mergedState);
                    palette.add(paletteEntry);
                }
                ListTag pos = block.getList("pos", Tag.TAG_INT);
                block.putInt("state", mergedState);
                block.put("pos", intList(pos.getInt(0), pos.getInt(1) + yOffset, pos.getInt(2)));
                blocks.add(block);
            }
            yOffset += size[1] + gap;
        }

        CompoundTag merged = new CompoundTag();
        merged.putInt("DataVersion", 3465);
        merged.put("size", intList(maxX, totalY, maxZ));
        merged.put("palette", palette);
        merged.put("blocks", blocks);
        merged.put("entities", new ListTag());
        NbtIo.writeCompressed(merged, output.toFile());
    }

    private static ListTag intList(int x, int y, int z) {
        ListTag list = new ListTag();
        list.add(net.minecraft.nbt.IntTag.valueOf(x));
        list.add(net.minecraft.nbt.IntTag.valueOf(y));
        list.add(net.minecraft.nbt.IntTag.valueOf(z));
        return list;
    }

    private static int[] readSize(CompoundTag tag) throws IOException {
        if (tag.contains("size", Tag.TAG_INT_ARRAY)) {
            return tag.getIntArray("size");
        }
        ListTag list = tag.getList("size", Tag.TAG_INT);
        if (list.size() != 3) {
            throw new IOException("NBT 缺少有效 size");
        }
        return new int[]{list.getInt(0), list.getInt(1), list.getInt(2)};
    }
}
