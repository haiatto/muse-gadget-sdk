/*
 * museStopWatch mini apps: the functions an app calls (include/msw_app.h),
 * except drawing (msw_app_gfx.c) and sound (msw_app_sound.c).
 *
 * Everything an app takes is freed when it ends: its memory (malloc and
 * friends are tracked here), its lvgl screen with all its widgets, lvgl
 * timers and animations that call into its code, its sounds and the IMU.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "lvgl.h"
#include "core/lv_global.h"
#include "misc/lv_anim_private.h"
#include "misc/lv_timer_private.h"

#include "muse_link.h"
#include "msw_text.h"
#include "msw_app_priv.h"

/* board_m5stack_stopwatch.c */
extern esp_err_t msw_board_imu_start(void);
extern bool msw_board_imu_read(int16_t out[6]);
extern void msw_board_imu_stop(void);
extern void msw_board_vibrate(int pct);
/* sanotts (weak: builds without it have no voice) */
extern const char *msw_tts_say(const char *text) __attribute__((weak));

/* ---- memory --------------------------------------------------------------- */

/* Each block the app takes carries this header, linked into one list. 16
 * bytes keep the app's pointer 16-byte aligned. */
typedef struct blk {
    struct blk *prev, *next;
    uint32_t size;
    uint32_t magic;
} blk_t;
#define BLK_MAGIC 0x4D53574Du

static blk_t *s_blocks;
static size_t s_used;
static portMUX_TYPE s_mem_mux = portMUX_INITIALIZER_UNLOCKED;

void *msw_app_malloc(size_t n)
{
    blk_t *b = heap_caps_malloc(sizeof(blk_t) + n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        return NULL;
    }
    b->size = (uint32_t)n;
    b->magic = BLK_MAGIC;
    b->prev = NULL;
    portENTER_CRITICAL(&s_mem_mux);
    b->next = s_blocks;
    if (s_blocks) {
        s_blocks->prev = b;
    }
    s_blocks = b;
    s_used += n;
    portEXIT_CRITICAL(&s_mem_mux);
    return b + 1;
}

void msw_app_free(void *p)
{
    if (!p) {
        return;
    }
    blk_t *b = (blk_t *)p - 1;
    if (b->magic != BLK_MAGIC) {
        msw_app_logf("free() of a pointer malloc() did not give: %p", p);
        return;
    }
    portENTER_CRITICAL(&s_mem_mux);
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        s_blocks = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    s_used -= b->size;
    portEXIT_CRITICAL(&s_mem_mux);
    b->magic = 0;
    heap_caps_free(b);
}

void *msw_app_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = msw_app_malloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void *msw_app_realloc(void *p, size_t n)
{
    if (!p) {
        return msw_app_malloc(n);
    }
    if (!n) {
        msw_app_free(p);
        return NULL;
    }
    blk_t *b = (blk_t *)p - 1;
    if (b->magic != BLK_MAGIC) {
        return NULL;
    }
    void *q = msw_app_malloc(n);
    if (q) {
        memcpy(q, p, b->size < n ? b->size : n);
        msw_app_free(p);
    }
    return q;
}

char *msw_app_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = msw_app_malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void free_all_blocks(void)
{
    portENTER_CRITICAL(&s_mem_mux);
    blk_t *b = s_blocks;
    s_blocks = NULL;
    size_t used = s_used;
    s_used = 0;
    portEXIT_CRITICAL(&s_mem_mux);
    int n = 0;
    while (b) {
        blk_t *next = b->next;
        b->magic = 0;
        heap_caps_free(b);
        b = next;
        n++;
    }
    if (n) {
        msw_app_logf("freed %d blocks (%u bytes) the app left", n, (unsigned)used);
    }
}

/* ---- life ------------------------------------------------------------------- */

static char s_id[MSW_APP_ID_MAX];
static uintptr_t s_code_lo, s_code_hi, s_code_d;
/* elf_loader rounds .text up to 4 bytes when mapping, so a .rodata that
 * starts right after an odd-sized .text has its first bytes relocated into
 * the code (old builds; sdk/build.py now aligns .rodata). Such pointers are
 * sent to the real .rodata copy. */
