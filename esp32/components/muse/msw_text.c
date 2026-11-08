/*
 * museStopWatch: non-ASCII text in the settings screens (msw_text.h).
 * Runs in the LVGL task only.
 */
#include "msw_text.h"

#include <string.h>

#include "sdkconfig.h"
#include "muse_text.h"

#if CONFIG_MUSE_CJK_FONT_NOTO_JP
LV_FONT_DECLARE(muse_font_cjk_noto_jp_16)
#define CJK_FONT (&muse_font_cjk_noto_jp_16)
#elif CONFIG_MUSE_CJK_FONT_NOTO
LV_FONT_DECLARE(muse_font_cjk_noto_16)
#define CJK_FONT (&muse_font_cjk_noto_16)
#elif CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#define CJK_FONT (&muse_font_cjk_16)
#endif

#define CACHE_MAX 8

const lv_font_t *msw_font_cjk(const lv_font_t *font)
{
#ifdef CJK_FONT
    static const lv_font_t *base[CACHE_MAX];
    static lv_font_t copy[CACHE_MAX];
    if (!font || font->fallback) {
        return font;   /* already has one (Vietnamese menus, or one of ours) */
    }
    for (int i = 0; i < CACHE_MAX; i++) {
        if (base[i] == font) {
            return &copy[i];
        }
        if (!base[i]) {
            copy[i] = *font;
            copy[i].fallback = CJK_FONT;
            base[i] = font;
            return &copy[i];
        }
    }
#endif
    return font;
}

/* The code point at s, and its length in *len (1 for a broken byte). */
static uint32_t utf8(const unsigned char *s, size_t *len)
{
    if (s[0] < 0x80) {
        *len = 1;
        return s[0];
    }
    int n = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    uint32_t cp = n == 2 ? s[0] & 0x1F : n == 3 ? s[0] & 0x0F : s[0] & 0x07;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            n = 0;
            break;
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (!n) {
        *len = 1;
        return 0xFFFD;
    }
    *len = (size_t)n;
    return cp;
}

static bool can_draw(const lv_font_t *font, uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0) && !dsc.is_placeholder;
}

const char *msw_text_showable(const lv_font_t *font, const char *text, char *buf, size_t cap)
{
    const unsigned char *p = (const unsigned char *)text;
    bool fold = false;
    while (*p) {
        size_t len;
        uint32_t cp = utf8(p, &len);
        if (cp >= 0x80 && !can_draw(font, cp)) {
            fold = true;
            break;
        }
        p += len;
    }
    if (!fold || cap == 0) {
        return text;
    }
    /* Keep what the font has; fold each other character on its own. */
    size_t o = 0;
    for (p = (const unsigned char *)text; *p && o + 1 < cap;) {
        size_t len;
        uint32_t cp = utf8(p, &len);
        if (cp < 0x80 || can_draw(font, cp)) {
            if (o + len >= cap) {
                break;
            }
            memcpy(buf + o, p, len);
            o += len;
        } else {
            char one[16];
            memcpy(one, p, len);
            one[len] = '\0';
            muse_text_to_ascii(one, sizeof(one));
            size_t n = strlen(one);
            if (o + n >= cap) {
                break;
            }
            memcpy(buf + o, one, n);
            o += n;
        }
        p += len;
    }
    buf[o] = '\0';
    return buf;
}
