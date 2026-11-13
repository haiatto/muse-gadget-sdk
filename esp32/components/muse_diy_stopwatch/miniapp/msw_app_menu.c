/*
 * museStopWatch mini apps: the app menu (msw_app.h, MSW_BUTTON_YELLOW).
 *
 * The yellow rim button pauses the running app and opens this menu over it:
 * the app's name, "つづける" (go on) and "おわる" (quit). It is lvgl on the top
 * layer, so with frame drawing only the menu's box is drawn over the last
 * frame; msw_app_core.c sends the frame again when the menu closes. An app
 * that calls msw_menu_own(true) gets the yellow button instead.
 *
 * While it is open the blue button talks to Muse (msw_app_core.c), and the
 * box holds a small Muse (the face screen's avatar, muse_pixel) and a speech
 * bubble. Muse is drawn as the face screen draws it: no picture of it is
 * kept, an lvgl image decoder scales Muse's 64 x 64 cells a strip at a time
 * while lvgl draws (muse_pixel_scale_px). Muse moves as on the face screen
 * (listening, thinking, speaking) and the bubble shows its caption, the reply
 * as it is said. All of it is inside the box, which is opaque: with frame
 * drawing lvgl must not redraw any place outside it.
 *
 * The same box is the start screen of an app that uses PORT.A
 * (msw_menu_open_start), over the whole screen: what the app says to plug
 * in and what it does with it, "はじめる" and "やめる". The app starts only after "はじめる" tapped while the blue
 * button is held, so a stray tap does not start it.
 *
 * Open, close and msw_menu_poll run on the app task with the display locked;
 * the button events run on the lvgl task and only set s_choice.
 */
#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "src/draw/lv_image_decoder_private.h"   /* the small Muse's decoder */
#include "muse_link.h"
#include "muse_pixel.h"
#include "muse_state.h"
#include "muse_text.h"
#include "msw_app_priv.h"

#define BOX_W 320
#define BOX_H 272
#define BTN_W 220
#define BTN_H 48
#define TOP 46           /* Muse and the bubble, under the title */
#define MUSE_MINI 96     /* Muse, left */
#define BUB_X (14 + MUSE_MINI + 14)
#define BUB_W (BOX_W - BUB_X - 14)
#define FRAME_MS 40

static lv_obj_t *s_box;
static atomic_int s_choice;   /* MSW_MENU_NONE, _GO_ON or _QUIT */
static lv_obj_t *s_muse, *s_text;
static lv_image_dsc_t s_muse_img;   /* no pixels: the decoder below draws it */
static lv_image_decoder_t *s_dec;
#define STRIP_ROWS 16
#define STRIPS LV_DRAW_SW_DRAW_UNIT_CNT
static lv_draw_buf_t *s_strips[STRIPS];   /* only while the menu is open */
static bool s_strip_busy[STRIPS];
static int64_t s_muse_at;     /* when Muse was last rendered, us */
static float s_level;
static uint32_t s_caption_ver;
static bool s_said;           /* the bubble holds Muse's words, not the hint */
static bool s_said_activity;  /* and they are Muse's status (idea 008) */
static int s_hint_key;        /* which hint the bubble shows; -1 none */
static EXT_RAM_BSS_ATTR char s_caption[MUSE_CAPTION_MAX];

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
    lv_obj_set_ext_click_area(b, 5);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, (void *)(intptr_t)choice);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, msw_font(0), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x101010), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *plain(lv_obj_t *parent, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* The decoder, as muse_ui.c's: each draw unit takes its own strip. */
static lv_result_t dec_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src != &s_muse_img) {
        return LV_RESULT_INVALID;
    }
    *header = s_muse_img.header;
    return LV_RESULT_OK;
}

static lv_result_t dec_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] && !s_strip_busy[i]) {
            s_strip_busy[i] = true;
            dsc->user_data = s_strips[i];
            return LV_RESULT_OK;
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t dec_get_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                                const lv_area_t *full, lv_area_t *area)
{
    (void)dec;
    int32_t y1 = area->y1 == LV_COORD_MIN ? full->y1 : area->y2 + 1;
    if (y1 > full->y2) {
        return LV_RESULT_INVALID;
    }
    int32_t y2 = LV_MIN(y1 + STRIP_ROWS - 1, full->y2);
    lv_draw_buf_t *buf = lv_draw_buf_reshape(dsc->user_data, LV_COLOR_FORMAT_RGB565,
                                             lv_area_get_width(full), y2 - y1 + 1, LV_STRIDE_AUTO);
    if (!buf) {
        return LV_RESULT_INVALID;
    }
    muse_pixel_scale_px(MUSE_MINI, (uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t),
                        full->x1, full->x2, y1, y2);
    area->x1 = full->x1;
    area->x2 = full->x2;
    area->y1 = y1;
    area->y2 = y2;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

static void dec_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] == dsc->user_data) {
            s_strip_busy[i] = false;
        }
    }
}

/* Muse as on the face screen. That screen stops drawing while an app runs,
 * so the menu renders the pose itself (on lvgl's lock, as the face screen
 * does) and has lvgl draw Muse again. */
