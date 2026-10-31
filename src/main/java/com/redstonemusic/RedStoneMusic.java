package com.redstonemusic;

import com.mojang.logging.LogUtils;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.context.CommandContext;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Vec3i;
import net.minecraft.core.registries.Registries;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructurePlaceSettings;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplate;
import net.minecraft.world.phys.Vec3;
import net.minecraftforge.common.MinecraftForge;
import net.minecraftforge.event.RegisterCommandsEvent;
import net.minecraftforge.event.server.ServerStartingEvent;
import net.minecraftforge.event.server.ServerStoppingEvent;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;
import net.minecraftforge.fml.event.lifecycle.FMLCommonSetupEvent;
import net.minecraftforge.fml.javafmlmod.FMLJavaModLoadingContext;
import org.slf4j.Logger;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

@Mod(RedStoneMusic.MODID)
public class RedStoneMusic {
    public static final String MODID = "redstone_music";
    public static final Logger LOGGER = LogUtils.getLogger();
    private static MusicStorage storage;
    private static MusicToolRunner toolRunner;

    public RedStoneMusic(FMLJavaModLoadingContext context) {
        IEventBus modEventBus = context.getModEventBus();
        modEventBus.addListener(this::commonSetup);
        MinecraftForge.EVENT_BUS.register(this);
    }

    private void commonSetup(final FMLCommonSetupEvent event) {
        try {
            storage = new MusicStorage(Path.of("config"));
            toolRunner = new MusicToolRunner();
            LOGGER.info("RedStoneMusic 文件目录: {}", storage.root());
        } catch (IOException e) {
            LOGGER.error("RedStoneMusic 初始化失败", e);
        }
    }

    @SubscribeEvent
    public void onRegisterCommands(RegisterCommandsEvent event) {
        event.getDispatcher().register(
            Commands.literal("RedStoneMusic").requires(source -> source.hasPermission(2))
                .then(Commands.literal("list")
                    .then(Commands.literal("mp3").executes(context -> listFiles(context, "mp3")))
                    .then(Commands.literal("ntb").executes(context -> listFiles(context, "ntb"))))
                .then(convertCommand("Convert"))
                .then(Commands.literal("place")
                    .then(Commands.argument("file", StringArgumentType.word())
                        .executes(context -> placeExternal(context, StringArgumentType.getString(context, "file")))))
                .then(Commands.literal("debug")
                    .then(Commands.literal("place")
                        .then(Commands.argument("structure", StringArgumentType.word())
                            .executes(context -> placeBuiltin(context, StringArgumentType.getString(context, "structure"))))))
        );
        event.getDispatcher().register(
            Commands.literal("redstonemusic").requires(source -> source.hasPermission(2))
                .then(Commands.literal("list")
                    .then(Commands.literal("mp3").executes(context -> listFiles(context, "mp3")))
                    .then(Commands.literal("ntb").executes(context -> listFiles(context, "ntb"))))
                .then(convertCommand("convert"))
                .then(Commands.literal("place")
                    .then(Commands.argument("file", StringArgumentType.word())
                        .executes(context -> placeExternal(context, StringArgumentType.getString(context, "file")))))
                .then(Commands.literal("debug")
                    .then(Commands.literal("place")
                        .then(Commands.argument("structure", StringArgumentType.word())
                            .executes(context -> placeBuiltin(context, StringArgumentType.getString(context, "structure"))))))
        );
    }

