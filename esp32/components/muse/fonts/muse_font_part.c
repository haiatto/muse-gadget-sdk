/*
 * museStopWatch: maps the msw_font partition for muse_font_part.h.
 */
#include "muse_font_part.h"

#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HEADER 16

typedef struct {
    uint32_t size;
    uint32_t hash;
    const uint8_t *bitmaps;
    TaskHandle_t caller;
} map_job_t;

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 0x01000193u;
    }
    return h;
}

/* esp_partition_mmap() freezes the cache, and IDF asserts that the caller's
 * stack is in internal RAM then. LVGL's tasks may have theirs in PSRAM, so a
 * short task with an internal stack maps it (as sanotts/msw_tts.c does). */
static void map_task(void *arg)
{
    map_job_t *job = arg;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "msw_font");
    if (!p) {
        printf("@msw.font error no msw_font partition: Japanese shows as boxes (write it over USB)\n");
    } else if (p->size < HEADER + job->size) {
        printf("@msw.font error msw_font is %u B, the font needs %u\n", (unsigned)p->size,
               (unsigned)(HEADER + job->size));
    } else {
        const void *ptr = NULL;
        esp_partition_mmap_handle_t h;
        esp_err_t e = esp_partition_mmap(p, 0, HEADER + job->size, ESP_PARTITION_MMAP_DATA, &ptr, &h);
        if (e != ESP_OK) {
            printf("@msw.font error mmap: %s\n", esp_err_to_name(e));
        } else {
            const uint8_t *b = ptr;
            uint32_t hdr[4];
            memcpy(hdr, b, sizeof(hdr));
            uint32_t hash = hdr[2] == job->size ? fnv1a(b + HEADER, job->size) : 0;
            if (memcmp(b, "MSWF", 4) != 0 || hdr[1] != 1 || hdr[2] != job->size || hdr[3] != job->hash ||
                hash != job->hash) {
                printf("@msw.font error msw_font holds another font (size %u, hash %08x; want %u, %08x):"
                       " Japanese shows as boxes (write it over USB)\n",
                       (unsigned)hdr[2], (unsigned)hash, (unsigned)job->size, (unsigned)job->hash);
                esp_partition_munmap(h);
            } else {
                job->bitmaps = b + HEADER;
                printf("@msw.font mapped %u B of glyph bitmaps from flash\n", (unsigned)job->size);
            }
        }
    }
    xTaskNotifyGive(job->caller);
    vTaskDelete(NULL);
}

const uint8_t *muse_font_part_map(uint32_t size, uint32_t hash)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    static StaticSemaphore_t lock_buf;
    static SemaphoreHandle_t lock;
    static volatile bool tried;
    static const uint8_t *volatile bitmaps;

    if (tried) {
        return bitmaps;
    }
    taskENTER_CRITICAL(&mux);
    if (!lock) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);
    }
    taskEXIT_CRITICAL(&mux);
    /* The first caller maps; the others wait for it (once, at the first text). */
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!tried) {
        map_job_t job = { .size = size, .hash = hash, .caller = xTaskGetCurrentTaskHandle() };
        if (xTaskCreate(map_task, "msw_font_map", 3072, &job, 5, NULL) == pdPASS) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            printf("@msw.font error map task\n");
        }
        bitmaps = job.bitmaps;
        tried = true;
    }
    xSemaphoreGive(lock);
    return bitmaps;
}
