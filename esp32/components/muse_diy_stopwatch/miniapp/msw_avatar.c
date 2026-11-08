/*
 * museStopWatch avatars (idea 009): Muse drawn by a small ELF in place of
 * the official avatar (avatar/muse_pixel.c).
 *
 * An avatar is a package like an app ("MSWV", <id>.mswava; msw_app_core.c
 * stores and loads it). Its app_main calls msw_avatar_define with a render
 * function that fills Muse's 64 x 64 grid and a 256-colour palette; the
 * official renderer's hook (msw_avatar_hook_render) calls it each frame and
 * the firmware still blows the grid up to the screen. One avatar is loaded at
 * a time and the choice is kept in NVS ("msw" / "avatar").
 *
 * The official avatar is always one step away:
 *  - a crash while the avatar draws restarts the device; RTC memory says so
 *    and the next boot comes up with the official avatar;
 *  - drawing slower than SLOW_MS on average over WINDOW_US sends it back too;
 *  - the picker (swipe up from Muse's face) lists "公式" first.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "nvs.h"

#include "muse_pixel.h"
#include "msw_stack.h"
#include "msw_text.h"
#include "msw_app_priv.h"

#define NVS_NS "msw"
#define NVS_KEY "avatar"
#define RTC_MAGIC 0x4D535756u    /* "MSWV" */
#define WINDOW_US (3 * 1000 * 1000)
#define SLOW_MS 25               /* average per frame over a window: back to the official avatar */

typedef struct {
    esp_elf_t elf;
    char id[MSW_APP_ID_MAX];
    char name[MSW_APP_NAME_MAX + 1];
    void (*render)(const msw_avatar_pose_t *pose, uint8_t *grid, uint16_t *palette);
    uint32_t (*accent)(int mode);
    bool fresh;                  /* clear the grid and palette before its first frame */
} avatar_t;

static avatar_t *s_cur;                  /* on screen; changed with s_lock held */
static SemaphoreHandle_t s_lock;         /* held while the avatar's code runs */
static SemaphoreHandle_t s_use_lock;     /* one change of avatar at a time */
static EXT_RAM_BSS_ATTR uint16_t s_pal[MSW_AVATAR_COLORS];
static EXT_RAM_BSS_ATTR uint16_t s_dim[MSW_AVATAR_COLORS];
static const msw_avatar_def_t *s_def;    /* set by msw_avatar_define in app_main */
static volatile uint32_t s_ver;          /* bumps when the choice or the list changes */

/* Draw timing of the avatar on screen. */
static int64_t s_win_start;
static uint32_t s_win_frames;
static int64_t s_win_us, s_max_us;
static float s_last_avg_ms = -1;

/* Which avatar's code was running, kept over a crash's restart. */
typedef struct {
    uint32_t magic;
    uint32_t inside;             /* 1 in render, 2 in accent, 3 in app_main */
    char id[MSW_APP_ID_MAX];
} mark_t;
static RTC_NOINIT_ATTR mark_t s_mark;

/* The list the picker shows (by name), refreshed when the store changes. */
static msw_app_info_t *s_list;           /* PSRAM, MSW_AVATAR_MAX */
static int s_nlist;

/* ---- the choice in NVS (on the service task: flash) ---------------------- */

static char s_saved[MSW_APP_ID_MAX];

static void save_choice(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, s_saved);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void read_choice(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    size_t n = sizeof(s_saved);
    s_saved[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, NVS_KEY, s_saved, &n) != ESP_OK) {
            s_saved[0] = '\0';
        }
        nvs_close(h);
    }
}

static void save(const char *id)
{
    strlcpy(s_saved, id ? id : "", sizeof(s_saved));
    msw_app_service(save_choice, NULL);
}

/* ---- the hooks in avatar/muse_pixel.c ------------------------------------ */

static inline void mark(uint32_t inside)
{
    s_mark.inside = inside;
}

