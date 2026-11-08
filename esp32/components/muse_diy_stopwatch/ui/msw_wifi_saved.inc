/*
 * museStopWatch: the saved networks in Settings > Wi-Fi. Included by
 * muse_settings_ui.c (before its saved list) to use its helpers.
 *
 *   tap                      switch to that network (its saved password)
 *   swipe left / long press  show a red x on the row; swipe right hides it
 *   tap the x                ask, then forget the network
 *
 * Upstream forgot a network on two taps; that's gone. The dialog and the x
 * come from msw_side_ui.inc.
 */

static lv_obj_t *s_msw_cards[MUSE_WIFI_SAVED_MAX];
static lv_obj_t *s_msw_x[MUSE_WIFI_SAVED_MAX];
static int s_msw_shown_x = -1;
static char s_msw_ask_ssid[MUSE_SSID_MAX + 1];

static void msw_show_x(int i)
{
    for (int j = 0; j < s_saved_n; j++) {
        if (!s_msw_x[j]) {
            continue;
        }
        if (j == i) {
            lv_obj_remove_flag(s_msw_x[j], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_saved_vals[j], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_msw_x[j], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_saved_vals[j], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_msw_shown_x = i;
}

static void msw_ask_close(void)
{
    msw_dialog_close();
    s_msw_ask_ssid[0] = '\0';
}

static void msw_on_ask_no(lv_event_t *e)
{
    (void)e;
    msw_ask_close();
    msw_show_x(-1);
}

static void msw_on_ask_yes(lv_event_t *e)
{
    if (s_msw_ask_ssid[0]) {
        muse_wifi_forget(s_msw_ask_ssid);
    }
    msw_on_ask_no(e);
}

static void msw_ask_forget(int i)
{
    strlcpy(s_msw_ask_ssid, s_saved[i].ssid, sizeof(s_msw_ask_ssid));
    char text[MUSE_SSID_MAX + 96];
    snprintf(text, sizeof(text), "「%s」を削除しますか？\n保存したパスワードも消えます。", s_saved[i].ssid);
    msw_dialog(text, "削除", msw_on_ask_yes, msw_on_ask_no);
}

static void msw_on_x(lv_event_t *e)
{
    msw_ask_forget((int)(intptr_t)lv_event_get_user_data(e));
}

static void msw_on_saved(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (i >= s_saved_n) {
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        lv_indev_wait_release(lv_indev_active());
        msw_show_x(i);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_msw_shown_x >= 0) {
            msw_show_x(-1);   /* a tap elsewhere puts the x away */
        } else {
            muse_wifi_choose(s_saved[i].ssid);
        }
    }
}

/* A saved network's row, in place of row(). */
static void msw_saved_row(lv_obj_t *list, int i, const char *ssid)
{
    lv_obj_t *c = row(list, LV_SYMBOL_WIFI, ssid, &s_saved_vals[i], msw_on_saved, (void *)(intptr_t)i);
    lv_obj_remove_event_cb(c, msw_on_saved);   /* row() set it for CLICKED */
    lv_obj_add_event_cb(c, msw_on_saved, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(c, msw_on_saved, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    s_msw_cards[i] = c;

    lv_obj_t *x = msw_x_button(c, msw_on_x, (void *)(intptr_t)i);
    s_msw_x[i] = x;
}

/* Called first by the page's swipe handler: true if it took the swipe. */
static bool msw_saved_gesture(void)
{
    if (msw_side_gesture()) {   /* Settings > Muse > Side chat */
        return true;
    }
    if (s_current != s_wifi || !s_wifi) {
        return false;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT && s_msw_shown_x >= 0) {
        lv_indev_wait_release(lv_indev_active());
        msw_show_x(-1);
        return true;
    }
    if (dir != LV_DIR_LEFT) {
        return false;
    }
    for (lv_obj_t *o = lv_indev_get_active_obj(); o; o = lv_obj_get_parent(o)) {
        for (int i = 0; i < s_saved_n; i++) {
            if (o == s_msw_cards[i]) {
                lv_indev_wait_release(lv_indev_active());
                msw_show_x(i);
                return true;
            }
        }
    }
    return false;
}

/* The list was rebuilt or the page dropped. */
static void msw_saved_reset(void)
{
    memset(s_msw_cards, 0, sizeof(s_msw_cards));
    memset(s_msw_x, 0, sizeof(s_msw_x));
    s_msw_shown_x = -1;
    msw_ask_close();
}
