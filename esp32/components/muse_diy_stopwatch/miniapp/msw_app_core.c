/*
 * museStopWatch mini apps: store, loading, the app task, console, crashes.
 *
 * Apps are packages (msw_app_priv.h) in a FAT volume on the "apps"
 * partition (partitions_musestopwatch.csv), mounted at /apps. All flash
 * access runs on one service task whose stack is internal RAM: a flash
 * operation freezes the cache, and IDF asserts if the caller's stack is in
 * PSRAM (found on 2026-11-07). The ELF is relocated there too, into PSRAM,
 * where the app runs (CONFIG_ELF_LOADER_LOAD_PSRAM; the firmware itself runs
 * from PSRAM: CONFIG_SPIRAM_FETCH_INSTRUCTIONS).
 *
 * One app runs at a time, on its own task with its stack in PSRAM. Its
 * callbacks run with the display locked. Nothing protects the firmware from
 * an app's bugs: a crash restarts the device, and RTC memory remembers which
 * app was running so the next boot records it and does not start it again.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "muse_board.h"
#include "muse_state.h"
#include "msw_app_priv.h"

static const char *TAG = "msw_app";

#define LOG_LINES 48
#define LOG_LINE 128
#define APP_STACK (16 * 1024)
#define APP_PRIO 3
#define APP_CORE 1
#define SVC_STACK 6144
#define YELLOW_HOLD_MS 1500   /* quits an app that took the yellow button */
#define RTC_MAGIC 0x4D535741u   /* "MSWA" */

/* ---- log ---------------------------------------------------------------- */

static char (*s_log)[LOG_LINE];
static int s_log_head, s_log_count;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

void msw_app_logf(const char *fmt, ...)
{
    char line[LOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    if (!s_log) {
        return;
    }
    portENTER_CRITICAL(&s_log_mux);
    int at = (s_log_head + s_log_count) % LOG_LINES;
    memcpy(s_log[at], line, LOG_LINE);
    if (s_log_count < LOG_LINES) {
        s_log_count++;
    } else {
        s_log_head = (s_log_head + 1) % LOG_LINES;
    }
    portEXIT_CRITICAL(&s_log_mux);
}

size_t msw_app_log_text(char *buf, size_t cap)
{
    size_t n = 0;
    if (cap) {
        buf[0] = '\0';
    }
    if (!s_log || !cap) {
        return 0;
    }
    portENTER_CRITICAL(&s_log_mux);
    for (int i = 0; i < s_log_count && n + 1 < cap; i++) {
        int k = snprintf(buf + n, cap - n, "%s\n", s_log[(s_log_head + i) % LOG_LINES]);
        n += k < 0 ? 0 : (size_t)k;
    }
    portEXIT_CRITICAL(&s_log_mux);
    return n < cap ? n : cap - 1;
}

/* ---- the service task ----------------------------------------------------- */

typedef struct {
    void (*fn)(void *arg);
    void *arg;
    SemaphoreHandle_t done;
} svc_req_t;

static QueueHandle_t s_svc_q;
static TaskHandle_t s_svc_task;

static void svc_task(void *arg)
{
    (void)arg;
    svc_req_t r;
    for (;;) {
        if (xQueueReceive(s_svc_q, &r, portMAX_DELAY) == pdTRUE) {
            r.fn(r.arg);
            xSemaphoreGive(r.done);
        }
    }
}

void msw_app_service(void (*fn)(void *arg), void *arg)
{
    if (!s_svc_q || xTaskGetCurrentTaskHandle() == s_svc_task) {
        fn(arg);
        return;
    }
    svc_req_t r = { fn, arg, xSemaphoreCreateBinary() };
    if (!r.done) {
        return;
    }
    xQueueSend(s_svc_q, &r, portMAX_DELAY);
    xSemaphoreTake(r.done, portMAX_DELAY);
    vSemaphoreDelete(r.done);
}

/* ---- the store ------------------------------------------------------------ */

static bool s_mounted;
static msw_app_info_t *s_apps;    /* PSRAM, MSW_APP_MAX_APPS */
static int s_napps;
static SemaphoreHandle_t s_apps_lock;
static atomic_uint s_changes;

uint32_t msw_app_changes(void)
{
    return atomic_load(&s_changes);
}

static void mount(void *arg)
{
    (void)arg;
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    wl_handle_t wl = WL_INVALID_HANDLE;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MSW_APP_DIR, "apps", &cfg, &wl);
    s_mounted = err == ESP_OK;
    if (s_mounted) {
        mkdir(MSW_APP_DIR "/data", 0775);
        ESP_LOGI(TAG, "apps volume mounted in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGE(TAG, "apps volume: %s (no \"apps\" partition?)", esp_err_to_name(err));
    }
}

static bool valid_id(const char *id)
{
    size_t n = strnlen(id, MSW_APP_ID_MAX);
    if (n == 0 || n >= MSW_APP_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '_')) {
            return false;
        }
    }
    return true;
}

