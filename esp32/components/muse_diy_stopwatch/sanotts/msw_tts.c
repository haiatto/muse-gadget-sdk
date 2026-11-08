/*
 * museStopWatch: sanoTTS-jp on the StopWatch: speech through the speaker, and a
 * serial bench.
 *
 *   >msw.say <text>              speak it (sentence by sentence, while it plays)
 *   >msw.say.stop                stop speaking
 *   >msw.tts <text>              synthesize without sound and report the timing
 *   >msw.tts.cfg key=value ...   model=psram|flash  arena=psram|int  prio=N  core=0|1|any  rep=N
 *   >msw.tts.cfg                 print the settings
 *
 * Bench lines start "@msw.tts", speech lines "@msw.say"; each command ends with
 * "@msw.tts done" / "@msw.say done".
 *
 * Settings (bench of 2026-11-07, work/sanotts-bench-2026-11-07.md): the model is
 * copied into PSRAM (faster than reading it from flash), the arena (inference
 * work area, about 120 KB) is in PSRAM (internal RAM has no block that big), the
 * task runs on core 0 at priority 4: core 1 is where the UI draws (lvgl,
 * priority 5), and above it the UI stops.
 *
 * Timing follows upstream esp32/main/main.c: the steady xRT is the median of the
 * full-chunk pulls after the first one (a chunk is 8 frames x 256 samples at
 * 22050 Hz = 92.9 ms of audio), divided by that chunk's length. Under 1.0 means
 * it makes sound faster than it plays.
 *
 * Speech: the model makes 22050 Hz; replies play at 16 kHz (muse_audio.h), so a
 * windowed-sinc resampler (320/441, 24 taps) feeds a PSRAM ring that the voice
 * task plays when it is idle (muse_voice_set_player). A press stops it.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "g2p.h"
#include "jdict.h"
#include "msw_stack.h"
#include "nvs.h"
#include "muse_audio.h"
#include "muse_settings.h"
#include "muse_voice.h"
#include "saan_kanji.h"
#include "saan_pcm.h"
#include "saanotts.h"
#include "saanotts_stream.h"

#define MAX_IDS 350                 /* upstream SAAN_MAX_IDS (the model's trained length) */
#define IDS_CAP (2 * 512 + 3)       /* upstream SAAN_G2P_IDS_CAP */
#define MAX_PULLS 256
#define CHUNK_SAMPLES (SAAN_CHUNK * SAAN_HOP)
#define TTS_GAIN 1.0f              /* see s_cfg.gain */
#define TASK_STACK 12288            /* bench: about 7.7 KB used */

static struct {
    bool arena_int;
    bool model_psram;
    int prio;
    int core;   /* -1 = any */
    int rep;
    bool quiet;   /* speech plays silence of the same length (tests at night) */
    float gain;   /* speech level: the model's output times this (msw.tts.cfg gain=) */
} s_cfg = { false, true, 4, 0, 1, false, TTS_GAIN };

static const void *s_model_map, *s_dict_map;
static size_t s_model_n, s_dict_n;
static void *s_model_copy;
static jdict_t s_dict;
static bool s_dict_ok;
static atomic_bool s_busy;

/* --- model and dictionary ------------------------------------------------ */

static bool map_part(const char *label, const void **ptr, size_t *n)
{
    if (*ptr) {
        return true;
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p) {
        printf("@msw.tts error no partition %s\n", label);
        return false;
    }
    esp_partition_mmap_handle_t h;
    esp_err_t e = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, ptr, &h);
    if (e != ESP_OK) {
        printf("@msw.tts error mmap %s: %s\n", label, esp_err_to_name(e));
        *ptr = NULL;
        return false;
    }
    *n = p->size;
    return true;
}

/* esp_partition_mmap() freezes the cache, and IDF asserts that the caller's
 * stack is in internal RAM then (esp_cache_utils.c). The TTS task's stack is
 * in PSRAM, so a short task with an internal stack maps both, once. */