    // Convert <file> [ai|trad] [true|false] [to ntb] —— 引擎与多轨开关是命令语法节点，不是字符串参数
    private static com.mojang.brigadier.builder.LiteralArgumentBuilder<CommandSourceStack> convertCommand(String literal) {
        return Commands.literal(literal)
            .then(Commands.argument("file", StringArgumentType.word())
                .executes(context -> convert(context, getFile(context), "ai", false, true))
                .then(lanesNode("ai", false, true))
                .then(Commands.literal("to")
                    .then(Commands.literal("ntb").executes(context -> convert(context, getFile(context), "ai", false, false))))
                .then(Commands.literal("true")
                    .executes(context -> convert(context, getFile(context), "ai", true, true))
                    .then(lanesNode("ai", true, true)))
                .then(Commands.literal("false")
                    .executes(context -> convert(context, getFile(context), "ai", false, true))
                    .then(lanesNode("ai", false, true)))
                .then(Commands.literal("ai")
                    .executes(context -> convert(context, getFile(context), "ai", false, true))
                    .then(Commands.literal("true")
                        .executes(context -> convert(context, getFile(context), "ai", true, true))
                        .then(lanesNode("ai", true, true)))
                    .then(Commands.literal("false")
                        .executes(context -> convert(context, getFile(context), "ai", false, true))
                        .then(lanesNode("ai", false, true))))
                .then(Commands.literal("trad")
                    .executes(context -> convert(context, getFile(context), "trad", false, true))
                    .then(Commands.literal("true")
                        .executes(context -> convert(context, getFile(context), "trad", true, true))
                        .then(lanesNode("trad", true, true)))
                    .then(Commands.literal("false")
                        .executes(context -> convert(context, getFile(context), "trad", false, true))
                        .then(lanesNode("trad", false, true)))));
    }

    // lanes <N> [split pitch|bank] [poly 1..128]：最终 MIDI → N 轨直排结构
    private static final int DEFAULT_POLY = 20;

    private static com.mojang.brigadier.builder.LiteralArgumentBuilder<CommandSourceStack> lanesNode(
            String engine, boolean separate, boolean place) {
        return Commands.literal("lanes")
            .then(Commands.argument("lanes", IntegerArgumentType.integer(1, 3))
                .executes(context -> convert(context, getFile(context), engine, separate, place,
                    lanes(context), "pitch", DEFAULT_POLY))
                .then(splitNode(engine, separate, place))
                .then(polyNode(engine, separate, place)));
    }

    private static com.mojang.brigadier.builder.LiteralArgumentBuilder<CommandSourceStack> splitNode(
            String engine, boolean separate, boolean place) {
        return Commands.literal("split")
            .then(Commands.literal("pitch")
                .executes(context -> convert(context, getFile(context), engine, separate, place,
                    lanes(context), "pitch", DEFAULT_POLY))
                .then(polyUnderSplit(engine, separate, place, "pitch")))
            .then(Commands.literal("bank")
                .executes(context -> convert(context, getFile(context), engine, separate, place,
                    lanes(context), "bank", DEFAULT_POLY))
                .then(polyUnderSplit(engine, separate, place, "bank")));
    }

    private static com.mojang.brigadier.builder.LiteralArgumentBuilder<CommandSourceStack> polyUnderSplit(
            String engine, boolean separate, boolean place, String split) {
        return Commands.literal("poly")
            .then(Commands.argument("poly", IntegerArgumentType.integer(1, 128))
                .executes(context -> convert(context, getFile(context), engine, separate, place,
                    lanes(context), split, poly(context))));
    }

    private static com.mojang.brigadier.builder.LiteralArgumentBuilder<CommandSourceStack> polyNode(
            String engine, boolean separate, boolean place) {
        var pitch = Commands.literal("pitch")
            .executes(context -> convert(context, getFile(context), engine, separate, place,
                lanes(context), "pitch", poly(context)));
        var bank = Commands.literal("bank")
            .executes(context -> convert(context, getFile(context), engine, separate, place,
                lanes(context), "bank", poly(context)));
        var split = Commands.literal("split").then(pitch).then(bank);
        var polyArg = Commands.argument("poly", IntegerArgumentType.integer(1, 128))
            .executes(context -> convert(context, getFile(context), engine, separate, place,
                lanes(context), "pitch", poly(context)))
            .then(split);
        return Commands.literal("poly").then(polyArg);
    }

