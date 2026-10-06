package com.besson.tutorial;

import net.minecraft.core.HolderLookup;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistrySetBuilder;
import net.minecraft.core.registries.Registries;
import net.minecraft.data.PackOutput;
import net.minecraft.resources.ResourceLocation;
import net.minecraftforge.common.data.DatapackBuiltinEntriesProvider;
import net.minecraftforge.registries.ForgeRegistries;

import java.util.Set;
import java.util.concurrent.CompletableFuture;

import static com.besson.tutorial.TutorialMod.MOD_ID;

public class ModWorldGenProvider extends DatapackBuiltinEntriesProvider {
    // 注册表构建器，连接矿石生成配置

    public static final RegistrySetBuilder BUILDER = new RegistrySetBuilder()

            .add(Registries.CONFIGURED_FEATURE, ModOre::bootstrapConfiguredFeatures)
            .add(Registries.PLACED_FEATURE, ModOre::bootstrapPlacedFeatures);


    public ModWorldGenProvider(PackOutput output,
                               CompletableFuture<HolderLookup.Provider> registries
                             ) {
        super(output, registries, BUILDER, Set.of(MOD_ID));
    }

}