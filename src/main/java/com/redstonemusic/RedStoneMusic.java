package com.redstonemusic;

import com.mojang.logging.LogUtils;
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
                .then(Commands.literal("Convert")
                    .then(Commands.argument("file", StringArgumentType.greedyString())
                        .executes(context -> convert(context, StringArgumentType.getString(context, "file"), true))))
                .then(Commands.literal("place")
                    .then(Commands.argument("file", StringArgumentType.word())
                        .executes(context -> placeExternal(context, StringArgumentType.getString(context, "file")))))
        );
        event.getDispatcher().register(
            Commands.literal("redstonemusic").requires(source -> source.hasPermission(2))
                .then(Commands.literal("list")
                    .then(Commands.literal("mp3").executes(context -> listFiles(context, "mp3")))
                    .then(Commands.literal("ntb").executes(context -> listFiles(context, "ntb"))))
                .then(Commands.literal("convert")
                    .then(Commands.argument("file", StringArgumentType.greedyString())
                        .executes(context -> convert(context, StringArgumentType.getString(context, "file"), true))))
                .then(Commands.literal("place")
                    .then(Commands.argument("file", StringArgumentType.word())
                        .executes(context -> placeExternal(context, StringArgumentType.getString(context, "file")))))
        );
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

    private static int convert(CommandContext<CommandSourceStack> context, String raw, boolean placeAfter) {
        if (!ready(context)) return 0;
        String parsedFile = raw.trim();
        boolean shouldPlace = placeAfter;
        if (parsedFile.toLowerCase(Locale.ROOT).endsWith(" to ntb")) {
            parsedFile = parsedFile.substring(0, parsedFile.length() - 6).trim();
            shouldPlace = false;
        }
        final String file = parsedFile;
        final boolean finalShouldPlace = shouldPlace;
        try {
            Path input = storage.resolveMp3(file);
            if (!Files.isRegularFile(input)) throw new IOException("mp3 文件不存在: " + file);
            String outputName = stripExtension(input.getFileName().toString()) + ".ntb";
            Path output = storage.ntbDirectory().resolve(outputName);
            LOGGER.info("CONVERT 开始: {} -> {}", input, output);
            context.getSource().sendSuccess(() -> Component.literal("开始转换: " + file), false);
            Path finalInput = input;
            toolRunner.convertAsync(finalInput, output).whenComplete((result, error) -> {
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
                    if (finalShouldPlace) {
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
