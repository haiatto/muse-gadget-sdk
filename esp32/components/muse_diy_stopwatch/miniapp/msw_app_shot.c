/*
 * museStopWatch mini apps: app.screenshot, the screen as a JPEG.
 *
 * In frame drawing the picture is the app's frame with Muse's avatar and
 * bubble over it, as msw_fb_present sends it. Otherwise (lvgl apps, or no
 * app running) lvgl draws the active screen into a buffer (lv_snapshot).
 * The screen is round: the corners are outside it and mean nothing.
 * Buffers are PSRAM. The work runs on its own task with a 16 KB PSRAM stack:
 * lv_snapshot draws on the caller's stack, which overflowed the serial
 * console's 3.5 KB (2026-11-08); Muse's command task is small too.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "muse_board.h"
#include "msw_stack.h"
#include "msw_app_priv.h"

bool msw_gfx_copy_frame(uint16_t *out);

#define W MSW_SCREEN_W
#define H MSW_SCREEN_H
#define JPEG_MAX (192 * 1024)

/* The screen through lvgl, as RGB565 big endian in out. */
static bool lvgl_copy(uint16_t *out, const char **why)
{
    uint32_t stride = lv_draw_buf_width_to_stride(W, LV_COLOR_FORMAT_RGB565);
    size_t size = (size_t)stride * H;
    uint8_t *mem = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    if (!mem) {
        *why = "out_of_memory";
        return false;
    }
    lv_draw_buf_t db;
    bool ok = lv_draw_buf_init(&db, W, H, LV_COLOR_FORMAT_RGB565, stride, mem, size) == LV_RESULT_OK &&
              lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &db) == LV_RESULT_OK;
    if (ok) {
        for (int y = 0; y < H; y++) {
            const uint16_t *src = (const uint16_t *)(mem + (size_t)y * stride);
            uint16_t *o = out + (size_t)y * W;
            for (int x = 0; x < W; x++) {
                o[x] = (uint16_t)((src[x] >> 8) | (src[x] << 8));
            }
        }
    } else {
        *why = "snapshot_failed";
    }
    heap_caps_free(mem);
    return ok;
}

static uint8_t *take(int quality, size_t *n, const char **source, const char **why, int *ms)
{
    int64_t t0 = esp_timer_get_time();
    *n = 0;
    *why = NULL;
    uint16_t *px = heap_caps_aligned_alloc(16, (size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    uint8_t *jpg = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM);
    if (!px || !jpg) {
        heap_caps_free(px);
        heap_caps_free(jpg);
        *why = "out_of_memory";
        return NULL;
    }
    muse_board->display_lock(-1);
    bool got = msw_gfx_copy_frame(px);
    *source = got ? "frame" : "lvgl";
    if (!got) {
        got = lvgl_copy(px, why);
    }
    muse_board->display_unlock();
    if (got) {
        jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
        cfg.width = W;
        cfg.height = H;
        cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        cfg.subsampling = JPEG_SUBSAMPLE_420;
        cfg.quality = quality < 10 ? 10 : quality > 95 ? 95 : quality;
        jpeg_enc_handle_t enc = NULL;
        int out = 0;
        if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK ||
            jpeg_enc_process(enc, (const uint8_t *)px, W * H * 2, jpg, JPEG_MAX, &out) != JPEG_ERR_OK || out <= 0) {
            *why = "jpeg_failed";
        } else {
            *n = (size_t)out;
        }
        if (enc) {
            jpeg_enc_close(enc);
        }
    }
    heap_caps_free(px);
    if (!*n) {
        heap_caps_free(jpg);
        return NULL;
    }
    *ms = (int)((esp_timer_get_time() - t0) / 1000);
    return jpg;
}

typedef struct {
    int quality;
    size_t n;
    const char *source, *why;
    int ms;
    uint8_t *jpg;
    SemaphoreHandle_t done;
} job_t;

static void shot_task(void *arg)
{
    job_t *j = arg;
    j->jpg = take(j->quality, &j->n, &j->source, &j->why, &j->ms);
    xSemaphoreGive(j->done);
    msw_task_delete_self();
}

uint8_t *msw_app_screenshot(int quality, size_t *n, const char **source, const char **why, int *ms)
{
    job_t j = { .quality = quality, .source = "", .done = xSemaphoreCreateBinary() };
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
    *source = j.source;
    *why = j.why;
    *ms = j.ms;
    return j.jpg;
}
