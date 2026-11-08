/*
 * museStopWatch mini apps: the launcher, the tile left of Muse's face
 * (muse_ui.c builds it through the weak msw_launcher_build).
 *
 * Installed apps show as round icons, three to a row; the list scrolls.
 * Tap an icon to start the app. Hold one for its details and a red x that
 * removes it. The grid is rebuilt by an lvgl timer whenever the store
 * changes (msw_app_changes), so other tasks never touch lvgl here.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "muse_board.h"
#include "msw_text.h"
#include "msw_app_priv.h"

#define COLS 3
#define CELL 112
#define ICON_D 84

LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_20)

static lv_obj_t *s_tile, *s_grid, *s_title, *s_empty, *s_info;
static uint32_t s_seen = UINT32_MAX;
static bool s_ready;

/* Icons the grid shows: copies, since a rescan frees the store's own. */
typedef struct {
    msw_app_info_t info;
    lv_image_dsc_t img;
} item_t;
static item_t *s_items;
static int s_nitems;

static const lv_font_t *jp(void)
{
    return msw_font_cjk(&lv_font_montserrat_14);
}

static void close_info(void)
{
    if (s_info) {
        lv_obj_delete(s_info);
        s_info = NULL;
    }
}

static void on_tap(lv_event_t *e)
{
    item_t *it = lv_event_get_user_data(e);
    if (s_info) {
        return;
    }
    const char *code = NULL;
    char msg[160];
    if (!msw_app_start(it->info.id, &code, msg, sizeof(msg))) {
        lv_label_set_text_fmt(s_title, "起動できません: %s", msg);
    }
}

static void on_info_close(lv_event_t *e)
{
    (void)e;
    close_info();
}

static void on_remove(lv_event_t *e)
{
    item_t *it = lv_event_get_user_data(e);
    char id[MSW_APP_ID_MAX];
    strlcpy(id, it->info.id, sizeof(id));
    close_info();
    /* Removing goes through the flash task; lvgl stays locked meanwhile, which
     * is short (a file delete). */
    msw_app_remove(id);
}

static void on_hold(lv_event_t *e)
{
    item_t *it = lv_event_get_user_data(e);
    close_info();
    s_info = lv_obj_create(s_tile);
    lv_obj_remove_style_all(s_info);
    lv_obj_set_size(s_info, 320, 230);
    lv_obj_center(s_info);
    lv_obj_set_style_bg_color(s_info, lv_color_hex(0x202830), 0);
    lv_obj_set_style_bg_opa(s_info, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_info, 24, 0);
    lv_obj_set_style_border_width(s_info, 2, 0);
    lv_obj_set_style_border_color(s_info, lv_color_hex(0x5A6878), 0);
    lv_obj_remove_flag(s_info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_info, on_info_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(s_info);
    lv_obj_set_style_text_font(l, jp(), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_width(l, 280);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(l, "%s\nID: %s\nバージョン %u / API %u\n%u KB\n\nタップで閉じる",
                          it->info.name, it->info.id, it->info.ver, it->info.api,
                          (unsigned)((it->info.bytes + 1023) / 1024));
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 18);

    lv_obj_t *x = lv_button_create(s_info);
    lv_obj_set_size(x, 64, 64);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(0xE53935), 0);
    lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_t *xl = lv_label_create(x);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_20, 0);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, on_remove, LV_EVENT_CLICKED, it);

    lv_obj_t *dl = lv_label_create(s_info);
    lv_obj_set_style_text_font(dl, jp(), 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xFF8A80), 0);
    lv_label_set_text(dl, "削除");
    lv_obj_align_to(dl, x, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

static void free_items(void)
{
    for (int i = 0; i < s_nitems; i++) {
        heap_caps_free(s_items[i].info.icon);
    }
    heap_caps_free(s_items);
    s_items = NULL;
    s_nitems = 0;
}

static lv_obj_t *cell(lv_obj_t *parent, item_t *it, int idx)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, CELL, CELL + 8);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(c, on_tap, LV_EVENT_SHORT_CLICKED, it);
    lv_obj_add_event_cb(c, on_hold, LV_EVENT_LONG_PRESSED, it);

    static const uint32_t colors[] = { 0x3F7FBF, 0x43A047, 0xF4511E, 0x8E24AA, 0x00897B, 0xFDD835 };
    lv_obj_t *ring = lv_obj_create(c);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, ICON_D, ICON_D);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(it->info.icon ? 0x1A1F26 : colors[idx % 6]), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x4A5868), 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(0x3A4858), LV_STATE_PRESSED);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_clip_corner(ring, true, 0);
    if (it->info.icon) {
        lv_obj_t *img = lv_image_create(ring);
        lv_image_set_src(img, &it->img);
        lv_image_set_scale(img, 512);     /* 32 px icons, drawn 64 px */
        lv_obj_center(img);
    } else {
        lv_obj_t *l = lv_label_create(ring);
        lv_obj_set_style_text_font(l, msw_font_cjk(&lv_font_montserrat_20), 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        char first[5] = { 0 };
        const unsigned char *n = (const unsigned char *)it->info.name;
        int k = n[0] < 0x80 ? 1 : n[0] >= 0xF0 ? 4 : n[0] >= 0xE0 ? 3 : 2;
        memcpy(first, n, k);
        lv_label_set_text(l, first);
        lv_obj_center(l);
    }
    lv_obj_t *name = lv_label_create(c);
    lv_obj_set_width(name, CELL);
    lv_obj_set_style_text_font(name, jp(), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xD0D8E0), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, it->info.name);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, ICON_D + 4);
    return c;
}

