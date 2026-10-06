package com.besson.tutorial;

import com.besson.tutorial.block.ModBlocks;
import com.besson.tutorial.block.entity.ModBlockEntities;
import com.besson.tutorial.command.TDollCommand;
import com.besson.tutorial.ModEntities;
import com.besson.tutorial.Entity.TDollEntity;
import com.besson.tutorial.dimension.MindUpgradeDimension;
import com.besson.tutorial.dimension.ModDimensions;
import com.besson.tutorial.item.ModCreativeModeTabs;
import com.besson.tutorial.item.ModEffects;
import com.besson.tutorial.item.ModItems;
import com.besson.tutorial.item.custom.T_Doll_Core;
import com.besson.tutorial.item.custom.TDollCoreItem;
import com.besson.tutorial.item.custom.TrainingDataItem;
import com.besson.tutorial.menu.ModMenuTypes;
import com.besson.tutorial.network.NetworkHandler;
import com.besson.tutorial.event.GunDamageHandler;
import com.besson.tutorial.instance.MindUpgradeInstanceManager;
import com.besson.tutorial.tdoll.TDollCoreConfig;
import com.besson.tutorial.tdoll.TDollStateManager;
import com.besson.tutorial.sound.ModSounds;
import com.besson.tutorial.ysm.YSMIntegration;
import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.logging.LogUtils;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.commands.arguments.EntityAnchorArgument;
import net.minecraft.core.BlockPos;
import net.minecraft.core.HolderLookup;
import net.minecraft.data.PackOutput;
import net.minecraft.data.worldgen.BootstapContext;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.tags.EntityTypeTags;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.*;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.ai.behavior.EntityTracker;
import net.minecraft.world.entity.animal.Cod;
import net.minecraft.world.entity.animal.Pufferfish;
import net.minecraft.world.entity.animal.Salmon;
import net.minecraft.world.entity.animal.TropicalFish;
import net.minecraft.world.entity.boss.enderdragon.EnderDragon;
import net.minecraft.world.entity.boss.wither.WitherBoss;
import net.minecraft.world.entity.monster.*;
import net.minecraft.world.entity.monster.hoglin.Hoglin;
import net.minecraft.world.entity.monster.piglin.Piglin;
import net.minecraft.world.entity.monster.piglin.PiglinBrute;
import net.minecraft.world.entity.monster.warden.Warden;
import net.minecraft.world.entity.npc.Villager;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.GenerationStep;
import net.minecraft.world.level.levelgen.feature.ConfiguredFeature;
import net.minecraft.world.level.levelgen.placement.PlacedFeature;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraftforge.api.distmarker.Dist;
import net.minecraftforge.common.MinecraftForge;
import net.minecraftforge.data.event.GatherDataEvent;
import net.minecraftforge.event.BuildCreativeModeTabContentsEvent;
import net.minecraftforge.event.RegisterCommandsEvent;
import net.minecraftforge.event.TickEvent;
import net.minecraftforge.event.entity.living.LivingDeathEvent;
import net.minecraftforge.event.entity.living.LivingEvent;
import net.minecraftforge.event.entity.living.LivingHurtEvent;
import net.minecraftforge.event.entity.player.PlayerEvent;
import net.minecraftforge.event.entity.player.PlayerInteractEvent;
import net.minecraftforge.event.entity.living.LivingDropsEvent;
import net.minecraftforge.event.entity.EntityAttributeCreationEvent;
import net.minecraftforge.event.server.ServerStartingEvent;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import net.minecraftforge.fml.common.Mod;
import net.minecraftforge.fml.config.ModConfig;
import net.minecraftforge.fml.event.lifecycle.FMLClientSetupEvent;
import net.minecraftforge.fml.event.lifecycle.FMLCommonSetupEvent;
import net.minecraftforge.fml.javafmlmod.FMLJavaModLoadingContext;
import net.minecraftforge.registries.ForgeRegistries;
import net.minecraftforge.network.NetworkHooks;
import org.slf4j.Logger;

import java.util.*;
import java.util.concurrent.CompletableFuture;

import static com.besson.tutorial.CustomKey.MY_P_KEY;


// The value here should match an entry in the META-INF/mods.toml file
@Mod(TutorialMod.MOD_ID)
public class TutorialMod {
    // Define mod id in a common place for everything to reference
    public static final String MOD_ID = "girls_frontline";
    // Directly reference a slf4j logger
    private static final Logger LOGGER = LogUtils.getLogger();

    /**
     * 兼容 Forge 47.3.x：老版本加载器实例化 mod 类时使用无参构造查找 &lt;init&gt;()。
     * 委托给带 context 的构造，逻辑完全一致。
     */
    public TutorialMod() {
        this(FMLJavaModLoadingContext.get());
    }

    public TutorialMod(FMLJavaModLoadingContext context) {
        IEventBus modEventBus = context.getModEventBus();

        modEventBus.register(CustomKey.class);
        modEventBus.addListener(this::gatherData);
        // 仅当你不用JSON方式、选择代码注册生物群系生成规则时需要添加此行
        modEventBus.addListener(this::commonSetup);
        ModItems.register(modEventBus);
        ModEntities.register(modEventBus);
        ModCreativeModeTabs.register(modEventBus);
        ModBlocks.register(modEventBus);
        ModEffects.register(modEventBus);
        ModMenuTypes.register(modEventBus);
        ModBlockEntities.register(modEventBus);
        ModSounds.register(modEventBus);
        // 注册心智升级维度的 ChunkGenerator Codec
        ModDimensions.register(modEventBus);
        // Register the commonSetup method for modloading
        modEventBus.addListener(this::commonSetup);
        // Register ourselves for server and other game events we are interested in
        MinecraftForge.EVENT_BUS.register(this);
        MinecraftForge.EVENT_BUS.register(GunDamageHandler.class);

        // Register the item to a creative tab
        modEventBus.addListener(this::addCreative);
        // Register our mod's ForgeConfigSpec so that Forge can create and load the config file for us
        context.registerConfig(ModConfig.Type.COMMON, Config.SPEC);
    }

    private void gatherData(GatherDataEvent event) {
        PackOutput output = event.getGenerator().getPackOutput();
        CompletableFuture<HolderLookup.Provider> lookupProvider = event.getLookupProvider();

        // 注册矿石生成器
        event.getGenerator().addProvider(
                event.includeServer(),
                new ModWorldGenProvider(output, lookupProvider)
        );
    }

    public void commonSetup(final FMLCommonSetupEvent event) {
        // Some common setup code
        LOGGER.info("HELLO FROM COMMON SETUP");
        if (Config.logDirtBlock)
            LOGGER.info("DIRT BLOCK >> {}", ForgeRegistries.BLOCKS.getKey(Blocks.DIRT));
        LOGGER.info(Config.magicNumberIntroduction + Config.magicNumber);
        Config.items.forEach((item) -> LOGGER.info("ITEM >> {}", item.toString()));

        // 注册网络包
        NetworkHandler.register();

        // 部署自定义 YSM 模型到配置目录
        YSMIntegration.deployModels();


    }

