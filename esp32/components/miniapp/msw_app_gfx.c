/*
 * museStopWatch mini apps: frame drawing, sprites and text (msw_app.h).
 *
 * The frame is a 466 x 466 RGB565 buffer in PSRAM, in the CPU's byte order.
 * msw_fb_present() sends it straight to the panel with the board's
 * draw_frame (muse_lcd_bands_frame), bypassing lvgl: the fill callback swaps
 * each row to the panel's byte order and lays Muse's avatar and bubble over
 * it (msw_app_api.c). lvgl keeps running with the app's screen black and
 * idle, so it has nothing to draw, and touch still works through it.
 *
 * Text is drawn from the firmware's lvgl fonts directly (4 bits a pixel,
 * uncompressed: CONFIG_LV_USE_FONT_COMPRESSED is off).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "muse_board.h"
#include "msw_app_priv.h"

void *msw_app_malloc(size_t n);
void msw_app_free(void *p);
void *msw_app_calloc(size_t n, size_t size);

static msw_gfx_t *s_fb;
static int64_t s_present_us, s_present_px;
static uint32_t s_presents;

bool msw_gfx_frame_mode(void)
{
    return s_fb != NULL;
}

void msw_gfx_begin(lv_obj_t *screen)
{
    (void)screen;
    s_fb = NULL;
}

void msw_gfx_end(void)
{
    if (s_presents) {
        msw_app_logf("frame: %u presents, %.1f ms each on average, %.2f us a pixel",
                     (unsigned)s_presents, s_present_us / 1000.0 / s_presents,
                     s_present_px ? (double)s_present_us / s_present_px : 0.0);
    }
    s_presents = 0;
    s_present_us = s_present_px = 0;
    if (s_fb) {
        msw_gfx_free(s_fb);
        s_fb = NULL;
    }
}

void msw_gfx_overlay_changed(void)
{
    /* Drawn at the next present. */
}

msw_gfx_t *msw_gfx_new(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        return NULL;
    }
    msw_gfx_t *g = msw_app_malloc(sizeof(*g) + (size_t)w * h * 2 + 16);
    if (!g) {
        return NULL;
    }
    g->px = (uint16_t *)(((uintptr_t)(g + 1) + 15) & ~(uintptr_t)15);
    g->w = w;
    g->h = h;
    g->stride = w;
    memset(g->px, 0, (size_t)w * h * 2);
    return g;
}

void msw_gfx_free(msw_gfx_t *g)
{
    if (g == s_fb) {
        s_fb = NULL;
    }
    msw_app_free(g);
}

msw_gfx_t *msw_fb(void)
{
    if (!s_fb) {
        s_fb = msw_gfx_new(MSW_SCREEN_W, MSW_SCREEN_H);
        if (!s_fb) {
            msw_log("msw_fb: out of memory");
        }
    }
    return s_fb;
}

/* ---- present -------------------------------------------------------------- */

typedef struct {
    int x1, y1, x2;
    bool overlay;
} present_t;

static void fill_rows(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    const present_t *p = arg;
    int w = p->x2 - p->x1;
    for (int r = 0; r < rows; r++) {
        int y = p->y1 + row + r;
        uint16_t *out = (uint16_t *)(dst + (size_t)r * stride);
        const uint16_t *src = s_fb->px + (size_t)y * s_fb->stride + p->x1;
        if (p->overlay) {
            memcpy(out, src, (size_t)w * 2);
            msw_overlay_draw(out, y, p->x1, p->x2);
            for (int x = 0; x < w; x++) {
                out[x] = (uint16_t)((out[x] >> 8) | (out[x] << 8));
            }
        } else {
            for (int x = 0; x < w; x++) {
                out[x] = (uint16_t)((src[x] >> 8) | (src[x] << 8));
            }
        }
    }
}

void msw_fb_present_rect(int x, int y, int w, int h)
{
    if (!s_fb || !muse_board->draw_frame) {
        return;
    }
    int x1 = x < 0 ? 0 : x & ~1;
    int y1 = y < 0 ? 0 : y & ~1;
    int x2 = x + w > MSW_SCREEN_W ? MSW_SCREEN_W : (x + w + 1) & ~1;
    int y2 = y + h > MSW_SCREEN_H ? MSW_SCREEN_H : (y + h + 1) & ~1;
    if (x2 > MSW_SCREEN_W) {
        x2 = MSW_SCREEN_W;
    }
    if (y2 > MSW_SCREEN_H) {
        y2 = MSW_SCREEN_H;
    }
    if (x2 <= x1 || y2 <= y1) {
        return;
    }
    present_t p = { x1, y1, x2, msw_overlay_active() };
    int64_t t0 = esp_timer_get_time();
    muse_board->draw_frame(x1, y1, x2, y2, 8, fill_rows, &p);
    s_present_us += esp_timer_get_time() - t0;
    s_present_px += (int64_t)(x2 - x1) * (y2 - y1);
    s_presents++;
}