static void rebuild(void)
{
    close_info();
    lv_obj_clean(s_grid);
    free_items();
    static msw_app_info_t list[MSW_APP_MAX_APPS];
    int n = msw_app_list(list, MSW_APP_MAX_APPS);
    s_items = n ? heap_caps_calloc(n, sizeof(item_t), MALLOC_CAP_SPIRAM) : NULL;
    if (n && !s_items) {
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        item_t *it = &s_items[i];
        it->info = list[i];
        it->info.icon = NULL;
        if (list[i].icon) {
            size_t bytes = MSW_APP_ICON * MSW_APP_ICON * 2;
            it->info.icon = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
            if (it->info.icon) {
                memcpy(it->info.icon, list[i].icon, bytes);
                it->img.header.magic = LV_IMAGE_HEADER_MAGIC;
                it->img.header.cf = LV_COLOR_FORMAT_RGB565;
                it->img.header.w = MSW_APP_ICON;
                it->img.header.h = MSW_APP_ICON;
                it->img.header.stride = MSW_APP_ICON * 2;
                it->img.data_size = bytes;
                it->img.data = (const uint8_t *)it->info.icon;
            }
        }
    }
    s_nitems = n;
    for (int i = 0; i < n; i++) {
        cell(s_grid, &s_items[i], i);
    }
    const char *running = msw_app_running();
    if (!s_ready) {
        lv_label_set_text(s_title, "アプリ");
    } else if (running) {
        lv_label_set_text_fmt(s_title, "実行中: %s", running);
    } else {
        lv_label_set_text_fmt(s_title, "アプリ  %d", n);
    }
    if (n == 0) {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_empty, s_ready ? "アプリはまだありません。\nMuse に「アプリを作って」と\n頼んでみてください。"
                                           : "準備中…");
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

static void poll(lv_timer_t *t)
{
    (void)t;
    uint32_t now = msw_app_changes();
    if (now != s_seen && s_ready) {
        s_seen = now;
        rebuild();
    }
}

static volatile uint32_t s_kicks;

void msw_launcher_refresh(void)
{
    s_kicks++;    /* msw_app_changes() moves too; the timer picks it up */
}

static void init_task(void *arg)
{
    (void)arg;
    msw_app_core_init();
    s_ready = true;
    vTaskDelete(NULL);
}

void msw_launcher_build(lv_obj_t *tile)
{
    s_tile = tile;
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(tile);
    lv_obj_set_style_text_font(s_title, msw_font_cjk(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_label_set_text(s_title, "アプリ");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 36);

    s_grid = lv_obj_create(tile);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, COLS * CELL + 12, 340);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    lv_obj_set_style_pad_column(s_grid, 4, 0);
    lv_obj_set_style_pad_bottom(s_grid, 80, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);

    s_empty = lv_label_create(tile);
    lv_obj_set_style_text_font(s_empty, jp(), 0);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(0xA0A8B0), 0);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_empty, "準備中…");
    lv_obj_center(s_empty);

    lv_timer_create(poll, 400, NULL);
    /* Mounting the apps volume can take a while (formatting on first boot),
     * so it runs apart from the UI build. */
    xTaskCreate(init_task, "msw_app_init", 4096, NULL, 2, NULL);
}
