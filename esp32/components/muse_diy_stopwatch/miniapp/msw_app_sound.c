/*
 * museStopWatch mini apps: a 4-channel synth and raw samples, mixed over the
 * local TTS (msw_app.h, sound).
 *
 * The voice task plays one "player" when it is idle (muse_voice.h). The TTS
 * sets its own at boot; this file wraps it: the wrapper plays the TTS as
 * before and adds the app's sound on top. While an app runs and has made a
 * sound in the last 3 s, the wrapper keeps the player open, so the next
 * note starts at once instead of waiting for the voice task's idle poll.
 * A press of the talk button stops playback (the voice task asks the player
 * to stop); the next note opens it again.
 */
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_voice.h"
#include "esp_attr.h"
#include "msw_app_priv.h"

#define RATE 16000
#define KEEP_US (3 * 1000000LL)
#define PCM_RING 8192           /* 0.5 s */
#define CH_PEAK 7000            /* one channel at volume 100; 4 together still fit */

typedef struct {
    int wave;
    uint32_t phase;
    int64_t step, step_end;     /* phase step, 32-bit fraction of a cycle per sample */
    int32_t total, left;        /* samples */
    int vol;                    /* 0..100 */
    bool fade;
    uint16_t lfsr;
    int16_t noise;
} chan_t;

static chan_t s_ch[MSW_SOUND_CHANNELS];
static EXT_RAM_BSS_ATTR int16_t s_pcm[PCM_RING];   /* 16 KB: PSRAM (internal RAM is short) */
static int s_pcm_r, s_pcm_n;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static const muse_voice_player_t *s_prev;
static atomic_bool s_app;
static atomic_bool s_prev_active;
static volatile int64_t s_last_sound;
static int16_t s_sine[256];

static bool synth_busy_locked(void)
{
    for (int i = 0; i < MSW_SOUND_CHANNELS; i++) {
        if (s_ch[i].left > 0) {
            return true;
        }
    }
    return s_pcm_n > 0;
}

static bool synth_wanted(void)
{
    if (!atomic_load(&s_app)) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    bool busy = synth_busy_locked();
    portEXIT_CRITICAL(&s_mux);
    return busy || esp_timer_get_time() - s_last_sound < KEEP_US;
}

/* One sample of a channel, -32768..32767 at full scale. Locked. */
static int32_t chan_next(chan_t *c)
{
    int32_t elapsed = c->total - c->left;
    int64_t step = c->step;
    if (c->step_end != c->step && c->total > 0) {
        step = c->step + (c->step_end - c->step) * elapsed / c->total;
    }
    uint32_t before = c->phase;
    c->phase += (uint32_t)step;
    int32_t v;
    switch (c->wave) {
    case MSW_WAVE_TRIANGLE: {
        uint32_t p = c->phase >> 16;     /* 0..65535 */
        v = p < 32768 ? (int32_t)p * 2 - 32768 : 32767 - ((int32_t)p - 32768) * 2;
        break;
    }
    case MSW_WAVE_SAW:
        v = (int32_t)(c->phase >> 16) - 32768;
        break;
    case MSW_WAVE_NOISE:
        if (c->phase < before) {         /* a new cycle: next noise value */
            uint16_t bit = ((c->lfsr >> 0) ^ (c->lfsr >> 1)) & 1;
            c->lfsr = (uint16_t)((c->lfsr >> 1) | (bit << 14));
            c->noise = (c->lfsr & 1) ? 32767 : -32767;
        }
        v = c->noise;
        break;
    case MSW_WAVE_SINE:
        v = s_sine[c->phase >> 24];
        break;
    default:
        v = c->phase < 0x80000000u ? 32767 : -32767;
        break;
    }
    int32_t amp = CH_PEAK * c->vol / 100;
    if (c->fade && c->total > 0) {
        amp = amp * c->left / c->total;
    } else if (c->left < 80) {
        amp = amp * c->left / 80;      /* no click at the end */
    }
    c->left--;
    return v * amp / 32768;
}

