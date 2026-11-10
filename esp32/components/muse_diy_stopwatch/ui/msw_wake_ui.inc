/*
 * museStopWatch: Settings > Wake word. Included by muse_settings_ui.c after
 * msw_settings_qr.inc (its page helpers and msw_jp_note).
 *
 * When the wake word listens (msw_wake.h): Off (the default), Screen on, or
 * Screen on + USB. The wake word component is reached through weak
 * references, so this page only shows when it is built in.
 */
extern int msw_wake_mode(void) __attribute__((weak));
extern void msw_wake_set_mode(int mode) __attribute__((weak));
extern const char *msw_wake_word(void) __attribute__((weak));
extern void msw_wake_init(void) __attribute__((weak));

#define MSW_WAKE_CHOICES 3
static const char *const MSW_WAKE_NAMES[MSW_WAKE_CHOICES] = { "Off", "Screen on", "Screen on + USB" };
static const char *const MSW_WAKE_SHORT[MSW_WAKE_CHOICES] = { "Off", "Screen", "Screen+USB" };

static lv_obj_t *s_wake, *s_home_wake;
static lv_obj_t *s_wake_checks[MSW_WAKE_CHOICES];

static bool msw_wake_built_in(void)
{
    return msw_wake_mode && msw_wake_set_mode && msw_wake_word;
}

static void msw_tick_wake(void)
{
    int cur = msw_wake_mode();
    for (int i = 0; i < MSW_WAKE_CHOICES; i++) {
        set_text(s_wake_checks[i], i == cur ? LV_SYMBOL_OK : "");
    }
}

static void msw_on_wake_choice(lv_event_t *e)
{
    msw_wake_set_mode((int)(intptr_t)lv_event_get_user_data(e));
    msw_tick_wake();
}

static void msw_build_wake_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    char text[160];
    s_wake = page(tile, "WAKE WORD", true, &list);
    snprintf(text, sizeof(text), "「%s」と言うと、黄ボタンを押したときのように話し始めます。", msw_wake_word());
    msw_jp_note(list, COLOR_DIM, text);
    for (int i = 0; i < MSW_WAKE_CHOICES; i++) {
        row(list, NULL, MSW_WAKE_NAMES[i], &s_wake_checks[i], msw_on_wake_choice, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_wake_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    msw_jp_note(list, COLOR_DIM,
                "Screen on: 画面がついている間だけ聞きます。\n"
                "Screen on + USB: USB につないでいる間は画面が消えていても聞きます。\n"
                "電池で画面が消えている間は聞きません。話し終わって少し黙ると Muse に送ります。");
    msw_tick_wake();
}

static const page_t MSW_WAKE = { &s_wake, msw_build_wake_page };

static void msw_tick_home_wake(void)
{
    if (s_home_wake) {
        int m = msw_wake_mode();
        set_text(s_home_wake, m >= 0 && m < MSW_WAKE_CHOICES ? MSW_WAKE_SHORT[m] : "");
    }
}
