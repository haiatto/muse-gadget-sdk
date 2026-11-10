/*
 * museStopWatch: an LVGL font whose glyph bitmaps sit in a flash partition
 * (msw_font), mapped in place on first use (muse_font_part.c). The firmware
 * runs from PSRAM, so bitmaps in the image would take that much PSRAM.
 *
 * A generated font (tools/muse/split_cjk_noto_jp_font.py) defines
 * MSW_FONT_BITMAP_SIZE and MSW_FONT_BITMAP_HASH, includes this, puts
 * MSW_FONT_PART_DEFINE(font_dsc) before its lv_font_t, and uses
 * msw_font_get_glyph_dsc as the font's get_glyph_dsc.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bitmaps after the partition's header, or NULL when the partition is
 * missing or holds other bitmaps (wrong size or hash). Tried once; later
 * calls return the same. Any task may call it. */
const uint8_t *muse_font_part_map(uint32_t size, uint32_t hash);

#define MSW_FONT_PART_DEFINE(dsc)                                                              \
    static bool msw_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,       \
                                       uint32_t letter, uint32_t next)                         \
    {                                                                                          \
        if (!(dsc).glyph_bitmap) {                                                             \
            (dsc).glyph_bitmap = muse_font_part_map(MSW_FONT_BITMAP_SIZE, MSW_FONT_BITMAP_HASH); \
            if (!(dsc).glyph_bitmap) {                                                         \
                return false;                                                                  \
            }                                                                                  \
        }                                                                                      \
        return lv_font_get_glyph_dsc_fmt_txt(font, out, letter, next);                         \
    }

#ifdef __cplusplus
}
#endif