static void muse_draw(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_muse || now - s_muse_at < FRAME_MS * 1000) {
        return;
    }
    s_muse_at = now;
    float mode_t;
    muse_mode_t mode = muse_state_mode(&mode_t);
    float level = muse_state_level();
    s_level += (level - s_level) * (level > s_level ? 0.6f : 0.2f);
    muse_pose_t pose = {
        .mode = mode,
        .t = (float)now / 1e6f,
        .mode_t = mode_t,
        .level = s_level,
        .happy = muse_state_happiness(),
    };
    muse_pixel_render(&pose);
    lv_obj_invalidate(s_muse);
}

static void make_muse(void)
{
    if (!s_dec) {
        s_dec = lv_image_decoder_create();
        if (!s_dec) {
            return;   /* the bubble alone */
        }
        lv_image_decoder_set_info_cb(s_dec, dec_info);
        lv_image_decoder_set_open_cb(s_dec, dec_open);
        lv_image_decoder_set_get_area_cb(s_dec, dec_get_area);
        lv_image_decoder_set_close_cb(s_dec, dec_close);
        s_dec->name = "msw_muse";
    }
    for (int i = 0; i < STRIPS; i++) {
        s_strips[i] = lv_draw_buf_create(MUSE_MINI, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        s_strip_busy[i] = false;
    }
    s_muse_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muse_img.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muse_img.header.w = MUSE_MINI;
    s_muse_img.header.h = MUSE_MINI;
    s_muse_img.header.stride = MUSE_MINI * sizeof(uint16_t);
    s_muse_img.data = (const uint8_t *)&s_muse_img;   /* lvgl skips sources without data */
    s_muse = lv_image_create(s_box);
    lv_image_set_src(s_muse, &s_muse_img);
    lv_obj_align(s_muse, LV_ALIGN_TOP_LEFT, 14, TOP);
    s_muse_at = 0;
    muse_draw();
}

static void free_strips(void)
{
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i]) {
            lv_draw_buf_destroy(s_strips[i]);
            s_strips[i] = NULL;
        }
    }
}

static void make_bubble(void)
{
    lv_obj_t *b = plain(s_box, BUB_W, MUSE_MINI, 0xffffff);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, BUB_X, TOP);
    lv_obj_t *tip = plain(s_box, 14, 14, 0xffffff);   /* the bubble's point, toward Muse */
    lv_obj_set_style_radius(tip, 3, 0);
    lv_obj_align(tip, LV_ALIGN_TOP_LEFT, BUB_X - 8, TOP + MUSE_MINI / 2 - 7);
    s_text = lv_label_create(b);
    lv_obj_set_style_text_font(s_text, msw_font(0), 0);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_MODE_WRAP);   /* Muse pages a long reply */
    lv_obj_set_size(s_text, BUB_W - 20, MUSE_MINI - 14);
    lv_obj_center(s_text);
    lv_label_set_text(s_text, "");
}

/* Muse's status captions are English capitals ("LISTENING 1.2s", "SENDING
 * VOICE NOTE"); the words of a reply have small letters or are not Latin. */
static bool is_status(const char *s)
{
    bool caps = false;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80 || (c >= 'a' && c <= 'z')) {
            return false;
        }
        caps |= c >= 'A' && c <= 'Z';
    }
    return caps;
}

static void bubble_show(const char *text, bool said)
{
    lv_obj_set_style_text_color(s_text, lv_color_hex(said ? 0x101010 : 0x707880), 0);
    lv_label_set_text(s_text, text);
}