static inline uint16_t dim565(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (uint16_t)(((r * 184 >> 8) << 11) | ((g * 184 >> 8) << 5) | (b * 184 >> 8));
}

static void retire(avatar_t *a, const char *why);

bool msw_avatar_hook_render(const muse_pose_t *p, uint8_t *grid, const uint16_t **pal, const uint16_t **dim)
{
    if (!s_cur || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    avatar_t *a = s_cur;
    if (!a) {
        xSemaphoreGive(s_lock);
        return false;
    }
    if (a->fresh) {
        memset(grid, 0, MSW_AVATAR_W * MSW_AVATAR_H);
        memset(s_pal, 0, sizeof(s_pal));
        a->fresh = false;
        s_win_start = esp_timer_get_time();
        s_win_frames = 0;
        s_win_us = s_max_us = 0;
    }
    msw_avatar_pose_t pose = {
        .mode = p->mode, .t = p->t, .mode_t = p->mode_t,
        .level = p->level, .happy = p->happy, .working = p->working,
    };
    int64_t t0 = esp_timer_get_time();
    mark(1);
    a->render(&pose, grid, s_pal);
    mark(0);
    int64_t t1 = esp_timer_get_time();
    for (int i = 0; i < MSW_AVATAR_COLORS; i++) {
        s_dim[i] = dim565(s_pal[i]);
    }
    int64_t took = t1 - t0;
    s_win_frames++;
    s_win_us += took;
    s_max_us = took > s_max_us ? took : s_max_us;
    if (t1 - s_win_start >= WINDOW_US) {
        float avg = s_win_us / 1000.0f / s_win_frames;
        s_last_avg_ms = avg;
        if (avg > SLOW_MS) {
            char why[120];
            snprintf(why, sizeof(why), "drawing took %.1f ms a frame on average (%.1f at most); keep it under 5 ms",
                     avg, s_max_us / 1000.0);
            retire(a, why);       /* s_cur is NULL now: the official avatar from the next frame */
            xSemaphoreGive(s_lock);
            return false;
        }
        s_win_start = t1;
        s_win_frames = 0;
        s_win_us = 0;
    }
    xSemaphoreGive(s_lock);
    *pal = s_pal;
    *dim = s_dim;
    return true;
}

bool msw_avatar_hook_accent(muse_mode_t mode, uint32_t *rgb)
{
    if (!s_cur || !s_cur->accent || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    avatar_t *a = s_cur;
    bool ok = a && a->accent;
    if (ok) {
        mark(2);
        *rgb = a->accent(mode) & 0xFFFFFF;
        mark(0);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* ---- loading -------------------------------------------------------------- */

void msw_avatar_define(const msw_avatar_def_t *def)
{
    s_def = def;
}

static void unload(avatar_t *a)
{
    if (a) {
        msw_app_unload_avatar(&a->elf);
        free(a);
    }
}

/* Off the screen, unloaded later on the service task (we may be inside the
 * screen's drawing, which must not wait for flash). s_lock is held. */
static avatar_t *s_retired;

static void retired_job(void *arg)
{
    (void)arg;
    /* Not s_use_lock: a change of avatar holding it waits for this task. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    avatar_t *a = s_retired;
    s_retired = NULL;
    xSemaphoreGive(s_lock);
    if (a) {
        if (!strcmp(s_saved, a->id)) {
            strlcpy(s_saved, "", sizeof(s_saved));
            save_choice(NULL);
        }
        unload(a);
    }
}

static void retire(avatar_t *a, const char *why)
{
    msw_app_logf("avatar %s: %s: back to the official avatar", a->id, why);
    s_cur = NULL;
    s_retired = a;
    s_ver++;
    msw_app_service_async(retired_job, NULL);
}

/* The avatar's code address -> where its bytes are readable as data. */
static const void *readable(const avatar_t *a, const void *p)
{
    uintptr_t lo = esp_elf_runtime_base((esp_elf_t *)&a->elf);
    uintptr_t hi = lo + a->elf.text_alloc_size;
    uintptr_t v = (uintptr_t)p;
    return v >= lo && v < hi ? (const void *)(v - lo + (uintptr_t)a->elf.ptext) : p;
}

static bool in_code(const avatar_t *a, const void *fn)
{
    uintptr_t lo = esp_elf_runtime_base((esp_elf_t *)&a->elf);
    uintptr_t v = (uintptr_t)fn;
    return v >= lo && v < lo + a->elf.text_alloc_size;
}

static void refresh_list(void)
{
    if (!s_list) {
        s_list = heap_caps_calloc(MSW_AVATAR_MAX, sizeof(*s_list), MALLOC_CAP_SPIRAM);
        if (!s_list) {
            return;
        }
    }
    static EXT_RAM_BSS_ATTR msw_app_info_t tmp[MSW_AVATAR_MAX];
    int n = msw_app_list_avatars(tmp, MSW_AVATAR_MAX);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_list, tmp, n * sizeof(*tmp));
    s_nlist = n;
    s_ver++;
    xSemaphoreGive(s_lock);
}

/* Loads an avatar and runs its app_main. NULL with *code and msg on failure. */
static avatar_t *load_one(const char *id, const char **code, char *msg, size_t msg_len)
{
    *code = NULL;
    avatar_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_SPIRAM);
    if (!a) {
        *code = "out_of_memory";
        snprintf(msg, msg_len, "out of memory");
        return NULL;
    }
    strlcpy(a->id, id, sizeof(a->id));
    if (!msw_app_load_avatar(id, &a->elf, a->name, code, msg, msg_len)) {
        free(a);
        msw_app_logf("avatar %s: %s", id, msg);
        return NULL;
    }
    char *argv[] = { a->id, NULL };
    xSemaphoreTake(s_lock, portMAX_DELAY);   /* s_def and the crash mark are shared */
    strlcpy(s_mark.id, a->id, sizeof(s_mark.id));
    s_mark.magic = RTC_MAGIC;
    s_def = NULL;
    mark(3);
    msw_app_init_arrays(&a->elf);
    int rc = a->elf.entry(1, argv);
    mark(0);
    const msw_avatar_def_t *def = s_def ? readable(a, s_def) : NULL;
    if (s_cur) {
        strlcpy(s_mark.id, s_cur->id, sizeof(s_mark.id));
    }
    xSemaphoreGive(s_lock);
    if (rc != 0 || !def) {
        *code = "not_an_avatar";
        snprintf(msg, msg_len, rc ? "app_main returned %d" : "app_main did not call msw_avatar_define", rc);
    } else {
        a->render = def->render;
        a->accent = def->accent;
        if (!a->render || !in_code(a, (const void *)a->render)
            || (a->accent && !in_code(a, (const void *)a->accent))) {
            *code = "not_an_avatar";
            snprintf(msg, msg_len, "render (and accent) must be functions of the avatar");
        }
    }
    if (*code) {
        msw_app_logf("avatar %s: %s", id, msg);
        unload(a);
        return NULL;
    }
    a->fresh = true;
    return a;
}

/* id NULL or "official": the official avatar. */
static bool use(const char *id, bool keep, const char **code, char *msg, size_t msg_len)
{
    *code = NULL;
    if (!s_lock) {
        *code = "not_ready";
        snprintf(msg, msg_len, "the apps volume is not ready yet");
        return false;
    }
    bool official = !id || !id[0] || !strcmp(id, "official");
    avatar_t *a = NULL;
    if (!official) {
        a = load_one(id, code, msg, msg_len);
        if (!a) {
            return false;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    avatar_t *old = s_cur;
    s_cur = a;
    if (a) {
        strlcpy(s_mark.id, a->id, sizeof(s_mark.id));   /* whose code the screen now runs */
        s_mark.magic = RTC_MAGIC;
    }
    s_last_avg_ms = -1;
    s_ver++;
    xSemaphoreGive(s_lock);
    unload(old);
    if (keep) {
        save(official ? NULL : id);
    }
    msw_app_logf("avatar: %s", official ? "official" : id);
    return true;
}

bool msw_avatar_use(const char *id, const char **code, char *msg, size_t msg_len)
{
    if (!s_use_lock) {
        *code = "not_ready";
        snprintf(msg, msg_len, "the apps volume is not ready yet");
        return false;
    }
    xSemaphoreTake(s_use_lock, portMAX_DELAY);
    bool ok = use(id, true, code, msg, msg_len);
    xSemaphoreGive(s_use_lock);
    return ok;
}

const char *msw_avatar_current(char *name, size_t name_len, float *avg_ms)
{
    static char id[MSW_APP_ID_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(id, s_cur ? s_cur->id : "official", sizeof(id));
    if (name) {
        strlcpy(name, s_cur ? s_cur->name : "公式", name_len);
    }
    if (avg_ms) {
        *avg_ms = s_cur ? s_last_avg_ms : -1;
    }
    xSemaphoreGive(s_lock);
    return id;
}

int msw_avatar_list(msw_app_info_t *out, int max)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_nlist < max ? s_nlist : max;
    memcpy(out, s_list, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

void msw_avatar_installed(const char *id)
{
    if (!s_use_lock) {
        return;
    }
    refresh_list();
    xSemaphoreTake(s_use_lock, portMAX_DELAY);
    if (id && s_cur && !strcmp(s_cur->id, id)) {
        const char *code;
        char msg[160];
        if (!use(id, false, &code, msg, sizeof(msg))) {   /* the new build on screen */
            use(NULL, true, &code, msg, sizeof(msg));
        }
    }
    xSemaphoreGive(s_use_lock);
}

void msw_avatar_removing(const char *id)
{
    if (!s_use_lock) {
        return;
    }
    xSemaphoreTake(s_use_lock, portMAX_DELAY);
    if (s_cur && !strcmp(s_cur->id, id)) {
        const char *code;
        char msg[80];
        use(NULL, true, &code, msg, sizeof(msg));
    }
    xSemaphoreGive(s_use_lock);
}

void msw_avatar_boot(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_use_lock = xSemaphoreCreateMutex();
    msw_app_service(read_choice, NULL);
    refresh_list();
    esp_reset_reason_t why = esp_reset_reason();
    bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
    if (s_mark.magic == RTC_MAGIC && s_mark.inside && crash) {
        s_mark.id[sizeof(s_mark.id) - 1] = '\0';
        static const char *const IN[] = { "", "render", "accent", "app_main" };
        msw_app_logf("avatar %s crashed the device (in its %s): back to the official avatar", s_mark.id,
                     IN[s_mark.inside < 4 ? s_mark.inside : 0]);
        if (!strcmp(s_saved, s_mark.id)) {
            save(NULL);
        }
    }
    s_mark.magic = 0;
    s_mark.inside = 0;
    if (s_saved[0]) {
        const char *code;
        char msg[160];
        char id[MSW_APP_ID_MAX];
        strlcpy(id, s_saved, sizeof(id));
        xSemaphoreTake(s_use_lock, portMAX_DELAY);
        if (!use(id, false, &code, msg, sizeof(msg))) {
            save(NULL);
        }
        xSemaphoreGive(s_use_lock);
    }
}

/* ---- app.avatar_preview: an avatar in one state, as a sheet of frames ---- */

/* A second copy of the avatar is loaded (the one on screen keeps its own
 * statics) and run at 25 frames a second from the start of the state,
 * after half a second of IDLE. Frames are taken evenly over `seconds` and
 * laid out in a grid, each frame the 64 x 64 grid blown up `scale` times (2). This
 * is the board's answer to the official avatar.py's GIFs: Muse sees every
 * state, not only the one the face shows now. */

typedef struct {
    const char *name;
    int mode;
    bool working, happy;
    float seconds;      /* default length */
} state_t;

static const state_t STATES[] = {
    { "boot", MSW_MODE_BOOT, false, false, 1.6f },
    { "idle", MSW_MODE_IDLE, false, false, 2.4f },
    { "listening", MSW_MODE_LISTENING, false, false, 2.4f },
    { "thinking", MSW_MODE_THINKING, false, false, 2.4f },
    { "working", MSW_MODE_THINKING, true, false, 2.4f },
    { "speaking", MSW_MODE_SPEAKING, false, false, 2.4f },
    { "happy", MSW_MODE_IDLE, false, true, 1.9f },
    { "error", MSW_MODE_ERROR, false, false, 1.6f },
    { "off", MSW_MODE_OFF, false, false, 1.4f },
};

const char *msw_avatar_state_names(void)
{
    return "boot, idle, listening, thinking, working, speaking, happy, error, off";
}

/* A voice for listening and speaking: syllables, and a pause each 1.6 s. */
static float synth_level(int mode, float t, float mode_t)
{
    if (mode == MSW_MODE_SPEAKING) {
        if (fmodf(mode_t, 1.6f) > 1.25f) {
            return 0;
        }
        float v = 0.55f + 0.35f * sinf(t * 11.0f) + 0.1f * sinf(t * 23.0f);
        return v < 0 ? 0 : v > 1 ? 1 : v;
    }
    if (mode == MSW_MODE_LISTENING) {
        float v = 0.3f + 0.25f * sinf(t * 7.0f);
        return v < 0 ? 0 : v;
    }
    return 0;
}

static uint8_t *encode(const uint16_t *img, int w, int h, int quality, size_t *n)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.subsampling = JPEG_SUBSAMPLE_444;   /* pixel art: keep the colours sharp */
    cfg.quality = quality;
    jpeg_enc_handle_t enc = NULL;
    *n = 0;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
        return NULL;
    }
    size_t cap = 128 * 1024;
    uint8_t *jpg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    int out = 0;
    if (jpg && jpeg_enc_process(enc, (const uint8_t *)img, w * h * 2, jpg, cap, &out) == JPEG_ERR_OK && out > 0) {
        *n = (size_t)out;
    }
    jpeg_enc_close(enc);
    if (!*n) {
        free(jpg);
        return NULL;
    }
    return jpg;
}

static void preview_task(void *arg)
{
    msw_avatar_preview_t *q = arg;
    const state_t *st = NULL;
    for (size_t i = 0; i < sizeof(STATES) / sizeof(STATES[0]); i++) {
        if (q->state && !strcmp(q->state, STATES[i].name)) {
            st = &STATES[i];
        }
    }
    uint8_t *grid = NULL;
    uint16_t *pal = NULL, *img = NULL;
    avatar_t *a = NULL;
    if (!st) {
        q->code = "bad_state";
        snprintf(q->msg, sizeof(q->msg), "state: one of %s", msw_avatar_state_names());
        goto done;
    }
    a = load_one(q->id, &q->code, q->msg, sizeof(q->msg));
    if (!a) {
        goto done;
    }
    int frames = q->frames < 1 ? 4 : q->frames > 9 ? 9 : q->frames;
    float seconds = q->seconds > 0 ? (q->seconds > 10 ? 10 : q->seconds) : st->seconds;
    int cols = frames == 1 ? 1 : frames <= 4 ? 2 : 3;
    int rows = (frames + cols - 1) / cols;
    int cell = q->scale >= 1 && q->scale <= 4 ? q->scale : 2;   /* screen pixels a grid cell */
    while (cell > 1 && cols * rows * cell * cell > 36) {
        cell--;                  /* at most 384 x 384 (the JPEG's 128 KB and PSRAM) */
    }
    int iw = cols * MSW_AVATAR_W * cell, ih = rows * MSW_AVATAR_H * cell;
    grid = heap_caps_calloc(MSW_AVATAR_W * MSW_AVATAR_H, 1, MALLOC_CAP_SPIRAM);
    pal = heap_caps_calloc(MSW_AVATAR_COLORS, 2, MALLOC_CAP_SPIRAM);
    img = heap_caps_calloc((size_t)iw * ih, 2, MALLOC_CAP_SPIRAM);
    if (!grid || !pal || !img) {
        q->code = "out_of_memory";
        snprintf(q->msg, sizeof(q->msg), "out of memory");
        goto done;
    }
    const float dt = 0.04f;
    float t = 100.0f;
    int64_t sum = 0, worst = 0;
    int calls = 0, got = 0;
    for (int f = -12; got < frames && f < 400; f++) {
        float mode_t = f < 0 ? 5.0f + (f + 12) * dt : f * dt;
        msw_avatar_pose_t pose = { .mode = f < 0 ? MSW_MODE_IDLE : st->mode, .t = t, .mode_t = mode_t };
        if (f >= 0) {
            pose.level = synth_level(st->mode, t, mode_t);
            pose.working = st->working ? 1.0f : 0.0f;
            if (st->happy) {
                pose.happy = mode_t < 0.3f ? mode_t / 0.3f : fmaxf(0, 1.0f - (mode_t - 0.3f) / 1.6f);
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_mark.id, a->id, sizeof(s_mark.id));
        s_mark.magic = RTC_MAGIC;
        int64_t t0 = esp_timer_get_time();
        mark(1);
        a->render(&pose, grid, pal);
        mark(0);
        int64_t took = esp_timer_get_time() - t0;
        if (s_cur) {
            strlcpy(s_mark.id, s_cur->id, sizeof(s_mark.id));
        }
        xSemaphoreGive(s_lock);
        sum += took;
        worst = took > worst ? took : worst;
        calls++;
        if (f >= 0 && mode_t + dt / 2 >= got * seconds / frames) {
            q->times[got] = mode_t;
            int cx = (got % cols) * MSW_AVATAR_W * cell, cy = (got / cols) * MSW_AVATAR_H * cell;
            for (int y = 0; y < MSW_AVATAR_H * cell; y++) {
                const uint8_t *row = grid + (y / cell) * MSW_AVATAR_W;
                uint16_t *o = img + (size_t)(cy + y) * iw + cx;
                for (int x = 0; x < MSW_AVATAR_W * cell; x++) {
                    uint16_t c = pal[row[x / cell]];
                    o[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            }
            got++;
        }
        t += dt;
    }
    q->frames_out = got;
    q->avg_ms = calls ? sum / 1000.0f / calls : 0;
    q->max_ms = worst / 1000.0f;
    q->jpg = encode(img, iw, ih, q->quality > 0 ? q->quality : 60, &q->n);
    q->w = iw;
    q->h = ih;
    if (!q->jpg) {
        q->code = "jpeg_failed";
        snprintf(q->msg, sizeof(q->msg), "could not make the picture");
    }
done:
    free(grid);
    free(pal);
    free(img);
    unload(a);
    xSemaphoreGive(q->done);
    msw_task_delete_self();
}

bool msw_avatar_preview(msw_avatar_preview_t *q)
{
    q->code = NULL;
    q->msg[0] = '\0';
    q->jpg = NULL;
    if (!s_lock) {
        q->code = "not_ready";
        snprintf(q->msg, sizeof(q->msg), "the apps volume is not ready yet");
        return false;
    }
    if (!q->id || !strcmp(q->id, "official")) {
        q->code = "bad_id";
        snprintf(q->msg, sizeof(q->msg), "give an installed avatar's id (the official one has no preview)");
        return false;
    }
    q->done = xSemaphoreCreateBinary();
    if (!q->done || msw_task_create(preview_task, "msw_avapre", 16 * 1024, q, 3, NULL) != pdPASS) {
        if (q->done) {
            vSemaphoreDelete(q->done);
        }
        q->code = "out_of_memory";
        snprintf(q->msg, sizeof(q->msg), "out of memory");
        return false;
    }
    xSemaphoreTake(q->done, portMAX_DELAY);
    vSemaphoreDelete(q->done);
    return q->jpg != NULL;
}

/* ---- the picker: swipe up from Muse's face (muse_ui.c, weak) -------------- */

LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_20)

static lv_obj_t *s_tile, *s_rows;
static uint32_t s_shown = UINT32_MAX;
static char s_tapped[MSW_APP_ID_MAX];

static void on_pick(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    strlcpy(s_tapped, id, sizeof(s_tapped));
}

static lv_obj_t *row(const char *label, const char *id, bool on)
{
    lv_obj_t *b = lv_button_create(s_rows);
    lv_obj_set_size(b, 280, 52);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? 0x3A7BD5 : 0x262A30), 0);
    lv_obj_set_style_border_width(b, on ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x46505A), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, msw_font_cjk(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, 240);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(l, "%s%s", on ? LV_SYMBOL_OK " " : "", label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, on_pick, LV_EVENT_CLICKED, (void *)id);
    return b;
}

static EXT_RAM_BSS_ATTR msw_app_info_t s_rows_info[MSW_AVATAR_MAX];

static void rebuild(void)
{
    lv_obj_clean(s_rows);
    const char *cur = msw_avatar_current(NULL, 0, NULL);
    row("公式", "official", !strcmp(cur, "official"));
    int n = msw_avatar_list(s_rows_info, MSW_AVATAR_MAX);
    for (int i = 0; i < n; i++) {
        row(s_rows_info[i].name[0] ? s_rows_info[i].name : s_rows_info[i].id, s_rows_info[i].id,
            !strcmp(cur, s_rows_info[i].id));
    }
    if (!n) {
        lv_obj_t *l = lv_label_create(s_rows);
        lv_obj_set_style_text_font(l, msw_font_cjk(&lv_font_montserrat_14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xA0A8B0), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, "ほかのアバターは\nMuse に作ってもらえます");
    }
}

static void picked_job(lv_timer_t *t)
{
    (void)t;
    if (s_tapped[0]) {
        char id[MSW_APP_ID_MAX];
        strlcpy(id, s_tapped, sizeof(id));
        s_tapped[0] = '\0';
        const char *code;
        char msg[160];
        if (!msw_avatar_use(id, &code, msg, sizeof(msg))) {
            s_shown = UINT32_MAX;
        } else {
            /* back to Muse's face, to see the new look */
            lv_obj_t *tv = lv_obj_get_parent(s_tile);
            int w = lv_obj_get_width(s_tile);
            if (w > 0) {
                lv_tileview_set_tile_by_index(tv, lv_obj_get_x(s_tile) / w, 0, LV_ANIM_ON);
            }
        }
    }
    if (s_ver != s_shown && s_lock) {
        s_shown = s_ver;
        rebuild();
    }
}

void msw_avatar_build(lv_obj_t *tile)
{
    s_tile = tile;
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(tile);
    lv_obj_set_style_text_font(title, msw_font_cjk(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "アバター");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    s_rows = lv_obj_create(tile);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_size(s_rows, 300, 320);
    lv_obj_align(s_rows, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_rows, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_rows, 10, 0);
    lv_obj_set_style_pad_bottom(s_rows, 60, 0);
    lv_obj_set_scroll_dir(s_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rows, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *hint = lv_label_create(tile);
    lv_obj_set_style_text_font(hint, msw_font_cjk(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x80888F), 0);
    lv_label_set_text(hint, LV_SYMBOL_DOWN " 下にスワイプでもどる");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -36);

    lv_timer_create(picked_job, 200, NULL);
}
