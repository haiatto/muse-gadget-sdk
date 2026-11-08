# museStopWatch: the device's own screens and services on top of the muse
# component: SoftAP and the Web settings page (msw_net), the QR pages, side
# chats, Japanese labels (msw_text), the memory monitor and msw.* console
# (msw_mem), the stopwatch.doc guide (msw_doc). Built where muse_settings_ui.c is (Muse with the touch UI, not
# FreeInk); muse_settings_ui.c also includes the three .inc pages from here.
#
# muse and this component need each other (muse_settings_ui.c calls in here,
# these files use muse's headers). WHOLE_ARCHIVE keeps every object, so the
# link finds them whatever the order.
set(srcs)
set(embed)
set(embed_txt)
if(CONFIG_MUSE_ENABLED AND NOT CONFIG_MUSE_UI_FREEINK)
    set(srcs "msw_net.c" "msw_text.c" "msw_side.c" "msw_mem.c" "msw_doc.c")
    set(embed "msw_web.html")
    set(embed_txt "../docs/stopwatch_guide.md")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_FILES ${embed}
    EMBED_TXTFILES ${embed_txt}
    WHOLE_ARCHIVE
    REQUIRES muse
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif nvs_flash esp_timer esp_pm
)