    public void addCreative(BuildCreativeModeTabContentsEvent event) {
        if (event.getTabKey().equals(net.minecraft.world.item.CreativeModeTabs.TOOLS_AND_UTILITIES)) {
            event.accept(ModItems.TD_DOLL_BODY.get());
        }
    }



    @SubscribeEvent
    public void onRegisterCommands(RegisterCommandsEvent event) {
        TDollCommand.register(event.getDispatcher());
    }

    @SubscribeEvent
    public void onServerStarting(ServerStartingEvent event) {
        // Do something when the server starts
        LOGGER.info("HELLO from server starting");
        // 服务器启动时再触发一次 YSM 模型重载，确保我们的模型被加载
        YSMIntegration.reloadModels();
    }


    // ======================== Mod.EventBusSubscriber 自动注册静态方法 ========================
    @Mod.EventBusSubscriber(modid = MOD_ID, bus = Mod.EventBusSubscriber.Bus.MOD)
    public static class ModEvents {
        @SubscribeEvent
        public static void onEntityAttributeCreation(EntityAttributeCreationEvent event) {
            event.put(ModEntities.T_DOLL_ENTITY.get(), com.besson.tutorial.Entity.TDollEntity.createAttributes().build());
            event.put(ModEntities.HOSTILE_T_DOLL_ENTITY.get(), com.besson.tutorial.Entity.TDollEntity.createAttributes().build());
        }
    }

    // You can use EventBusSubscriber to automatically register all static methods in the class annotated with @SubscribeEvent
    @Mod.EventBusSubscriber(modid = MOD_ID, bus = Mod.EventBusSubscriber.Bus.MOD, value = Dist.CLIENT)
    public static class ClientModEvents {
        @SubscribeEvent
        public static void onClientSetup(FMLClientSetupEvent event) {
            // Some client setup code
            LOGGER.info("HELLO FROM CLIENT SETUP");
            LOGGER.info("MINECRAFT NAME >> {}", Minecraft.getInstance().getUser().getName());

            // 注册核心容器界面
            event.enqueueWork(() -> {
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.T_DOLL_CORE_MENU.get(),
                        com.besson.tutorial.client.screen.TDollCoreScreen::new);

                // 注册人形物品栏界面
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.T_DOLL_INVENTORY.get(),
                        com.besson.tutorial.client.screen.TDollInventoryScreen::new);

                // 注册人形指令面板界面
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.T_DOLL_COMMAND.get(),
                        com.besson.tutorial.client.screen.TDollCommandScreen::new);

                // 注册人形控制终端界面
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.T_DOLL_TERMINAL.get(),
                        com.besson.tutorial.client.screen.TDollTerminalScreen::new);