    private static int lanes(CommandContext<CommandSourceStack> context) {
        return IntegerArgumentType.getInteger(context, "lanes");
    }

    private static int poly(CommandContext<CommandSourceStack> context) {
        return IntegerArgumentType.getInteger(context, "poly");
    }

    private static String getFile(CommandContext<CommandSourceStack> context) {
        return StringArgumentType.getString(context, "file");
    }

    private static int listFiles(CommandContext<CommandSourceStack> context, String type) {
        if (!ready(context)) return 0;
        try {
            var entries = storage.list(type);
            context.getSource().sendSuccess(() -> Component.literal(
                type + " 文件列表（" + entries.size() + " 个，目录: " + storage.root() + "/" + type + "）"), false);
            for (MusicStorage.FileEntry entry : entries) {
                context.getSource().sendSuccess(() -> Component.literal(
                    entry.name() + " | " + entry.size() + " bytes | " + entry.modifiedText()), false);
            }
            LOGGER.info("LIST {}: {} 个文件", type, entries.size());
            return 1;
        } catch (Exception e) {
            return fail(context, "查询文件失败: " + e.getMessage(), e);
        }
    }

    private static int convert(CommandContext<CommandSourceStack> context, String file,
                               String engine, boolean separate, boolean shouldPlace) {
        return convert(context, file, engine, separate, shouldPlace, 0, "pitch", DEFAULT_POLY);
    }

    private static int convert(CommandContext<CommandSourceStack> context, String file,
                               String engine, boolean separate, boolean shouldPlace, int lanes) {
        return convert(context, file, engine, separate, shouldPlace, lanes, "pitch", DEFAULT_POLY);
    }

    private static int convert(CommandContext<CommandSourceStack> context, String file,
                               String engine, boolean separate, boolean shouldPlace,
                               int lanes, String split) {
        return convert(context, file, engine, separate, shouldPlace, lanes, split, DEFAULT_POLY);
    }

    private static int convert(CommandContext<CommandSourceStack> context, String file,
                               String engine, boolean separate, boolean shouldPlace,
                               int lanes, String split, int polyCap) {
        if (!ready(context)) return 0;
        try {
            Path input = storage.resolveMp3(file);
            if (!Files.isRegularFile(input)) throw new IOException("mp3 文件不存在: " + file);
            String base = stripExtension(input.getFileName().toString());
            String laneSuffix = lanes > 0 ? "_L" + lanes : "";
            String splitSuffix = (lanes > 0 && "bank".equalsIgnoreCase(split)) ? "_bank" : "";
            String polySuffix = (lanes > 0 && polyCap != DEFAULT_POLY) ? "_P" + polyCap : "";
            if (separate) {
                // 多音轨：文件名带 <engine>_true 签名；多轨结构再加 _L<N>[_bank][_P<cap>]
                Path output = storage.ntbDirectory().resolve(
                    base + "_" + engine + "_true" + laneSuffix + splitSuffix + polySuffix + ".ntb");
                return multiConvert(context, input, output, engine, shouldPlace, lanes, split, polyCap);
            }
            Path output = storage.ntbDirectory().resolve(
                base + "_" + engine + "_false" + laneSuffix + splitSuffix + polySuffix + ".ntb");
            LOGGER.info("CONVERT 开始: {} -> {} (lanes={}, split={}, poly={})",
                input, output, lanes, split, polyCap);
            context.getSource().sendSuccess(() -> Component.literal(
                (lanes > 0 ? ("开始 " + lanes + " 轨(" + split + ", poly " + polyCap + ")结构转换: ")
                    : "开始转换: ") + file), false);
            Path finalInput = input;
            toolRunner.convertAsync(finalInput, output, lanes, split, polyCap).whenComplete((result, error) -> {
                ServerPlayer player;
                try {
                    player = context.getSource().getPlayerOrException();
                } catch (Exception e) {
                    LOGGER.error("CONVERT 完成后找不到玩家", e);
                    return;
                }
                player.server.execute(() -> {
                    if (error != null) {
                        LOGGER.error("CONVERT 失败: {}", finalInput, error);
                        context.getSource().sendFailure(Component.literal("转换失败: " + rootMessage(error)));
                        return;
                    }
                    LOGGER.info("CONVERT 完成: {}", result);
                    context.getSource().sendSuccess(() -> Component.literal("转换完成: " + result.getFileName()), false);
                    if (shouldPlace) {
                        context.getSource().sendSuccess(() -> Component.literal("正在放置结构..."), false);
                        placeFile(context, result);
                    }
                });
            });
            return 1;
        } catch (Exception e) {
            return fail(context, "转换失败: " + e.getMessage(), e);
        }
    }