void msw_menu_poll(void)
{
    if (!s_box) {
        return;
    }
    bool online = muse_link_state() == MUSE_LINK_ONLINE;
    muse_mode_t mode = muse_state_mode(NULL);
    muse_draw();

    bool changed = muse_state_caption(s_caption, sizeof(s_caption), &s_caption_ver);
    if (changed && !s_caption[0] && s_said_activity) {
        s_said = false;   /* Muse's status went (it is done): back to the hint */
        s_said_activity = false;
    }
    if (changed && s_caption[0]) {
        /* Muse's status (idea 008), in a turn or between turns: it goes when
         * Muse is done, so the bubble goes back to the hint then. */
        static char act[MUSE_ACTIVITY_MAX];
        uint32_t v = UINT32_MAX;
        muse_state_activity(act, sizeof(act), &v);
        muse_text_to_caption(act, sizeof(act));   /* as the caption has it */
        s_said_activity = act[0] && !strcmp(act, s_caption);
        const char *say = s_caption;   /* an error or a hint stays as Muse says it */
        if (is_status(s_caption) && mode == MUSE_MODE_LISTENING) {
            say = "きいてるよ…";
        } else if (is_status(s_caption) && mode == MUSE_MODE_THINKING) {
            say = "かんがえちゅう…";
        }
        bubble_show(say, true);
        s_said = true;
        s_hint_key = -1;
    } else if (!s_said && s_hint_key != online) {
        s_hint_key = online;
        bubble_show(online ? "青ボタンを おして\nはなしてね" : "Muse に つながっていません", false);
    }
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
    /* A new object is first laid out at (0, 0) and then centred; lvgl would
     * join both places into one dirty area from the top-left corner and, with
     * frame drawing, paint it black (the app's lvgl screen) over the frame.
     * Build it unseen, then mark only the box's own place. */
    lv_display_t *d = lv_display_get_default();
    lv_display_enable_invalidation(d, false);
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
    lv_obj_set_width(t, BOX_W - 48);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(t, title && title[0] ? title : "アプリ");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    make_muse();
    make_bubble();
    add_button(s_box, "つづける", 0x81c784, TOP + MUSE_MINI + 10, MSW_MENU_GO_ON);
    add_button(s_box, "おわる", 0xef9a9a, TOP + MUSE_MINI + 10 + BTN_H + 10, MSW_MENU_QUIT);
    /* An old caption is not news: the hint stays until Muse says something.
     * But Muse's status now (working between turns) is: it shows. */
    muse_state_caption(s_caption, sizeof(s_caption), &s_caption_ver);
    s_said = false;
    s_said_activity = false;
    s_hint_key = -1;
    static char act[MUSE_ACTIVITY_MAX];
    uint32_t v = UINT32_MAX;
    muse_state_activity(act, sizeof(act), &v);
    muse_text_to_caption(act, sizeof(act));
    if (act[0] && !strcmp(act, s_caption)) {
        s_caption_ver--;   /* seen as new by the poll below */
    }
    msw_menu_poll();
    lv_obj_update_layout(s_box);
    lv_display_enable_invalidation(d, true);
    lv_obj_invalidate(s_box);
}

/* The start screen fills the round screen (466 x 466): the app has drawn
 * nothing yet, so there is no frame to keep. Each line is placed where the
 * circle is wide enough for it. */
#define SCR 466
#define START_BTN_Y 338
#define START_BTN_W 150
static lv_obj_t *s_start_hint;   /* "青ボタンを押しながら…" */
static lv_obj_t *s_start_go;     /* "はじめる": grey until the blue button is held */
#define GO_OFF 0x5f6670
#define GO_ON 0x81c784

/* The blue button went down (true) or up: "はじめる" turns green or grey. */
void msw_menu_start_ready(bool on)
{
    if (s_start_go) {
        lv_obj_set_style_bg_color(s_start_go, lv_color_hex(on ? GO_ON : GO_OFF), 0);
    }
}

/* "はじめる" was tapped without the blue button: the hint turns red. */
void msw_menu_start_nudge(void)
{
    if (s_start_hint) {
        lv_obj_set_style_text_color(s_start_hint, lv_color_hex(0xff5252), 0);
    }
}

static lv_obj_t *start_text(const char *text, uint32_t color, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(s_box);
    lv_obj_set_style_text_font(l, msw_font(0), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (w) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, w);
        if (h) {
            lv_obj_set_height(l, h);
        }
    }
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

void msw_menu_open_start(const char *title, const char *connect, const char *about)
{
    if (s_box) {
        return;
    }
    atomic_store(&s_choice, MSW_MENU_NONE);
    s_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, SCR, SCR);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x1c2430), 0);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_CLICKABLE);   /* taps don't reach what is under it */
    lv_obj_center(s_box);
    start_text(title && title[0] ? title : "アプリ", 0xffffff, 52, 300, 0);
    start_text("PORT.A（Grove）につなぐもの", 0xffd54f, 88, 0, 0);
    start_text(connect && connect[0] ? connect : "（このアプリは、書いていません）", 0xffffff, 112, 340, 46);
    start_text("このアプリがすること", 0xffd54f, 168, 0, 0);
    start_text(about && about[0] ? about : "（このアプリは、書いていません）", 0xe0e0e0, 192, 390, 96);
    s_start_hint = start_text("青ボタンを押しながら「はじめる」", 0x4fc3f7, 300, 0, 0);
    lv_obj_t *quit = add_button(s_box, "やめる", 0xef9a9a, START_BTN_Y, MSW_MENU_QUIT);
    lv_obj_set_width(quit, START_BTN_W);
    lv_obj_align(quit, LV_ALIGN_TOP_MID, -(START_BTN_W / 2 + 8), START_BTN_Y);
    lv_obj_t *go = add_button(s_box, "はじめる", GO_OFF, START_BTN_Y, MSW_MENU_GO_ON);
    s_start_go = go;
    lv_obj_set_width(go, START_BTN_W);
    lv_obj_align(go, LV_ALIGN_TOP_MID, START_BTN_W / 2 + 8, START_BTN_Y);
}

void msw_menu_close(void)
{
    if (s_box) {
        lv_obj_delete(s_box);
        s_box = NULL;
    }
    s_muse = s_text = NULL;   /* children of the box */
    s_start_hint = NULL;
    s_start_go = NULL;
    free_strips();   /* the box and its image are gone: nothing draws Muse now */
    atomic_store(&s_choice, MSW_MENU_NONE);
}

int msw_menu_choice(void)
{
    return atomic_exchange(&s_choice, MSW_MENU_NONE);
}