static uintptr_t s_text_vaddr, s_ro_vaddr, s_ro_addr;
static size_t s_ro_size;

void msw_api_set_rodata(uintptr_t text_vaddr, uintptr_t ro_vaddr, size_t ro_size, uintptr_t ro_addr)
{
    s_text_vaddr = text_vaddr;
    s_ro_vaddr = ro_vaddr;
    s_ro_size = ro_size;
    s_ro_addr = ro_addr;
}
static int64_t s_start_us;
static int s_fps = 30;
static atomic_bool s_exit;
static msw_tick_cb s_tick;
static void *s_tick_user;
static msw_button_cb s_button;
static void *s_button_user;
static msw_exit_cb s_exit_cb;
static void *s_exit_user;
static msw_reply_cb s_reply;
static void *s_reply_user;
static lv_obj_t *s_screen, *s_prev_screen;

/* Muse's replies, handed over on the app's tick. */
static char *s_reply_text;
static portMUX_TYPE s_reply_mux = portMUX_INITIALIZER_UNLOCKED;

void msw_on_tick(msw_tick_cb cb, void *user)
{
    s_tick = cb;
    s_tick_user = user;
}

void msw_set_fps(int fps)
{
    s_fps = fps < 1 ? 1 : fps > 60 ? 60 : fps;
}

void msw_on_button(msw_button_cb cb, void *user)
{
    s_button = cb;
    s_button_user = user;
}

void msw_on_exit(msw_exit_cb cb, void *user)
{
    s_exit_cb = cb;
    s_exit_user = user;
}

void msw_exit(void)
{
    atomic_store(&s_exit, true);
}