    private static int multiConvert(CommandContext<CommandSourceStack> context, Path input,
                                    Path output, String engine, boolean placeAfter) {
        return multiConvert(context, input, output, engine, placeAfter, 0, "pitch", DEFAULT_POLY);
    }

    private static int multiConvert(CommandContext<CommandSourceStack> context, Path input,
                                    Path output, String engine, boolean placeAfter, int lanes) {
        return multiConvert(context, input, output, engine, placeAfter, lanes, "pitch", DEFAULT_POLY);
    }

    private static int multiConvert(CommandContext<CommandSourceStack> context, Path input,
                                    Path output, String engine, boolean placeAfter, int lanes, String split) {
        return multiConvert(context, input, output, engine, placeAfter, lanes, split, DEFAULT_POLY);
    }

    private static int multiConvert(CommandContext<CommandSourceStack> context, Path input,
                                    Path output, String engine, boolean placeAfter,
                                    int lanes, String split, int polyCap) {
        try {
            String engineLabel = "trad".equalsIgnoreCase(engine) ? "传统算法" : "AI(demucs)";
            String laneLabel = lanes > 0
                ? (" + " + lanes + " 轨(" + split + ", poly " + polyCap + ")结构") : "";
            context.getSource().sendSuccess(() -> Component.literal(
                "开始" + engineLabel + "多音轨转换" + laneLabel + ": " + input.getFileName()), false);
            toolRunner.convertMultiAsync(input, output, engine, 0.015, lanes, split, polyCap)
                .whenComplete((result, error) -> {
                try {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    player.server.execute(() -> {
                        if (error != null) {
                            LOGGER.error("多音轨转换失败: {}", input, error);
                            context.getSource().sendFailure(Component.literal("多音轨转换失败: " + rootMessage(error)));
                            return;
                        }
                        try {
                            context.getSource().sendSuccess(() -> Component.literal("多音轨转换完成: " + result.getFileName()), false);
                            if (placeAfter) {
                                placeFile(context, result);
                            }
                        } catch (Exception e) {
                            fail(context, "多音轨放置失败: " + e.getMessage(), e);
                        }
                    });
                } catch (Exception e) {
                    LOGGER.error("多音轨完成后找不到玩家", e);
                }
            });
            return 1;
        } catch (Exception e) {
            return fail(context, "多音轨转换失败: " + e.getMessage(), e);
        }
    }

    private static int placeExternal(CommandContext<CommandSourceStack> context, String file) {
        if (!ready(context)) return 0;
        try {
            Path path = storage.resolveNtb(file);
            if (!Files.isRegularFile(path)) throw new IOException("ntb 文件不存在: " + file);
            return placeFile(context, path);
        } catch (Exception e) {
            return fail(context, "放置失败: " + e.getMessage(), e);
        }
    }

