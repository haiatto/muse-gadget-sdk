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
             "msw_app_pkgs.c" "msw_app_nn.c" "msw_app_jpeg.c")
    # The guide tells Muse which SDK to clone. A release firmware (version.txt
    # not 999.0.0) points it at the tag of its own version, v<version>, so its
    # SDK stays the one it was built with; a development build keeps main
    # (2026-11-13). The copy in the build folder is the one embedded.
    idf_build_get_property(msw_ver PROJECT_VER)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/guide.md" msw_guide)
    set(msw_clone_re "git clone --depth 1 (https://github\\.com/[^ `]+)")
    if(NOT msw_guide MATCHES "${msw_clone_re}")
        message(FATAL_ERROR "miniapp/guide.md: no `git clone --depth 1 https://github.com/...` line")
    endif()
    if(NOT msw_ver STREQUAL "999.0.0")
        string(REGEX REPLACE "${msw_clone_re}" "git clone --depth 1 --branch v${msw_ver} \\1"
               msw_guide "${msw_guide}")
    endif()
    # CONFIGURE: written only when it changed, with the source's LF line ends
    # (file(WRITE) gives CRLF on Windows). guide.md has no @ signs for @ONLY.
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/guide.md" CONTENT "${msw_guide}"
         @ONLY NEWLINE_STYLE UNIX)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_LIST_DIR}/guide.md" "${CMAKE_SOURCE_DIR}/version.txt")
    set(embed "${CMAKE_CURRENT_BINARY_DIR}/guide.md" "avatar_guide.md" "include/msw_app.h")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    EMBED_TXTFILES ${embed}
    PRIV_REQUIRES muse ui nvs_flash esp_driver_i2c esp_driver_uart fatfs wear_levelling esp_partition esp_timer mbedtls vfs esp_hw_support esp_app_format
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