void msw_fb_present(void)
{
    msw_fb_present_rect(0, 0, MSW_SCREEN_W, MSW_SCREEN_H);
}

/* ---- primitives ------------------------------------------------------------ */

/* n pixels of one colour from p, two at a time where aligned (PSRAM writes
 * are the cost of most drawing). */
static inline void span16(uint16_t *p, int n, uint16_t color)
{
    if (n <= 0) {
        return;
    }
    if ((uintptr_t)p & 2) {
        *p++ = color;
        n--;
    }
    uint32_t two = ((uint32_t)color << 16) | color;
    uint32_t *q = (uint32_t *)p;
    for (int i = 0; i < n / 2; i++) {
        q[i] = two;
    }
    if (n & 1) {
        p[n - 1] = color;
    }
}

void msw_gfx_fill(msw_gfx_t *g, uint16_t color)
{
    if (!g) {
        return;
    }
    if (g->stride == g->w) {
        span16(g->px, g->w * g->h, color);
        return;
    }
    for (int y = 0; y < g->h; y++) {
        span16(g->px + (size_t)y * g->stride, g->w, color);
    }
}

void msw_gfx_pixel(msw_gfx_t *g, int x, int y, uint16_t color)
{
    if (g && (unsigned)x < (unsigned)g->w && (unsigned)y < (unsigned)g->h) {
        g->px[(size_t)y * g->stride + x] = color;
    }
}

static inline void hspan(msw_gfx_t *g, int x1, int x2, int y, uint16_t color)   /* x2 inclusive */
{
    if ((unsigned)y >= (unsigned)g->h) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (x2 >= g->w) {
        x2 = g->w - 1;
    }
    span16(g->px + (size_t)y * g->stride + x1, x2 - x1 + 1, color);
}

void msw_gfx_fill_rect(msw_gfx_t *g, int x, int y, int w, int h, uint16_t color)
{
    if (!g) {
        return;
    }
    int y2 = y + h;
    for (int yy = y < 0 ? 0 : y; yy < y2 && yy < g->h; yy++) {
        hspan(g, x, x + w - 1, yy, color);
    }
}

void msw_gfx_rect(msw_gfx_t *g, int x, int y, int w, int h, uint16_t color)
{
    if (!g || w <= 0 || h <= 0) {
        return;
    }
    msw_gfx_fill_rect(g, x, y, w, 1, color);
    msw_gfx_fill_rect(g, x, y + h - 1, w, 1, color);
    msw_gfx_fill_rect(g, x, y, 1, h, color);
    msw_gfx_fill_rect(g, x + w - 1, y, 1, h, color);
}

