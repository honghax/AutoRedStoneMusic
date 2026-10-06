package com.besson.tutorial;
import com.besson.tutorial.block.ModBlocks;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderGetter;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.Registries;


import net.minecraft.tags.BlockTags;
import net.minecraft.world.level.levelgen.VerticalAnchor;
import net.minecraft.world.level.levelgen.feature.ConfiguredFeature;
import net.minecraft.world.level.levelgen.feature.Feature;
import net.minecraft.world.level.levelgen.feature.configurations.OreConfiguration;

import net.minecraft.data.worldgen.BootstapContext;
import net.minecraft.resources.ResourceKey;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.world.level.levelgen.placement.*;
import net.minecraft.world.level.levelgen.structure.templatesystem.TagMatchTest;
import net.minecraft.world.level.levelgen.structure.templatesystem.RuleTest;
import java.util.List;

import static io.netty.util.internal.ThreadExecutorMap.apply;

public class ModOre {
    public static final String MOD_ID =    TutorialMod.MOD_ID;

    // 定义特征的资源键
    public static final ResourceKey<ConfiguredFeature<?, ?>> RUBY_ORE_KEY = ResourceKey.create(
            Registries.CONFIGURED_FEATURE, new ResourceLocation(MOD_ID, "ruby_ore"));
    public static final ResourceKey<PlacedFeature> RUBY_ORE_PLACED_KEY = ResourceKey.create(
            Registries.PLACED_FEATURE, new ResourceLocation(MOD_ID, "ruby_ore_placed"));

    // 注册配置化特征
    public static void bootstrapConfiguredFeatures(BootstapContext<ConfiguredFeature<?, ?>> context) {
        RuleTest stoneReplaceable = new TagMatchTest(BlockTags.STONE_ORE_REPLACEABLES);
        ConfiguredFeature<?, ?> configuredFeature = new ConfiguredFeature<>(
                Feature.ORE,
                new OreConfiguration(
                        List.of(OreConfiguration.target(stoneReplaceable, ModBlocks.RAW_LS.get().defaultBlockState())),
                        64, // 单矿脉最大矿石数量
                        0.0F // 暴露在空气中的矿脉废弃概率
                )
        );

        // 注册配置化特征
        context.register(RUBY_ORE_KEY, configuredFeature);
    }

    // 注册放置特征
    public static void bootstrapPlacedFeatures(BootstapContext<PlacedFeature> context) {
        // 获取之前注册的配置化特征
        HolderGetter<ConfiguredFeature<?, ?>> configuredFeatures = context.lookup(Registries.CONFIGURED_FEATURE);
        Holder<ConfiguredFeature<?, ?>> holder = configuredFeatures.getOrThrow(RUBY_ORE_KEY);

        // 创建放置特征
        List<PlacementModifier> placementModifiers = List.of(
                CountPlacement.of(100), // 每区块尝试生成的矿脉数量
                InSquarePlacement.spread(), // 矿脉在区块X/Z轴均匀分布
                HeightRangePlacement.uniform(VerticalAnchor.absolute(50), VerticalAnchor.absolute(300)), // 高度区间
                BiomeFilter.biome() // 遵循生物群系过滤规则
        );

        // 注册放置特征
        context.register(RUBY_ORE_PLACED_KEY, new PlacedFeature(holder, placementModifiers));
    }


}