/* Adds the app's sound to n samples. */
static void mix(int16_t *buf, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        int32_t s = buf[i];
        for (int k = 0; k < MSW_SOUND_CHANNELS; k++) {
            if (s_ch[k].left > 0) {
                s += chan_next(&s_ch[k]);
            }
        }
        if (s_pcm_n > 0) {
            s += s_pcm[s_pcm_r];
            s_pcm_r = (s_pcm_r + 1) % PCM_RING;
            s_pcm_n--;
        }
        buf[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ---- the wrapping player ----------------------------------------------------- */

static bool w_ready(void)
{
    if (s_prev && s_prev->ready()) {
        atomic_store(&s_prev_active, true);
        return true;
    }
    return synth_wanted();
}

static size_t w_read(int16_t *mono, size_t max)
{
    size_t n = 0;
    if (s_prev && !atomic_load(&s_prev_active) && s_prev->ready()) {
        atomic_store(&s_prev_active, true);
    }
    if (atomic_load(&s_prev_active)) {
        n = s_prev->read(mono, max);
        if (n == 0 && s_prev->done()) {
            atomic_store(&s_prev_active, false);
        }
    }
    if (n == 0) {
        if (!synth_wanted()) {
            return 0;
        }
        memset(mono, 0, max * sizeof(*mono));
        n = max;
    }
    if (atomic_load(&s_app)) {
        mix(mono, n);
    }
    return n;
}

static bool w_done(void)
{
    return !atomic_load(&s_prev_active) && !synth_wanted();
}

static void w_stop(void)
{
    if (s_prev && atomic_load(&s_prev_active)) {
        s_prev->stop();
    }
    atomic_store(&s_prev_active, false);
    msw_sound_stop(-1);
    s_last_sound = 0;
}

static const muse_voice_player_t s_player = { w_ready, w_read, w_done, w_stop };

void msw_sound_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_sine[i] = (int16_t)(32767 * sinf(6.2831853f * i / 256));
    }
    const muse_voice_player_t *cur = muse_voice_get_player();
    if (cur != &s_player) {
        s_prev = cur;
        muse_voice_set_player(&s_player);
    }
}

void msw_sound_app(bool running)
{
    atomic_store(&s_app, running);
    if (!running) {
        msw_sound_stop(-1);
        s_last_sound = 0;
    }
}

/* ---- the app's calls --------------------------------------------------------- */

static void note(int ch, int wave, int hz, int hz_end, int ms, int volume, bool fade)
{
    if (ch < 0 || ch >= MSW_SOUND_CHANNELS || ms <= 0 || !atomic_load(&s_app)) {
        return;
    }
    hz = hz < 20 ? 20 : hz > 8000 ? 8000 : hz;
    hz_end = hz_end < 20 ? 20 : hz_end > 8000 ? 8000 : hz_end;
    chan_t c = {
        .wave = wave < 0 || wave > MSW_WAVE_SINE ? MSW_WAVE_SQUARE : wave,
        .step = ((int64_t)hz << 32) / RATE,
        .step_end = ((int64_t)hz_end << 32) / RATE,
        .total = (int32_t)((int64_t)ms * RATE / 1000),
        .vol = volume < 0 ? 0 : volume > 100 ? 100 : volume,
        .fade = fade,
        .lfsr = 0x4A5D,
    };
    c.left = c.total;
    portENTER_CRITICAL(&s_mux);
    s_ch[ch] = c;
    portEXIT_CRITICAL(&s_mux);
    s_last_sound = esp_timer_get_time();
    muse_voice_player_kick();
}

void msw_sound(int ch, int wave, int hz, int ms, int volume, bool fade)
{
    note(ch, wave, hz, hz, ms, volume, fade);
}

void msw_sound_slide(int ch, int wave, int hz, int hz_end, int ms, int volume)
{
    note(ch, wave, hz, hz_end, ms, volume, false);
}

void msw_sound_stop(int ch)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MSW_SOUND_CHANNELS; i++) {
        if (ch < 0 || ch == i) {
            s_ch[i].left = 0;
        }
    }
    if (ch < 0) {
        s_pcm_n = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool msw_sound_busy(int ch)
{
    if (ch < 0 || ch >= MSW_SOUND_CHANNELS) {
        return false;
    }
    return s_ch[ch].left > 0;
}

int msw_pcm_write(const int16_t *mono, int n)
{
    mono = n > 0 ? msw_api_readable(mono, (size_t)n * 2, "msw_pcm_write") : NULL;
    if (!mono || n <= 0 || !atomic_load(&s_app)) {
        return 0;
    }
    int took = 0;
    portENTER_CRITICAL(&s_mux);
    while (took < n && s_pcm_n < PCM_RING) {
        s_pcm[(s_pcm_r + s_pcm_n) % PCM_RING] = mono[took++];
        s_pcm_n++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (took) {
        s_last_sound = esp_timer_get_time();
        muse_voice_player_kick();
    }
    return took;
}