uint32_t msw_millis(void)
{
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

uint32_t msw_random(void)
{
    return esp_random();
}

void msw_log(const char *fmt, ...)
{
    char line[112];
    fmt = msw_api_readable(fmt, 1, "msw_log");
    if (!fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    msw_app_logf("%s: %s", s_id, line);
}

int msw_api_fps(void)
{
    return s_fps;
}

bool msw_api_exit_asked(void)
{
    return atomic_load(&s_exit);
}

void msw_api_button(int button, bool down)
{
    if (s_button) {
        s_button(button, down, s_button_user);
    }
}

static msw_pause_cb s_pause;
static void *s_pause_user;
static bool s_menu_own;

void msw_on_pause(msw_pause_cb cb, void *user)
{
    s_pause = cb;
    s_pause_user = user;
}

void msw_menu_own(bool own)
{
    s_menu_own = own;
}

bool msw_api_menu_owned(void)
{
    return s_menu_own;
}

void msw_api_pause(bool paused)
{
    if (s_pause) {
        s_pause(paused, s_pause_user);
    }
}

void msw_api_tick(uint32_t dt_ms)
{
    char *reply = NULL;
    portENTER_CRITICAL(&s_reply_mux);
    reply = s_reply_text;
    s_reply_text = NULL;
    portEXIT_CRITICAL(&s_reply_mux);
    if (reply) {
        if (s_reply) {
            s_reply(reply, s_reply_user);
        }
        free(reply);
    }
    if (s_tick) {
        s_tick(dt_ms, s_tick_user);
    }
}

void msw_api_call_exit_cb(void)
{
    if (s_exit_cb) {
        msw_exit_cb cb = s_exit_cb;
        s_exit_cb = NULL;
        cb(s_exit_user);
    }
}

void msw_on_muse_reply(msw_reply_cb cb, void *user)
{
    s_reply = cb;
    s_reply_user = user;
}

void msw_app_muse_reply(const char *text)
{
    if (!s_reply || !text || !msw_app_running()) {
        return;
    }
    char *copy = strdup(text);
    if (!copy) {
        return;
    }
    portENTER_CRITICAL(&s_reply_mux);
    char *old = s_reply_text;
    s_reply_text = copy;
    portEXIT_CRITICAL(&s_reply_mux);
    free(old);
}

bool msw_muse_online(void)
{
    return muse_link_state() == MUSE_LINK_ONLINE;
}

void muse_input_app_talk(bool pressed);   /* muse_input.c */

void msw_muse_talk(bool down)
{
    muse_input_app_talk(down);
}

/* Removes lvgl timers and animations whose callbacks are in the app's code. */
static bool in_app(const void *fn)
{
    uintptr_t a = (uintptr_t)fn;
    return a >= s_code_lo && a < s_code_hi;
}

static void drop_app_callbacks(void)
{
    int timers = 0, anims = 0;
    lv_ll_t *tl = &LV_GLOBAL_DEFAULT()->timer_state.timer_ll;
    lv_timer_t *t = lv_ll_get_head(tl);
    while (t) {
        lv_timer_t *next = lv_ll_get_next(tl, t);
        if (in_app((const void *)t->timer_cb)) {
            lv_timer_delete(t);
            timers++;
        }
        t = next;
    }
    lv_ll_t *al = &LV_GLOBAL_DEFAULT()->anim_state.anim_ll;
    lv_anim_t *a = lv_ll_get_head(al);
    for (int guard = 0; a && guard < 500; guard++) {
        lv_anim_t *next = lv_ll_get_next(al, a);
        if (in_app((const void *)a->exec_cb) || in_app((const void *)a->custom_exec_cb) ||
            in_app((const void *)a->completed_cb) || in_app((const void *)a->path_cb)) {
            a->deleted_cb = NULL;
            a->completed_cb = NULL;
            lv_anim_delete(a->var, a->exec_cb);
            anims++;
            next = lv_ll_get_head(al);    /* the delete can take others with it */
        }
        a = next;
    }
    if (timers || anims) {
        msw_app_logf("removed %d lvgl timers and %d animations the app left", timers, anims);
    }
}

static void overlay_reset(void);

/* ---- pointers from the app ----------------------------------------------------- */

#define WARNED_MAX 8
static const char *s_warned[WARNED_MAX];   /* fn names already logged this run */

static bool warn_once(const char *fn)
{
    for (int i = 0; i < WARNED_MAX; i++) {
        if (s_warned[i] == fn) {
            return false;
        }
        if (!s_warned[i]) {
            s_warned[i] = fn;
            return true;
        }
    }
    return false;
}

static bool firmware_can_read(uintptr_t a)
{
    const void *p = (const void *)a;
    return esp_ptr_byte_accessible(p) || esp_ptr_external_ram(p) || esp_ptr_in_drom(p);
}

const void *msw_api_readable(const void *p, size_t n, const char *fn)
{
    uintptr_t a = (uintptr_t)p;
    if (!p) {
        return NULL;
    }
    n = n ? n : 1;
    if (s_code_d && a >= s_code_lo && a < s_code_hi) {
        uintptr_t v = s_text_vaddr + (a - s_code_lo);
        if (s_ro_addr && v >= s_ro_vaddr && v - s_ro_vaddr < s_ro_size) {
            if (warn_once(fn)) {
                msw_app_logf("%s: %s got a .rodata pointer the loader put in the code (0x%08x); "
                             "read from .rodata. Rebuild the app with the latest sdk/build.py",
                             s_id, fn, (unsigned)a);
            }
            return (const void *)(s_ro_addr + (v - s_ro_vaddr));
        }
        if (warn_once(fn)) {
            msw_app_logf("%s: %s got a pointer into the app's code (0x%08x, offset 0x%x), read through its data copy; "
                         "data in code is slow and not readable by every function",
                         s_id, fn, (unsigned)a, (unsigned)(a - s_code_lo));
        }
        return (const void *)(a - s_code_lo + s_code_d);
    }
    if (!firmware_can_read(a) || !firmware_can_read(a + n - 1)) {
        if (warn_once(fn)) {
            msw_app_logf("%s: %s got a pointer it can't read (0x%08x): a bad or freed pointer? ignored",
                         s_id, fn, (unsigned)a);
        }
        return NULL;
    }
    return p;
}

void *msw_api_writable(void *p, size_t n, const char *fn)
{
    uintptr_t a = (uintptr_t)p;
    if (!p) {
        return NULL;
    }
    n = n ? n : 1;
    const void *e = (const void *)(a + n - 1);
    bool ok = (esp_ptr_byte_accessible(p) || esp_ptr_external_ram(p)) && (esp_ptr_byte_accessible(e) || esp_ptr_external_ram(e));
    if (!ok || (a >= s_code_d && a < s_code_d + (s_code_hi - s_code_lo))) {
        if (warn_once(fn)) {
            msw_app_logf("%s: %s got a pointer it can't write (0x%08x): a bad pointer, or a const array? ignored",
                         s_id, fn, (unsigned)a);
        }
        return NULL;
    }
    return p;
}

void msw_api_begin(const char *id, uintptr_t code_lo, uintptr_t code_hi, uintptr_t code_d)
{
    strlcpy(s_id, id, sizeof(s_id));
    s_code_lo = code_lo;
    s_code_hi = code_hi;
    s_code_d = code_d;
    memset(s_warned, 0, sizeof(s_warned));
    s_start_us = esp_timer_get_time();
    s_fps = 30;
    atomic_store(&s_exit, false);
    s_tick = NULL;
    s_button = NULL;
    s_exit_cb = NULL;
    s_reply = NULL;
    s_pause = NULL;
    s_menu_own = false;
    s_prev_screen = lv_screen_active();
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);
    msw_gfx_begin(s_screen);
}

void msw_api_end(void)
{
    s_code_d = 0;
    msw_gfx_end();
    overlay_reset();
    drop_app_callbacks();
    if (s_prev_screen) {
        lv_screen_load(s_prev_screen);
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_tick = NULL;
    s_button = NULL;
    s_reply = NULL;
    free_all_blocks();
    portENTER_CRITICAL(&s_reply_mux);
    char *old = s_reply_text;
    s_reply_text = NULL;
    portEXIT_CRITICAL(&s_reply_mux);
    free(old);
}

lv_obj_t *msw_screen(void)
{
    return s_screen;
}

LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_16)
LV_FONT_DECLARE(lv_font_montserrat_20)
LV_FONT_DECLARE(lv_font_montserrat_28)

const lv_font_t *msw_font(int size)
{
    switch (size) {
    case 14:
        return &lv_font_montserrat_14;
    case 20:
        return &lv_font_montserrat_20;
    case 28:
        return &lv_font_montserrat_28;
    case 0:
        return msw_font_cjk(&lv_font_montserrat_16);
    default:
        return &lv_font_montserrat_16;
    }
}

/* ---- touch --------------------------------------------------------------- */

bool msw_touch(int *x, int *y)
{
    lv_indev_t *in = lv_indev_get_next(NULL);
    while (in && lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) {
        in = lv_indev_get_next(in);
    }
    if (!in || lv_indev_get_state(in) != LV_INDEV_STATE_PRESSED) {
        return false;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (x) {
        *x = p.x;
    }
    if (y) {
        *y = p.y;
    }
    return true;
}

/* ---- vibration ----------------------------------------------------------- */

static esp_timer_handle_t s_vib_timer;

static void vib_off(void *arg)
{
    (void)arg;
    msw_board_vibrate(0);
}

void msw_vibrate(int ms, int strength)
{
    if (!s_vib_timer) {
        const esp_timer_create_args_t a = { .callback = vib_off, .name = "msw_vib" };
        if (esp_timer_create(&a, &s_vib_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_vib_timer);
    if (ms <= 0 || strength <= 0) {
        msw_board_vibrate(0);
        return;
    }
    msw_board_vibrate(strength > 100 ? 100 : strength);
    esp_timer_start_once(s_vib_timer, (uint64_t)(ms > 2000 ? 2000 : ms) * 1000);
}

/* ---- IMU ----------------------------------------------------------------- */

/*
 * Raw BMI270 axes to screen axes (msw_app.h: x right, y down, z out of the
 * screen). M5StopWatch-UserDemo maps display x to raw y and display y to raw
 * x. The signs come from >msw.imu on the device: set them so that face up
 * reads az = +1, and tilting the bottom of the screen down reads ay < 0.
 */
#define IMU_X_FROM_RAW 1    /* raw index for screen x */
#define IMU_Y_FROM_RAW 0
#define IMU_SX 1.0f
#define IMU_SY 1.0f
#define IMU_SZ 1.0f
#define ACC_LSB_PER_G 8192.0f      /* +-4 g */
#define GYR_LSB_PER_DPS 16.4f      /* +-2000 deg/s */

static bool s_imu_on;
static int64_t s_imu_last_us;
static msw_imu_t s_imu_last;

static bool imu_read(msw_imu_t *out, int16_t raw[6])
{
    if (!s_imu_on) {
        if (msw_board_imu_start() != ESP_OK) {
            return false;
        }
        s_imu_on = true;
    }
    if (!msw_board_imu_read(raw)) {
        return false;
    }
    out->ax = IMU_SX * raw[IMU_X_FROM_RAW] / ACC_LSB_PER_G;
    out->ay = IMU_SY * raw[IMU_Y_FROM_RAW] / ACC_LSB_PER_G;
    out->az = IMU_SZ * raw[2] / ACC_LSB_PER_G;
    out->gx = IMU_SX * raw[3 + IMU_X_FROM_RAW] / GYR_LSB_PER_DPS;
    out->gy = IMU_SY * raw[3 + IMU_Y_FROM_RAW] / GYR_LSB_PER_DPS;
    out->gz = IMU_SZ * raw[5] / GYR_LSB_PER_DPS;
    return true;
}

bool msw_imu(msw_imu_t *out)
{
    int64_t now = esp_timer_get_time();
    if (s_imu_on && now - s_imu_last_us < 5000) {    /* the sensor runs at 100 Hz */
        *out = s_imu_last;
        return true;
    }
    int16_t raw[6];
    if (!imu_read(&s_imu_last, raw)) {
        return false;
    }
    s_imu_last_us = now;
    *out = s_imu_last;
    return true;
}

extern bool msw_board_imu_steps(uint32_t *out);

int msw_steps(void)
{
    if (!s_imu_on) {
        if (msw_board_imu_start() != ESP_OK) {
            return -1;
        }
        s_imu_on = true;
    }
    uint32_t n;
    return msw_board_imu_steps(&n) ? (int)n : -1;
}

bool msw_imu_probe(msw_imu_t *out)
{
    int16_t raw[6];
    bool ok = imu_read(out, raw);
    if (ok) {
        printf("@msw.imu raw acc %d %d %d gyr %d %d %d\n", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    }
    if (!msw_app_running()) {
        msw_imu_release();
    }
    return ok;
}

void msw_imu_release(void)
{
    if (s_imu_on) {
        msw_board_imu_stop();
        s_imu_on = false;
    }
    msw_vibrate(0, 0);
}

/* ---- storage -------------------------------------------------------------- */

static bool valid_key(const char *key)
{
    size_t n = strnlen(key, 16);
    if (n == 0 || n > 15) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

typedef struct {
    const char *key;
    void *data;
    size_t n;
    bool put;
    int result;
} store_req_t;

static void store_io(void *arg)
{
    store_req_t *r = arg;
    char dir[64], path[96];
    snprintf(dir, sizeof(dir), MSW_APP_DIR "/data/%s", s_id);
    snprintf(path, sizeof(path), "%s/%s", dir, r->key);
    r->result = -1;
    if (r->put) {
        mkdir(dir, 0775);
        FILE *f = fopen(path, "wb");
        if (f) {
            bool ok = fwrite(r->data, 1, r->n, f) == r->n;
            r->result = fclose(f) == 0 && ok ? (int)r->n : -1;
        }
    } else {
        FILE *f = fopen(path, "rb");
        if (f) {
            r->result = (int)fread(r->data, 1, r->n, f);
            fclose(f);
        }
    }
}

/* The data goes through a copy in internal RAM: the service task writes
 * flash, and app memory is in PSRAM. */
bool msw_store_put(const char *key, const void *data, size_t n)
{
    key = msw_api_readable(key, 1, "msw_store_put");
    data = n ? msw_api_readable(data, n, "msw_store_put") : data;
    if (!key || !valid_key(key) || n > 4096 || (!data && n)) {
        msw_log("store_put: bad key or more than 4 KB");
        return false;
    }
    void *copy = heap_caps_malloc(n ? n : 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) {
        return false;
    }
    memcpy(copy, data, n);
    store_req_t r = { .key = key, .data = copy, .n = n, .put = true };
    msw_app_service(store_io, &r);
    heap_caps_free(copy);
    return r.result == (int)n;
}

int msw_store_get(const char *key, void *data, size_t cap)
{
    key = msw_api_readable(key, 1, "msw_store_get");
    data = cap ? msw_api_writable(data, cap > 4096 ? 4096 : cap, "msw_store_get") : data;
    if (!key || !valid_key(key) || (cap && !data)) {
        return -1;
    }
    size_t n = cap > 4096 ? 4096 : cap;
    void *copy = heap_caps_malloc(n ? n : 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) {
        return -1;
    }
    store_req_t r = { .key = key, .data = copy, .n = n };
    msw_app_service(store_io, &r);
    if (r.result > 0) {
        memcpy(data, copy, (size_t)r.result);
    }
    heap_caps_free(copy);
    return r.result;
}

/* ---- voice ----------------------------------------------------------------- */

const char *msw_say(const char *text)
{
    if (!msw_tts_say) {
        return "no_voice";
    }
    text = msw_api_readable(text, 1, "msw_say");
    return msw_tts_say(text);
}

/* ---- Muse avatar and bubble -------------------------------------------------- */

/*
 * Drawn two ways, like the app: as lvgl objects on the app's screen, or, in
 * frame drawing, as small pictures composited over the frame while it goes
 * to the panel (msw_overlay_draw). The avatar is a small face drawn here, not
 * Muse's full avatar: it must fit any app.
 */
#define AV_SIZE 64
#define BUBBLE_W 300
#define BUBBLE_MAX_H 120
#define KEY_COLOR 0x0821     /* not drawn */

static bool s_av_on;
static int s_av_x, s_av_y, s_av_mood;
static msw_gfx_t *s_av_pic;        /* frame drawing */
static msw_gfx_t *s_bub_pic;
static int s_bub_x, s_bub_y;
static int64_t s_bub_until;        /* 0: no bubble; INT64_MAX: until cleared */
static lv_obj_t *s_av_obj, *s_bub_obj, *s_bub_label;

static void face_draw(msw_gfx_t *g, int mood)
{
    msw_gfx_fill(g, KEY_COLOR);
    int c = AV_SIZE / 2;
    msw_gfx_fill_circle(g, c, c, c - 1, MSW_RGB(255, 255, 255));
    msw_gfx_fill_circle(g, c, c, c - 4, MSW_RGB(255, 196, 64));
    uint16_t ink = MSW_RGB(40, 30, 20);
    if (mood == 2) {
        msw_gfx_fill_circle(g, c - 11, c - 6, 5, ink);
        msw_gfx_fill_circle(g, c + 11, c - 6, 5, ink);
        msw_gfx_fill_circle(g, c, c + 13, 6, ink);
    } else {
        msw_gfx_fill_rect(g, c - 14, c - 10, 6, 10, ink);
        msw_gfx_fill_rect(g, c + 8, c - 10, 6, 10, ink);
        if (mood == 1) {
            msw_gfx_fill_circle(g, c, c + 8, 11, ink);
            msw_gfx_fill_rect(g, c - 12, c - 4, 24, 12, MSW_RGB(255, 196, 64));
        } else {
            msw_gfx_fill_rect(g, c - 9, c + 11, 18, 3, ink);
        }
    }
}

static void bubble_place(int h)
{
    if (s_av_on) {
        s_bub_x = s_av_x - BUBBLE_W / 2;
        s_bub_y = s_av_y > MSW_SCREEN_H / 2 ? s_av_y - AV_SIZE / 2 - 8 - h : s_av_y + AV_SIZE / 2 + 8;
    } else {
        s_bub_x = (MSW_SCREEN_W - BUBBLE_W) / 2;
        s_bub_y = MSW_SCREEN_H - 70 - h;
    }
    s_bub_x = s_bub_x < 40 ? 40 : s_bub_x > MSW_SCREEN_W - 40 - BUBBLE_W ? MSW_SCREEN_W - 40 - BUBBLE_W : s_bub_x;
    s_bub_y = s_bub_y < 40 ? 40 : s_bub_y > MSW_SCREEN_H - 40 - h ? MSW_SCREEN_H - 40 - h : s_bub_y;
}

extern bool msw_gfx_frame_mode(void);
extern int msw_gfx_text_wrapped(msw_gfx_t *g, int x, int y, int w, const char *utf8, uint16_t color);

static void overlay_lvgl_sync(void)
{
    if (!s_screen) {
        return;
    }
    if (s_av_on && !s_av_obj) {
        s_av_obj = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_av_obj);
        lv_obj_set_size(s_av_obj, AV_SIZE, AV_SIZE);
        lv_obj_set_style_radius(s_av_obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_av_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_av_obj, lv_color_hex(0xFFC440), 0);
        lv_obj_set_style_border_width(s_av_obj, 3, 0);
        lv_obj_set_style_border_color(s_av_obj, lv_color_white(), 0);
        lv_obj_t *l = lv_label_create(s_av_obj);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x281E14), 0);
        lv_obj_center(l);
        lv_obj_remove_flag(s_av_obj, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_av_obj) {
        if (s_av_on) {
            lv_obj_set_pos(s_av_obj, s_av_x - AV_SIZE / 2, s_av_y - AV_SIZE / 2);
            lv_label_set_text(lv_obj_get_child(s_av_obj, 0), s_av_mood == 1 ? "^ ^" : s_av_mood == 2 ? "o o" : "- -");
            lv_obj_remove_flag(s_av_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_av_obj);
        } else {
            lv_obj_add_flag(s_av_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void overlay_reset(void)
{
    s_av_on = false;
    s_bub_until = 0;
    if (s_av_pic) {
        msw_gfx_free(s_av_pic);
        s_av_pic = NULL;
    }
    if (s_bub_pic) {
        msw_gfx_free(s_bub_pic);
        s_bub_pic = NULL;
    }
    s_av_obj = s_bub_obj = s_bub_label = NULL;   /* children of the app screen: deleted with it */
}

void msw_avatar_show(int x, int y, int mood)
{
    s_av_on = true;
    s_av_x = x;
    s_av_y = y;
    s_av_mood = mood < 0 || mood > 2 ? 0 : mood;
    if (msw_gfx_frame_mode()) {
        if (!s_av_pic) {
            s_av_pic = msw_gfx_new(AV_SIZE, AV_SIZE);
        }
        if (s_av_pic) {
            face_draw(s_av_pic, s_av_mood);
        }
        msw_gfx_overlay_changed();
    } else {
        overlay_lvgl_sync();
    }
}

void msw_avatar_hide(void)
{
    s_av_on = false;
    if (msw_gfx_frame_mode()) {
        msw_gfx_overlay_changed();
    } else {
        overlay_lvgl_sync();
    }
}

void msw_bubble(const char *text, int ms)
{
    text = msw_api_readable(text, 1, "msw_bubble");
    if (!text || !text[0]) {
        s_bub_until = 0;
        if (s_bub_obj) {
            lv_obj_add_flag(s_bub_obj, LV_OBJ_FLAG_HIDDEN);
        }
        msw_gfx_overlay_changed();
        return;
    }
    s_bub_until = ms > 0 ? esp_timer_get_time() + (int64_t)ms * 1000 : INT64_MAX;
    if (msw_gfx_frame_mode()) {
        if (!s_bub_pic) {
            s_bub_pic = msw_gfx_new(BUBBLE_W, BUBBLE_MAX_H);
        }
        if (!s_bub_pic) {
            return;
        }
        msw_gfx_fill(s_bub_pic, KEY_COLOR);
        msw_gfx_t probe = *s_bub_pic;
        int h = msw_gfx_text_wrapped(NULL, 0, 0, BUBBLE_W - 24, text, 0) + 20;
        h = h > BUBBLE_MAX_H ? BUBBLE_MAX_H : h;
        probe.h = h;
        msw_gfx_fill_rect(&probe, 6, 0, BUBBLE_W - 12, h, MSW_RGB(255, 255, 255));
        msw_gfx_fill_rect(&probe, 0, 6, BUBBLE_W, h - 12, MSW_RGB(255, 255, 255));
        msw_gfx_fill_circle(&probe, 6, 6, 6, MSW_RGB(255, 255, 255));
        msw_gfx_fill_circle(&probe, BUBBLE_W - 7, 6, 6, MSW_RGB(255, 255, 255));
        msw_gfx_fill_circle(&probe, 6, h - 7, 6, MSW_RGB(255, 255, 255));
        msw_gfx_fill_circle(&probe, BUBBLE_W - 7, h - 7, 6, MSW_RGB(255, 255, 255));
        msw_gfx_text_wrapped(&probe, 12, 10, BUBBLE_W - 24, text, MSW_RGB(20, 20, 20));
        s_bub_pic->h = h;
        bubble_place(h);
        msw_gfx_overlay_changed();
        return;
    }
    if (!s_screen) {
        return;
    }
    if (!s_bub_obj) {
        s_bub_obj = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_bub_obj);
        lv_obj_set_width(s_bub_obj, BUBBLE_W);
        lv_obj_set_height(s_bub_obj, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_bub_obj, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_bub_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_bub_obj, 12, 0);
        lv_obj_set_style_pad_all(s_bub_obj, 10, 0);
        lv_obj_remove_flag(s_bub_obj, LV_OBJ_FLAG_CLICKABLE);
        s_bub_label = lv_label_create(s_bub_obj);
        lv_obj_set_width(s_bub_label, BUBBLE_W - 20);
        lv_label_set_long_mode(s_bub_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_bub_label, msw_font(0), 0);
        lv_obj_set_style_text_color(s_bub_label, lv_color_hex(0x141414), 0);
    }
    lv_label_set_text(s_bub_label, text);
    lv_obj_update_layout(s_bub_obj);
    int h = lv_obj_get_height(s_bub_obj);
    bubble_place(h);
    lv_obj_set_pos(s_bub_obj, s_bub_x, s_bub_y);
    lv_obj_remove_flag(s_bub_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bub_obj);
}

bool msw_overlay_active(void)
{
    if (s_bub_until && esp_timer_get_time() > s_bub_until) {
        s_bub_until = 0;
        if (s_bub_obj) {
            lv_obj_add_flag(s_bub_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return (s_av_on && s_av_pic) || (s_bub_until && s_bub_pic);
}

static void overlay_row(const msw_gfx_t *pic, int px, int py, uint16_t *row, int y, int x1, int x2)
{
    if (!pic || y < py || y >= py + pic->h) {
        return;
    }
    const uint16_t *src = pic->px + (y - py) * pic->stride;
    int a = px > x1 ? px : x1;
    int b = px + pic->w < x2 ? px + pic->w : x2;
    for (int x = a; x < b; x++) {
        uint16_t c = src[x - px];
        if (c != KEY_COLOR) {
            row[x - x1] = c;
        }
    }
}

/* One row of the frame, x1..x2 (end exclusive), in native order, before it
 * is swapped for the panel. */
void msw_overlay_draw(uint16_t *row_px, int y, int x1, int x2)
{
    if (s_av_on) {
        overlay_row(s_av_pic, s_av_x - AV_SIZE / 2, s_av_y - AV_SIZE / 2, row_px, y, x1, x2);
    }
    if (s_bub_until) {
        overlay_row(s_bub_pic, s_bub_x, s_bub_y, row_px, y, x1, x2);
    }
}
