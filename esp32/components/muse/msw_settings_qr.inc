/*
 * museStopWatch: the QR pages of Settings. Included by muse_settings_ui.c so
 * it can use that file's page helpers (page, row, button, note, show, ...);
 * kept apart to make merging upstream easy.
 *
 *   SETTINGS > QR       : "AP Mode QR", "Web QR", AP on/off state
 *   QR > AP Mode QR     : turns the AP on and shows a Wi-Fi join QR
 *   QR > Web QR         : shows the Web page's address as a QR
 *
 * Menu words stay English like the rest of Settings; explanations are Japanese.
 */
#include "msw_net.h"

#if !LV_USE_QRCODE
#error "museStopWatch needs CONFIG_LV_USE_QRCODE (devices/sdkconfig.musestopwatch)"
#endif

#define MSW_QR_PX 210

static lv_obj_t *s_qr, *s_qr_show;
static lv_obj_t *s_qr_ap_state, *s_qr_ap_off_btn;
static lv_obj_t *s_qr_code, *s_qr_text;
static bool s_qr_show_ap;            /* which QR s_qr_show holds */
static char s_qr_shown[128];          /* its data, to redraw only on a change */

/* Montserrat 16 for ASCII, the CJK caption font for Japanese. */
static const lv_font_t *msw_jp_font(void)
{
    return msw_font_cjk(&lv_font_montserrat_16);
}

/* A wrapped, centered Japanese note. Unlike note(), the text is not passed
 * through muse_text_showable(): it is our own, and its CJK must stay. */