static void app_path(char *buf, size_t cap, const char *id, const char *ext)
{
    snprintf(buf, cap, MSW_APP_DIR "/%s.%s", id, ext);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const msw_app_info_t *)a)->name, ((const msw_app_info_t *)b)->name);
}

/* Rereads every package header and icon. On the service task. */
static void rescan(void *arg)
{
    (void)arg;
    msw_app_info_t *list = heap_caps_calloc(MSW_APP_MAX_APPS, sizeof(*list), MALLOC_CAP_SPIRAM);
    int n = 0;
    DIR *d = s_mounted && list ? opendir(MSW_APP_DIR) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MSW_APP_MAX_APPS) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".mswapp")) {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), MSW_APP_DIR "/%s", e->d_name);
        FILE *f = fopen(path, "rb");
        msw_app_pkg_t h;
        if (!f) {
            continue;
        }
        if (fread(&h, 1, sizeof(h), f) == sizeof(h) && !memcmp(h.magic, "MSWA", 4)) {
            msw_app_info_t *a = &list[n];
            memcpy(a->id, h.id, sizeof(a->id));
            a->id[sizeof(a->id) - 1] = '\0';
            memcpy(a->name, h.name, sizeof(a->name));
            a->name[sizeof(a->name) - 1] = '\0';
            a->ver = h.ver;
            a->api = h.api;
            struct stat st;
            a->bytes = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
            if (h.icon_w == MSW_APP_ICON && h.icon_h == MSW_APP_ICON && h.icon_size == MSW_APP_ICON * MSW_APP_ICON * 2) {
                a->icon = heap_caps_malloc(h.icon_size, MALLOC_CAP_SPIRAM);
                if (a->icon && fread(a->icon, 1, h.icon_size, f) != h.icon_size) {
                    free(a->icon);
                    a->icon = NULL;
                }
            }
            n++;
        }
        fclose(f);
    }
    if (d) {
        closedir(d);
    }
    if (list) {
        qsort(list, n, sizeof(*list), by_name);
    }
    xSemaphoreTake(s_apps_lock, portMAX_DELAY);
    msw_app_info_t *old = s_apps;
    int old_n = s_napps;
    s_apps = list;
    s_napps = list ? n : 0;
    xSemaphoreGive(s_apps_lock);
    for (int i = 0; old && i < old_n; i++) {
        free(old[i].icon);
    }
    free(old);
    atomic_fetch_add(&s_changes, 1);
}

int msw_app_list(msw_app_info_t *out, int max)
{
    if (!s_apps_lock) {
        return 0;
    }
    xSemaphoreTake(s_apps_lock, portMAX_DELAY);
    int n = s_napps < max ? s_napps : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_apps[i];     /* icons stay valid until the next rescan's swap */
    }
    xSemaphoreGive(s_apps_lock);
    return n;
}

/* ---- package checks ------------------------------------------------------- */

#define ELF_SHT_DYNSYM 11
#define ELF_STB_WEAK 2

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} elf_shdr_t;

typedef struct __attribute__((packed)) {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
} elf_sym_t;

