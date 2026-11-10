/*
 * museStopWatch mini apps: the microphone (msw_app.h, microphone).
 *
 * The voice task reads the mic every 20 ms while it is idle and while it
 * plays sound (muse_voice.c). With an app's mic on, it hands each read to
 * msw_mic_tap, which keeps the last second in a ring for msw_mic_read.
 * While the user talks to Muse, while Muse answers, and at rest (screen off
 * on battery) the voice task doesn't call it: the app gets nothing then and
 * msw_mic_level says -1.
 */
#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_audio.h"
#include "msw_app_priv.h"

#define RING 16000                  /* 1 s at 16 kHz */
#define STALE_US (100 * 1000LL)     /* no read for this long: the mic is not flowing */

static EXT_RAM_BSS_ATTR int16_t s_ring[RING];   /* 32 KB: PSRAM (internal RAM is short) */
static int s_r, s_n;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_on;
static volatile int64_t s_last_us;
static volatile int s_level;

/* muse_voice.c, through weak references. */
bool msw_mic_wanted(void)
{
    return atomic_load(&s_on);
}

void msw_mic_tap(const int16_t *mono, size_t n)
{
    if (!atomic_load(&s_on) || !n) {
        return;
    }
    int level = (int)(muse_audio_level(mono, n) * 100.0f + 0.5f);
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        if (s_n == RING) {          /* full: drop the oldest */
            s_r = (s_r + 1) % RING;
            s_n--;
        }
        s_ring[(s_r + s_n) % RING] = mono[i];
        s_n++;
    }
    portEXIT_CRITICAL(&s_mux);
    s_level = level < 0 ? 0 : level > 100 ? 100 : level;
    s_last_us = esp_timer_get_time();
}

static void clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_r = s_n = 0;
    portEXIT_CRITICAL(&s_mux);
    s_last_us = 0;
    s_level = 0;
}

/* msw_app_core.c: an app ended. */
void msw_mic_app_end(void)
{
    atomic_store(&s_on, false);
    clear();
}

/* ---- the app's calls --------------------------------------------------------- */

void msw_mic_on(bool on)
{
    if (on != atomic_load(&s_on)) {
        clear();
        atomic_store(&s_on, on);
    }
}

int msw_mic_read(int16_t *mono, int max)
{
    mono = max > 0 ? msw_api_writable(mono, (size_t)max * 2, "msw_mic_read") : NULL;
    if (!mono || max <= 0) {
        return 0;
    }
    int got = 0;
    portENTER_CRITICAL(&s_mux);
    while (got < max && s_n > 0) {
        int run = RING - s_r;
        run = run < s_n ? run : s_n;
        run = run < max - got ? run : max - got;
        memcpy(mono + got, s_ring + s_r, (size_t)run * 2);
        got += run;
        s_r = (s_r + run) % RING;
        s_n -= run;
    }
    portEXIT_CRITICAL(&s_mux);
    return got;
}

int msw_mic_level(void)
{
    if (!atomic_load(&s_on) || esp_timer_get_time() - s_last_us > STALE_US) {
        return -1;
    }
    return s_level;
}