static lv_obj_t *msw_jp_note(lv_obj_t *list, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(list);
    lv_obj_set_style_text_font(l, msw_jp_font(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(l, 4, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    return l;
}

/* ---------- QR display page ---------- */

static void msw_qr_set(const char *data)
{
    if (strcmp(s_qr_shown, data) == 0) {
        return;
    }
    strlcpy(s_qr_shown, data, sizeof(s_qr_shown));
    if (data[0]) {
        lv_obj_remove_flag(s_qr_code, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(s_qr_code, data, strlen(data));
    } else {
        lv_obj_add_flag(s_qr_code, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Escapes ; , : \ " in a WIFI: QR field. */
static void msw_wifi_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if (strchr("\\;,:\"", *in)) {
            out[o++] = '\\';
        }
        out[o++] = *in;
    }
    out[o] = '\0';
}

static void msw_qr_fill(void)
{
    char text[400];
    if (s_qr_show_ap) {
        char ssid[MSW_AP_SSID_MAX + 1], pass[MSW_AP_PASS_MAX + 1];
        char essid[2 * MSW_AP_SSID_MAX + 1], epass[2 * MSW_AP_PASS_MAX + 1];
        msw_ap_credentials(ssid, pass);
        msw_wifi_escape(ssid, essid, sizeof(essid));
        msw_wifi_escape(pass, epass, sizeof(epass));
        char data[128];
        snprintf(data, sizeof(data), "WIFI:T:WPA;S:%s;P:%s;;", essid, epass);
        if (msw_ap_on()) {
            msw_qr_set(data);
            int n = msw_ap_clients();
            snprintf(text, sizeof(text),
                     "スマホのカメラで読み取ると、本体の Wi-Fi に接続します。\n"
                     "名前: %s\nパスワード: %s\n"
                     "接続したら Web QR から設定ページを開けます。%s",
                     ssid, pass, n > 0 ? "\n（スマホが接続中）" : "");
        } else {
            msw_qr_set("");
            snprintf(text, sizeof(text), "AP モードを開始できませんでした。");
        }
        memset(pass, 0, sizeof(pass));
        memset(epass, 0, sizeof(epass));
        memset(data, 0, sizeof(data));
    } else {
        /* The QR carries the access key; the text shows the plain address. */
        char sta[MSW_URL_MAX], ap[MSW_URL_MAX], keyed[MSW_URL_MAX];
        msw_web_sta_url(sta);
        msw_web_ap_url(ap);
        if (sta[0]) {
            msw_web_key_url(sta, keyed);
            msw_qr_set(keyed);
            snprintf(text, sizeof(text),
                     "本体と同じ Wi-Fi のスマホで読み取ると、設定ページが開きます。\n%s%s%s",
                     sta, ap[0] ? "\nAP モードで接続中のスマホは: " : "", ap);
        } else if (ap[0]) {
            msw_web_key_url(ap, keyed);
            msw_qr_set(keyed);
            snprintf(text, sizeof(text),
                     "AP モードで本体に接続したスマホで読み取ると、設定ページが開きます。\n%s", ap);
        } else {
            msw_qr_set("");
            snprintf(text, sizeof(text),
                     "Wi-Fi に接続していません。\n先に Wi-Fi に接続するか、AP Mode QR を使ってください。");
        }
        if (!msw_web_running()) {
            strlcat(text, "\n（Web サーバーを起動できませんでした）", sizeof(text));
        }
    }
    if (strcmp(lv_label_get_text(s_qr_text), text) != 0) {
        lv_label_set_text(s_qr_text, text);
    }
}

static void msw_build_qr_show(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_qr_show = page(tile, s_qr_show_ap ? "AP MODE" : "WEB", true, &list);
    s_qr_shown[0] = '\0';
    s_qr_code = lv_qrcode_create(list);
    lv_qrcode_set_size(s_qr_code, MSW_QR_PX);
    lv_qrcode_set_dark_color(s_qr_code, lv_color_black());
    lv_qrcode_set_light_color(s_qr_code, lv_color_white());
    lv_qrcode_set_quiet_zone(s_qr_code, true);
    lv_obj_add_flag(s_qr_code, LV_OBJ_FLAG_HIDDEN);
    s_qr_text = msw_jp_note(list, COLOR_TEXT, "");
}

static void msw_open_qr(bool ap)
{
    /* One display page, rebuilt for the QR asked for. */
    if (s_qr_show && s_qr_show_ap != ap) {
        lv_obj_delete(s_qr_show);
        s_qr_show = NULL;
    }
    s_qr_show_ap = ap;
    if (!s_qr_show) {
        msw_build_qr_show(s_tile);
    }
    show(s_qr_show);
    msw_qr_fill();
}

static void msw_on_ap_qr(lv_event_t *e)
{
    (void)e;
    msw_ap_start();
    msw_open_qr(true);
}

static void msw_on_web_qr(lv_event_t *e)
{
    (void)e;
    msw_web_start();
    msw_open_qr(false);
}

static void msw_on_ap_off(lv_event_t *e)
{
    (void)e;
    msw_ap_stop();
}

/* ---------- QR list page ---------- */

static void msw_build_qr_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_qr = page(tile, "QR", true, &list);
    row(list, LV_SYMBOL_WIFI, "AP Mode QR", NULL, msw_on_ap_qr, NULL);
    row(list, LV_SYMBOL_HOME, "Web QR", NULL, msw_on_web_qr, NULL);
    s_qr_ap_state = msw_jp_note(list, COLOR_DIM, "");
    button(list, "AP Mode off", COLOR_DANGER, msw_on_ap_off, NULL);
    s_qr_ap_off_btn = lv_obj_get_child(list, lv_obj_get_child_count(list) - 1);
    msw_jp_note(list, COLOR_DIM,
                "AP Mode QR: 本体が Wi-Fi の親機になり、スマホを直接つなぐ QR を出します。\n"
                "Web QR: スマホのブラウザで設定ページを開く QR を出します。");
}

static const page_t MSW_QR = { &s_qr, msw_build_qr_page };

static void msw_tick_qr(void)
{
    if (msw_tick_side()) {
        return;
    }
    if (s_current == s_qr_show && s_qr_show) {
        msw_qr_fill();
        return;
    }
    if (s_current != s_qr || !s_qr) {
        return;
    }
    char text[96];
    if (msw_ap_on()) {
        int n = msw_ap_clients();
        snprintf(text, sizeof(text), "AP モード: 動作中（接続 %d 台）", n);
        lv_obj_remove_flag(s_qr_ap_off_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        strlcpy(text, "AP モード: 停止中", sizeof(text));
        lv_obj_add_flag(s_qr_ap_off_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (strcmp(lv_label_get_text(s_qr_ap_state), text) != 0) {
        lv_label_set_text(s_qr_ap_state, text);
    }
}

/* drop() hook: forget our pages when Settings deletes them. */
static void msw_forget_page(lv_obj_t *p)
{
    msw_side_forget(p);
    if (p == s_wifi) {
        msw_saved_reset();
    }
    if (p == s_qr) {
        s_qr = NULL;
    }
    if (p == s_qr_show) {
        s_qr_show = NULL;
        s_qr_shown[0] = '\0';
    }
}

/* Back from the QR display goes to the QR list, not home; back from the list
 * goes home (go_back) and takes the hidden display page with it. */
static bool msw_go_back(void)
{
    if (msw_side_back()) {
        return true;
    }
    if (s_current == s_qr_show && s_qr_show) {
        if (!s_qr) {
            msw_build_qr_page(s_tile);
        }
        show(s_qr);
        msw_tick_qr();
        return true;
    }
    if (s_current == s_qr && s_qr_show) {
        lv_obj_delete_async(s_qr_show);
        s_qr_show = NULL;
        s_qr_shown[0] = '\0';
    }
    return false;
}