/* Every symbol the ELF needs must be one the firmware gives. On failure,
 * msg lists the missing ones. */
static bool check_symbols(const uint8_t *elf, size_t n, char *msg, size_t msg_len)
{
    const elf_ehdr_t *eh = (const elf_ehdr_t *)elf;
    if (n < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->machine != 94 /* Xtensa */) {
        snprintf(msg, msg_len, "not an Xtensa ELF");
        return false;
    }
    if (eh->shoff + (size_t)eh->shnum * sizeof(elf_shdr_t) > n) {
        snprintf(msg, msg_len, "ELF section table out of the file");
        return false;
    }
    const elf_shdr_t *sh = (const elf_shdr_t *)(elf + eh->shoff);
    size_t used = 0;
    int missing = 0;
    msg[0] = '\0';
    for (int i = 0; i < eh->shnum; i++) {
        if (sh[i].type != ELF_SHT_DYNSYM || sh[i].link >= eh->shnum) {
            continue;
        }
        const elf_shdr_t *str = &sh[sh[i].link];
        if (sh[i].offset + sh[i].size > n || str->offset + str->size > n) {
            snprintf(msg, msg_len, "ELF symbols out of the file");
            return false;
        }
        const elf_sym_t *sym = (const elf_sym_t *)(elf + sh[i].offset);
        for (size_t k = 1; k < sh[i].size / sizeof(*sym); k++) {
            if (sym[k].shndx != 0 || sym[k].name >= str->size || (sym[k].info >> 4) == ELF_STB_WEAK) {
                continue;
            }
            const char *name = (const char *)elf + str->offset + sym[k].name;
            if (!name[0] || esp_elf_find_symbol(name)) {
                continue;
            }
            if (!missing++) {
                used = (size_t)snprintf(msg, msg_len, "the firmware has no: ");
            }
            if (used < msg_len) {
                used += (size_t)snprintf(msg + used, msg_len - used, "%s%s", missing > 1 ? ", " : "", name);
            }
        }
    }
    return missing == 0;
}

/* Header, sizes and CRC; the ELF must be loadable. */
static bool check_package(const uint8_t *data, size_t n, const char **code, char *msg, size_t msg_len)
{
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)data;
    *code = "bad_format";
    if (n < sizeof(*h) || memcmp(h->magic, "MSWA", 4)) {
        snprintf(msg, msg_len, "not a mini-app package (.mswapp)");
        return false;
    }
    if (sizeof(*h) + (size_t)h->icon_size + h->elf_size != n) {
        snprintf(msg, msg_len, "package size %u does not match its header", (unsigned)n);
        return false;
    }
    if (esp_rom_crc32_le(0, data + sizeof(*h), n - sizeof(*h)) != h->crc32) {
        snprintf(msg, msg_len, "package CRC mismatch (broken in transfer?)");
        return false;
    }
    char id[MSW_APP_ID_MAX];
    memcpy(id, h->id, sizeof(id));
    id[sizeof(id) - 1] = '\0';
    if (!valid_id(id)) {
        snprintf(msg, msg_len, "bad id \"%s\": use a-z, 0-9 and _", id);
        return false;
    }
    if (h->api > MSW_API_VERSION) {
        *code = "api_too_new";
        snprintf(msg, msg_len, "built for API %u; this firmware has API %d", h->api, MSW_API_VERSION);
        return false;
    }
    if (!check_symbols(data + sizeof(*h) + h->icon_size, h->elf_size, msg, msg_len)) {
        *code = "missing_symbol";
        return false;
    }
    *code = NULL;
    return true;
}

