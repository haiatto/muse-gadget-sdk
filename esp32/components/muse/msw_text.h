/*
 * museStopWatch: Japanese and other non-ASCII text in the settings screens.
 *
 * Upstream folds every non-ASCII character of a settings label to an ASCII
 * stand-in (muse_text_showable), so Japanese notes and SSIDs can't show.
 * With the CJK font built in, these keep each character the label's font
 * (with the CJK font as its fallback) can draw, and fold only the rest.
 */
#pragma once

#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* font, or a copy of it that falls back to the CJK font. */
const lv_font_t *msw_font_cjk(const lv_font_t *font);

/* text, or a copy in buf with what font can't draw folded to ASCII. */
const char *msw_text_showable(const lv_font_t *font, const char *text, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
