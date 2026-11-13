/*
 * museStopWatch mini apps: store, loading, the app task, console, crashes.
 *
 * Apps and avatars are packages (msw_app_priv.h), each in one piece in the
 * "msw_pkgs" partition (msw_app_pkgs.c), used where they lie in flash. The
 * apps' saved data (msw_store_put) is in a small FAT volume on the "apps"
 * partition, mounted at /apps. All flash
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
#include "miniz.h"   /* the ROM's tinfl */

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

/* The line back lines before the newest (0 = newest), for the crash report:
 * no lock, as the panic handler calls it with everything stopped. */
int msw_app_log_peek(int back, const char **line)
{
    *line = NULL;
    if (!s_log || back < 0 || back >= s_log_count) {
        return 0;
    }
    *line = s_log[(s_log_head + s_log_count - 1 - back) % LOG_LINES];
    return 1;
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
            if (r.done) {
                xSemaphoreGive(r.done);
            }
        }
    }
}

void msw_app_service_async(void (*fn)(void *arg), void *arg)
{
    svc_req_t r = { fn, arg, NULL };
    if (s_svc_q) {
        xQueueSend(s_svc_q, &r, 0);
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
        ESP_LOGI(TAG, "saved data volume mounted in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGE(TAG, "saved data volume: %s (no \"apps\" partition?)", esp_err_to_name(err));
    }
    msw_pkgs_init();
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

static int by_name(const void *a, const void *b)
{
    return strcmp(((const msw_app_info_t *)a)->name, ((const msw_app_info_t *)b)->name);
}

/* The installed packages of one kind, with their headers (icons when
 * icons is true), by name. On the service task. */
static int list_packages(bool avatar, msw_app_info_t *out, int max, bool icons)
{
    static EXT_RAM_BSS_ATTR msw_pkg_entry_t ent[MSW_APP_MAX_APPS + MSW_AVATAR_MAX + 8];
    int total = msw_pkgs_list(ent, sizeof(ent) / sizeof(ent[0])), n = 0;
    for (int i = 0; i < total && n < max; i++) {
        size_t size;
        const uint8_t *pkg = ent[i].avatar == avatar ? msw_pkgs_find(ent[i].id, avatar, &size) : NULL;
        if (!pkg) {
            continue;
        }
        const msw_app_pkg_t *h = (const msw_app_pkg_t *)pkg;
        msw_app_info_t *a = &out[n++];
        memset(a, 0, sizeof(*a));
        memcpy(a->id, h->id, sizeof(a->id));
        a->id[sizeof(a->id) - 1] = '\0';
        memcpy(a->name, h->name, sizeof(a->name));
        a->name[sizeof(a->name) - 1] = '\0';
        a->ver = h->ver;
        a->api = h->api;
        a->bytes = (uint32_t)size;
        if (icons && h->icon_w == MSW_APP_ICON && h->icon_h == MSW_APP_ICON
            && h->icon_size == MSW_APP_ICON * MSW_APP_ICON * 2 && sizeof(*h) + h->icon_size <= size) {
            a->icon = heap_caps_malloc(h->icon_size, MALLOC_CAP_SPIRAM);
            if (a->icon) {
                memcpy(a->icon, pkg + sizeof(*h), h->icon_size);
            }
        }
    }
    qsort(out, n, sizeof(*out), by_name);
    return n;
}

/* Rereads every app's header and icon. On the service task. */
static void rescan(void *arg)
{
    (void)arg;
    msw_app_info_t *list = heap_caps_calloc(MSW_APP_MAX_APPS, sizeof(*list), MALLOC_CAP_SPIRAM);
    int n = list ? list_packages(false, list, MSW_APP_MAX_APPS, true) : 0;
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

/* An ELF read by offset: the package's, mapped from flash or still in
 * PSRAM (a package sent whole). */
typedef struct {
    const uint8_t *mem;
    size_t size;
} elf_rd_t;

static int elf_rd_read(void *ctx, size_t off, void *buf, size_t len)
{
    elf_rd_t *rd = ctx;
    if (off > rd->size || len > rd->size - off) {
        return -EINVAL;
    }
    memcpy(buf, rd->mem + off, len);
    return 0;
}

/* Part of the ELF in a new PSRAM buffer (+1 byte, NUL), or NULL. */
static void *elf_rd_part(elf_rd_t *rd, size_t off, size_t len)
{
    char *p = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (p && elf_rd_read(rd, off, p, len) != 0) {
        free(p);
        return NULL;
    }
    if (p) {
        p[len] = '\0';
    }
    return p;
}

/* Every symbol the ELF needs must be one the firmware gives. On failure,
 * msg lists the missing ones. An avatar (idea 009) runs inside the screen's
 * drawing, so of the msw_* functions it may use msw_avatar_define only. */
/* The functions that reach outside through PORT.A (msw_app_port.c): an app
 * that imports any of them gets the start screen. msw_i2c_write and the rest
 * need msw_i2c_begin first. */
static bool is_port_fn(const char *name)
{
    static const char *const fns[] = { "msw_port_power", "msw_i2c_begin", "msw_pin_read" };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        if (!strcmp(name, fns[i])) {
            return true;
        }
    }
    return false;
}

static bool check_symbols(elf_rd_t *rd, bool avatar, bool *port, char *msg, size_t msg_len)
{
    elf_ehdr_t eh;
    if (elf_rd_read(rd, 0, &eh, sizeof(eh)) != 0 || memcmp(eh.ident, "\x7f" "ELF", 4)
        || eh.machine != 94 /* Xtensa */) {
        snprintf(msg, msg_len, "not an Xtensa ELF");
        return false;
    }
    size_t sh_len = (size_t)eh.shnum * sizeof(elf_shdr_t);
    if (eh.shoff > rd->size || sh_len > rd->size - eh.shoff) {
        snprintf(msg, msg_len, "ELF section table out of the file");
        return false;
    }
    elf_shdr_t *sh = elf_rd_part(rd, eh.shoff, sh_len);
    if (!sh) {
        snprintf(msg, msg_len, "cannot read the ELF section table");
        return false;
    }
    size_t used = 0;
    int missing = 0;
    bool ok = true;
    msg[0] = '\0';
    for (int i = 0; ok && i < eh.shnum; i++) {
        if (sh[i].type != ELF_SHT_DYNSYM || sh[i].link >= eh.shnum) {
            continue;
        }
        const elf_shdr_t *s = &sh[sh[i].link];
        elf_sym_t *sym = elf_rd_part(rd, sh[i].offset, sh[i].size);
        char *str = sym ? elf_rd_part(rd, s->offset, s->size) : NULL;
        if (!sym || !str) {
            snprintf(msg, msg_len, "ELF symbols out of the file");
            ok = false;
        }
        for (size_t k = 1; ok && k < sh[i].size / sizeof(*sym); k++) {
            if (sym[k].shndx != 0 || sym[k].name >= s->size || (sym[k].info >> 4) == ELF_STB_WEAK) {
                continue;
            }
            const char *name = str + sym[k].name;
            if (port && is_port_fn(name)) {
                *port = true;
            }
            if (avatar && !strncmp(name, "msw_", 4) && strcmp(name, "msw_avatar_define")) {
                snprintf(msg, msg_len, "an avatar can't call %s: of the msw_ functions only msw_avatar_define", name);
                ok = false;
                break;
            }
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
        free(sym);
        free(str);
    }
    free(sh);
    return ok && missing == 0;
}

/* What follows the ELF (msw_app_priv.h): the source and the data files. */
typedef struct {
    const uint8_t *src;         /* NULL: no source */
    uint32_t src_len, src_raw;  /* stored and text lengths */
    bool zipped;
    const msw_pkg_file_t *files;
    uint32_t nfiles;
} pkg_parts_t;

/* The sections after the ELF of the package (n bytes). False when they do
 * not fill it exactly or a data file lies outside its section. */
static bool pkg_parts(const uint8_t *pkg, size_t n, pkg_parts_t *out)
{
    memset(out, 0, sizeof(*out));
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)pkg;
    if (n < sizeof(*h)) {
        return false;
    }
    size_t pos = sizeof(*h) + (size_t)h->icon_size + h->elf_size;
    if (pos > n) {
        return false;
    }
    while (pos < n) {
        uint32_t len;
        if (n - pos < 8) {
            return false;
        }
        memcpy(&len, pkg + pos + 4, 4);
        const uint8_t *tag = pkg + pos, *body = pkg + pos + 8;
        if (len > n - pos - 8) {
            return false;
        }
        if (!memcmp(tag, "MSWZ", 4) && len >= 4) {
            memcpy(&out->src_raw, body, 4);
            out->src = body + 4;
            out->src_len = len - 4;
            out->zipped = true;
        } else if (!memcmp(tag, "MSWS", 4)) {
            out->src = body;
            out->src_len = out->src_raw = len;
            out->zipped = false;
        } else if (!memcmp(tag, "MSWD", 4) && len >= 4) {
            uint32_t count;
            memcpy(&count, body, 4);
            if (count > (len - 4) / sizeof(msw_pkg_file_t)) {
                return false;
            }
            out->files = (const msw_pkg_file_t *)(body + 4);
            out->nfiles = count;
            size_t lo = pos + 8, hi = pos + 8 + len;
            for (uint32_t i = 0; i < count; i++) {
                msw_pkg_file_t f;
                memcpy(&f, &out->files[i], sizeof(f));
                if (f.off < lo || f.off > hi || f.size > hi - f.off || !memchr(f.name, 0, sizeof(f.name))) {
                    return false;
                }
            }
        }   /* other tags: for later firmware, skipped */
        pos += 8 + (size_t)len;
    }
    return true;
}

/* The zlib text in a new NUL-terminated PSRAM buffer of raw bytes, or NULL.
 * The ROM's inflate (tinfl); its 11 KB state goes in PSRAM, not the stack. */
static char *inflate_source(const uint8_t *in, size_t in_len, uint32_t raw)
{
    char *out = heap_caps_malloc((size_t)raw + 1, MALLOC_CAP_SPIRAM);
    tinfl_decompressor *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    bool ok = out && d;
    if (ok) {
        tinfl_init(d);
        size_t in_n = in_len, out_n = raw;
        tinfl_status st = tinfl_decompress(d, in, &in_n, (mz_uint8 *)out, (mz_uint8 *)out, &out_n,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        ok = st == TINFL_STATUS_DONE && out_n == raw;
    }
    free(d);
    if (!ok) {
        free(out);
        return NULL;
    }
    out[raw] = '\0';
    return out;
}

typedef struct {
    const char *id;
    char *text;        /* malloc'd, NUL-terminated */
    const char *code;
} src_req_t;

static void read_source(void *arg)
{
    src_req_t *q = arg;
    size_t n = 0;
    const uint8_t *pkg = msw_pkgs_find(q->id, false, &n);
    if (!pkg) {
        pkg = msw_pkgs_find(q->id, true, &n);   /* an avatar */
    }
    q->code = "not_found";
    pkg_parts_t parts;
    if (!pkg) {
        return;
    }
    q->code = "no_source";
    if (!pkg_parts(pkg, n, &parts) || !parts.src) {
        return;
    }
    if (parts.zipped) {
        q->text = inflate_source(parts.src, parts.src_len, parts.src_raw);
        q->code = q->text ? NULL : "broken_source";
    } else {
        q->text = heap_caps_malloc(parts.src_len + 1, MALLOC_CAP_SPIRAM);
        q->code = q->text ? NULL : "out_of_memory";
        if (q->text) {
            memcpy(q->text, parts.src, parts.src_len);
            q->text[parts.src_len] = '\0';
        }
    }
}

char *msw_app_source(const char *id, const char **code)
{
    src_req_t q = { .id = id };
    if (!valid_id(id)) {
        *code = "not_found";
        return NULL;
    }
    msw_app_service(read_source, &q);
    *code = q.code;
    return q.text;
}

/* Header, sizes and CRC; the ELF must be loadable. */
static bool check_package(const uint8_t *data, size_t n, const char **code, char *msg, size_t msg_len)
{
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)data;
    *code = "bad_format";
    bool avatar = n >= sizeof(*h) && !memcmp(h->magic, "MSWV", 4);
    if (n < sizeof(*h) || (memcmp(h->magic, "MSWA", 4) && !avatar)) {
        snprintf(msg, msg_len, "not a mini-app package (.mswapp)");
        return false;
    }
    pkg_parts_t parts;
    if (!pkg_parts(data, n, &parts)) {
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
    elf_rd_t rd = { .mem = data + sizeof(*h) + h->icon_size, .size = h->elf_size };
    if (!check_symbols(&rd, avatar, NULL, msg, msg_len)) {
        *code = "missing_symbol";
        return false;
    }
    *code = NULL;
    return true;
}

/* ---- install and remove --------------------------------------------------- */

bool msw_app_space(uint32_t *total, uint32_t *free_bytes)
{
    uint32_t largest;
    msw_pkgs_space(total, free_bytes, &largest);
    return msw_pkgs_ready();
}

struct msw_app_inst {
    msw_pkg_writer_t *w;
    size_t size;            /* 0: not known */
    bool retried;           /* compacted once after an overflow */
    /* for the service task's calls */
    const uint8_t *p;
    size_t n;
    bool ok, restart;
    const char *code;
    char msg[120];
};

static const char *pinned(void)
{
    return msw_app_running();
}

static void inst_begin(void *arg)
{
    msw_app_inst_t *in = arg;
    in->w = msw_pkgs_begin(in->size, pinned(), &in->code, in->msg, sizeof(in->msg));
}

msw_app_inst_t *msw_app_install_begin(size_t size, const char **code, char *msg, size_t msg_len)
{
    msw_app_inst_t *in = heap_caps_calloc(1, sizeof(*in), MALLOC_CAP_SPIRAM);
    if (!in) {
        *code = "out_of_memory";
        snprintf(msg, msg_len, "out of memory");
        return NULL;
    }
    in->size = size;
    msw_app_service(inst_begin, in);
    if (!in->w) {
        *code = in->code;
        snprintf(msg, msg_len, "%s", in->msg);
        free(in);
        return NULL;
    }
    return in;
}

static void inst_write(void *arg)
{
    msw_app_inst_t *in = arg;
    in->restart = false;
    in->ok = msw_pkgs_write(in->w, in->p, in->n);
    size_t written;
    if (in->ok || !msw_pkgs_overflowed(in->w, &written)) {
        return;
    }
    /* It did not fit in the largest gap: make the free space one piece and
     * have it sent again, once. */
    uint32_t total, free_b, largest;
    msw_pkgs_space(&total, &free_b, &largest);
    msw_pkgs_abort(in->w);
    in->w = NULL;
    in->code = "no_space";
    if (in->retried || written + in->n > free_b) {
        snprintf(in->msg, sizeof(in->msg), "not enough space: the package is over %u KB, %u KB free of %u KB",
                 (unsigned)((written + in->n) / 1024), (unsigned)(free_b / 1024), (unsigned)(total / 1024));
        return;
    }
    in->retried = true;
    if (msw_pkgs_compact(pinned()) < 0) {
        snprintf(in->msg, sizeof(in->msg), "flash error while making room");
        return;
    }
    in->w = msw_pkgs_begin(0, pinned(), &in->code, in->msg, sizeof(in->msg));
    in->restart = in->w != NULL;
}

/* The size became known before any part (app.install from the VM): start
 * the writer again with it, so room for the whole package is made now, not
 * after it overflows and has to be sent again (2026-11-13). */
static void inst_size(void *arg)
{
    msw_app_inst_t *in = arg;
    size_t written;
    msw_pkgs_written(in->w, &written);
    if (written) {
        return;
    }
    msw_pkgs_abort(in->w);
    in->w = msw_pkgs_begin(in->size, pinned(), &in->code, in->msg, sizeof(in->msg));
}

bool msw_app_install_size(msw_app_inst_t *in, size_t size)
{
    if (!in || !in->w || !size) {
        return in && in->w;
    }
    in->size = size;
    msw_app_service(inst_size, in);
    return in->w != NULL;
}

bool msw_app_install_write(msw_app_inst_t *in, const uint8_t *p, size_t n, bool *restart)
{
    *restart = false;
    if (!in || !in->w) {
        return false;
    }
    in->p = p;
    in->n = n;
    msw_app_service(inst_write, in);
    *restart = in->restart;
    return in->ok;
}

static void inst_check(void *arg)
{
    msw_app_inst_t *in = arg;
    size_t n;
    const uint8_t *pkg = msw_pkgs_written(in->w, &n);
    in->ok = check_package(pkg, n, &in->code, in->msg, sizeof(in->msg));
    if (!in->ok) {
        return;
    }
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)pkg;
    bool avatar = !memcmp(h->magic, "MSWV", 4);
    char id[MSW_APP_ID_MAX];
    memcpy(id, h->id, sizeof(id));
    id[sizeof(id) - 1] = '\0';
    size_t old_n;
    int max = avatar ? MSW_AVATAR_MAX : MSW_APP_MAX_APPS;
    if (!msw_pkgs_find(id, avatar, &old_n) && msw_pkgs_count(avatar) >= max) {
        in->ok = false;
        in->code = "too_many_apps";
        snprintf(in->msg, sizeof(in->msg), "%d %s already: remove one first (app.remove)", max,
                 avatar ? "avatars" : "apps");
    }
}

static void inst_commit(void *arg)
{
    msw_app_inst_t *in = arg;
    size_t n;
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)msw_pkgs_written(in->w, &n);
    bool avatar = !memcmp(h->magic, "MSWV", 4);
    in->ok = msw_pkgs_commit(in->w);
    in->w = NULL;
    if (!in->ok) {
        in->code = "write_failed";
        snprintf(in->msg, sizeof(in->msg), "flash write failed");
    } else if (!avatar) {
        rescan(NULL);
    }
}

static void inst_abort(void *arg)
{
    msw_app_inst_t *in = arg;
    msw_pkgs_abort(in->w);
    in->w = NULL;
}

bool msw_app_install_end(msw_app_inst_t *in, bool ok, char *id_out, uint16_t *ver_out, const char **code,
                         char *msg, size_t msg_len)
{
    if (!in) {
        return false;
    }
    if (ok && in->w) {
        msw_app_service(inst_check, in);
        ok = in->ok;
    } else if (!in->code) {
        in->code = "incomplete";
        snprintf(in->msg, sizeof(in->msg), "the package did not arrive whole");
    }
    char id[MSW_APP_ID_MAX] = "";
    uint16_t ver = 0;
    bool avatar = false;
    if (ok) {
        size_t n;
        const msw_app_pkg_t *h = (const msw_app_pkg_t *)msw_pkgs_written(in->w, &n);
        memcpy(id, h->id, sizeof(id));
        id[sizeof(id) - 1] = '\0';
        ver = h->ver;
        avatar = !memcmp(h->magic, "MSWV", 4);
        size_t bytes = n;
        /* An app being replaced must not run from the old package. */
        const char *running = avatar ? NULL : msw_app_running();
        if (running && !strcmp(running, id)) {
            msw_app_stop();
            for (int i = 0; i < 300 && msw_app_running(); i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        msw_app_service(inst_commit, in);
        ok = in->ok;
        if (ok) {
            msw_app_logf("installed %s%s v%u (%u bytes)", avatar ? "avatar " : "", id, ver, (unsigned)bytes);
        }
    }
    if (in->w) {
        msw_app_service(inst_abort, in);
    }
    if (!ok) {
        *code = in->code ? in->code : "install_failed";
        snprintf(msg, msg_len, "%s", in->msg);
    }
    free(in);
    if (!ok) {
        return false;
    }
    if (id_out) {
        strlcpy(id_out, id, MSW_APP_ID_MAX);
    }
    if (ver_out) {
        *ver_out = ver;
    }
    if (avatar) {
        msw_avatar_installed(id);
    } else {
        msw_launcher_refresh();
    }
    return true;
}

bool msw_app_install(const uint8_t *data, size_t n, char *id_out, uint16_t *ver_out,
                     const char **code, char *msg, size_t msg_len)
{
    /* Checked here first, so a broken package never takes flash. */
    if (!check_package(data, n, code, msg, msg_len)) {
        return false;
    }
    msw_app_inst_t *in = msw_app_install_begin(n, code, msg, msg_len);
    bool restart;
    bool ok = in && msw_app_install_write(in, data, n, &restart);
    return in && msw_app_install_end(in, ok, id_out, ver_out, code, msg, msg_len);
}

typedef struct {
    const char *id;
    bool ok;
} remove_req_t;

static void remove_package(void *arg)
{
    remove_req_t *r = arg;
    r->ok = msw_pkgs_remove(r->id, false);
    r->ok = msw_pkgs_remove(r->id, true) || r->ok;
    char dir[64];
    snprintf(dir, sizeof(dir), MSW_APP_DIR "/data/%s", r->id);
    DIR *d = s_mounted ? opendir(dir) : NULL;
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
    msw_avatar_removing(id);    /* the avatar on screen goes first */
    remove_req_t r = { .id = id };
    msw_app_service(remove_package, &r);
    if (r.ok) {
        msw_app_logf("removed %s", id);
        msw_launcher_refresh();
        msw_avatar_installed(NULL);   /* the avatar picker's list */
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
    const uint8_t *pkg;     /* in flash; compacting leaves it (pinned) */
    size_t pkg_n;
    bool port;              /* calls PORT.A: the start screen first */
} run_t;

static run_t *volatile s_run;      /* the app now, set while its task lives */
static TaskHandle_t s_app_task;    /* its task */
static atomic_bool s_stop;
static atomic_uint s_stop_at;      /* ms + 1 when the stop was first asked; 0 when not */
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
    unsigned none = 0;
    atomic_compare_exchange_strong(&s_stop_at, &none, (unsigned)(esp_timer_get_time() / 1000) + 1);
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
        } else {
            /* Blue: the app's, but Muse's while the menu is open. A release
             * goes where its press went (Muse's own input task takes the
             * release itself once its talk has started). */
            static bool to_muse;
            if (down) {
                to_muse = msw_menu_is_open();
            }
            if (to_muse) {
                msw_muse_talk(down);
            } else if (!msw_menu_is_open()) {
                msw_api_button(MSW_BUTTON_BLUE, down);
            }
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

/* The app's code not returning: an esp_timer looks each second while the
 * app runs and logs it once. The app's code holds the display lock, and a
 * task can't be killed with a lock held, so a stop asked meanwhile (app.stop,
 * the yellow button held) that it doesn't answer in STOP_GRACE_MS restarts
 * the device: msw_crash_stuck() keeps where it was for app.log. */
#define STUCK_MS 3000
#define STOP_GRACE_MS 5000
#define SLOW_TICK_MS 300
static atomic_uint s_busy_since;      /* ms + 1 when the app's code was entered; 0 outside */
static const char *s_busy_in;
static bool s_stuck_logged;
static const char *s_watch_id;

static void watch(void *arg)
{
    (void)arg;
    uint32_t since = atomic_load(&s_busy_since);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000) + 1;
    if (!since || now - since < STUCK_MS) {
        return;
    }
    if (!s_stuck_logged) {
        s_stuck_logged = true;
        msw_app_logf("%s: %s has not returned for %u s (a loop that waits for something?)",
                     s_watch_id, s_busy_in, (unsigned)((now - since) / 1000));
        msw_app_logf("%s: if app.stop or the yellow button held asks it to end, the device restarts in %d s",
                     s_watch_id, STOP_GRACE_MS / 1000);
    }
    /* Nothing reads the buttons while the app's code runs: a held yellow asks for the stop here. */
    if (atomic_load(&s_yellow) && esp_timer_get_time() - s_yellow_at >= YELLOW_HOLD_MS * 1000LL) {
        msw_app_stop();
    }
    uint32_t asked = atomic_load(&s_stop_at);
    if (asked && now - asked >= STOP_GRACE_MS && now - since >= STOP_GRACE_MS) {
        msw_app_logf("%s: %s did not return to end: restarting the device", s_watch_id, s_busy_in);
        msw_crash_stuck(s_app_task, s_busy_in, (now - since) / 1000);
        esp_restart();
    }
}

int msw_app_stuck_secs(void)
{
    uint32_t since = atomic_load(&s_busy_since);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000) + 1;
    return since && now - since >= STUCK_MS ? (int)((now - since) / 1000) : 0;
}

static void busy(const char *in)
{
    if (in) {
        s_busy_in = in;
        atomic_store(&s_busy_since, (uint32_t)(esp_timer_get_time() / 1000) + 1);
    } else {
        atomic_store(&s_busy_since, 0);
    }
}

static const char *pkg_text(run_t *r, const char *name, char *buf, size_t cap);

/* An app that uses PORT.A: show what it says to plug in and what it does
 * with it (app.json "port"), and wait
 * for "はじめる" tapped while the blue button is held (a stray tap or a
 * stray press alone does not start it). False for "やめる", the yellow
 * button or app.stop. On "はじめる" the screen stays up: app_task takes it
 * away once the app's own screen is loaded, so the launcher under it does
 * not show for a moment in between (2026-11-13). */
static bool start_screen(run_t *r)
{
    static EXT_RAM_BSS_ATTR char what[400];   /* connect, a newline, about (sdk/build.py) */
    pkg_text(r, ".port", what, sizeof(what));
    char *about = strchr(what, '\n');
    if (about) {
        *about++ = 0;
    }
    msw_app_logf("%s: uses PORT.A; waiting for \"はじめる\" with the blue button held", r->id);
    atomic_store(&s_button_events, 0);
    muse_board->display_lock(-1);
    msw_menu_open_start(r->name, what, about);
    muse_board->display_unlock();
    int choice = MSW_MENU_NONE;
    int64_t poked = 0;
    bool blue = false, shown = false;
    while (choice == MSW_MENU_NONE && !atomic_load(&s_stop)) {
        unsigned ev = atomic_exchange(&s_button_events, 0);
        for (int i = 0; i < 8 && ev; i++, ev >>= 4) {
            unsigned e = ev & 0xFu;
            if (e & EV_YELLOW) {
                choice = (e & EV_PRESS) ? MSW_MENU_QUIT : choice;
            } else if (e & (EV_PRESS | EV_RELEASE)) {
                blue = e & EV_PRESS;
            }
        }
        if (choice == MSW_MENU_NONE) {
            muse_board->display_lock(-1);
            if (blue != shown) {
                msw_menu_start_ready(blue);
                shown = blue;
            }
            choice = msw_menu_choice();
            if (choice == MSW_MENU_GO_ON && !blue) {
                msw_menu_start_nudge();
                choice = MSW_MENU_NONE;
            }
            muse_board->display_unlock();
        }
        int64_t now = esp_timer_get_time();
        if (now - poked > 1000000) {
            muse_state_poke();       /* no screen sleep while it waits */
            poked = now;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    bool go = choice == MSW_MENU_GO_ON;
    if (!go) {
        muse_board->display_lock(-1);
        msw_menu_close();
        muse_board->display_unlock();
    }
    atomic_store(&s_button_events, 0);
    msw_app_logf("%s: %s", r->id, go ? "the user picked \"はじめる\"" : "not started (やめる, the yellow button or app.stop)");
    return go;
}

static void app_task(void *arg)
{
    run_t *r = arg;
    char *argv[] = { r->id, NULL };
    mark_running(r->id);
    bool started = !r->port || start_screen(r);
    if (r->port && started) {
        msw_port_allow(true);
    }
    msw_sound_app(true);
    muse_board->display_lock(-1);
    uintptr_t lo = esp_elf_runtime_base(&r->elf);
    msw_crash_app(r->id, lo, lo + r->elf.text_alloc_size, r->elf.sec[ELF_SEC_TEXT].v_addr);
    msw_api_begin(r->id, lo, lo + r->elf.text_alloc_size, (uintptr_t)r->elf.ptext);
    msw_api_set_rodata(r->elf.sec[ELF_SEC_TEXT].v_addr, r->elf.sec[ELF_SEC_RODATA].v_addr,
                       r->elf.sec[ELF_SEC_RODATA].size, r->elf.sec[ELF_SEC_RODATA].addr);
    if (r->port && started) {
        msw_menu_close();     /* the start screen, now over the app's screen */
    }
    s_watch_id = r->id;
    s_stuck_logged = false;
    esp_timer_handle_t watcher = NULL;
    const esp_timer_create_args_t wa = { .callback = watch, .name = "msw_watch" };
    if (esp_timer_create(&wa, &watcher) == ESP_OK) {
        esp_timer_start_periodic(watcher, 1000 * 1000);
    }
    int rc = 1;
    if (started) {
        busy("app_main");
        run_init_arrays(&r->elf);
        rc = r->elf.entry(1, argv);
        busy(NULL);
    }
    muse_board->display_unlock();
    if (started && rc != 0) {
        msw_app_logf("%s: app_main returned %d", r->id, rc);
    }
    int64_t start = esp_timer_get_time(), next = start, poked = start;
    uint32_t last_ms = 0;
    uint32_t ticks = 0;
    int64_t busy_us = 0, worst_us = 0;
    int slow_logged = 0;
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
        busy("a button callback");
        bool go_on = deliver_buttons(r);
        busy(NULL);
        if (!go_on) {
            muse_board->display_unlock();
            msw_app_logf("%s: quit from the app menu", r->id);
            break;
        }
        if (msw_menu_is_open()) {
            msw_menu_poll();                  /* Muse's face and words in the menu */
            muse_board->display_unlock();
            last_ms = ms;                     /* paused: no ticks, no time */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        busy("a tick");
        msw_api_tick(ms - last_ms);
        busy(NULL);
        int64_t took = esp_timer_get_time() - t0;
        muse_board->display_unlock();
        if (took > SLOW_TICK_MS * 1000LL && slow_logged < 3) {
            slow_logged++;
            msw_app_logf("%s: a tick took %d ms; keep it to a few ms (the screen, sound and buttons wait)%s",
                         r->id, (int)(took / 1000), slow_logged == 3 ? " (not logged again)" : "");
        }
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
    busy("the exit callback");
    msw_api_call_exit_cb();
    busy(NULL);
    if (watcher) {
        esp_timer_stop(watcher);
        esp_timer_delete(watcher);
    }
    msw_api_end();
    muse_board->display_unlock();
    msw_sound_app(false);
    msw_mic_app_end();
    msw_port_app_end();
    msw_imu_release();
    msw_crash_app(NULL, 0, 0, 0);
    msw_app_service(unload, r);
    mark_running(NULL);
    msw_app_logf("%s: ended", r->id);
    s_run = NULL;
    atomic_store(&s_stop, false);
    atomic_store(&s_stop_at, 0);
    atomic_store(&s_yellow, 0);
    atomic_store(&s_button_events, 0);
    atomic_fetch_add(&s_changes, 1);
    free(r);
    msw_launcher_refresh();
    vTaskDeleteWithCaps(NULL);   /* a plain vTaskDelete leaves its 16 KB stack behind each run */
}

/* Checks and relocates the package's ELF into elf, from where it lies in
 * flash: no copy of the package in PSRAM. On the service task. */
static bool load_elf(const char *id, bool avatar, esp_elf_t *elf, char *name, const char **code,
                     char *err, size_t err_len, const uint8_t **pkg_out, size_t *pkg_n, bool *port)
{
    size_t n = 0;
    const uint8_t *pkg = msw_pkgs_find(id, avatar, &n);
    *code = "not_found";
    if (!pkg) {
        snprintf(err, err_len, "no %s \"%s\"", avatar ? "avatar" : "app", id);
        return false;
    }
    /* The package's CRC was checked when it was installed and at boot. */
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)pkg;
    bool ok = n >= sizeof(*h) && !memcmp(h->magic, avatar ? "MSWV" : "MSWA", 4)
              && h->elf_size && sizeof(*h) + (size_t)h->icon_size + h->elf_size <= n;
    if (!ok || h->api > MSW_API_VERSION) {
        *code = ok ? "api_too_new" : "bad_format";
        if (ok) {
            snprintf(err, err_len, "built for API %u; this firmware has API %d", h->api, MSW_API_VERSION);
        } else {
            snprintf(err, err_len, "not a mini-app package (.mswapp)");
        }
        return false;
    }
    const uint8_t *elf_at = pkg + sizeof(*h) + h->icon_size;
    elf_rd_t rd = { .mem = elf_at, .size = h->elf_size };
    if (!check_symbols(&rd, avatar, port, err, err_len)) {
        *code = "missing_symbol";
        return false;
    }
    memcpy(name, h->name, MSW_APP_NAME_MAX);
    name[MSW_APP_NAME_MAX] = 0;
    int64_t t0 = esp_timer_get_time();
    esp_elf_init(elf);
    int ret = esp_elf_relocate(elf, elf_at, h->elf_size);
    if (ret != 0) {
        esp_elf_deinit(elf);
        if (ret == -ENOMEM) {
            *code = "out_of_memory";
            snprintf(err, err_len, "not enough PSRAM to load the %s: %u B free, largest block %u B",
                     avatar ? "avatar" : "app", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        } else {
            *code = "load_failed";
            snprintf(err, err_len, "ELF relocation failed (%d)", ret);
        }
        return false;
    }
    *code = NULL;
    if (pkg_out) {
        *pkg_out = pkg;
        *pkg_n = n;
    }
    msw_app_logf("%s: loaded in %d ms, PSRAM free %u", id, (int)((esp_timer_get_time() - t0) / 1000),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

/* The app's ELF into r. On the service task. */
static void load(void *arg)
{
    run_t *r = arg;
    r->loaded = load_elf(r->id, false, &r->elf, r->name, &r->code, r->err, sizeof(r->err), &r->pkg, &r->pkg_n,
                         &r->port);
}

typedef struct {
    const char *id;
    esp_elf_t *elf;
    char *name;
    const char *code;
    char *err;
    size_t err_len;
    bool ok;
} avatar_load_t;

static void load_avatar(void *arg)
{
    avatar_load_t *a = arg;
    a->ok = load_elf(a->id, true, a->elf, a->name, &a->code, a->err, a->err_len, NULL, NULL, NULL);
}

bool msw_app_load_avatar(const char *id, esp_elf_t *elf, char *name, const char **code, char *err, size_t err_len)
{
    if (!valid_id(id)) {
        *code = "not_found";
        snprintf(err, err_len, "bad id");
        return false;
    }
    avatar_load_t a = { .id = id, .elf = elf, .name = name, .err = err, .err_len = err_len };
    msw_app_service(load_avatar, &a);
    *code = a.code;
    return a.ok;
}

void msw_app_unload_avatar(esp_elf_t *elf)
{
    esp_elf_deinit(elf);
}

void msw_app_init_arrays(esp_elf_t *elf)
{
    run_init_arrays(elf);
}

typedef struct {
    msw_app_info_t *out;
    int max, n;
} avatar_scan_t;

static void scan_avatars(void *arg)
{
    avatar_scan_t *q = arg;
    q->n = list_packages(true, q->out, q->max, false);
}

int msw_app_list_avatars(msw_app_info_t *out, int max)
{
    avatar_scan_t q = { .out = out, .max = max };
    msw_app_service(scan_avatars, &q);
    return q.n;
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
    atomic_store(&s_stop_at, 0);
    atomic_store(&s_button_events, 0);
    s_run = r;
    if (xTaskCreatePinnedToCoreWithCaps(app_task, "msw_app", APP_STACK, r, APP_PRIO, &s_app_task, APP_CORE,
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

/* A text file of r's package (sdk/build.py's ".port") into buf, cut to fit;
 * "" when there is none. */
static const char *pkg_text(run_t *r, const char *name, char *buf, size_t cap)
{
    pkg_parts_t parts;
    buf[0] = 0;
    if (!r->pkg || !pkg_parts(r->pkg, r->pkg_n, &parts)) {
        return buf;
    }
    for (uint32_t i = 0; i < parts.nfiles; i++) {
        msw_pkg_file_t f;
        memcpy(&f, &parts.files[i], sizeof(f));
        if (!strcmp(f.name, name)) {
            size_t n = f.size < cap - 1 ? f.size : cap - 1;
            memcpy(buf, r->pkg + f.off, n);
            buf[n] = 0;
            break;
        }
    }
    return buf;
}

/* An app's data file (msw_app.h), where it lies in flash. From the app's
 * task: the running app's package stays put while it runs. */
const void *msw_app_data(const char *name, size_t *size)
{
    run_t *r = s_run;
    pkg_parts_t parts;
    if (size) {
        *size = 0;
    }
    if (!r || !r->pkg || !name || !pkg_parts(r->pkg, r->pkg_n, &parts)) {
        return NULL;
    }
    for (uint32_t i = 0; i < parts.nfiles; i++) {
        msw_pkg_file_t f;
        memcpy(&f, &parts.files[i], sizeof(f));
        if (!strcmp(f.name, name)) {
            if (size) {
                *size = f.size;
            }
            return r->pkg + f.off;
        }
    }
    msw_app_logf("msw_app_data: no file \"%.40s\" in the package", name);
    return NULL;
}

/* ---- serial console: >msw.app.* ---------------------------------------------- */

static msw_app_inst_t *s_put;
static size_t s_put_len, s_put_cap;
static bool s_put_bad;

static void print_list(void)
{
    static EXT_RAM_BSS_ATTR msw_app_info_t list[MSW_APP_MAX_APPS];   /* 3 KB: PSRAM */
    int n = msw_app_list(list, MSW_APP_MAX_APPS);
    for (int i = 0; i < n; i++) {
        printf("@msw.app %s v%u %u bytes \"%s\"\n", list[i].id, list[i].ver, (unsigned)list[i].bytes, list[i].name);
    }
    uint32_t total = 0, free_bytes = 0, largest = 0;
    msw_pkgs_space(&total, &free_bytes, &largest);
    printf("@msw.app.list done %d apps (max %d), %u KB free of %u KB (largest gap %u KB), running %s\n", n,
           MSW_APP_MAX_APPS, (unsigned)(free_bytes / 1024), (unsigned)(total / 1024), (unsigned)(largest / 1024),
           msw_app_running() ? msw_app_running() : "-");
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
    } else if (!strcmp(cmd, "menu")) {
        /* As a press of the yellow button: opens or closes the app menu. */
        bool ok = msw_app_button(true);
        msw_app_button(false);
        printf("@msw.app.menu %s\n", ok ? "ok" : "no app");
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
    } else if (!strcmp(cmd, "shot") || !strcmp(cmd, "shot full")) {
        /* app.screenshot's JPEG in base64 lines (tools: decode to see it);
         * half size as Muse's default, "shot full" for the whole screen */
        extern uint8_t *msw_app_screenshot(int quality, int scale, size_t *n, int *w, int *h,
                                           const char **source, const char **why, int *ms);
        bool full = cmd[4] != 0;
        size_t n;
        int w = 0, h = 0;
        const char *source = "", *why = NULL;
        int ms = 0;
        uint8_t *jpg = msw_app_screenshot(full ? 70 : 40, full ? 1 : 2, &n, &w, &h, &source, &why, &ms);
        if (!jpg) {
            printf("@msw.app.shot error %s\n", why ? why : "failed");
        } else {
            printf("@msw.app.shot %u bytes, %dx%d, %d ms, %s\n", (unsigned)n, w, h, ms, source);
            for (size_t i = 0; i < n; i += 57) {
                unsigned char line[80];
                size_t len = 0;
                mbedtls_base64_encode(line, sizeof(line), &len, jpg + i, n - i < 57 ? n - i : 57);
                printf("%.*s\n", (int)len, line);
            }
            free(jpg);
            printf("@msw.app.shot done\n");
        }
    } else if (!strncmp(cmd, "avatar ", 7)) {
        /* avatar <id>|official */
        bool ok = msw_avatar_use(cmd + 7, &code, msg, sizeof(msg));
        printf("@msw.app.avatar %s\n", ok ? msw_avatar_current(NULL, 0, NULL) : msg);
    } else if (!strncmp(cmd, "avatar_preview ", 15)) {
        /* avatar_preview <id> <state> [frames]: app.avatar_preview's JPEG in base64 lines */
        char id[MSW_APP_ID_MAX] = "", state[16] = "";
        int frames = 0;
        sscanf(cmd + 15, "%23s %15s %d", id, state, &frames);
        msw_avatar_preview_t q = { .id = id, .state = state, .frames = frames };
        if (!msw_avatar_preview(&q)) {
            printf("@msw.app.shot error %s %s\n", q.code ? q.code : "failed", q.msg);
        } else {
            printf("@msw.app.shot %u bytes, %dx%d, %.1f ms a frame (%.1f at most), preview\n", (unsigned)q.n, q.w,
                   q.h, q.avg_ms, q.max_ms);
            for (size_t i = 0; i < q.n; i += 57) {
                unsigned char line[80];
                size_t len = 0;
                mbedtls_base64_encode(line, sizeof(line), &len, q.jpg + i, q.n - i < 57 ? q.n - i : 57);
                printf("%.*s\n", (int)len, line);
            }
            free(q.jpg);
            printf("@msw.app.shot done\n");
        }
    } else if (!strcmp(cmd, "avatars")) {
        static EXT_RAM_BSS_ATTR msw_app_info_t list[MSW_AVATAR_MAX];
        int n = msw_avatar_list(list, MSW_AVATAR_MAX);
        float ms;
        for (int i = 0; i < n; i++) {
            printf("@msw.app.avatar %s v%u %u bytes \"%s\"\n", list[i].id, list[i].ver, (unsigned)list[i].bytes,
                   list[i].name);
        }
        const char *cur = msw_avatar_current(NULL, 0, &ms);
        printf("@msw.app.avatars done %d, current %s, %.1f ms a frame\n", n, cur, ms);
    } else if (!strncmp(cmd, "src ", 4)) {
        const char *why;
        char *text = msw_app_source(cmd + 4, &why);
        if (text) {
            fputs(text, stdout);
            free(text);
            printf("@msw.app.src done\n");
        } else {
            printf("@msw.app.src error %s\n", why);
        }
    } else if (!strncmp(cmd, "put ", 4)) {
        /* put <bytes>, then data <base64> lines, then end: written to flash
         * as it comes */
        if (s_put) {
            msw_app_install_end(s_put, false, NULL, NULL, &code, msg, sizeof(msg));
        }
        s_put_cap = (size_t)strtoul(cmd + 4, NULL, 10);
        s_put_len = 0;
        s_put_bad = false;
        s_put = s_put_cap ? msw_app_install_begin(s_put_cap, &code, msg, sizeof(msg)) : NULL;
        if (s_put) {
            printf("@msw.app.put ready\n");
        } else {
            printf("@msw.app.put failed %s %s\n", code ? code : "bad_size", s_put_cap ? msg : "");
        }
    } else if (!strncmp(cmd, "data ", 5)) {
        static EXT_RAM_BSS_ATTR uint8_t bin[768];   /* a line: 1024 letters at most */
        size_t got = 0;
        const char *b64 = cmd + 5;
        bool restart;
        if (!s_put || s_put_bad
            || mbedtls_base64_decode(bin, sizeof(bin), &got, (const unsigned char *)b64, strlen(b64)) != 0
            || got > s_put_cap - s_put_len || !msw_app_install_write(s_put, bin, got, &restart)) {
            s_put_bad = true;
            printf("@msw.app.data failed at %u\n", (unsigned)s_put_len);
        } else {
            s_put_len += got;
            printf("@msw.app.data %u\n", (unsigned)s_put_len);
        }
    } else if (!strcmp(cmd, "putpos")) {
        /* how much of the put arrived: a sender that lost a line's answer
         * goes on from here */
        printf("@msw.app.putpos %u%s\n", (unsigned)s_put_len, s_put && !s_put_bad ? "" : " failed");
    } else if (!strcmp(cmd, "compact")) {
        extern int msw_app_compact(void);
        int moved = msw_app_compact();
        printf("@msw.app.compact %s %d KB moved\n", moved < 0 ? "failed" : "ok", moved < 0 ? 0 : moved / 1024);
    } else if (!strcmp(cmd, "end")) {
        char id[MSW_APP_ID_MAX] = "";
        uint16_t ver = 0;
        bool whole = s_put && !s_put_bad && s_put_len == s_put_cap;
        bool ok = s_put && msw_app_install_end(s_put, whole, id, &ver, &code, msg, sizeof(msg));
        if (ok) {
            printf("@msw.app.end ok %s v%u\n", id, ver);
        } else {
            printf("@msw.app.end failed %s %s\n", code ? code : "incomplete", s_put ? msg : "(no put)");
        }
        s_put = NULL;
    } else {
        printf("@msw.app.error unknown: list, run <id>, stop, rm <id>, log, src <id>, put <n>, data <b64>, end, putpos, compact\n");
    }
    fflush(stdout);
    return true;
}

typedef struct {
    int moved;
} compact_req_t;

static void do_compact(void *arg)
{
    compact_req_t *q = arg;
    q->moved = msw_pkgs_compact(msw_app_running());
    rescan(NULL);
}

int msw_app_compact(void)
{
    compact_req_t q = { 0 };
    msw_app_service(do_compact, &q);
    return q.moved;
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
                     crash ? "it crashed the device" : why == ESP_RST_SW ? "a restart" : "reset or power off");
    }
    s_mark.magic = 0;
    msw_crash_boot_report();

    s_svc_q = xQueueCreate(4, sizeof(svc_req_t));
    if (!s_svc_q || xTaskCreatePinnedToCore(svc_task, "msw_apps", SVC_STACK, NULL, 3, &s_svc_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "no service task");
        return;
    }
    msw_app_service(mount, NULL);
    msw_app_service(rescan, NULL);
    msw_sound_init();
    msw_console_add(console);
    msw_avatar_boot();
    ESP_LOGI(TAG, "mini apps: %d installed, API %d", s_napps, MSW_API_VERSION);
}
