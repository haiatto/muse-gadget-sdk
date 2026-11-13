/*
 * museStopWatch mini apps: JPEG decoding (msw_app.h, msw_jpeg_size and
 * msw_jpeg_decode), 2026-11-14.
 *
 * It uses TJpgDec in the ESP32-S3's ROM (rom/tjpgd.h), as display.draw_url
 * does (main/image_fetch.c), so it adds next to nothing to the firmware: the
 * esp_new_jpeg decoder was about 50 KB more. TJpgDec gives RGB888 blocks
 * (MCUs); each goes straight into the app's msw_gfx_t, clipped.
 */
#include <stdlib.h>
#include <string.h>

#include "rom/tjpgd.h"

#include "msw_app_priv.h"

#define POOL_BYTES 3100      /* TJpgDec's work area: as image_fetch.c */
#define MAX_SIDE 4096

typedef struct {
    const uint8_t *src;
    size_t len, at;
    msw_gfx_t *g;
    int x, y;
} jpg_t;

static UINT jpg_in(JDEC *jd, BYTE *buf, UINT len)
{
    jpg_t *j = jd->device;
    size_t n = j->len - j->at < len ? j->len - j->at : len;
    if (buf) {
        memcpy(buf, j->src + j->at, n);
    }
    j->at += n;
    return (UINT)n;
}

static UINT jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_t *j = jd->device;
    msw_gfx_t *g = j->g;
    int w = rect->right - rect->left + 1;
    const uint8_t *rgb = bitmap;
    for (int row = rect->top; row <= rect->bottom; row++) {
        int gy = j->y + row;
        for (int col = 0; col < w; col++, rgb += 3) {
            int gx = j->x + rect->left + col;
            if (gx >= 0 && gx < g->w && gy >= 0 && gy < g->h) {
                g->px[(size_t)gy * g->stride + gx] =
                    (uint16_t)(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
            }
        }
    }
    return 1;
}

/* Reads the header; *pool is freed by the caller. */
static bool prepare(JDEC *jd, jpg_t *j, void **pool, const void *jpg, int len, const char *fn)
{
    *pool = NULL;
    if (len <= 0 || !msw_api_readable(jpg, (size_t)len, fn)) {
        return false;
    }
    j->src = jpg;
    j->len = (size_t)len;
    j->at = 0;
    *pool = malloc(POOL_BYTES);
    return *pool && jd_prepare(jd, jpg_in, *pool, POOL_BYTES, j) == JDR_OK && jd->width && jd->height
           && jd->width <= MAX_SIDE && jd->height <= MAX_SIDE;
}

bool msw_jpeg_size(const void *jpg, int len, int *w, int *h)
{
    JDEC jd;
    jpg_t j = { 0 };
    void *pool;
    bool ok = prepare(&jd, &j, &pool, jpg, len, "msw_jpeg_size");
    free(pool);
    if (!ok) {
        return false;
    }
    if ((w = msw_api_writable(w, sizeof(*w), "msw_jpeg_size"))) {
        *w = jd.width;
    }
    if ((h = msw_api_writable(h, sizeof(*h), "msw_jpeg_size"))) {
        *h = jd.height;
    }
    return true;
}

bool msw_jpeg_decode(const void *jpg, int len, msw_gfx_t *g, int x, int y, int shrink)
{
    const char *fn = "msw_jpeg_decode";
    if (shrink < 0 || shrink > 3) {
        msw_app_logf("%s: shrink is 0 (whole), 1 (1/2), 2 (1/4) or 3 (1/8)", fn);
        return false;
    }
    if (!msw_api_readable(g, sizeof(*g), fn) || g->w <= 0 || g->h <= 0 || g->stride < g->w
        || !msw_api_writable(g->px, (size_t)g->stride * g->h * 2, fn)) {
        return false;
    }
    JDEC jd;
    jpg_t j = { .g = g, .x = x, .y = y };
    void *pool;
    bool ok = prepare(&jd, &j, &pool, jpg, len, fn) && jd_decomp(&jd, jpg_out, (uint8_t)shrink) == JDR_OK;
    free(pool);
    return ok;
}