    // 内部调试：放置模组自带的结构资源 data/redstone_music/structures/<name>.nbt
    private static int placeBuiltin(CommandContext<CommandSourceStack> context, String name) {
        try {
            String id = name.toLowerCase(Locale.ROOT);
            if (!id.matches("[a-z0-9_.-]+")) throw new IOException("非法结构名: " + name);
            ServerLevel level = context.getSource().getLevel();
            ResourceLocation loc = new ResourceLocation(MODID, id);
            Optional<StructureTemplate> template = level.getStructureManager().get(loc);
            if (template.isEmpty()) throw new IOException("结构资源不存在: " + loc);
            ServerPlayer player = context.getSource().getPlayerOrException();
            Vec3 look = player.getLookAngle();
            BlockPos origin = BlockPos.containing(player.getX() + look.x * 4.0D, player.getY(), player.getZ() + look.z * 4.0D);
            Vec3i size = template.get().getSize();
            if (size.getX() < 1 || size.getY() < 1 || size.getZ() < 1) throw new IOException("结构尺寸无效");
            boolean ok = template.get().placeInWorld(level, origin, origin, new StructurePlaceSettings(), level.random, Block.UPDATE_ALL);
            if (!ok) throw new IOException("StructureTemplate 放置失败");
            LOGGER.info("DEBUG PLACE: {} origin={} size={}x{}x{}", loc, origin, size.getX(), size.getY(), size.getZ());
            context.getSource().sendSuccess(() -> Component.literal("已放置内置结构 " + loc + "，尺寸 " + size.getX() + "x" + size.getY() + "x" + size.getZ()), true);
            return 1;
        } catch (Exception e) {
            return fail(context, "放置失败: " + e.getMessage(), e);
        }
    }

    private static int placeFile(CommandContext<CommandSourceStack> context, Path path) {
        try {
            ServerPlayer player = context.getSource().getPlayerOrException();
            ServerLevel level = context.getSource().getLevel();
            CompoundTag tag = NbtIo.readCompressed(path.toFile());
            StructureTemplate template = new StructureTemplate();
            template.load(level.registryAccess().lookupOrThrow(Registries.BLOCK), tag);
            Vec3 look = player.getLookAngle();
            BlockPos origin = BlockPos.containing(player.getX() + look.x * 4.0D, player.getY(), player.getZ() + look.z * 4.0D);
            Vec3i size = template.getSize();
            if (size.getX() < 1 || size.getY() < 1 || size.getZ() < 1) throw new IOException("NBT 结构尺寸无效");
            boolean ok = template.placeInWorld(level, origin, origin, new StructurePlaceSettings(), level.random, Block.UPDATE_ALL);
            if (!ok) throw new IOException("StructureTemplate 放置失败");
            LOGGER.info("PLACE 完成: {} origin={} size={}x{}x{}", path, origin, size.getX(), size.getY(), size.getZ());
            context.getSource().sendSuccess(() -> Component.literal("已放置 " + path.getFileName() + "，尺寸 " + size.getX() + "x" + size.getY() + "x" + size.getZ()), true);
            return 1;
        } catch (Exception e) {
            return fail(context, "放置失败: " + e.getMessage(), e);
        }
    }

    private static boolean ready(CommandContext<CommandSourceStack> context) {
        if (storage != null && toolRunner != null) return true;
        context.getSource().sendFailure(Component.literal("RedStoneMusic 尚未完成初始化，请查看日志"));
        return false;
    }

    private static int fail(CommandContext<CommandSourceStack> context, String message, Throwable error) {
        LOGGER.error(message, error);
        context.getSource().sendFailure(Component.literal(message));
        return 0;
    }

    private static String rootMessage(Throwable error) {
        Throwable current = error;
        while (current.getCause() != null) current = current.getCause();
        return current.getMessage() == null ? current.toString() : current.getMessage();
    }

    private static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    @SubscribeEvent
    public void onServerStarting(ServerStartingEvent event) {
        LOGGER.info("RedStoneMusic server starting");
    }

    @SubscribeEvent
    public void onServerStopping(ServerStoppingEvent event) {
        LOGGER.info("RedStoneMusic server stopping; retaining tools for subsequent worlds");
    }
}
