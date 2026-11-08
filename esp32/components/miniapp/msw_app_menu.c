/*
 * museStopWatch mini apps: the app menu (msw_app.h, MSW_BUTTON_YELLOW).
 *
 * The yellow rim button pauses the running app and opens this menu over it:
 * the app's name, "つづける" (go on) and "おわる" (quit). It is lvgl on the top
 * layer, so with frame drawing only the menu's box is drawn over the last
 * frame; msw_app_core.c sends the frame again when the menu closes. An app
 * that calls msw_menu_own(true) gets the yellow button instead.
 *
 * Open and close run on the app task with the display locked; the button
 * events run on the lvgl task and only set s_choice.
 */
#include <stdatomic.h>
#include <string.h>

#include "msw_app_priv.h"

#define BOX_W 300
#define BOX_H 230
#define BTN_W 220
#define BTN_H 60

static lv_obj_t *s_box;
static atomic_int s_choice;   /* MSW_MENU_NONE, _GO_ON or _QUIT */

static void on_click(lv_event_t *e)
{
    atomic_store(&s_choice, (int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, uint32_t color, int y, int choice)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_set_style_radius(b, BTN_H / 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, (void *)(intptr_t)choice);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, msw_font(0), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x101010), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

bool msw_menu_is_open(void)
{
    return s_box != NULL;
}

void msw_menu_open(const char *title)
{
    if (s_box) {
        return;
    }
    atomic_store(&s_choice, MSW_MENU_NONE);
    s_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, BOX_W, BOX_H);
    lv_obj_set_style_radius(s_box, 28, 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x1c2430), 0);
    lv_obj_set_style_border_width(s_box, 3, 0);
    lv_obj_set_style_border_color(s_box, lv_color_hex(0xffc107), 0);   /* the yellow button's colour */
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_CLICKABLE);   /* taps on the box don't reach the app */
    lv_obj_center(s_box);
    lv_obj_t *t = lv_label_create(s_box);
    lv_obj_set_style_text_font(t, msw_font(0), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, BOX_W - 40);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(t, title && title[0] ? title : "アプリ");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);
    add_button(s_box, "つづける", 0x81c784, 64, MSW_MENU_GO_ON);
    add_button(s_box, "おわる", 0xef9a9a, 142, MSW_MENU_QUIT);
}

void msw_menu_close(void)
{
    if (s_box) {
        lv_obj_delete(s_box);
        s_box = NULL;
    }
    atomic_store(&s_choice, MSW_MENU_NONE);
}

int msw_menu_choice(void)
{
    return atomic_exchange(&s_choice, MSW_MENU_NONE);
}