                // 注册人形制造车间界面
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.TDOLL_WORKSHOP_MENU.get(),
                        com.besson.tutorial.client.screen.TDollWorkshopScreen::new);

                // 注册装备制造工厂界面
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.EQUIPMENT_WORKSHOP_MENU.get(),
                        com.besson.tutorial.client.screen.EquipmentWorkshopScreen::new);

                // 注册心智升级研究台界面
                net.minecraft.client.gui.screens.MenuScreens.register(ModMenuTypes.MIND_UPGRADE_STATION_MENU.get(),
                        com.besson.tutorial.client.screen.MindUpgradeStationScreen::new);

                // 注册人形制造车间方块渲染器
                net.minecraft.client.renderer.blockentity.BlockEntityRenderers.register(
                        com.besson.tutorial.block.entity.ModBlockEntities.TDOLL_WORKSHOP.get(),
                        com.besson.tutorial.client.renderer.TDollWorkshopRenderer::new
                );

                // 注册装备制造工厂方块渲染器
                net.minecraft.client.renderer.blockentity.BlockEntityRenderers.register(
                        com.besson.tutorial.block.entity.ModBlockEntities.EQUIPMENT_WORKSHOP.get(),
                        com.besson.tutorial.client.renderer.EquipmentWorkshopRenderer::new
                );

                // 注册心智升级研究台方块渲染器
                net.minecraft.client.renderer.blockentity.BlockEntityRenderers.register(
                        com.besson.tutorial.block.entity.ModBlockEntities.MIND_UPGRADE_STATION.get(),
                        com.besson.tutorial.client.renderer.MindUpgradeStationRenderer::new
                );

                // 注册人形素体实体渲染器
                // 注意：Forge 1.20.1 的 EntityRenderers.register 返回 void，
                // 不能直接拿到 renderer 实例引用。但 TDollRenderer 内部用 static 缓存 animatables，
                // 我们可以从外部 LoggingOut 事件直接调用 TDollRenderer.clearAllAnimatables()。
                net.minecraft.client.renderer.entity.EntityRenderers.register(
                        ModEntities.T_DOLL_ENTITY.get(),
                        com.besson.tutorial.client.renderer.TDollRenderer::new
                );

                // 敌对 T-Doll 复用同一个 Renderer（共享 YSM 模型 + TACZ 渲染管线）
                net.minecraft.client.renderer.entity.EntityRenderers.register(
                        ModEntities.HOSTILE_T_DOLL_ENTITY.get(),
                        com.besson.tutorial.client.renderer.TDollRenderer::new
                );
            });
        }
    }

    /**
     * 玩家退出存档时清空 TDollRenderer 的 animatable 缓存。
     * 防止上一世界残留的 animatable 引用已 dispose 的旧 entity 实体，
     * 导致下一世界同 UUID 的实体渲染动画状态错乱（走路/跑步变成滑步）。
     * 走 MinecraftForge.EVENT_BUS（而非 mod event bus）。
     */
    @SubscribeEvent
    public void onClientPlayerLogout(net.minecraftforge.client.event.ClientPlayerNetworkEvent.LoggingOut event) {
        com.besson.tutorial.client.renderer.TDollRenderer.clearAllAnimatables();
    }

    /**
     * 玩家游戏刻事件监听
     */

    private double a = 0;
    @SubscribeEvent
    public void onTick(TickEvent.ClientTickEvent event) {
}

    /**
     * 玩家切换维度事件：进入心智升级维度时自动重置进度（确保每次进入都是新挑战）。
     */
    @SubscribeEvent
    public void onPlayerChangedDimension(PlayerEvent.PlayerChangedDimensionEvent event) {
        if (event.getEntity() instanceof ServerPlayer player) {
            if (event.getTo() == MindUpgradeDimension.LEVEL_KEY) {
                // 进入心智升级维度 → 启动副本（15s 准备 + 重置）
                MindUpgradeInstanceManager.INSTANCE.onPlayerEnter(player);
            } else if (event.getFrom() == MindUpgradeDimension.LEVEL_KEY) {
                // 离开心智升级维度 → 清理进度（按需求6，再次进入时重置）
                MindUpgradeInstanceManager.INSTANCE.onPlayerLeave(player);
            }
        }
    }

    /**
     * 服务端 tick：驱动心智升级副本状态机。
     */
    @SubscribeEvent
    public void onServerTick(TickEvent.ServerTickEvent event) {
        if (event.phase != TickEvent.Phase.END) return;
        if (event.getServer() == null) return;
        ServerLevel level = event.getServer().getLevel(com.besson.tutorial.dimension.MindUpgradeDimension.LEVEL_KEY);
        if (level == null) return;
        MindUpgradeInstanceManager.INSTANCE.tick(level);
    }
    @SubscribeEvent
    public void onPlayerTick(TickEvent.PlayerTickEvent event) {
        Player player = event.player;
        if (event.phase == TickEvent.Phase.END && player instanceof ServerPlayer serverPlayer) {
            com.besson.tutorial.skill.HK416SkillManager.tick(serverPlayer);
            com.besson.tutorial.skill.M4A1SkillManager.tick(serverPlayer);
            com.besson.tutorial.skill.UMP45SkillManager.tick(serverPlayer);
            com.besson.tutorial.skill.UMP9SkillManager.tick(serverPlayer);
            com.besson.tutorial.skill.G11SkillManager.tick(serverPlayer);
        }

      if (player.hasEffect(ModEffects.AIM_AUTO_EFFECT.get()) && TDollStateManager.isActive(player)) {
          if (event.phase == TickEvent.Phase.END) {
              while (MY_P_KEY.consumeClick()) {  // 消耗点击事件

                  if (player != null) {
                      // 发送系统消息到聊天框
                      if(a == 0 ){
                         player.sendSystemMessage(Component.literal("打开自瞄（原版怪物锁定）"));
                        this.a  = 1;
                        break;
                      }
                     if(a == 1){
                         player.sendSystemMessage(Component.literal("打开自瞄（全实体锁定）"));
                         this.a= 2;
                         break;

                     }
                     if(a==2){
                         player.sendSystemMessage(Component.literal("关闭自瞄"));
                         this.a= 0;
                         break;

                     }


                  }
              }
          }if(a == 1){
            Entity target = findNearestEntity(player, 80.0);

            if (target != null) {


                lookAtEntity(player, target);}

            }
          if(a == 2){
              Entity target = findNearestEntity2(player, 80.0);

              if (target != null) {


                  lookAtEntity(player, target);}

          }

    }}


    // * @param viewer 观察者 (玩家)
    // * @param target 目标实体
    public static Entity findNearestEntity2 (Player player,double range){
        Level world = player.level();

        List<LivingEntity> entities = world.getEntitiesOfClass(
                LivingEntity.class,
                player.getBoundingBox().inflate(range),
                e -> e != player && e.isAlive()
        );

        return entities.stream()
                .filter(e -> e.isAlive() && player.hasLineOfSight(e))
                .min(Comparator.comparingDouble(e -> e.distanceToSqr(player)))
                .orElse(null);}

    public static Entity findNearestEntity (Player player,double range){
        Level world = player.level();

// 1. 获取玩家周围实体（排除其他玩家），只查找 LivingEntity (生物) 以提高效率
        List<LivingEntity> entities = world.getEntitiesOfClass(
                LivingEntity.class, // 只查找生物，避免瞄准掉落物
                player.getBoundingBox().inflate(range),
                e ->e != player &&
                        e.isAlive() &&
                        e.getType().getCategory() != MobCategory.CREATURE &&
                        e.getType().getCategory() != MobCategory.AMBIENT&&
                        e.getType().getCategory() != MobCategory.WATER_CREATURE&&!(e instanceof Villager)&&!(e instanceof Cod)&&!(e instanceof Salmon)&&!(e instanceof Pufferfish)
&&!(e instanceof TropicalFish)
        );

        return entities.stream()
                .filter(e -> e.isAlive() && player.hasLineOfSight(e))
                .min(Comparator.comparingDouble(e -> e.distanceToSqr(player)))
                .orElse(null);
    }

    // * @param viewer 观察者 (玩家)
    // * @param target 目标实体

    public static void lookAtEntity(Entity viewer, Entity target) {

        // 获取观察者和目标的位置
        // 获取观察者和目标的位置
        Vec3 eyePos = viewer.getEyePosition(1.0F);
        Vec3 targetEyePos = target.getEyePosition(1.0F);

        double dx = targetEyePos.x() - eyePos.x();
        double dy = targetEyePos.y() - eyePos.y();
        double dz = targetEyePos.z() - eyePos.z();

        double horizontalDist = Math.sqrt(dx * dx + dz * dz);
        float pitch = (float)(-Math.atan2(dy, horizontalDist) * 180.0D / Math.PI);
        float yaw = (float)(Math.atan2(dz, dx) * 180.0D / Math.PI) - 90.0F;

// 角度规范化处理
        yaw = Mth.wrapDegrees(yaw);
        pitch = Mth.wrapDegrees(pitch);

// 直接设置视角（去掉平滑过渡）
        viewer.setYRot(yaw);
        viewer.setXRot(pitch);

    }



        // *******************************************************
        // 事件监听：玩家游戏刻事件 (核心逻辑)
        // *******************************************************


    @SubscribeEvent
    public void onLivingHurt(LivingHurtEvent event) {
        LivingEntity entity = event.getEntity();
        if (event.getSource().getEntity() instanceof Player attacker
                && (com.besson.tutorial.skill.HK416SkillManager.hasActiveSkillGun(attacker)
                || com.besson.tutorial.skill.HK416SkillManager.hasSkillDamageWindow(attacker))
                && com.besson.tutorial.skill.HK416SkillManager.isHK416(attacker)) {
            float skillDamage = com.besson.tutorial.skill.HK416SkillManager.getSkillDamage(attacker);
            if (skillDamage > 0.0F) event.setAmount(skillDamage);
        }
        if (entity instanceof Player player && TDollStateManager.isActive(player)) {
            float reduction = TDollStateManager.getDamageReduction(player);
            event.setAmount(event.getAmount() * (1.0F - reduction));
        }
    }

    @SubscribeEvent
    public void onPlayerClone(PlayerEvent.Clone event) {
        Player original = event.getOriginal();
        Player newPlayer = event.getEntity();
        
        LOGGER.info("[TDoll] === PlayerCloneEvent ===");
        LOGGER.info("[TDoll] Original player: {}", original.getName().getString());
        LOGGER.info("[TDoll] New player: {}", newPlayer.getName().getString());
        LOGGER.info("[TDoll] Original is active: {}", TDollStateManager.isActive(original));
        LOGGER.info("[TDoll] Original is bound: {}", TDollStateManager.isBound(original));
        LOGGER.info("[TDoll] Original bound core ID: {}", TDollStateManager.getBoundCoreId(original));
        LOGGER.info("[TDoll] Original bound core UUID: {}", TDollStateManager.getBoundCoreUUID(original));
        
        if (TDollStateManager.isActive(original)) {
            String coreId = TDollStateManager.getActiveCoreId(original);
            newPlayer.getPersistentData().putBoolean(TDollStateManager.ACTIVE_KEY, true);
            newPlayer.getPersistentData().putString(TDollStateManager.CORE_ID_KEY, coreId);
            LOGGER.info("[TDoll] Copied active core: {}", coreId);
        }
        
        if (TDollStateManager.isBound(original)) {
            String boundCoreId = TDollStateManager.getBoundCoreId(original);
            String boundCoreUUID = TDollStateManager.getBoundCoreUUID(original);
            newPlayer.getPersistentData().putString(TDollStateManager.BOUND_CORE_ID_KEY, boundCoreId);
            newPlayer.getPersistentData().putString(TDollStateManager.BOUND_CORE_UUID_KEY, boundCoreUUID);
            LOGGER.info("[TDoll] Copied bound core: {} ({})", boundCoreId, boundCoreUUID);
            
            if (boundCoreUUID != null && !boundCoreUUID.isEmpty()) {
                TDollStateManager.coreUUIDToPlayerUUID.put(boundCoreUUID, newPlayer.getUUID());
                LOGGER.info("[TDoll] Updated coreUUIDToPlayerUUID mapping: {} -> {}", boundCoreUUID, newPlayer.getUUID());
            }
            
            if (original.getPersistentData().contains(TDollStateManager.BOUND_CORE_NBT_KEY, 10)) {
                net.minecraft.nbt.CompoundTag savedNBT = original.getPersistentData().getCompound(TDollStateManager.BOUND_CORE_NBT_KEY);
                newPlayer.getPersistentData().put(TDollStateManager.BOUND_CORE_NBT_KEY, savedNBT);
                LOGGER.info("[TDoll] Copied saved core NBT data");
            }
        }
        
        LOGGER.info("[TDoll] New player is bound: {}", TDollStateManager.isBound(newPlayer));
        LOGGER.info("[TDoll] New player bound core UUID: {}", TDollStateManager.getBoundCoreUUID(newPlayer));
    }

    @SubscribeEvent
    public void onPlayerRespawn(PlayerEvent.PlayerRespawnEvent event) {
        Player player = event.getEntity();
        LOGGER.info("[TDoll] === PlayerRespawnEvent ===");
        LOGGER.info("[TDoll] Player: {}", player.getName().getString());
        LOGGER.info("[TDoll] Is bound: {}", TDollStateManager.isBound(player));
        LOGGER.info("[TDoll] Is active: {}", TDollStateManager.isActive(player));
        
        if (!player.level().isClientSide) {
            // 恢复 coreUUIDToPlayerUUID 映射（防止 PlayerClone 后映射丢失）
            if (TDollStateManager.isBound(player)) {
                String boundUUID = TDollStateManager.getBoundCoreUUID(player);
                if (boundUUID != null && !boundUUID.isEmpty()) {
                    UUID existing = TDollStateManager.coreUUIDToPlayerUUID.get(boundUUID);
                    if (existing == null) {
                        TDollStateManager.coreUUIDToPlayerUUID.put(boundUUID, player.getUUID());
                        LOGGER.info("[TDoll] Restored coreUUIDToPlayerUUID mapping on respawn: {} -> {}",
                                boundUUID, player.getUUID());
                    }
                }
            }
            
            if (TDollStateManager.isActive(player)) {
                TDollCoreConfig.TDollCoreEntry core = TDollStateManager.getActiveCore(player);
                if (core != null) {
                    LOGGER.info("[TDoll] Respawn applying active core: {}", core.nameZh);
                    // 优先从玩家背包中查找绑定的核心（最新状态，含等级经验）
                    ItemStack coreStack = ItemStack.EMPTY;
                    if (TDollStateManager.isBound(player)) {
                        String boundUUID = TDollStateManager.getBoundCoreUUID(player);
                        coreStack = findBoundCoreStack(player, boundUUID);
                        if (!coreStack.isEmpty()) {
                            LOGGER.info("[TDoll] Respawn restored core stack from inventory: uuid={}", boundUUID);
                            // 用背包中最新的核心同步 savedCoreNBT
                            TDollStateManager.saveBoundCoreNBT(player, coreStack);
                        }
                    }
                    // 背包找不到时，回退到死亡前保存的 NBT
                    if (coreStack.isEmpty()) {
                        CompoundTag savedCoreNBT = TDollStateManager.getSavedCoreNBT(player);
                        coreStack = TDollStateManager.restoreCoreFromNBT(savedCoreNBT);
                        if (!coreStack.isEmpty()) {
                            LOGGER.info("[TDoll] Respawn restored core stack from saved NBT");
                        }
                    }

                    TDollStateManager.applyModifiers(player, core, coreStack.isEmpty() ? null : coreStack);
                    if (!coreStack.isEmpty()) {
                        TDollStateManager.applyAttachmentModifiers(player, coreStack);
                        TDollStateManager.applyGunDamageBonus(player, coreStack);
                    }
                    // 重生后血量通常已被重置，这里将血量恢复到新的最大值
                    if (player.getHealth() > 0) {
                        float newMax = player.getMaxHealth();
                        player.setHealth(newMax);
                        LOGGER.info("[TDoll] Respawn set player health to max: {}/{}", newMax, newMax);
                    }
                    YSMIntegration.setModel(player, TDollCoreItem.getCoreModelId(coreStack.isEmpty() ? ItemStack.EMPTY : coreStack), core.defaultTexture);
                    LOGGER.info("[TDoll] Applied modifiers and model for active core: {}", core.nameZh);

                    // 根据死亡不掉落规则决定是否重新发放枪械
                    // keepInventory=true：玩家保留物品，不需要重新发枪
                    // keepInventory=false：玩家掉落物品，需要从核心 NBT 重新发枪
                    boolean keepInv = player.level().getGameRules().getBoolean(net.minecraft.world.level.GameRules.RULE_KEEPINVENTORY);
                    if (!keepInv && !coreStack.isEmpty()) {
                        TDollCoreItem.giveGunAndAmmoFromCore(player, coreStack, core.id);
                        LOGGER.info("[TDoll] keepInventory=false, restored gun and ammo from core on respawn");
                    } else {
                        LOGGER.info("[TDoll] keepInventory={}, skipped gun restoration on respawn", keepInv);
                    }
                }
            }
            TDollStateManager.sendSyncPacket(player);
            giveBoundCoreToPlayer(player);
        }
    }

    @SubscribeEvent
    public void onPlayerLoggedIn(PlayerEvent.PlayerLoggedInEvent event) {
        Player player = event.getEntity();
        LOGGER.info("[TDoll] === PlayerLoggedInEvent ===");
        LOGGER.info("[TDoll] Player: {}", player.getName().getString());
        LOGGER.info("[TDoll] Is bound: {}", TDollStateManager.isBound(player));
        LOGGER.info("[TDoll] Is active: {}", TDollStateManager.isActive(player));
        
        if (!player.level().isClientSide) {
            // ======================== 关键修复：恢复 coreUUIDToPlayerUUID 映射 ========================
            // 服务器重启后 coreUUIDToPlayerUUID 静态 Map 为空，必须从玩家持久化数据恢复，
            // 否则 isCoreBoundToOtherPlayer 等检查会失效，导致核心绑定状态异常。
            if (TDollStateManager.isBound(player)) {
                String boundUUID = TDollStateManager.getBoundCoreUUID(player);
                if (boundUUID != null && !boundUUID.isEmpty()) {
                    UUID existing = TDollStateManager.coreUUIDToPlayerUUID.get(boundUUID);
                    if (existing == null) {
                        TDollStateManager.coreUUIDToPlayerUUID.put(boundUUID, player.getUUID());
                        LOGGER.info("[TDoll] Restored coreUUIDToPlayerUUID mapping: {} -> {}", 
                                boundUUID, player.getUUID());
                    } else if (!existing.equals(player.getUUID())) {
                        LOGGER.warn("[TDoll] coreUUIDToPlayerUUID conflict: {} already mapped to {}, "
                                + "current player is {}", boundUUID, existing, player.getUUID());
                    }
                }
            }
            
            if (TDollStateManager.isActive(player)) {
                TDollCoreConfig.TDollCoreEntry core = TDollStateManager.getActiveCore(player);
                if (core != null) {
                    // 优先从玩家背包中查找绑定的核心（最新状态，含等级经验）
                    ItemStack coreStack = ItemStack.EMPTY;
                    if (TDollStateManager.isBound(player)) {
                        String boundUUID = TDollStateManager.getBoundCoreUUID(player);
                        coreStack = findBoundCoreStack(player, boundUUID);
                        if (!coreStack.isEmpty()) {
                            LOGGER.info("[TDoll] Login restored core stack from inventory: uuid={}", boundUUID);
                            // 用背包中最新的核心同步 savedCoreNBT
                            TDollStateManager.saveBoundCoreNBT(player, coreStack);
                        }
                    }
                    // 背包找不到时，回退到保存的 NBT
                    if (coreStack.isEmpty()) {
                        CompoundTag savedCoreNBT = TDollStateManager.getSavedCoreNBT(player);
                        coreStack = TDollStateManager.restoreCoreFromNBT(savedCoreNBT);
                        if (!coreStack.isEmpty()) {
                            LOGGER.info("[TDoll] Login restored core stack from saved NBT");
                        }
                    }

                    if (coreStack.isEmpty()) {
                        LOGGER.warn("[TDoll] Login no saved core NBT or inventory core found for player {}, applying base stats", player.getName().getString());
                    }
                    float healthBefore = player.getHealth();
                    float maxHealthBefore = player.getMaxHealth();
                    LOGGER.info("[TDoll] Login health before apply: {}/{}", healthBefore, maxHealthBefore);
                    TDollStateManager.applyModifiers(player, core, coreStack.isEmpty() ? null : coreStack);
                    if (!coreStack.isEmpty()) {
                        TDollStateManager.applyAttachmentModifiers(player, coreStack);
                        TDollStateManager.applyGunDamageBonus(player, coreStack);
                    }
                    // 进入存档时血量可能已被截断到基础上限，这里恢复到新的最大值
                    if (player.getHealth() > 0) {
                        float newMax = player.getMaxHealth();
                        player.setHealth(newMax);
                        LOGGER.info("[TDoll] Login set player health to max: {}/{} (was {}/{})",
                                newMax, newMax, healthBefore, maxHealthBefore);
                    }
                    YSMIntegration.setModel(player, TDollCoreItem.getCoreModelId(coreStack.isEmpty() ? ItemStack.EMPTY : coreStack), core.defaultTexture);
                    LOGGER.info("[TDoll] Applied modifiers and model for active core: {}", core.nameZh);
                }
            }
            TDollStateManager.sendSyncPacket(player);
            if (player instanceof ServerPlayer serverPlayer) {
                com.besson.tutorial.skill.HK416SkillManager.syncState(serverPlayer);
                if (com.besson.tutorial.skill.M4A1SkillManager.isM4A1(player)) {
                    com.besson.tutorial.skill.M4A1SkillManager.syncState(serverPlayer);
                } else if (com.besson.tutorial.skill.UMP45SkillManager.isUMP45(player)) {
                    com.besson.tutorial.skill.UMP45SkillManager.syncState(serverPlayer);
                } else if (com.besson.tutorial.skill.UMP9SkillManager.isUMP9(player)) {
                    com.besson.tutorial.skill.UMP9SkillManager.syncState(serverPlayer);
                } else if (com.besson.tutorial.skill.G11SkillManager.isG11(player)) {
                    com.besson.tutorial.skill.G11SkillManager.syncState(serverPlayer);
                }
            }
            giveBoundCoreToPlayer(player);
        }
    }

    @SubscribeEvent
    public void onLivingDeath(LivingDeathEvent event) {
        // 敌对 T-Doll 死亡：发放副本奖励（记忆碎片 / 火控元件）
        if (event.getEntity() instanceof com.besson.tutorial.Entity.HostileTDollEntity hostile) {
            if (hostile.level().isClientSide) return;
            // 找最近玩家作为击杀者
            ServerPlayer killer = null;
            if (event.getSource() != null && event.getSource().getEntity() instanceof ServerPlayer sp) {
                killer = sp;
            } else if (hostile.getLastAttacker() instanceof ServerPlayer sp2) {
                killer = sp2;
            } else {
                List<Player> candidates = hostile.level().getEntitiesOfClass(
                        Player.class, hostile.getBoundingBox().inflate(32), Player::isAlive);
                if (!candidates.isEmpty()) killer = (ServerPlayer) candidates.get(0);
            }
            if (killer != null) {
                MindUpgradeInstanceManager.InstanceState state =
                        MindUpgradeInstanceManager.INSTANCE.get(killer.getUUID());
                int wave = state != null ? state.currentWave : 0;
                MindUpgradeInstanceManager.grantKillRewards(killer, wave);
                // 从 alive 列表中移除
                if (state != null) state.aliveEnemies.remove(hostile.getUUID());
            }
            return;
        }

        // 友方 T-Doll 死亡：归还核心和素体到主人背包
        if (event.getEntity() instanceof com.besson.tutorial.Entity.TDollEntity tdoll
                && !(event.getEntity() instanceof com.besson.tutorial.Entity.HostileTDollEntity)) {
            if (tdoll.level().isClientSide) return;
            String ownerUUID = tdoll.getOwnerUUID();
            if (ownerUUID == null || ownerUUID.isEmpty()) return;
            ServerPlayer owner = tdoll.level().getServer() != null
                    ? tdoll.level().getServer().getPlayerList().getPlayer(java.util.UUID.fromString(ownerUUID))
                    : null;
            if (owner == null) return;

            // 从 BoundCoreNBT 恢复核心
            net.minecraft.nbt.CompoundTag savedNBT = tdoll.getPersistentData().getCompound("BoundCoreNBT");
            if (!savedNBT.isEmpty()) {
                ItemStack coreStack = ItemStack.of(savedNBT);
                if (!coreStack.isEmpty() && coreStack.getItem() instanceof TDollCoreItem) {
                    // 重新设置 CoreUUID，允许重新绑定
                    String newUUID = java.util.UUID.randomUUID().toString();
                    coreStack.getOrCreateTag().putString("CoreUUID", newUUID);
                    // 归还核心到玩家背包
                    if (!owner.getInventory().add(coreStack)) {
                        owner.drop(coreStack, false);
                    }
                    owner.sendSystemMessage(net.minecraft.network.chat.Component.literal("§a你的人形核心已归还到背包"));
                }
            }

            // 归还素体
            net.minecraftforge.registries.RegistryObject<Item> bodyItem = com.besson.tutorial.item.ModItems.TD_DOLL_BODY;
            if (bodyItem != null && bodyItem.isPresent()) {
                ItemStack body = new ItemStack(bodyItem.get());
                if (!owner.getInventory().add(body)) {
                    owner.drop(body, false);
                }
                owner.sendSystemMessage(net.minecraft.network.chat.Component.literal("§a你的人形素体已归还到背包"));
            }

            LOGGER.info("[TDoll] Returned core and body to player {} on TDoll death", owner.getName().getString());
            return;
        }

        if (event.getEntity() instanceof Player player) {
            LOGGER.info("[TDoll] === LivingDeathEvent ===");
            LOGGER.info("[TDoll] Player: {}", player.getName().getString());
            LOGGER.info("[TDoll] Is active before death: {}", TDollStateManager.isActive(player));
            LOGGER.info("[TDoll] Is bound before death: {}", TDollStateManager.isBound(player));
            LOGGER.info("[TDoll] Bound core UUID: {}", TDollStateManager.getBoundCoreUUID(player));
            
            // 不 deactivate 核心，保留激活状态用于复活后恢复
            if (TDollStateManager.isBound(player)) {
                String boundUUID = TDollStateManager.getBoundCoreUUID(player);
                ItemStack boundCoreStack = findBoundCoreStack(player, boundUUID);
                
                if (!boundCoreStack.isEmpty()) {
                    boolean keepInv = player.level().getGameRules().getBoolean(net.minecraft.world.level.GameRules.RULE_KEEPINVENTORY);
                    
                    if (!keepInv) {
                        // keepInventory=false：玩家会丢失物品，死亡前将枪械保存回核心 NBT 供复活后恢复
                        var handler = TDollCoreItem.getInventory(boundCoreStack);
                        if (handler.getStackInSlot(com.besson.tutorial.menu.TDollCoreMenu.SLOT_GUN).isEmpty()) {
                            for (int i = 0; i < player.getInventory().getContainerSize(); i++) {
                                ItemStack item = player.getInventory().getItem(i);
                                if (com.tacz.guns.api.item.IGun.getIGunOrNull(item) != null) {
                                    handler.setStackInSlot(com.besson.tutorial.menu.TDollCoreMenu.SLOT_GUN, item.copy());
                                    LOGGER.info("[TDoll] Saved gun back to core NBT on death: {}", item.getDisplayName().getString());
                                    break;
                                }
                            }
                        }
                        if (handler.getStackInSlot(com.besson.tutorial.menu.TDollCoreMenu.SLOT_AMMO).isEmpty()) {
                            for (int i = 0; i < player.getInventory().getContainerSize(); i++) {
                                ItemStack item = player.getInventory().getItem(i);
                                net.minecraft.resources.ResourceLocation rl = net.minecraftforge.registries.ForgeRegistries.ITEMS.getKey(item.getItem());
                                if (rl != null && rl.getNamespace().equals("tacz") && rl.getPath().equals("ammo_box")) {
                                    handler.setStackInSlot(com.besson.tutorial.menu.TDollCoreMenu.SLOT_AMMO, item.copy());
                                    LOGGER.info("[TDoll] Saved ammo back to core NBT on death");
                                    break;
                                }
                            }
                        }
                        TDollCoreItem.setInventory(boundCoreStack, handler);
                        LOGGER.info("[TDoll] keepInventory=false, saved gun/ammo to core NBT for respawn");
                    } else {
                        LOGGER.info("[TDoll] keepInventory=true, skipping save gun to core NBT (player keeps items)");
                    }
                    
                    LOGGER.info("[TDoll] Saving bound core NBT for player: {}", player.getName().getString());
                    TDollStateManager.saveBoundCoreNBT(player, boundCoreStack);
                } else {
                    LOGGER.warn("[TDoll] Could not find bound core stack in player inventory");
                }
            }

            // 玩家死亡：清理副本状态，关闭 HUD
            if (!player.level().isClientSide && player instanceof ServerPlayer sp) {
                MindUpgradeInstanceManager.INSTANCE.onPlayerLeave(sp);
            }
            
            LOGGER.info("[TDoll] Is active after deactivation: {}", TDollStateManager.isActive(player));
            LOGGER.info("[TDoll] Is bound after deactivation: {}", TDollStateManager.isBound(player));
        }
    }
    
    private ItemStack findBoundCoreStack(Player player, String boundUUID) {
        for (int i = 0; i < player.getInventory().getContainerSize(); i++) {
            ItemStack stack = player.getInventory().getItem(i);
            if (stack.getItem() instanceof TDollCoreItem) {
                String stackUUID = TDollCoreItem.getCoreUUID(stack);
                if (boundUUID != null && boundUUID.equals(stackUUID)) {
                    return stack;
                }
            }
        }
        return ItemStack.EMPTY;
    }

    @SubscribeEvent
    public void onLivingDrops(LivingDropsEvent event) {
        if (event.getEntity() instanceof Player player) {
            if (TDollStateManager.isBound(player)) {
                String boundUUID = TDollStateManager.getBoundCoreUUID(player);
                LOGGER.info("[TDoll] Player {} is bound to core UUID: {}, removing from drops", player.getName().getString(), boundUUID);
                
                event.getDrops().removeIf(drop -> {
                    ItemStack stack = drop.getItem();
                    if (stack.getItem() instanceof TDollCoreItem) {
                        String stackUUID = TDollCoreItem.getCoreUUID(stack);
                        boolean isBoundCore = boundUUID != null && boundUUID.equals(stackUUID);
                        if (isBoundCore) {
                            LOGGER.info("[TDoll] Removed bound core from drops: {}", stackUUID);
                        }
                        return isBoundCore;
                    }
                    return false;
                });
            }
        }
    }

    private void giveBoundCoreToPlayer(Player player) {
        LOGGER.info("[TDoll] === giveBoundCoreToPlayer ===");
        LOGGER.info("[TDoll] Player: {}", player.getName().getString());
        LOGGER.info("[TDoll] Is bound: {}", TDollStateManager.isBound(player));
        
        if (!TDollStateManager.isBound(player)) {
            LOGGER.info("[TDoll] Player is not bound, returning");
            return;
        }

        String boundUUID = TDollStateManager.getBoundCoreUUID(player);
        String boundCoreId = TDollStateManager.getBoundCoreId(player);
        
        LOGGER.info("[TDoll] Bound core UUID: {}", boundUUID);
        LOGGER.info("[TDoll] Bound core ID: {}", boundCoreId);
        
        boolean hasCore = false;
        for (int i = 0; i < player.getInventory().getContainerSize(); i++) {
            ItemStack stack = player.getInventory().getItem(i);
            if (stack.getItem() instanceof TDollCoreItem) {
                String stackUUID = TDollCoreItem.getCoreUUID(stack);
                if (boundUUID != null && boundUUID.equals(stackUUID)) {
                    hasCore = true;
                    LOGGER.info("[TDoll] Found bound core in inventory slot {}", i);
                    break;
                }
            }
        }

        if (!hasCore) {
            LOGGER.info("[TDoll] Player does not have bound core, trying to restore from saved NBT");
            
            net.minecraft.nbt.CompoundTag savedNBT = TDollStateManager.getSavedCoreNBT(player);
            if (savedNBT != null) {
                LOGGER.info("[TDoll] Found saved core NBT, restoring...");
                ItemStack restoredStack = TDollStateManager.restoreCoreFromNBT(savedNBT);
                
                if (!restoredStack.isEmpty()) {
                    boolean added = player.getInventory().add(restoredStack);
                    if (added) {
                        LOGGER.info("[TDoll] Successfully restored bound core from saved NBT");
                    } else {
                        player.drop(restoredStack, false);
                        LOGGER.info("[TDoll] Inventory full, dropped restored core on ground");
                    }
                    return;
                }
            }
            
            LOGGER.info("[TDoll] No saved NBT found or restore failed, creating new core");
            
            TDollCoreConfig.TDollCoreEntry core = TDollCoreConfig.getCore(boundCoreId);
            if (core != null) {
                LOGGER.info("[TDoll] Core config found: {}", core.nameZh);
                
                ResourceLocation coreRl = new ResourceLocation(TutorialMod.MOD_ID, boundCoreId);
                Item coreItem = ForgeRegistries.ITEMS.getValue(coreRl);
                
                if (coreItem != null && coreItem != net.minecraft.world.item.Items.AIR) {
                    LOGGER.info("[TDoll] Core item found: {}", coreItem.getDescriptionId());
                    
                    ItemStack coreStack = new ItemStack(coreItem);
                    if (!coreStack.isEmpty()) {
                        if (boundUUID != null && !boundUUID.isEmpty()) {
                            coreStack.getOrCreateTag().putString("CoreUUID", boundUUID);
                            LOGGER.info("[TDoll] Set CoreUUID on stack");
                        }
                        
                        boolean added = player.getInventory().add(coreStack);
                        if (added) {
                            LOGGER.info("[TDoll] Successfully added core to inventory");
                        } else {
                            player.drop(coreStack, false);
                            LOGGER.info("[TDoll] Inventory full, dropped core on ground");
                        }
                    } else {
                        LOGGER.warn("[TDoll] Core stack is empty!");
                    }
                } else {
                    LOGGER.error("[TDoll] Core item not found for ID: {}", boundCoreId);
                }
            } else {
                LOGGER.error("[TDoll] Core config not found for ID: {}", boundCoreId);
            }
        } else {
            LOGGER.info("[TDoll] Player already has bound core in inventory");
        }
    }

    @SubscribeEvent
    public void onPlayerInteractEntity(PlayerInteractEvent.EntityInteract event) {
        if (event.getLevel().isClientSide) return;

        if (!(event.getTarget() instanceof TDollEntity tdoll)) return;

        Player player = event.getEntity();
        ItemStack stack = event.getItemStack();

        // ======================== 手持训练数据 → 直接升级人形核心 ========================
        if (stack.getItem() instanceof TrainingDataItem) {
            handleTrainingDataOnEntity(player, tdoll, stack);
            event.setCanceled(true);
            return;
        }

        // ======================== 手持 TDollCoreItem + 人形核心卡在突破点 → 尝试突破 ========================
        if (stack.getItem() instanceof TDollCoreItem) {
            if (TDollCoreItem.tryBreakthroughOnEntity(player, tdoll, stack)) {
                event.setCanceled(true);
                return;
            }
        }

        // ======================== 非 Shift 右键 → 打开人形物品栏 ========================
        if (!player.isShiftKeyDown()) {
            if (player instanceof ServerPlayer serverPlayer) {
                openTDollInventory(serverPlayer, tdoll);
                LOGGER.info("[TDoll] Opened inventory UI for entity: {}", tdoll.getUUID().toString().substring(0, 8));
            }
            event.setCanceled(true);
            return;
        }

        // ======================== Shift+右键 → 绑定核心或打开物品栏 ========================
        // 如果没有手持核心，则打开物品栏
        if (!(stack.getItem() instanceof TDollCoreItem coreItem)) {
            if (player instanceof ServerPlayer serverPlayer) {
                openTDollInventory(serverPlayer, tdoll);
                LOGGER.info("[TDoll] Opened inventory UI for entity: {}", tdoll.getUUID().toString().substring(0, 8));
            }
            event.setCanceled(true);
            return;
        }

        // ======================== Shift+右键 + 手持核心 → 绑定核心 ========================
        String coreId = coreItem.getCoreId();
        TDollCoreConfig.TDollCoreEntry core = TDollCoreConfig.getCore(coreId);
        String coreUUID = TDollCoreItem.getCoreUUID(stack);
        
        LOGGER.info("[TDoll] === PlayerInteractEntity (Shift+RightClick with Core) ===");
        LOGGER.info("[TDoll] Player: {}", player.getName().getString());
        LOGGER.info("[TDoll] Player UUID: {}", player.getUUID());
        LOGGER.info("[TDoll] Core ID: {}", coreId);
        LOGGER.info("[TDoll] Core UUID: {}", coreUUID);
        LOGGER.info("[TDoll] TDoll entity UUID: {}", tdoll.getUUID());
        LOGGER.info("[TDoll] TDoll is bound: {}", tdoll.isBound());
        LOGGER.info("[TDoll] Core is bound to other player: {}", TDollStateManager.isCoreBoundToOtherPlayer(coreUUID, player));
        
        if (core == null) {
            player.sendSystemMessage(Component.literal("§c未知的人形核心: " + coreId));
            LOGGER.info("[TDoll] Aborted: Core not found");
            return;
        }

        if (tdoll.isBound()) {
            player.sendSystemMessage(Component.literal("§c这个人形素体已经绑定了核心！"));
            LOGGER.info("[TDoll] Aborted: TDoll entity already bound");
            return;
        }

        if (TDollStateManager.isCoreBoundToAnyPlayer(coreUUID)) {
            player.sendSystemMessage(Component.literal("§c此核心已被玩家绑定！"));
            LOGGER.info("[TDoll] Aborted: Core bound to any player");
            return;
        }

        boolean firstUse = TDollCoreItem.isFirstUse(stack);
        if (firstUse) {
            TDollCoreItem.generateInitialEquipment(stack, core);
            TDollCoreItem.markAsUsed(stack);
            LOGGER.info("[TDoll] Generated initial equipment for bound core");
        }
        
        tdoll.setBoundCoreId(coreId);
        tdoll.setBoundCoreUUID(coreUUID);
        // 同步核心阶段到实体（用于客户端模型选择）
        tdoll.setCoreStage(TDollCoreItem.getCoreStage(stack));
        
        // ======================== 设置主人 UUID ========================
        tdoll.setOwnerUUID(player.getUUID().toString());
        LOGGER.info("[TDoll] Set owner UUID on TDoll: {} -> {}", player.getUUID(), tdoll.getUUID().toString().substring(0, 8));
        
        CompoundTag coreNBT = stack.serializeNBT();
        tdoll.getPersistentData().put("BoundCoreNBT", coreNBT);
        LOGGER.info("[TDoll] Saved core NBT to TDoll entity: {}", tdoll.getUUID());
        
        // ======================== 自动将核心内枪和子弹转给人形 ========================
        LOGGER.info("[TDoll] === Transferring equipment to TDoll ===");
        TDollCoreItem.giveGunAndAmmoToEntity(tdoll, stack);
        LOGGER.info("[TDoll] ✅ Transferred gun and ammo to TDoll entity");
        LOGGER.info("[TDoll] TDoll mainHandItem after transfer: {}",
                tdoll.getMainHandItem().getDisplayName().getString());
        
        stack.shrink(1);
        LOGGER.info("[TDoll] Removed core item from player inventory");
        
        player.sendSystemMessage(Component.literal("§a已将" + core.nameZh + "绑定到这个人形素体！武器已发放"));
        LOGGER.info("[TDoll] Successfully bound core {} to TDoll entity {}", coreId, tdoll.getUUID());
        
        tdoll.applyCoreAttributes();
        LOGGER.info("[TDoll] Applied core attributes to TDoll entity");
        
        event.setCanceled(true);
    }

    /**
     * 手持训练数据右键人形 → 直接升级核心。
     * 核心绑定在实体身上（PersistentData 中的 BoundCoreNBT），不需要在玩家背包中查找。
     */
    private void handleTrainingDataOnEntity(Player player, TDollEntity tdoll, ItemStack trainingData) {
        if (player.level().isClientSide) return;
        if (!tdoll.isBound()) {
            player.sendSystemMessage(Component.literal("§c该人形尚未绑定核心！"));
            return;
        }

        CompoundTag savedNBT = tdoll.getPersistentData().getCompound("BoundCoreNBT");
        if (savedNBT.isEmpty()) {
            player.sendSystemMessage(Component.literal("§c该人形的核心数据异常！"));
            return;
        }

        ItemStack coreStack = ItemStack.of(savedNBT);
        if (!(coreStack.getItem() instanceof TDollCoreItem)) {
            player.sendSystemMessage(Component.literal("§c该人形的核心数据异常！"));
            return;
        }

        boolean leveledUp = TDollCoreItem.addXP(coreStack, TrainingDataItem.XP_PER_TRAINING);
        // 保存回实体 PersistentData
        tdoll.getPersistentData().put("BoundCoreNBT", coreStack.serializeNBT());
        // 同步核心阶段到实体（绑定后阶段可能通过心智升级改变）
        tdoll.setCoreStage(TDollCoreItem.getCoreStage(coreStack));
        // 重新计算人形属性（等级倍率可能变化）
        tdoll.recalculateCoreAttributes();

        trainingData.shrink(1);

        int currentLevel = TDollCoreItem.getCoreLevel(coreStack);
        int maxLevel = TDollCoreItem.getCoreMaxLevel(coreStack);
        double currentXP = TDollCoreItem.getCoreXP(coreStack);
        double needed = TDollCoreItem.getXPForNextLevel(currentLevel);
        String suffix;
        if (currentLevel >= maxLevel) {
            suffix = " §a已满级！";
        } else if (leveledUp) {
            int pending = TDollCoreItem.getPendingBreakthrough(coreStack);
            if (pending > 0) {
                suffix = " §e⬆ 升级！当前等级: " + currentLevel + "/" + maxLevel
                        + " §c⚠ 已到达 " + pending + " 级突破点！请手持或副手放一个同型号材料核心右键人形完成突破。";
            } else {
                suffix = " §e⬆ 升级！当前等级: " + currentLevel + "/" + maxLevel;
            }
        } else {
            suffix = " §7当前等级: " + currentLevel + "/" + maxLevel + " 经验: " + String.format("%.0f", currentXP) + "/" + String.format("%.0f", needed);
        }
        player.sendSystemMessage(Component.literal("§a人形训练成功！" + suffix));
        LOGGER.info("[TrainingData] Trained TDoll entity core, leveledUp={}, level={}/{}", leveledUp, currentLevel, maxLevel);
    }

    /**
     * 打开人形物品栏 GUI
     */
    private void openTDollInventory(ServerPlayer player, TDollEntity tdoll) {
        TDollCoreConfig.TDollCoreEntry core = tdoll.getBoundCore();
        String title = core != null ? core.nameZh + " - 人形物品栏" : "人形素体 - 物品栏";

        LOGGER.info("[TDollInventory] === Opening inventory GUI ===");
        LOGGER.info("[TDollInventory] Player: {}", player.getName().getString());
        LOGGER.info("[TDollInventory] TDoll UUID: {}, EntityId: {}", tdoll.getUUID(), tdoll.getId());
        LOGGER.info("[TDollInventory] Bound: {}, Core: {}", tdoll.isBound(), core != null ? core.nameZh : "none");
        LOGGER.info("[TDollInventory] Gun: {}, Ammo: {}",
                tdoll.getGun().getDisplayName().getString(),
                tdoll.getAmmo().getDisplayName().getString());
        LOGGER.info("[TDollInventory] Title: {}", title);
        
        NetworkHooks.openScreen(player, new net.minecraft.world.MenuProvider() {
            @Override
            public net.minecraft.world.inventory.AbstractContainerMenu createMenu(int windowId, net.minecraft.world.entity.player.Inventory inventory, Player p) {
                return new com.besson.tutorial.menu.TDollInventoryMenu(windowId, inventory, tdoll.getId());
            }

            @Override
            public Component getDisplayName() {
                return Component.literal(title);
            }
        }, buf -> {
            buf.writeInt(tdoll.getId());
        });
    }

}