/* ---- install and remove --------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t n;
    const char *id;
    bool ok;
    char err[48];
} write_req_t;

static void write_package(void *arg)
{
    write_req_t *r = arg;
    char tmp[64], path[64];
    app_path(tmp, sizeof(tmp), r->id, "tmp");
    app_path(path, sizeof(path), r->id, "mswapp");
    FILE *f = s_mounted ? fopen(tmp, "wb") : NULL;
    if (!f) {
        snprintf(r->err, sizeof(r->err), s_mounted ? "cannot create the file" : "no apps volume");
        return;
    }
    bool ok = fwrite(r->data, 1, r->n, f) == r->n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        snprintf(r->err, sizeof(r->err), "write failed (full?)");
        return;
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        snprintf(r->err, sizeof(r->err), "rename failed");
        return;
    }
    r->ok = true;
    rescan(NULL);
}

bool msw_app_install(const uint8_t *data, size_t n, char *id_out, uint16_t *ver_out,
                     const char **code, char *msg, size_t msg_len)
{
    if (!check_package(data, n, code, msg, msg_len)) {
        return false;
    }
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)data;
    char id[MSW_APP_ID_MAX];
    memcpy(id, h->id, sizeof(id));
    id[sizeof(id) - 1] = '\0';
    const char *running = msw_app_running();
    if (running && !strcmp(running, id)) {
        msw_app_stop();
        for (int i = 0; i < 300 && msw_app_running(); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    write_req_t r = { .data = data, .n = n, .id = id };
    msw_app_service(write_package, &r);
    if (!r.ok) {
        *code = "no_space";
        snprintf(msg, msg_len, "%s", r.err);
        return false;
    }
    if (id_out) {
        strlcpy(id_out, id, MSW_APP_ID_MAX);
    }
    if (ver_out) {
        *ver_out = h->ver;
    }
    msw_app_logf("installed %s v%u (%u bytes)", id, h->ver, (unsigned)n);
    msw_launcher_refresh();
    return true;
}

typedef struct {
    const char *id;
    bool ok;
} remove_req_t;

static void remove_package(void *arg)
{
    remove_req_t *r = arg;
    char path[64];
    app_path(path, sizeof(path), r->id, "mswapp");
    r->ok = unlink(path) == 0;
    char dir[64];
    snprintf(dir, sizeof(dir), MSW_APP_DIR "/data/%s", r->id);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[96];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    if (d) {
        closedir(d);
        rmdir(dir);
    }
    rescan(NULL);
}

bool msw_app_remove(const char *id)
{
    if (!valid_id(id)) {
        return false;
    }
    const char *running = msw_app_running();
    if (running && !strcmp(running, id)) {
        msw_app_stop();
        for (int i = 0; i < 300 && msw_app_running(); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    remove_req_t r = { .id = id };
    msw_app_service(remove_package, &r);
    if (r.ok) {
        msw_app_logf("removed %s", id);
        msw_launcher_refresh();
    }
    return r.ok;
}

/* ---- running -------------------------------------------------------------- */

typedef struct {
    char id[MSW_APP_ID_MAX];
    char name[MSW_APP_NAME_MAX + 1];   /* for the app menu */
    esp_elf_t elf;
    bool loaded;
    char err[160];
    const char *code;
} run_t;

static run_t *volatile s_run;      /* the app now, set while its task lives */
static atomic_bool s_stop;
static atomic_int s_yellow;        /* 1 down, 0 up */
static int64_t s_yellow_at;
/* Button events queued, 4 bits each: 1 press, 2 release, + 4 for yellow. */
static atomic_uint s_button_events;
#define EV_PRESS 1u
#define EV_RELEASE 2u
#define EV_YELLOW 4u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char id[MSW_APP_ID_MAX];
} rtc_mark_t;
static RTC_NOINIT_ATTR rtc_mark_t s_mark;

static void mark_running(const char *id)
{
    if (id) {
        strlcpy(s_mark.id, id, sizeof(s_mark.id));
        s_mark.magic = RTC_MAGIC;
    } else {
        s_mark.magic = 0;
    }
}

const char *msw_app_running(void)
{
    run_t *r = s_run;
    return r ? r->id : NULL;
}

void msw_app_stop(void)
{
    atomic_store(&s_stop, true);
}

static void queue_button(unsigned ev)
{
    unsigned cur = atomic_load(&s_button_events);
    for (int i = 0; i < 8; i++) {     /* up to 8 queued */
        if (!((cur >> (4 * i)) & 0xFu)) {
            atomic_fetch_or(&s_button_events, ev << (4 * i));
            break;
        }
    }
}