void msw_gfx_line(msw_gfx_t *g, int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!g) {
        return;
    }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 4096; n++) {
        msw_gfx_pixel(g, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void msw_gfx_circle(msw_gfx_t *g, int cx, int cy, int r, uint16_t color)
{
    if (!g || r < 0) {
        return;
    }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        msw_gfx_pixel(g, cx + x, cy + y, color);
        msw_gfx_pixel(g, cx + y, cy + x, color);
        msw_gfx_pixel(g, cx - y, cy + x, color);
        msw_gfx_pixel(g, cx - x, cy + y, color);
        msw_gfx_pixel(g, cx - x, cy - y, color);
        msw_gfx_pixel(g, cx - y, cy - x, color);
        msw_gfx_pixel(g, cx + y, cy - x, color);
        msw_gfx_pixel(g, cx + x, cy - y, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void msw_gfx_fill_circle(msw_gfx_t *g, int cx, int cy, int r, uint16_t color)
{
    if (!g || r < 0) {
        return;
    }
    int dx = r;
    for (int dy = 0; dy <= r; dy++) {
        while (dx > 0 && dx * dx + dy * dy > r * r) {
            dx--;                          /* dx only shrinks as dy grows */
        }
        hspan(g, cx - dx, cx + dx, cy + dy, color);
        if (dy) {
            hspan(g, cx - dx, cx + dx, cy - dy, color);
        }
    }
}

void msw_gfx_blit(msw_gfx_t *g, int x, int y, const msw_gfx_t *src, int scale)
{
    if (!g || !src) {
        return;
    }
    scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    for (int sy = 0; sy < src->h; sy++) {
        const uint16_t *srow = src->px + (size_t)sy * src->stride;
        for (int ky = 0; ky < scale; ky++) {
            int ty = y + sy * scale + ky;
            if ((unsigned)ty >= (unsigned)g->h) {
                continue;
            }
            uint16_t *drow = g->px + (size_t)ty * g->stride;
            if (scale == 1) {
                int a = x < 0 ? -x : 0;
                int b = x + src->w > g->w ? g->w - x : src->w;
                if (b > a) {
                    memcpy(drow + x + a, srow + a, (size_t)(b - a) * 2);
                }
                continue;
            }
            for (int sx = 0; sx < src->w; sx++) {
                for (int kx = 0; kx < scale; kx++) {
                    int tx = x + sx * scale + kx;
                    if ((unsigned)tx < (unsigned)g->w) {
                        drow[tx] = srow[sx];
                    }
                }
            }
        }
    }
}

/* ---- sprites ---------------------------------------------------------------- */

#define SPRITE_KEY 0xF81Fu   /* magenta: transparent */

msw_sprite_t *msw_sprite_new(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        return NULL;
    }
    msw_sprite_t *s = msw_app_malloc(sizeof(*s) + (size_t)w * h * 2);
    if (!s) {
        return NULL;
    }
    s->w = w;
    s->h = h;
    s->px = (uint16_t *)(s + 1);
    s->key = SPRITE_KEY;
    for (int i = 0; i < w * h; i++) {
        s->px[i] = SPRITE_KEY;
    }
    return s;
}

void msw_sprite_free(msw_sprite_t *s)
{
    msw_app_free(s);
}

msw_sprite_t *msw_sprite_from_art(const char *const *rows, int nrows, const msw_pal_t *pal)
{
    if (!rows || nrows <= 0 || !rows[0]) {
        return NULL;
    }
    int w = (int)strlen(rows[0]);
    for (int y = 1; y < nrows; y++) {
        if ((int)strlen(rows[y]) != w) {
            msw_log("sprite art: row %d is %d long, row 0 is %d", y, (int)strlen(rows[y]), w);
            return NULL;
        }
    }
    msw_sprite_t *s = msw_sprite_new(w, nrows);
    if (!s) {
        return NULL;
    }
    for (int y = 0; y < nrows; y++) {
        for (int x = 0; x < w; x++) {
            char c = rows[y][x];
            if (c == '.' || c == ' ') {
                continue;
            }
            const msw_pal_t *p = pal;
            while (p && p->ch && p->ch != c) {
                p++;
            }
            if (!p || !p->ch) {
                msw_log("sprite art: '%c' (row %d) is not in the palette", c, y);
                msw_sprite_free(s);
                return NULL;
            }
            s->px[y * w + x] = p->color == SPRITE_KEY ? SPRITE_KEY - 1 : p->color;
        }
    }
    return s;
}

void msw_sprite_draw(msw_gfx_t *g, const msw_sprite_t *s, int x, int y, int scale, int flip)
{
    if (!g || !s) {
        return;
    }
    scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    for (int sy = 0; sy < s->h; sy++) {
        int ty0 = y + sy * scale;
        if (ty0 >= g->h || ty0 + scale <= 0) {
            continue;
        }
        const uint16_t *srow = s->px + (size_t)((flip & MSW_FLIP_Y) ? s->h - 1 - sy : sy) * s->w;
        for (int sx = 0; sx < s->w; sx++) {
            uint16_t c = srow[(flip & MSW_FLIP_X) ? s->w - 1 - sx : sx];
            if (c == s->key) {
                continue;
            }
            int tx = x + sx * scale, n = scale;
            if (tx >= g->w || tx + n <= 0) {
                continue;
            }
            if (tx < 0) {
                n += tx;
                tx = 0;
            }
            if (tx + n > g->w) {
                n = g->w - tx;
            }
            for (int k = 0; k < scale; k++) {
                int ty = ty0 + k;
                if ((unsigned)ty < (unsigned)g->h) {
                    uint16_t *d = g->px + (size_t)ty * g->stride + tx;
                    for (int i = 0; i < n; i++) {
                        d[i] = c;
                    }
                }
            }
        }
    }
}

/* ---- text -------------------------------------------------------------------- */

static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n > 1) {
        c &= 0x3F >> (n - 1);
        for (int i = 1; i < n; i++) {
            if ((s[i] & 0xC0) != 0x80) {
                *p += i;
                return 0xFFFD;
            }
            c = (c << 6) | (s[i] & 0x3F);
        }
    }
    *p += n;
    return c;
}

static inline uint16_t blend(uint16_t fg, uint16_t bg, unsigned a)   /* a 0..15 */
{
    if (a >= 15) {
        return fg;
    }
    unsigned r = (((fg >> 11) * a) + ((bg >> 11) * (15 - a))) / 15;
    unsigned gg = ((((fg >> 5) & 63) * a) + (((bg >> 5) & 63) * (15 - a))) / 15;
    unsigned b = (((fg & 31) * a) + ((bg & 31) * (15 - a))) / 15;
    return (uint16_t)((r << 11) | (gg << 5) | b);
}

/* Draws one glyph with its line's top at y; returns its advance. */
static int glyph(msw_gfx_t *g, const lv_font_t *font, int x, int y, uint32_t c, uint32_t next, uint16_t color)
{
    lv_font_glyph_dsc_t d;
    memset(&d, 0, sizeof(d));
    if (!lv_font_get_glyph_dsc(font, &d, c, next)) {
        return font->line_height / 3;
    }
    if (!g || d.box_w == 0 || !d.resolved_font || !d.resolved_font->get_glyph_bitmap) {
        return d.adv_w;
    }
    d.req_raw_bitmap = 1;
    const uint8_t *bmp = d.resolved_font->get_glyph_bitmap(&d, NULL);
    int bpp = d.format == LV_FONT_GLYPH_FORMAT_A1 ? 1 : d.format == LV_FONT_GLYPH_FORMAT_A2 ? 2
              : d.format == LV_FONT_GLYPH_FORMAT_A8 ? 8 : 4;
    if (!bmp) {
        return d.adv_w;
    }
    const lv_font_t *rf = d.resolved_font;
    int top = y + (font->line_height - font->base_line) - d.box_h - d.ofs_y;
    (void)rf;
    int left = x + d.ofs_x;
    uint32_t bit = 0;
    for (int r = 0; r < d.box_h; r++) {
        int py = top + r;
        for (int k = 0; k < d.box_w; k++, bit += bpp) {
            unsigned v = (bmp[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1u << bpp) - 1);
            unsigned a = bpp == 4 ? v : bpp == 8 ? v >> 4 : bpp == 2 ? v * 5 : v * 15;
            int px = left + k;
            if (!a || (unsigned)px >= (unsigned)g->w || (unsigned)py >= (unsigned)g->h) {
                continue;
            }
            uint16_t *dst = &g->px[(size_t)py * g->stride + px];
            *dst = blend(color, *dst, a);
        }
    }
    return d.adv_w;
}

int msw_gfx_text(msw_gfx_t *g, int x, int y, const char *utf8, uint16_t color, int size)
{
    if (!utf8) {
        return 0;
    }
    const lv_font_t *font = msw_font(size);
    int x0 = x;
    const char *p = utf8;
    uint32_t c = *p ? utf8_next(&p) : 0;
    while (c) {
        uint32_t next = *p ? utf8_next(&p) : 0;
        if (c == '\n') {
            break;
        }
        x += glyph(g, font, x, y, c, next, color);
        c = next;
    }
    return x - x0;
}

int msw_text_width(const char *utf8, int size)
{
    return msw_gfx_text(NULL, 0, 0, utf8, 0, size);
}

/* Japanese 16 px text wrapped to w; g NULL only measures. Returns the height. */
int msw_gfx_text_wrapped(msw_gfx_t *g, int x, int y, int w, const char *utf8, uint16_t color)
{
    const lv_font_t *font = msw_font(0);
    int lh = font->line_height + 2;
    int cx = 0, cy = 0;
    const char *p = utf8;
    uint32_t c = p && *p ? utf8_next(&p) : 0;
    while (c) {
        uint32_t next = *p ? utf8_next(&p) : 0;
        if (c == '\n') {
            cx = 0;
            cy += lh;
        } else {
            int adv = glyph(NULL, font, 0, 0, c, next, color);
            if (cx + adv > w && cx > 0) {
                cx = 0;
                cy += lh;
            }
            glyph(g, font, x + cx, y + cy, c, next, color);
            cx += adv;
        }
        c = next;
    }
    return cy + lh;
}
