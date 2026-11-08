/*
 * museStopWatch: Settings > Muse > Side chat, and the confirm dialog and red x
 * that it shares with the saved networks (msw_wifi_saved.inc). Included by
 * muse_settings_ui.c to use its page helpers.
 *
 *   Main chat                 tap: talk in the main chat
 *   Side 1, Side 2, ...       tap: talk in that side chat
 *                             swipe left / long press: red x, then ask and forget
 *   + New side chat           make one and switch to it
 *
 * Back goes to the Muse page. The list lives in msw_side.c.
 */
#include "msw_side.h"

static void build_hatch_page(lv_obj_t *tile);
static void tick_hatch(void);

/* ---------- confirm dialog ---------- */

static lv_obj_t *s_msw_dialog;   /* on the top layer */

static void msw_dialog_close(void)
{
    if (s_msw_dialog) {
        lv_obj_delete_async(s_msw_dialog);
        s_msw_dialog = NULL;
    }
}

/* text, a red yes button and Cancel. A tap outside is Cancel. */
static void msw_dialog(const char *text, const char *yes, lv_event_cb_t on_yes, lv_event_cb_t on_no)
{
    msw_dialog_close();
    lv_obj_t *dim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);   /* nothing under it takes taps */
    lv_obj_add_event_cb(dim, on_no, LV_EVENT_CLICKED, NULL);
    s_msw_dialog = dim;

    lv_obj_t *box = lv_obj_create(dim);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, muse_board->width * 70 / 100, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x241d40), 0);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   /* a tap on it isn't a tap on dim */

    lv_obj_t *l = label(box, &lv_font_montserrat_16, COLOR_TEXT, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    button(box, yes, COLOR_DANGER, on_yes, NULL);
    button(box, "キャンセル", COLOR_TEXT, on_no, NULL);
}

/* A hidden red x at the right of a row card. */
static lv_obj_t *msw_x_button(lv_obj_t *card, lv_event_cb_t cb, void *user)
{
    lv_obj_t *x = lv_button_create(card);
    lv_obj_remove_style_all(x);
    lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(x, 40, 40);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(COLOR_DANGER), 0);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_white(), 0);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, cb, LV_EVENT_CLICKED, user);
    lv_obj_add_flag(x, LV_OBJ_FLAG_HIDDEN);
    return x;
}

/* ---------- Side chat page ---------- */

static lv_obj_t *s_side, *s_side_rows, *s_side_main_val, *s_side_new_btn, *s_side_state;
static lv_obj_t *s_side_cards[MSW_SIDE_MAX], *s_side_vals[MSW_SIDE_MAX], *s_side_x[MSW_SIDE_MAX];
static msw_side_t s_side_shown[MSW_SIDE_MAX];   /* as listed */
static int s_side_n = -1;                        /* -1 before the first fill */
static int s_side_shown_x = -1;
static char s_side_ask_id[MSW_SIDE_ID_LEN + 1];
static lv_obj_t *s_hatch_side;                   /* its row on the Muse page */

static void msw_side_show_x(int i)
{
    for (int j = 0; j < s_side_n; j++) {
        lv_obj_set_flag(s_side_x[j], LV_OBJ_FLAG_HIDDEN, j != i);
        lv_obj_set_flag(s_side_vals[j], LV_OBJ_FLAG_HIDDEN, j == i);
    }
    s_side_shown_x = i;
}

static void msw_side_ask_no(lv_event_t *e)
{
    (void)e;
    msw_dialog_close();
    s_side_ask_id[0] = '\0';
    if (s_side) {
        msw_side_show_x(-1);
    }
}

static void msw_side_ask_yes(lv_event_t *e)
{
    /* By ID: the Web page may have changed the list meanwhile. */
    int i = msw_side_find(s_side_ask_id);
    if (i >= 0) {
        msw_side_delete(i);
    }
    msw_side_ask_no(e);
}

static void msw_side_on_x(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_side_n) {
        return;
    }
    strlcpy(s_side_ask_id, s_side_shown[i].id, sizeof(s_side_ask_id));
    char text[96];
    snprintf(text, sizeof(text), "「%s」を一覧から削除しますか？\nMuse 側の会話は残ります。", s_side_shown[i].name);
    msw_dialog(text, "削除", msw_side_ask_yes, msw_side_ask_no);
}

static bool msw_tick_side(void);

static void msw_side_on_main(lv_event_t *e)
{
    (void)e;
    if (s_side_shown_x >= 0) {
        msw_side_show_x(-1);   /* a tap elsewhere puts the x away */
        return;
    }
    msw_side_select(-1);
    msw_tick_side();
}

static void msw_side_on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_side_n) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        lv_indev_wait_release(lv_indev_active());
        msw_side_show_x(i);
    } else if (s_side_shown_x >= 0) {
        msw_side_show_x(-1);
    } else {
        int at = msw_side_find(s_side_shown[i].id);
        if (at >= 0) {
            msw_side_select(at);
        }
        msw_tick_side();
    }
}

static const char *s_side_said = "";   /* the last Add's trouble, shown under the button */

static void msw_side_on_id_done(const char *text)
{
    int i = msw_side_add(text);
    s_side_said = i == MSW_SIDE_BAD_ID ? "セッション ID の形式が違います。\n8-4-4-4-12 の 36 文字です。"
                  : i < 0              ? "追加できませんでした。"
                                       : "";
}

static void msw_side_on_new(lv_event_t *e)
{
    (void)e;
    msw_side_show_x(-1);
    s_side_said = "";
    open_text("Session ID", "", false, MSW_SIDE_ID_LEN, "空欄で新規作成", msw_side_on_id_done, s_side);
}

