# Tachi Badge 是二次开发应用：固件只编译它自己的模块。
#
# 基线的 demo_*.c / ui_pixel.c 仍留在树里（它们是板级硬件的参考实现，也是上游
# 的分支共同点），但不参与本应用——界面是重新设计的，不是那个测试菜单的外壳。
idf_component_register(
    SRCS "main.c"
         "badge_json.c"
         "badge_proto.c"
         "badge_link.c"
         "badge_ble.c"
         "badge_state.c"
         "badge_power.c"
         "badge_sound.c"
         "badge_ui.c"
         "badge_font_16.c"
         "badge_font_icon_16.c"
         "badge_sound_ask_16k.c"
         "badge_sound_done_16k.c"
         "badge_avatar.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_driver_usb_serial_jtag esp_timer lvgl nvs_flash bt
)
