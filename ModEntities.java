package com.besson.tutorial;

import com.besson.tutorial.Entity.TDollEntity;
import com.besson.tutorial.Entity.TripMineEntity;
import com.besson.tutorial.Entity.HostileTDollEntity;
import com.besson.tutorial.TutorialMod;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.MobCategory;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.registries.DeferredRegister;
import net.minecraftforge.registries.ForgeRegistries;
import net.minecraftforge.registries.RegistryObject;

public class ModEntities {
    public static final DeferredRegister<EntityType<?>> ENTITY_TYPES =
            DeferredRegister.create(ForgeRegistries.ENTITY_TYPES, TutorialMod.MOD_ID);

    public static final RegistryObject<EntityType<TDollEntity>> T_DOLL_ENTITY =
            ENTITY_TYPES.register("t_doll_entity",
                    () -> EntityType.Builder.of(TDollEntity::new, MobCategory.MISC)
                            .sized(0.6f, 1.8f)
                            .clientTrackingRange(10)
                            .updateInterval(2)
                            .build(new ResourceLocation(TutorialMod.MOD_ID, "t_doll_entity").toString())
            );

    public static final RegistryObject<EntityType<HostileTDollEntity>> HOSTILE_T_DOLL_ENTITY =
            ENTITY_TYPES.register("hostile_t_doll_entity",
                    () -> EntityType.Builder.of(HostileTDollEntity::new, MobCategory.MONSTER)
                            .sized(0.6f, 1.8f)
                            .clientTrackingRange(10)
                            .updateInterval(2)
                            .build(new ResourceLocation(TutorialMod.MOD_ID, "hostile_t_doll_entity").toString())
            );

    public static final RegistryObject<EntityType<TripMineEntity>> TRIP_MINE_ENTITY =
            ENTITY_TYPES.register("trip_mine",
                    () -> EntityType.Builder.of(TripMineEntity::new, MobCategory.MISC)
                            .sized(0.5f, 0.5f)
                            .clientTrackingRange(10)
                            .updateInterval(20)
                            .build(new ResourceLocation(TutorialMod.MOD_ID, "trip_mine").toString())
            );

    public static void register(IEventBus eventBus) {
        ENTITY_TYPES.register(eventBus);
    }
}