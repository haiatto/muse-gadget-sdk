# museStopWatch mini apps: small ELF programs loaded into PSRAM and run
# (msw_app_priv.h lists the files). Only the StopWatch builds them; other
# boards register an empty component. lvgl, cJSON and elf_loader come from
# idf_component.yml. Requirements can't depend on Kconfig,
# so they are listed for every board.
set(srcs)
set(embed)
if(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH)
    set(srcs "msw_app_core.c" "msw_app_api.c" "msw_app_gfx.c" "msw_app_sound.c"
             "msw_app_launcher.c" "msw_app_cmd.c" "msw_app_menu.c" "msw_app_exports.c"
             "msw_app_crash.c" "msw_app_shot.c" "msw_avatar.c" "msw_app_mic.c" "msw_app_port.c"
             "msw_app_pkgs.c")
    set(embed "guide.md" "avatar_guide.md" "include/msw_app.h")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    EMBED_TXTFILES ${embed}
    PRIV_REQUIRES muse ui nvs_flash esp_driver_i2c fatfs wear_levelling esp_partition esp_timer mbedtls vfs esp_hw_support esp_app_format
)

if(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH)
    # The firmware reaches these through weak references (muse_ui.c, muse_input.c,
    # main/app.c); make the linker take them from this archive.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-u msw_launcher_build" "-u msw_app_button" "-u msw_app_blue" "-u msw_app_command"
        "-u msw_app_install_json" "-u msw_app_muse_reply"
        "-u msw_app_install_json_begin" "-u msw_app_install_json_write" "-u msw_app_install_json_end"
        # avatar/muse_pixel.c and muse_ui.c (idea 009: avatars)
        "-u msw_avatar_hook_render" "-u msw_avatar_hook_accent" "-u msw_avatar_build" "-u msw_mic_tap"
        # The crash report (msw_app_crash.c) sees each panic before IDF's handler.
        "-Wl,--wrap=esp_panic_handler")
    # Apps call into lvgl and libc by address: keep every exported symbol.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-format-truncation)
    # The table declares every symbol bare, as void f(void): only addresses are taken.
    set_source_files_properties(msw_app_exports.c PROPERTIES COMPILE_OPTIONS
        "-fno-builtin;-Wno-builtin-declaration-mismatch")
endif()
