package com.besson.tutorial;

import com.mojang.blaze3d.platform.InputConstants;
import net.minecraft.client.KeyMapping;
import net.minecraftforge.client.event.InputEvent;
import net.minecraftforge.client.event.RegisterKeyMappingsEvent;
import net.minecraftforge.eventbus.api.IEventBus;
import net.minecraftforge.eventbus.api.SubscribeEvent;
import org.lwjgl.glfw.GLFW;

import javax.swing.text.JTextComponent;


public class CustomKey {
    public static final KeyMapping MY_P_KEY = new KeyMapping(
            "key.my_mod.p_key",  // 语言键（需在 lang 文件中本地化）
            InputConstants.Type.KEYSYM,  // 使用键盘符号（推荐）
            GLFW.GLFW_KEY_P,      // 默认绑定 P 键
            "key.categories.my_mod"  // 自定义分类（需在 lang 文件中定义）
    );
    public static final KeyMapping HK416_SKILL_KEY = new KeyMapping(
            "key.girls_frontline.hk416_skill",
            InputConstants.Type.KEYSYM,
            GLFW.GLFW_KEY_Q,
            "key.categories.my_mod"
    );
    @SubscribeEvent
    public static void registerKeys(RegisterKeyMappingsEvent event) {
        event.register(MY_P_KEY);  // 注册到游戏
        event.register(HK416_SKILL_KEY);
    }

}


