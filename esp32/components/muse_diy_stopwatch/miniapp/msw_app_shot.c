/*
 * museStopWatch mini apps: app.screenshot, the screen as a JPEG.
 *
 * The picture is made a strip of rows at a time, as the JPEG encoder takes
 * it (esp_new_jpeg's block mode, 16 rows for 4:2:0): each strip is the
 * app's frame with Muse's avatar and bubble over it, as msw_fb_present sends
 * it, or, for lvgl apps and with no app running, lvgl draws the active
 * screen into the strip. lvgl's top layer (the app menu) is drawn over
 * either. So no picture of the whole screen is kept: a strip (15 KB, 30 KB
 * at half size) and the JPEG (JPEG_MAX) are all it needs. With an app running and Muse talking
 * PSRAM can be short (2026-11-08: a whole-screen buffer and a top-layer
 * snapshot, 1.3 MB, failed). The screen is round: the corners mean nothing.
 * The work runs on its own task with a 16 KB PSRAM stack: lvgl draws on the
 * caller's stack, which overflowed the serial console's 3.5 KB (2026-11-08).
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "src/core/lv_obj_private.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"
#include "muse_board.h"
#include "msw_stack.h"
#include "msw_app_priv.h"

bool msw_gfx_copy_rows(uint16_t *out, int stride_px, int y0, int n);

#define W MSW_SCREEN_W
#define H MSW_SCREEN_H
#define JPEG_MAX (128 * 1024)

/* lvgl draws obj's part in rows y0 .. y0 + n - 1 over what db holds, as
 * lv_snapshot_take_to_draw_buf does for a whole object. Display locked. */
static void draw_rows(lv_obj_t *obj, lv_draw_buf_t *db, int y0, int n)
{
    lv_area_t area = { 0, y0, W - 1, y0 + n - 1 };
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = db;
    layer.buf_area = (lv_area_t){ 0, y0, W - 1, y0 + (int)db->header.h - 1 };
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = area;
    layer.phy_clip_area = area;
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);
    lv_display_t *d = lv_obj_get_display(obj);
    lv_display_t *d_old = lv_refr_get_disp_refreshing();
    lv_layer_t *head_old = d->layer_head;
    d->layer_head = &layer;
    lv_refr_set_disp_refreshing(d);
    lv_obj_redraw(&layer, obj);
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    d->layer_head = head_old;
    lv_refr_set_disp_refreshing(d_old);
    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
}