/* From the input task: true if the app took the speaker button (yellow,
 * upper left). It opens the app menu, or goes to the app after
 * msw_menu_own(true). */
bool msw_app_button(bool down)
{
    if (!s_run) {
        return false;
    }
    atomic_store(&s_yellow, down);
    if (down) {
        s_yellow_at = esp_timer_get_time();
    }
    queue_button(EV_YELLOW | (down ? EV_PRESS : EV_RELEASE));
    return true;
}

/* From the input task: true if the app took the talk button (blue, upper
 * right). It is the app's button while an app runs. */
bool msw_app_blue(bool down)
{
    if (!s_run) {
        return false;
    }
    queue_button(down ? EV_PRESS : EV_RELEASE);
    return true;
}

/* The app menu (msw_app_menu.c). Display locked. */
static void menu_open(run_t *r)
{
    msw_menu_open(r->name);
    msw_sound_stop(-1);
    msw_api_pause(true);
}

static void menu_close(void)
{
    if (msw_gfx_frame_mode()) {
        /* Take the box away without lvgl painting its place black, then
         * send the app's last frame over it. */
        lv_display_t *d = lv_display_get_default();
        lv_display_enable_invalidation(d, false);
        msw_menu_close();
        lv_display_enable_invalidation(d, true);
        msw_fb_present();
    } else {
        msw_menu_close();
    }
    msw_api_pause(false);
}

/* Display locked. Returns false when the user picked "おわる". */
static bool deliver_buttons(run_t *r)
{
    unsigned ev = atomic_exchange(&s_button_events, 0);
    for (int i = 0; i < 8 && ev; i++, ev >>= 4) {
        unsigned e = ev & 0xFu;
        if (!(e & (EV_PRESS | EV_RELEASE))) {
            continue;
        }
        bool down = e & EV_PRESS;
        if (e & EV_YELLOW) {
            if (msw_api_menu_owned() && !msw_menu_is_open()) {
                msw_api_button(MSW_BUTTON_YELLOW, down);
            } else if (down && msw_menu_is_open()) {
                menu_close();
            } else if (down) {
                menu_open(r);
            }
        } else if (!msw_menu_is_open()) {
            msw_api_button(MSW_BUTTON_BLUE, down);
        }
    }
    int choice = msw_menu_is_open() ? msw_menu_choice() : MSW_MENU_NONE;
    if (choice == MSW_MENU_GO_ON) {
        menu_close();
    } else if (choice == MSW_MENU_QUIT) {
        msw_menu_close();
        return false;
    }
    return true;
}

static void run_init_arrays(esp_elf_t *elf)
{
    for (uint32_t i = 0; elf->preinit_array && i < elf->preinit_count; i++) {
        elf->preinit_array[i]();
    }
    for (uint32_t i = 0; elf->init_array && i < elf->init_count; i++) {
        uint32_t k = elf->init_reverse ? elf->init_count - 1 - i : i;
        elf->init_array[k]();
    }
    elf->init_done = true;
}

static void unload(void *arg)
{
    run_t *r = arg;
    if (r->loaded) {
        esp_elf_deinit(&r->elf);    /* runs .fini_array */
        r->loaded = false;
    }
}

