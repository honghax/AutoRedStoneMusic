package com.besson.tutorial;

import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.effect.MobEffects;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.UseAnim;
import net.minecraft.world.level.Level;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.Level;


    public class NightVisionItem extends Item {
        public NightVisionItem(Properties properties) {
            super(properties);
        }
        @Override
        public void inventoryTick(ItemStack stack, Level level, Entity entity, int slotId, boolean isSelected) {
            if (entity instanceof Player player && !level.isClientSide) {

                if (player.getMainHandItem() == stack || player.getOffhandItem() == stack) {

                    player.addEffect(new MobEffectInstance(MobEffects.NIGHT_VISION, 400, 0, true, true, true));
                } else {

                    player.removeEffect(MobEffects.NIGHT_VISION);
                }
            }
        }

        @Override
        public boolean isFoil(ItemStack stack) {
            return true;
        }

        @Override
        public boolean isFireResistant() {
            return true;
        }
    }