static void map_task(void *arg)
{
    if (map_part("sano_model", &s_model_map, &s_model_n)) {
        map_part("sano_dict", &s_dict_map, &s_dict_n);
    }
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

static bool open_all(void)
{
    if (!s_model_map || !s_dict_map) {
        if (xTaskCreate(map_task, "msw_tts_map", 3072, xTaskGetCurrentTaskHandle(), 5, NULL) != pdPASS) {
            printf("@msw.tts error map task\n");
            return false;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_model_map || !s_dict_map) {
            return false;
        }
    }
    if (!s_dict_ok) {
        int r = jdict_open(&s_dict, s_dict_map, s_dict_n);
        if (r != 0) {
            printf("@msw.tts error jdict_open %d\n", r);
            return false;
        }
        s_dict_ok = saan_kanji_init();
    }
    if (s_cfg.model_psram && !s_model_copy) {
        s_model_copy = heap_caps_aligned_alloc(16, s_model_n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_model_copy) {
            memcpy(s_model_copy, s_model_map, s_model_n);
        }   /* else: read it from flash, a little slower */
    }
    if (!s_cfg.model_psram && s_model_copy) {
        free(s_model_copy);
        s_model_copy = NULL;
    }
    return true;
}

static size_t arena_bytes(void)
{
    size_t a = saan_stream_arena_peak(MAX_IDS);
    size_t k = saan_kanji_workbytes();
    return ((a > k ? a : k) + 31) & ~(size_t)15;
}

typedef struct {
    uint8_t *arena;
    size_t arena_n;
    int32_t *ids;
    uint32_t *pull_us;
} work_t;

static bool work_alloc(work_t *w)
{
    w->arena_n = arena_bytes();
    w->arena = heap_caps_aligned_alloc(16, w->arena_n, s_cfg.arena_int ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                                                        : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    w->ids = heap_caps_malloc(IDS_CAP * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    w->pull_us = heap_caps_malloc(MAX_PULLS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!w->arena || !w->ids || !w->pull_us) {
        printf("@msw.tts error no memory: arena %u B in %s (largest free %u)\n", (unsigned)w->arena_n,
               s_cfg.arena_int ? "internal" : "psram",
               (unsigned)heap_caps_get_largest_free_block(s_cfg.arena_int ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM));
        return false;
    }
    return true;
}

static void work_free(work_t *w)
{
    free(w->arena);
    free(w->ids);
    free(w->pull_us);
    memset(w, 0, sizeof *w);
}

/* --- one sentence ---------------------------------------------------------- */

typedef bool (*sink_fn)(const float *pcm, int n);   /* false: stop */

typedef enum { SY_OK, SY_TOO_LONG, SY_REJECT, SY_FAIL, SY_STOPPED } sy_t;

typedef struct {
    int n_ids, n_tok;
    bool kanji;
    int32_t reject_byte;
    double g2p_ms, init_ms, first_ms, audio_s, synth_s, steady, p90;
    unsigned peak;
} stats_t;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static sy_t synth(const char *text, size_t nbytes, work_t *w, sink_fn sink, stats_t *st)
{
    memset(st, 0, sizeof *st);
    saan_g2p_status why;
    int32_t err_byte = -1, n_ids = 0;
    int n_tok = 0;
    int64_t t0 = esp_timer_get_time();
    saan_g2p_route route = saan_g2p_classify(text, nbytes, &why, &err_byte);
    if (route == SAAN_G2P_ROUTE_KANA) {
        saan_g2p_status gs = saan_g2p(text, nbytes, w->ids, IDS_CAP, &n_ids, NULL);
        if (gs == SAAN_G2P_ERR_OVERFLOW) {
            return SY_TOO_LONG;
        }
        if (gs != SAAN_G2P_OK) {
            return SY_FAIL;
        }
    } else if (route == SAAN_G2P_ROUTE_DICT) {
        st->kanji = true;
        saan_kanji_status ks = saan_kanji_to_ids(&s_dict, text, nbytes, w->arena, w->arena_n, w->ids, IDS_CAP,
                                                 &n_ids, &n_tok);
        if (ks == SAAN_KANJI_ERR_TOO_LONG || ks == SAAN_KANJI_ERR_IDS) {
            return SY_TOO_LONG;
        }
        if (ks != SAAN_KANJI_OK) {
            printf("@msw.tts kanji g2p: %s\n", saan_kanji_strerror(ks));
            return SY_FAIL;
        }
    } else {
        st->reject_byte = err_byte;
        return SY_REJECT;
    }
    st->n_ids = n_ids;
    st->n_tok = n_tok;
    st->g2p_ms = (esp_timer_get_time() - t0) / 1000.0;
    if (n_ids > MAX_IDS) {
        return SY_TOO_LONG;
    }

    saan_weights wt;
    if (saan_weights_open(&wt, s_model_copy ? s_model_copy : s_model_map, s_model_n) != SAAN_OK) {
        printf("@msw.tts error saan_weights_open\n");
        return SY_FAIL;
    }
    saan_pcm_reset();
    saan_arena a;
    saan_arena_init(&a, w->arena, w->arena_n);
    saan_stream ss;
    t0 = esp_timer_get_time();
    saan_status s = saan_stream_init(&ss, &wt, &a, w->ids, n_ids, SAAN_S_V);
    st->init_ms = (esp_timer_get_time() - t0) / 1000.0;
    if (s != SAAN_OK || a.failed || a.used != saan_stream_arena_used(n_ids)) {
        printf("@msw.tts error stream init: %s (used %u)\n", saan_strerror(s), (unsigned)a.used);
        return SY_FAIL;
    }

    int pulls = 0;
    uint32_t samples = 0;
    int64_t t_total = 0;
    for (;;) {
        const float *chunk = NULL;
        int32_t n = 0;
        t0 = esp_timer_get_time();
        s = saan_stream_pull_ptr(&ss, &chunk, &n);
        const int64_t dt = esp_timer_get_time() - t0;   /* synthesis only, not the sink */
        if (s != SAAN_OK || a.failed) {
            printf("@msw.tts error pull: %s\n", saan_strerror(s));
            return SY_FAIL;
        }
        if (n <= 0) {
            break;
        }
        t_total += dt;
        samples += (uint32_t)(n * SAAN_HOP);
        if (pulls < MAX_PULLS) {
            w->pull_us[pulls] = (n == SAAN_CHUNK) ? (uint32_t)dt : 0;
        }
        pulls++;
        if (!sink(chunk, n * SAAN_HOP)) {
            return SY_STOPPED;
        }
    }

    int m = 0;
    const uint32_t first = pulls ? w->pull_us[0] : 0;
    for (int i = 1; i < pulls && i < MAX_PULLS; i++) {
        if (w->pull_us[i]) {
            w->pull_us[m++] = w->pull_us[i];
        }
    }
    qsort(w->pull_us, m, sizeof w->pull_us[0], cmp_u32);
    const double chunk_us = 1e6 * CHUNK_SAMPLES / SAAN_SR;
    st->first_ms = first / 1000.0;
    st->audio_s = (double)samples / SAAN_SR;
    st->synth_s = t_total / 1e6;
    st->steady = m ? w->pull_us[m / 2] / chunk_us : 0;
    st->p90 = m ? w->pull_us[(m * 9) / 10] / chunk_us : 0;
    st->peak = (unsigned)a.peak;
    return SY_OK;
}

static void print_stats(const char *tag, int round, const stats_t *st)
{
    printf("@%s %d: %s ids %d tok %d | g2p %.1f ms init %.1f ms first %.1f ms | audio %.3f s synth %.3f s"
           " | xRT steady %.3f p90 %.3f full %.3f | arena peak %u\n",
           tag, round, st->kanji ? "kanji" : "kana", st->n_ids, st->n_tok, st->g2p_ms, st->init_ms, st->first_ms,
           st->audio_s, st->synth_s, st->steady, st->p90, st->audio_s > 0 ? st->synth_s / st->audio_s : 0,
           st->peak);
}

/* --- bench ------------------------------------------------------------------- */

static int16_t *s_bench_pcm;

static bool bench_sink(const float *pcm, int n)
{
    for (int i = 0; i < n && i < CHUNK_SAMPLES; i++) {
        s_bench_pcm[i] = saan_f32_to_i16(pcm[i]);
    }
    return true;
}

static void bench_task(void *arg)
{
    char *text = arg;
    work_t w = { 0 };
    const size_t int_free0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_bench_pcm = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_bench_pcm && work_alloc(&w) && open_all()) {
        printf("@msw.tts cfg arena %s model %s prio %d core %d | int free %u largest %u\n",
               esp_ptr_external_ram(w.arena) ? "psram" : "int", s_model_copy ? "psram" : "flash-mmap",
               (int)uxTaskPriorityGet(NULL), (int)xPortGetCoreID(),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        for (int r = 1; r <= s_cfg.rep; r++) {
            stats_t st;
            sy_t res = synth(text, strlen(text), &w, bench_sink, &st);
            if (res != SY_OK) {
                printf("@msw.tts error %s (byte %d)\n",
                       res == SY_TOO_LONG ? "too long: split the text" : res == SY_REJECT ? "rejected" : "failed",
                       (int)st.reject_byte);
                break;
            }
            print_stats("msw.tts run", r, &st);
            printf("@msw.tts sum %016llx\n", (unsigned long long)saan_pcm_checksum());
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        printf("@msw.tts mem: stack left %u B, int free %u (before %u) min %u, psram free %u\n",
               (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)int_free0, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    work_free(&w);
    free(s_bench_pcm);
    s_bench_pcm = NULL;
    free(text);
    printf("@msw.tts done\n");
    fflush(stdout);
    atomic_store(&s_busy, false);
    msw_task_delete_self();
}

/* --- speech: resampler and ring ---------------------------------------------- */

#define RS_L 320    /* 16000 / 22050 = 320 / 441 */
#define RS_M 441
#define RS_T 24     /* taps */
#define RING_N (MUSE_AUDIO_RATE * 6)            /* 6 s of 16 kHz audio, 192 KB */
#define PREROLL (MUSE_AUDIO_RATE * 3 / 10)      /* start playing at 0.3 s buffered */
#define IN_CAP (CHUNK_SAMPLES + RS_T)

static float *s_rs_h;          /* [RS_L][RS_T] */
static float *s_rs_in;         /* input samples not yet used, starting at absolute s_rs_in0 */
static int64_t s_rs_in0, s_rs_out;
static int s_rs_len;

static int16_t *s_ring;
static atomic_uint s_ring_w, s_ring_r;   /* free-running counts */
static atomic_bool s_say_active;         /* the producer is still making sound */
static atomic_bool s_say_stop;
static atomic_bool s_playing;            /* reached the pre-roll */
static atomic_bool s_reply;              /* speaking a reply: the chat session reads the ring, not the player */
static atomic_uint s_says;               /* says started (voice.say, >msw.say) */

static bool rs_init(void)
{
    if (!s_rs_h) {
        s_rs_h = heap_caps_malloc(RS_L * RS_T * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!s_rs_h) {
            return false;
        }
        /* Low-pass at 7.2 kHz (under 16 kHz's 8 kHz Nyquist), Hann window, each phase summed to 1. */
        const double fc = 2.0 * 7200.0 / SAAN_SR;
        for (int p = 0; p < RS_L; p++) {
            double sum = 0;
            for (int j = 0; j < RS_T; j++) {
                /* distance from output time to input sample j (taps centred) */
                double d = (double)p / RS_L + (RS_T / 2 - 1) - j;
                double x = fc * d;
                double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
                double win = 0.5 + 0.5 * cos(M_PI * d / (RS_T / 2));
                double h = fabs(d) >= RS_T / 2 ? 0 : fc * sinc * win;
                s_rs_h[p * RS_T + j] = (float)h;
                sum += h;
            }
            for (int j = 0; j < RS_T; j++) {
                s_rs_h[p * RS_T + j] /= (float)sum;
            }
        }
    }
    if (!s_rs_in) {
        s_rs_in = heap_caps_malloc((IN_CAP + RS_T) * sizeof(float), MALLOC_CAP_SPIRAM);
    }
    if (!s_ring) {
        s_ring = heap_caps_malloc(RING_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    return s_rs_in && s_ring;
}

/* The loudest output of this text (before the limiter) and how many samples the limiter touched. */
static float s_peak;
static int s_clipped;

/* --- voice level (Settings > Sound > Voice level, NVS msw/tts_level) -------------- */

#define LEVEL_MIN 50
#define LEVEL_MAX 300
#define LEVEL_DEFAULT 100
/* The codec's gain at volume 100 on this board, in dB: esp_codec_dev's curve tops at
 * 0 dB and board_m5stack_stopwatch.c's hw_gain adds 3.6 + 6 dB (pa_gain -6). */
#define CODEC_DB_AT_100 9.6f

static int s_level = -1;    /* percent; -1 = not read yet */
static float s_limit = 1;   /* this text's ceiling, so the codec's gain doesn't clip */

int msw_tts_level(void)
{
    if (s_level < 0) {
        s_level = LEVEL_DEFAULT;
        nvs_handle_t h;
        if (nvs_open("msw", NVS_READONLY, &h) == ESP_OK) {
            uint16_t v;
            if (nvs_get_u16(h, "tts_level", &v) == ESP_OK && v >= LEVEL_MIN && v <= LEVEL_MAX) {
                s_level = v;
            }
            nvs_close(h);
        }
    }
    return s_level;
}

void msw_tts_set_level(int percent)
{
    percent = percent < LEVEL_MIN ? LEVEL_MIN : percent > LEVEL_MAX ? LEVEL_MAX : percent;
    s_level = percent;
    nvs_handle_t h;
    if (nvs_open("msw", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "tts_level", (uint16_t)percent);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* The codec multiplies by 10^(dB/20) after us; past full scale it clips hard.
 * Keep the samples under 1/that, bending the loud part smoothly instead. */
static void limit_update(void)
{
    float db = -50.0f + 0.5f * muse_settings_volume() + CODEC_DB_AT_100;
    float g = powf(10.0f, db / 20.0f);
    s_limit = g > 1.0f ? 1.0f / g : 1.0f;
}

static float limit(float y)
{
    const float a = y < 0 ? -y : y;
    const float knee = 0.6f * s_limit;
    if (a <= knee) {
        return y;
    }
    s_clipped++;
    const float room = s_limit - knee;
    const float out = knee + room * tanhf((a - knee) / room);
    return y < 0 ? -out : out;
}

static void rs_reset(void)
{
    /* Start with half a window of silence, so the first output is centred on sample 0. */
    s_rs_len = RS_T / 2 - 1;
    memset(s_rs_in, 0, s_rs_len * sizeof(float));
    s_rs_in0 = -(RS_T / 2 - 1);
    s_rs_out = 0;
    s_peak = 0;
    s_clipped = 0;
    limit_update();
}

static bool ring_put(int16_t v)
{
    while (atomic_load(&s_ring_w) - atomic_load(&s_ring_r) >= RING_N) {
        if (atomic_load(&s_say_stop)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    unsigned w = atomic_load(&s_ring_w);
    s_ring[w % RING_N] = v;
    atomic_store(&s_ring_w, w + 1);
    if (!atomic_load(&s_playing) && w + 1 - atomic_load(&s_ring_r) >= PREROLL) {
        atomic_store(&s_playing, true);
        if (!atomic_load(&s_reply)) {
            muse_voice_player_kick();
        }
    }
    return true;
}

/* Resample what is buffered; with flush, also the last samples (padded with silence). */
static bool rs_run(bool flush)
{
    const int64_t real_end = s_rs_in0 + s_rs_len;
    if (flush) {
        memset(s_rs_in + s_rs_len, 0, RS_T * sizeof(float));
        s_rs_len += RS_T;
    }
    for (;;) {
        const int64_t pos = s_rs_out * RS_M;   /* in units of 1/RS_L input samples */
        const int64_t base = pos / RS_L;
        const int phase = (int)(pos % RS_L);
        const int64_t i0 = base - (RS_T / 2 - 1);   /* first tap's absolute input index */
        const int off = (int)(i0 - s_rs_in0);
        if (off + RS_T > s_rs_len || (flush && base >= real_end)) {
            break;
        }
        const float *h = s_rs_h + phase * RS_T, *x = s_rs_in + off;
        float y = 0;
        for (int j = 0; j < RS_T; j++) {
            y += h[j] * x[j];
        }
        y *= s_cfg.gain * msw_tts_level() / 100.0f;
        float a = y < 0 ? -y : y;
        if (a > s_peak) {
            s_peak = a;
        }
        if (!ring_put(saan_f32_to_i16(limit(y)))) {
            return false;
        }
        s_rs_out++;
    }
    /* Drop the input no later output needs. */
    const int64_t next_i0 = (s_rs_out * RS_M) / RS_L - (RS_T / 2 - 1);
    int drop = (int)(next_i0 - s_rs_in0);
    if (drop > 0) {
        if (drop > s_rs_len) {
            drop = s_rs_len;
        }
        memmove(s_rs_in, s_rs_in + drop, (s_rs_len - drop) * sizeof(float));
        s_rs_len -= drop;
        s_rs_in0 += drop;
    }
    return true;
}

static bool say_sink(const float *pcm, int n)
{
    while (n > 0) {
        int take = IN_CAP - s_rs_len;
        if (take > n) {
            take = n;
        }
        memcpy(s_rs_in + s_rs_len, pcm, take * sizeof(float));
        s_rs_len += take;
        pcm += take;
        n -= take;
        if (!rs_run(false)) {
            return false;
        }
    }
    return !atomic_load(&s_say_stop);
}

/* Player for the voice task (muse_voice.h). */
static bool pl_ready(void)
{
    return !atomic_load(&s_reply) && atomic_load(&s_playing) && atomic_load(&s_ring_w) != atomic_load(&s_ring_r);
}

static size_t pl_read(int16_t *mono, size_t max)
{
    unsigned r = atomic_load(&s_ring_r), w = atomic_load(&s_ring_w);
    size_t n = w - r;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        mono[i] = s_cfg.quiet ? 0 : s_ring[(r + i) % RING_N];
    }
    atomic_store(&s_ring_r, r + (unsigned)n);
    return n;
}

static bool pl_done(void)
{
    return !atomic_load(&s_say_active) && atomic_load(&s_ring_w) == atomic_load(&s_ring_r);
}

static void pl_stop(void)
{
    atomic_store(&s_say_stop, true);
    atomic_store(&s_playing, false);
    atomic_store(&s_ring_r, atomic_load(&s_ring_w));
}

static const muse_voice_player_t s_player = { pl_ready, pl_read, pl_done, pl_stop };

/* --- speech: sentences --------------------------------------------------------- */

static size_t utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

static bool is_end(const char *p)
{
    return !strncmp(p, "。", 3) || !strncmp(p, "！", 3) || !strncmp(p, "？", 3) || *p == '!' || *p == '?' ||
           *p == '\n';
}

/* Speaks text[0..n) as one unit; too long: split at the "、" nearest the middle
 * (or the middle character) and try both halves. Rejected characters are left
 * out one at a time (the model reads Japanese only). */
static sy_t say_unit(char *text, size_t n, work_t *w, int depth, int *spoken, int *dropped)
{
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\n' || text[n - 1] == '\r')) {
        n--;
    }
    while (n && (*text == ' ' || *text == '\n' || *text == '\r')) {
        text++;
        n--;
    }
    if (!n) {
        return SY_OK;
    }
    for (int tries = 0; tries < 64; tries++) {
        stats_t st;
        sy_t r = synth(text, n, w, say_sink, &st);
        if (r == SY_OK) {
            print_stats("msw.say part", ++*spoken, &st);
            return SY_OK;
        }
        if (r == SY_REJECT && st.reject_byte >= 0 && (size_t)st.reject_byte < n) {
            size_t k = utf8_len((unsigned char)text[st.reject_byte]);
            if (st.reject_byte + k > n) {
                k = n - st.reject_byte;
            }
            memmove(text + st.reject_byte, text + st.reject_byte + k, n - st.reject_byte - k);
            n -= k;
            ++*dropped;
            if (!n) {
                return SY_OK;
            }
            continue;
        }
        if (r == SY_TOO_LONG && depth < 6 && n > 6) {
            size_t best = 0, mid = n / 2;
            for (size_t i = 0; i + 3 <= n; i += utf8_len((unsigned char)text[i])) {
                if (!strncmp(text + i, "、", 3) && (best == 0 || labs((long)i - (long)mid) < labs((long)best - (long)mid))) {
                    best = i + 3;
                }
            }
            if (!best) {
                for (best = mid; best < n && (text[best] & 0xC0) == 0x80; best++) {
                }
            }
            sy_t a = say_unit(text, best, w, depth + 1, spoken, dropped);
            if (a == SY_STOPPED) {
                return a;
            }
            return say_unit(text + best, n - best, w, depth + 1, spoken, dropped);
        }
        return r;
    }
    return SY_FAIL;
}

static void say_task(void *arg)
{
    char *text = arg;
    work_t w = { 0 };
    int spoken = 0, dropped = 0;
    const int64_t t0 = esp_timer_get_time();
    if (work_alloc(&w) && open_all()) {
        rs_reset();
        size_t n = strlen(text);
        size_t start = 0;
        sy_t r = SY_OK;
        for (size_t i = 0; i <= n && r != SY_STOPPED; i += i < n ? utf8_len((unsigned char)text[i]) : 1) {
            if (i == n || is_end(text + i)) {
                size_t end = i < n ? i + utf8_len((unsigned char)text[i]) : n;
                r = say_unit(text + start, end - start, &w, 0, &spoken, &dropped);
                if (r != SY_OK && r != SY_STOPPED) {
                    printf("@msw.say skipped a sentence (%s)\n", r == SY_TOO_LONG ? "too long" : "failed");
                }
                start = end;
            }
        }
        if (r != SY_STOPPED) {
            rs_run(true);
        }
        if (!atomic_load(&s_playing) && atomic_load(&s_ring_w) != atomic_load(&s_ring_r)) {
            atomic_store(&s_playing, true);   /* short: under the pre-roll */
            if (!atomic_load(&s_reply)) {
                muse_voice_player_kick();
            }
        }
    }
    printf("@msw.say made %d parts in %.2f s: %lld samples in, %lld out at 16 kHz; %d characters left out, %s; peak %.3f (limit %.3f), limited %d\n",
           spoken, (esp_timer_get_time() - t0) / 1e6, (long long)(s_rs_in0 + s_rs_len), (long long)s_rs_out, dropped,
           atomic_load(&s_say_stop) ? "stopped" : "ok", s_peak, s_limit, s_clipped);
    work_free(&w);
    free(text);
    atomic_store(&s_say_active, false);
    /* The voice task plays the rest; wait for it, so the next say starts clean. */
    unsigned last_r = atomic_load(&s_ring_r);
    int64_t moved = esp_timer_get_time();
    while (!pl_done() && !atomic_load(&s_say_stop)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (atomic_load(&s_ring_r) != last_r) {
            last_r = atomic_load(&s_ring_r);
            moved = esp_timer_get_time();
        } else if (esp_timer_get_time() - moved > 30 * 1000000LL) {
            printf("@msw.say the voice task did not play it for 30 s: dropped\n");
            pl_stop();
        }
    }
    atomic_store(&s_playing, false);
    printf("@msw.say done\n");
    fflush(stdout);
    atomic_store(&s_busy, false);
    msw_task_delete_self();
}

static bool start_task(TaskFunction_t fn, char *text)
{
    BaseType_t ok = s_cfg.core < 0
        ? msw_task_create(fn, "msw_tts", TASK_STACK, text, s_cfg.prio, NULL)
        : msw_task_create_pinned(fn, "msw_tts", TASK_STACK, text, s_cfg.prio, NULL, s_cfg.core);
    return ok == pdPASS;
}

/* Speaks text (UTF-8, Japanese) and returns at once. NULL when it starts, else
 * why not: "empty", "speaker_off", "busy" (already speaking) or "out_of_memory".
 * main/ calls this through a weak reference (Muse's voice.say command). */
/* A copy of a reply that reads better: no URLs, Markdown marks or emoji (the
 * model would leave them out one at a time, each costing a G2P pass). */
static void clean_reply(char *s)
{
    char *o = s;
    for (const char *p = s; *p;) {
        if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
            while (*p && *p != ' ' && *p != '\n' && (unsigned char)*p < 0x80) {
                p++;
            }
            continue;
        }
        size_t k = utf8_len((unsigned char)*p);
        if (k == 4 || strchr("*#`|>_~[]<", *p)) {
            for (size_t j = 0; j < k && *p; j++) {
                p++;
            }
            continue;
        }
        for (size_t j = 0; j < k && *p; j++) {
            *o++ = *p++;
        }
    }
    *o = 0;
}

/* Starts speaking text; reply: the chat session reads the ring (else the voice
 * task's player plays it when idle). NULL when it starts, else why not. */
static const char *begin(const char *text, bool reply)
{
    const char *tag = reply ? "@msw.reply" : "@msw.say";
    const char *why = NULL;
    if (!text || !text[0]) {
        why = "empty";
    } else if (!muse_settings_speaker_on()) {
        why = "speaker_off";
    }
    if (!why && !reply && atomic_load(&s_reply) && atomic_load(&s_busy)) {
        /* A say takes over from a reply: stop it and wait for its task to end. */
        printf("%s: stopping the reply for it\n", tag);
        pl_stop();
        for (int i = 0; i < 300 && atomic_load(&s_busy); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    bool idle = false;
    if (!why && !atomic_compare_exchange_strong(&s_busy, &idle, true)) {
        why = "busy";
    }
    if (why) {
        printf("%s not started: %s\n", tag, why);
        return why;
    }
    char *copy = heap_caps_malloc(strlen(text) + 1, MALLOC_CAP_SPIRAM);
    if (!copy || !rs_init()) {
        free(copy);
        atomic_store(&s_busy, false);
        printf("%s not started: out_of_memory\n", tag);
        return "out_of_memory";
    }
    strcpy(copy, text);
    if (reply) {
        clean_reply(copy);
    }
    atomic_store(&s_say_stop, false);
    atomic_store(&s_playing, false);
    atomic_store(&s_ring_r, atomic_load(&s_ring_w));
    atomic_store(&s_reply, reply);
    atomic_store(&s_say_active, true);
    if (!reply) {
        atomic_fetch_add(&s_says, 1);
    }
    /* The player is set once at start-up (msw_tts_register): the mini apps wrap
     * it to mix their sound in, and setting it again here unwrapped them. */
    if (!start_task(say_task, copy)) {
        free(copy);
        atomic_store(&s_say_active, false);
        atomic_store(&s_busy, false);
        printf("%s not started: out_of_memory (task)\n", tag);
        return "out_of_memory";
    }
    return NULL;
}

/* Speaks text (UTF-8, Japanese) and returns at once. NULL when it starts, else
 * why not: "empty", "speaker_off", "busy" (already speaking) or "out_of_memory".
 * A reply being spoken stops for it, and the rest of that turn's replies stay
 * unspoken (muse_chat_session.cpp). main/ calls this through a weak reference
 * (Muse's voice.say command). */
const char *msw_tts_say(const char *text)
{
    return begin(text, false);
}

void msw_tts_stop(void)
{
    if (atomic_load(&s_say_active) || atomic_load(&s_playing)) {
        pl_stop();
    }
}

/* --- replies: the chat session speaks Muse's messages through these ---------------- */

static bool rv_start(const char *text)
{
    return begin(text, true) == NULL;
}

static int rv_read(int16_t *mono, size_t max)
{
    if (!atomic_load(&s_playing)) {
        /* Before the pre-roll: wait, unless nothing is coming. */
        return atomic_load(&s_say_active) || atomic_load(&s_ring_w) != atomic_load(&s_ring_r) ? 0 : -1;
    }
    size_t n = pl_read(mono, max);
    return n ? (int)n : pl_done() ? -1 : 0;
}

static uint32_t rv_says(void)
{
    return atomic_load(&s_says);
}

static bool rv_say_live(void)
{
    return !atomic_load(&s_reply) && (atomic_load(&s_say_active) || atomic_load(&s_ring_w) != atomic_load(&s_ring_r));
}

static void rv_cancel(void)
{
    msw_tts_stop();
}

static const muse_reply_voice_t s_reply_voice = { rv_start, rv_read, rv_says, rv_say_live, rv_cancel };

/* --- serial commands ------------------------------------------------------------- */

static void print_cfg(void)
{
    printf("@msw.tts cfg arena=%s model=%s prio=%d core=%s%d rep=%d quiet=%d gain=%.2f (arena %u B)\n", s_cfg.arena_int ? "int" : "psram",
           s_cfg.model_psram ? "psram" : "flash", s_cfg.prio, s_cfg.core < 0 ? "any" : "", s_cfg.core < 0 ? 0 : s_cfg.core,
           s_cfg.rep, s_cfg.quiet, s_cfg.gain, (unsigned)arena_bytes());
}

static void set_cfg(char *args)
{
    for (char *tok = strtok(args, " "); tok; tok = strtok(NULL, " ")) {
        char *v = strchr(tok, '=');
        if (!v) {
            continue;
        }
        *v++ = 0;
        if (!strcmp(tok, "arena")) {
            s_cfg.arena_int = !strcmp(v, "int");
        } else if (!strcmp(tok, "model")) {
            s_cfg.model_psram = !strcmp(v, "psram");
        } else if (!strcmp(tok, "prio")) {
            s_cfg.prio = atoi(v);
        } else if (!strcmp(tok, "core")) {
            s_cfg.core = !strcmp(v, "any") ? -1 : atoi(v);
        } else if (!strcmp(tok, "quiet")) {
            s_cfg.quiet = atoi(v) != 0;
        } else if (!strcmp(tok, "gain")) {
            s_cfg.gain = strtof(v, NULL);
        } else if (!strcmp(tok, "rep")) {
            s_cfg.rep = atoi(v) < 1 ? 1 : atoi(v);
        }
    }
}

void msw_console_add(bool (*fn)(const char *line));

bool msw_tts_console(const char *line);

/* Linked in by "-u msw_tts_console" (CMakeLists.txt); registers at start-up. */
__attribute__((constructor)) static void msw_tts_register(void)
{
    msw_console_add(msw_tts_console);
    muse_reply_voice_set(&s_reply_voice);   /* Muse's replies are spoken with this voice */
    muse_voice_set_player(&s_player);       /* before the mini apps wrap it (msw_sound_init) */
}

bool msw_tts_console(const char *line)
{
    if (!strncmp(line, "msw.tts.cfg", 11)) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", line + 11);
        set_cfg(buf);
        print_cfg();
        printf("@msw.tts done\n");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "msw.say.stop")) {
        msw_tts_stop();
        printf("@msw.say stopping\n");
        return true;
    }
    if (!strncmp(line, "msw.say ", 8)) {
        if (msw_tts_say(line + 8)) {
            printf("@msw.say done\n");
        }
        fflush(stdout);
        return true;
    }
    if (strncmp(line, "msw.tts ", 8)) {
        return false;
    }
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_busy, &idle, true)) {
        printf("@msw.tts error busy\n@msw.tts done\n");
        return true;
    }
    char *text = strdup(line + 8);
    if (!text || !start_task(bench_task, text)) {
        free(text);
        atomic_store(&s_busy, false);
        printf("@msw.tts error task\n@msw.tts done\n");
    }
    return true;
}