static void app_task(void *arg)
{
    run_t *r = arg;
    char *argv[] = { r->id, NULL };
    mark_running(r->id);
    msw_sound_app(true);
    muse_board->display_lock(-1);
    uintptr_t lo = esp_elf_runtime_base(&r->elf);
    msw_api_begin(r->id, lo, lo + r->elf.text_alloc_size);
    run_init_arrays(&r->elf);
    int rc = r->elf.entry(1, argv);
    muse_board->display_unlock();
    if (rc != 0) {
        msw_app_logf("%s: app_main returned %d", r->id, rc);
    }
    int64_t start = esp_timer_get_time(), next = start, poked = start;
    uint32_t last_ms = 0;
    uint32_t ticks = 0;
    int64_t busy_us = 0, worst_us = 0;
    while (rc == 0 && !atomic_load(&s_stop) && !msw_api_exit_asked()) {
        int64_t now = esp_timer_get_time();
        if (msw_api_menu_owned() && atomic_load(&s_yellow) && now - s_yellow_at >= YELLOW_HOLD_MS * 1000LL) {
            msw_app_logf("%s: yellow held, exiting", r->id);
            break;
        }
        if (now < next) {
            int64_t wait_ms = (next - now) / 1000;
            vTaskDelay(wait_ms > 1 ? pdMS_TO_TICKS(wait_ms) : 1);
            continue;
        }
        int fps = msw_api_fps();
        next += 1000000LL / fps;
        if (next < now) {
            next = now + 1000000LL / fps;    /* late: drop frames, don't catch up */
        }
        if (now - poked > 1000000) {
            muse_state_poke();                /* no screen sleep while an app runs */
            poked = now;
        }
        uint32_t ms = (uint32_t)((now - start) / 1000);
        muse_board->display_lock(-1);
        if (!deliver_buttons(r)) {
            muse_board->display_unlock();
            msw_app_logf("%s: quit from the app menu", r->id);
            break;
        }
        if (msw_menu_is_open()) {
            muse_board->display_unlock();
            last_ms = ms;                     /* paused: no ticks, no time */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        msw_api_tick(ms - last_ms);
        int64_t took = esp_timer_get_time() - t0;
        muse_board->display_unlock();
        busy_us += took;
        worst_us = took > worst_us ? took : worst_us;
        ticks++;
        last_ms = ms;
        vTaskDelay(1);                        /* let lvgl and lower tasks in */
    }
    if (ticks) {
        int64_t secs = (esp_timer_get_time() - start) / 1000;
        msw_app_logf("%s: %u ticks in %d s (%.1f a second), a tick took %.1f ms on average, %.1f at most",
                     r->id, (unsigned)ticks, (int)(secs / 1000), secs ? ticks * 1000.0 / secs : 0.0,
                     busy_us / 1000.0 / ticks, worst_us / 1000.0);
    }
    muse_board->display_lock(-1);
    msw_menu_close();
    msw_api_call_exit_cb();
    msw_api_end();
    muse_board->display_unlock();
    msw_sound_app(false);
    msw_imu_release();
    msw_app_service(unload, r);
    mark_running(NULL);
    msw_app_logf("%s: ended", r->id);
    s_run = NULL;
    atomic_store(&s_stop, false);
    atomic_store(&s_yellow, 0);
    atomic_store(&s_button_events, 0);
    atomic_fetch_add(&s_changes, 1);
    free(r);
    msw_launcher_refresh();
    vTaskDelete(NULL);
}

/* Reads, checks and relocates the app. On the service task. */
static void load(void *arg)
{
    run_t *r = arg;
    char path[64];
    app_path(path, sizeof(path), r->id, "mswapp");
    r->code = "not_found";
    FILE *f = s_mounted ? fopen(path, "rb") : NULL;
    if (!f) {
        snprintf(r->err, sizeof(r->err), "no app \"%s\"", r->id);
        return;
    }
    struct stat st;
    size_t n = fstat(fileno(f), &st) == 0 ? (size_t)st.st_size : 0;
    uint8_t *buf = n && n <= MSW_APP_MAX_BYTES ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
    bool read = buf && fread(buf, 1, n, f) == n;
    fclose(f);
    if (!read) {
        free(buf);
        r->code = "out_of_memory";
        snprintf(r->err, sizeof(r->err), "cannot read the app (%u bytes)", (unsigned)n);
        return;
    }
    if (!check_package(buf, n, &r->code, r->err, sizeof(r->err))) {
        free(buf);
        return;
    }
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)buf;
    memcpy(r->name, h->name, MSW_APP_NAME_MAX);
    r->name[MSW_APP_NAME_MAX] = 0;
    int64_t t0 = esp_timer_get_time();
    esp_elf_init(&r->elf);
    int ret = esp_elf_relocate(&r->elf, buf + sizeof(*h) + h->icon_size, h->elf_size);
    free(buf);
    if (ret != 0) {
        esp_elf_deinit(&r->elf);
        r->code = "load_failed";
        snprintf(r->err, sizeof(r->err), "ELF relocation failed (%d)", ret);
        return;
    }
    r->loaded = true;
    r->code = NULL;
    msw_app_logf("%s: loaded in %d ms, PSRAM free %u", r->id, (int)((esp_timer_get_time() - t0) / 1000),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

bool msw_app_start(const char *id, const char **code, char *msg, size_t msg_len)
{
    if (!valid_id(id)) {
        *code = "not_found";
        snprintf(msg, msg_len, "bad id");
        return false;
    }
    if (s_run) {
        msw_app_stop();
        for (int i = 0; i < 300 && s_run; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_run) {
            *code = "busy";
            snprintf(msg, msg_len, "the running app does not end");
            return false;
        }
    }
    run_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM);
    if (!r) {
        *code = "out_of_memory";
        snprintf(msg, msg_len, "out of memory");
        return false;
    }
    strlcpy(r->id, id, sizeof(r->id));
    msw_app_service(load, r);
    if (!r->loaded) {
        *code = r->code ? r->code : "load_failed";
        snprintf(msg, msg_len, "%s", r->err);
        msw_app_logf("%s: %s", id, r->err);
        free(r);
        return false;
    }
    atomic_store(&s_stop, false);
    atomic_store(&s_button_events, 0);
    s_run = r;
    if (xTaskCreatePinnedToCoreWithCaps(app_task, "msw_app", APP_STACK, r, APP_PRIO, NULL, APP_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_run = NULL;
        msw_app_service(unload, r);
        free(r);
        *code = "out_of_memory";
        snprintf(msg, msg_len, "cannot start the app task");
        return false;
    }
    atomic_fetch_add(&s_changes, 1);
    msw_app_logf("%s: started", id);
    return true;
}

/* ---- serial console: >msw.app.* ---------------------------------------------- */

static uint8_t *s_put;
static size_t s_put_len, s_put_cap;

static void print_list(void)
{
    static EXT_RAM_BSS_ATTR msw_app_info_t list[MSW_APP_MAX_APPS];   /* 3 KB: PSRAM */
    int n = msw_app_list(list, MSW_APP_MAX_APPS);
    for (int i = 0; i < n; i++) {
        printf("@msw.app %s v%u %u bytes \"%s\"\n", list[i].id, list[i].ver, (unsigned)list[i].bytes, list[i].name);
    }
    printf("@msw.app.list done %d apps, running %s\n", n, msw_app_running() ? msw_app_running() : "-");
}

static bool console(const char *line)
{
    if (!strncmp(line, "msw.tp ", 7)) {
        /* raw touch registers for a while (looking for a second finger) */
        extern void board_stopwatch_tp_dump(int seconds) __attribute__((weak));
        if (board_stopwatch_tp_dump) {
            board_stopwatch_tp_dump(atoi(line + 7));
        }
        printf("@msw.tp %s\n", board_stopwatch_tp_dump ? "ok" : "none");
        return true;
    }
    if (strncmp(line, "msw.app.", 8) && strcmp(line, "msw.imu")) {
        return false;
    }
    const char *cmd = line + 8;
    char msg[160];
    const char *code = NULL;
    if (!strcmp(line, "msw.imu")) {
        msw_imu_t m;
        if (msw_imu_probe(&m)) {
            printf("@msw.imu ax=%.2f ay=%.2f az=%.2f gx=%.0f gy=%.0f gz=%.0f\n", m.ax, m.ay, m.az, m.gx, m.gy, m.gz);
        } else {
            printf("@msw.imu failed\n");
        }
    } else if (!strcmp(cmd, "list")) {
        print_list();
    } else if (!strncmp(cmd, "run ", 4)) {
        bool ok = msw_app_start(cmd + 4, &code, msg, sizeof(msg));
        printf("@msw.app.run %s\n", ok ? "ok" : msg);
    } else if (!strcmp(cmd, "stop")) {
        msw_app_stop();
        printf("@msw.app.stop ok\n");
    } else if (!strncmp(cmd, "rm ", 3)) {
        printf("@msw.app.rm %s\n", msw_app_remove(cmd + 3) ? "ok" : "failed");
    } else if (!strcmp(cmd, "log")) {
        char *buf = heap_caps_malloc(LOG_LINES * LOG_LINE, MALLOC_CAP_SPIRAM);
        if (buf) {
            msw_app_log_text(buf, LOG_LINES * LOG_LINE);
            fputs(buf, stdout);
            free(buf);
        }
        printf("@msw.app.log done\n");
    } else if (!strncmp(cmd, "put ", 4)) {
        /* put <bytes>, then data <base64> lines, then end */
        free(s_put);
        s_put_cap = (size_t)strtoul(cmd + 4, NULL, 10);
        s_put_len = 0;
        s_put = s_put_cap && s_put_cap <= MSW_APP_MAX_BYTES ? heap_caps_malloc(s_put_cap, MALLOC_CAP_SPIRAM) : NULL;
        printf("@msw.app.put %s\n", s_put ? "ready" : "failed");
    } else if (!strncmp(cmd, "data ", 5)) {
        size_t got = 0;
        const char *b64 = cmd + 5;
        if (!s_put || mbedtls_base64_decode(s_put + s_put_len, s_put_cap - s_put_len, &got,
                                            (const unsigned char *)b64, strlen(b64)) != 0) {
            printf("@msw.app.data failed at %u\n", (unsigned)s_put_len);
        } else {
            s_put_len += got;
            printf("@msw.app.data %u\n", (unsigned)s_put_len);
        }
    } else if (!strcmp(cmd, "end")) {
        char id[MSW_APP_ID_MAX] = "";
        uint16_t ver = 0;
        bool ok = s_put && s_put_len == s_put_cap &&
                  msw_app_install(s_put, s_put_len, id, &ver, &code, msg, sizeof(msg));
        if (ok) {
            printf("@msw.app.end ok %s v%u\n", id, ver);
        } else {
            printf("@msw.app.end failed %s %s\n", code ? code : "incomplete",
                   s_put && s_put_len != s_put_cap ? "(not all data arrived)" : msg);
        }
        free(s_put);
        s_put = NULL;
    } else {
        printf("@msw.app.error unknown: list, run <id>, stop, rm <id>, log, put <n>, data <b64>, end\n");
    }
    fflush(stdout);
    return true;
}

/* ---- start-up ------------------------------------------------------------- */

extern const struct esp_elfsym msw_app_symbols[];
extern void msw_console_add(bool (*fn)(const char *line));

void msw_app_core_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    s_log = heap_caps_calloc(LOG_LINES, LOG_LINE, MALLOC_CAP_SPIRAM);
    s_apps_lock = xSemaphoreCreateMutex();
    esp_elf_register_symbol(msw_app_symbols);   /* after elf_loader's own: ours win */

    esp_reset_reason_t why = esp_reset_reason();
    if (s_mark.magic == RTC_MAGIC) {
        s_mark.id[sizeof(s_mark.id) - 1] = '\0';
        bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
        msw_app_logf("%s was running at the last restart (%s)", s_mark.id,
                     crash ? "it crashed the device" : "reset or power off");
    }
    s_mark.magic = 0;

    s_svc_q = xQueueCreate(4, sizeof(svc_req_t));
    if (!s_svc_q || xTaskCreatePinnedToCore(svc_task, "msw_apps", SVC_STACK, NULL, 3, &s_svc_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "no service task");
        return;
    }
    msw_app_service(mount, NULL);
    msw_app_service(rescan, NULL);
    msw_sound_init();
    msw_console_add(console);
    ESP_LOGI(TAG, "mini apps: %d installed, API %d", s_napps, MSW_API_VERSION);
}