/* Average of a 2 x 2 square of lvgl-native RGB565 pixels. */
static uint16_t avg4(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    unsigned r = (a >> 11) + (b >> 11) + (c >> 11) + (d >> 11);
    unsigned g = ((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63);
    unsigned bl = (a & 31) + (b & 31) + (c & 31) + (d & 31);
    return (uint16_t)(((r + 2) / 4) << 11 | ((g + 2) / 4) << 5 | (bl + 2) / 4);
}

/* scale 1: the whole W x H screen. scale 2: half size (each pixel is the
 * average of 2 x 2), even-sized, 232 x 232 for the 466 screen: a quarter
 * of the pixels, so a quarter of the base64 text Muse reads. */
static uint8_t *take(int quality, int scale, size_t *n, int *ow_out, int *oh_out,
                     const char **source, const char **why, int *ms)
{
    int64_t t0 = esp_timer_get_time();
    *n = 0;
    *why = NULL;
    *source = "lvgl";
    scale = scale == 2 ? 2 : 1;
    const int OW = (W / scale) & ~1, OH = (H / scale) & ~1;
    *ow_out = OW;
    *oh_out = OH;
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = OW;
    cfg.height = OH;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = quality < 10 ? 10 : quality > 95 ? 95 : quality;
    jpeg_enc_handle_t enc = NULL;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
        *why = "out_of_memory";
        return NULL;
    }
    int block = jpeg_enc_get_block_size(enc);
    int rows = block / (OW * 2);   /* 16 rows of the JPEG */
    int srows = rows * scale;      /* screen rows for them */
    uint8_t *jpg = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM);
    uint16_t *in = heap_caps_aligned_alloc(16, block, MALLOC_CAP_SPIRAM);
    lv_draw_buf_t *db = rows > 0 ? lv_draw_buf_create(W, srows, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO) : NULL;
    if (!jpg || !in || !db || rows * OW * 2 != block) {
        *why = rows > 0 && rows * OW * 2 != block ? "jpeg_failed" : "out_of_memory";
        goto done;
    }
    int out = 0;
    jpeg_error_t r = JPEG_ERR_FAIL;
    for (int y0 = 0; y0 < H; y0 += srows) {
        int nr = H - y0 < srows ? H - y0 : srows;
        int stride_px = (int)(db->header.stride / 2);
        muse_board->display_lock(-1);
        if (msw_gfx_copy_rows((uint16_t *)db->data, stride_px, y0, nr)) {
            *source = "frame";
        } else {
            lv_draw_buf_clear(db, NULL);
            draw_rows(lv_screen_active(), db, y0, nr);
        }
        lv_obj_t *top = lv_layer_top();
        if (top && lv_obj_get_child_count(top)) {
            draw_rows(top, db, y0, nr);
        }
        muse_board->display_unlock();
        memset(in, 0, block);
        for (int y = 0; y < nr / scale; y++) {
            const uint16_t *src = (const uint16_t *)(db->data + (size_t)(y * scale) * db->header.stride);
            uint16_t *o = in + (size_t)y * OW;
            if (scale == 1) {
                for (int x = 0; x < OW; x++) {
                    o[x] = (uint16_t)((src[x] >> 8) | (src[x] << 8));
                }
                continue;
            }
            const uint16_t *s2 = (const uint16_t *)((const uint8_t *)src + db->header.stride);
            for (int x = 0; x < OW; x++) {
                uint16_t p = avg4(src[2 * x], src[2 * x + 1], s2[2 * x], s2[2 * x + 1]);
                o[x] = (uint16_t)((p >> 8) | (p << 8));
            }
        }
        r = jpeg_enc_process_with_block(enc, (const uint8_t *)in, block, jpg, JPEG_MAX, &out);
        if (r <= JPEG_ERR_OK) {
            /* JPEG_ERR_OK: the image is done (after 29 blocks for 466 rows,
             * 15 for 232; another block would start a new image), or an
             * error. The last rows past a whole block are dropped. */
            break;
        }
    }
    if (r == JPEG_ERR_OK && out > 0) {
        *n = (size_t)out;
    } else {
        *why = "jpeg_failed";   /* or bigger than JPEG_MAX: a lower quality fits */
    }
done:
    jpeg_enc_close(enc);
    heap_caps_free(in);
    if (db) {
        lv_draw_buf_destroy(db);
    }
    if (!*n) {
        heap_caps_free(jpg);
        return NULL;
    }
    *ms = (int)((esp_timer_get_time() - t0) / 1000);
    return jpg;
}

typedef struct {
    int quality, scale, w, h;
    size_t n;
    const char *source, *why;
    int ms;
    uint8_t *jpg;
    SemaphoreHandle_t done;
} job_t;

static void shot_task(void *arg)
{
    job_t *j = arg;
    j->jpg = take(j->quality, j->scale, &j->n, &j->w, &j->h, &j->source, &j->why, &j->ms);
    xSemaphoreGive(j->done);
    msw_task_delete_self();
}

uint8_t *msw_app_screenshot(int quality, int scale, size_t *n, int *w, int *h,
                            const char **source, const char **why, int *ms)
{
    job_t j = { .quality = quality, .scale = scale, .source = "", .done = xSemaphoreCreateBinary() };
    *n = 0;
    *source = "";
    *why = "busy";
    if (!j.done) {
        return NULL;
    }
    if (msw_task_create(shot_task, "msw_shot", 16 * 1024, &j, 4, NULL) != pdPASS) {
        vSemaphoreDelete(j.done);
        *why = "out_of_memory";
        return NULL;
    }
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    *n = j.n;
    *w = j.w;
    *h = j.h;
    *source = j.source;
    *why = j.why;
    *ms = j.ms;
    return j.jpg;
}