static void msw_side_rebuild(void)
{
    msw_side_t now[MSW_SIDE_MAX];
    int n = 0;
    for (int c = msw_side_count(); n < c && n < MSW_SIDE_MAX && msw_side_get(n, &now[n]); n++) {
    }
    if (n == s_side_n && !memcmp(now, s_side_shown, n * sizeof(now[0]))) {
        return;
    }
    memcpy(s_side_shown, now, n * sizeof(now[0]));
    s_side_n = n;
    s_side_shown_x = -1;
    lv_obj_clean(s_side_rows);
    for (int i = 0; i < n; i++) {
        lv_obj_t *c = row(s_side_rows, LV_SYMBOL_LIST, now[i].name, &s_side_vals[i], msw_side_on_row, (void *)(intptr_t)i);
        lv_obj_remove_event_cb(c, msw_side_on_row);   /* row() set it for CLICKED */
        lv_obj_add_event_cb(c, msw_side_on_row, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(c, msw_side_on_row, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        s_side_cards[i] = c;
        s_side_x[i] = msw_x_button(c, msw_side_on_x, (void *)(intptr_t)i);
    }
}

static void msw_build_side_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_side = page(tile, "SIDE CHAT", true, &list);
    s_side_n = -1;
    s_side_shown_x = -1;
    row(list, LV_SYMBOL_HOME, "Main chat", &s_side_main_val, msw_side_on_main, NULL);
    s_side_rows = column(list);
    s_side_new_btn = button(list, LV_SYMBOL_PLUS "  Add side chat", COLOR_ACCENT, msw_side_on_new, NULL);
    s_side_state = note(list, "");
    note(list, "サイドチャットは、メインとは別の会話です。選んだ会話に話しかけ、返事もその会話から来ます。"
               "Add side chat で ID を空欄のまま決定すると新しく作り、最初に話しかけたときに Muse 側で始まります。"
               "今ある会話に参加するには、そのサイドチャットで Muse に「このチャットの ID を教えて」と聞くと教えてくれます。"
               "その ID を入れてください（Web 設定からも入れられます）。"
               "エージェント実行の ID ではなく、チャットの ID です。"
               "一覧はこの本体だけが覚えています（8 件まで）。"
               "左にスワイプか長押しで一覧から削除できます。Muse 側の会話は消えません。");
}

static const page_t MSW_SIDE = { &s_side, msw_build_side_page };

/* True when the Side chat page is on screen (and was filled in). */
static bool msw_tick_side(void)
{
    if (s_current != s_side || !s_side) {
        return false;
    }
    msw_side_rebuild();
    int cur = msw_side_current();
    set_text(s_side_main_val, cur < 0 ? LV_SYMBOL_OK : "");
    lv_obj_set_style_text_color(s_side_main_val, lv_color_hex(COLOR_ACCENT), 0);
    for (int i = 0; i < s_side_n; i++) {
        char shown[12];
        snprintf(shown, sizeof(shown), "%.8s", s_side_shown[i].id);
        bool on = i == cur;   /* the rebuild above made the list match msw_side's */
        set_text(s_side_vals[i], on ? LV_SYMBOL_OK : shown);
        lv_obj_set_style_text_color(s_side_vals[i], lv_color_hex(on ? COLOR_ACCENT : COLOR_DIM), 0);
    }
    bool full = s_side_n >= MSW_SIDE_MAX;
    lv_obj_set_flag(s_side_new_btn, LV_OBJ_FLAG_HIDDEN, full);
    set_text(s_side_state, s_side_said[0] ? s_side_said : full ? "8 件までです。使わないものを削除してください。" : "");
    lv_obj_set_style_text_color(s_side_state, lv_color_hex(s_side_said[0] ? COLOR_WARN : COLOR_DIM), 0);
    return true;
}

/* The value of the Side chat row on the Muse page. */
static void msw_tick_hatch_side(void)
{
    msw_side_t sc;
    int cur = msw_side_current();
    set_text(s_hatch_side, cur >= 0 && msw_side_get(cur, &sc) ? sc.name : "Main");
}

/* Swipes on the page: true if one was taken. */
static bool msw_side_gesture(void)
{
    if (s_current != s_side || !s_side) {
        return false;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT && s_side_shown_x >= 0) {
        lv_indev_wait_release(lv_indev_active());
        msw_side_show_x(-1);
        return true;
    }
    if (dir != LV_DIR_LEFT) {
        return false;
    }
    for (lv_obj_t *o = lv_indev_get_active_obj(); o; o = lv_obj_get_parent(o)) {
        for (int i = 0; i < s_side_n; i++) {
            if (o == s_side_cards[i]) {
                lv_indev_wait_release(lv_indev_active());
                msw_side_show_x(i);
                return true;
            }
        }
    }
    return false;
}

/* Back from Side chat goes to the Muse page and drops this one. */
static bool msw_side_back(void)
{
    if (s_current != s_side || !s_side) {
        return false;
    }
    if (!s_hatch) {
        build_hatch_page(s_tile);
    }
    lv_obj_t *p = s_side;
    show(s_hatch);
    drop(p);
    tick_hatch();
    return true;
}

/* drop() hook. Leaving Settings from here drops the Muse page under it too. */
static void msw_side_forget(lv_obj_t *p)
{
    if (p == s_side) {
        s_side = NULL;
        s_side_n = -1;
        s_side_shown_x = -1;
        memset(s_side_cards, 0, sizeof(s_side_cards));
        msw_dialog_close();
        if (s_hatch && s_hatch != s_current) {
            drop(s_hatch);
        }
    }
    if (p == s_hatch) {
        s_hatch_side = NULL;
    }
}
