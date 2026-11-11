/*
 * museStopWatch mini apps: Muse's app.* commands (main/app.c calls these
 * through weak references; main/noise_control.cpp describes them to Muse).
 *
 *   app.doc     the guide to read first (sdk/guide.md, built in)
 *   app.api     the API header, include/msw_app.h (built in)
 *   app.list    installed apps and the running one
 *   app.run     start an app by id          app.stop   end the running one
 *   app.remove  delete an app by id         app.log    the app log
 *   app.source  the app's files kept in its package (sdk/build.py adds them)
 *   app.screenshot  the screen as a JPEG, base64 (like camera.capture)
 *   app.avatar_doc, app.avatar_list, app.avatar_preview, app.avatar_use
 *               the face screen's avatar (avatar_guide.md, msw_avatar.c)
 *   app.install is async: main/app.c fetches the file from the VM and hands
 *               it to msw_app_install_json.
 *
 * app.doc and app.api return the whole text (up to PAGE bytes; the reply is
 * sent in chunks). Past PAGE they page: pass "offset" from the last "next".
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "mbedtls/base64.h"
#include "msw_app_priv.h"

extern const char guide_md_start[] asm("_binary_guide_md_start");
extern const char guide_md_end[] asm("_binary_guide_md_end");
extern const char msw_app_h_start[] asm("_binary_msw_app_h_start");
extern const char avatar_guide_md_start[] asm("_binary_avatar_guide_md_start");
extern const char avatar_guide_md_end[] asm("_binary_avatar_guide_md_end");
extern const char msw_app_h_end[] asm("_binary_msw_app_h_end");

#define PAGE (64 * 1024)   /* the guide is ~12 KB, the header ~11 KB */

static cJSON *ok_payload(cJSON **payload)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    *payload = cJSON_AddObjectToObject(r, "payload");
    return r;
}

static cJSON *fail(const char *code, const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

static const char *str(const cJSON *params, const char *key)
{
    const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

/* A page of text (embedded files end with a NUL that EMBED_TXTFILES adds). */
static cJSON *page(const char *start, const char *end, const cJSON *params)
{
    size_t len = strnlen(start, (size_t)(end - start));
    const cJSON *o = params ? cJSON_GetObjectItemCaseSensitive(params, "offset") : NULL;
    size_t off = cJSON_IsNumber(o) && o->valuedouble > 0 ? (size_t)o->valuedouble : 0;
    if (off > len) {
        off = len;
    }
    size_t n = len - off > PAGE ? PAGE : len - off;
    if (off + n < len) {                 /* end the page at a line, and never inside a UTF-8 letter */
        size_t cut = n;
        while (cut > PAGE / 2 && start[off + cut - 1] != '\n') {
            cut--;
        }
        n = cut > PAGE / 2 ? cut : n;
        while (n && ((unsigned char)start[off + n] & 0xC0) == 0x80) {
            n--;
        }
    }
    char *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return fail("out_of_memory", "out of memory");
    }
    memcpy(buf, start + off, n);
    buf[n] = '\0';
    cJSON *payload;
    cJSON *r = ok_payload(&payload);
    cJSON_AddStringToObject(payload, "text", buf);
    free(buf);
    cJSON_AddNumberToObject(payload, "offset", (double)off);
    cJSON_AddNumberToObject(payload, "total", (double)len);
    if (off + n < len) {
        cJSON_AddNumberToObject(payload, "next", (double)(off + n));
    }
    return r;
}

static cJSON *list(void)
{
    static EXT_RAM_BSS_ATTR msw_app_info_t apps[MSW_APP_MAX_APPS];   /* 3 KB: PSRAM */
    int n = msw_app_list(apps, MSW_APP_MAX_APPS);
    cJSON *payload;
    cJSON *r = ok_payload(&payload);
    cJSON *arr = cJSON_AddArrayToObject(payload, "apps");
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "id", apps[i].id);
        cJSON_AddStringToObject(a, "name", apps[i].name);
        cJSON_AddNumberToObject(a, "version", apps[i].ver);
        cJSON_AddNumberToObject(a, "api", apps[i].api);
        cJSON_AddNumberToObject(a, "bytes", apps[i].bytes);
        cJSON_AddItemToArray(arr, a);
    }
    const char *running = msw_app_running();
    if (running) {
        cJSON_AddStringToObject(payload, "running", running);
    } else {
        cJSON_AddNullToObject(payload, "running");
    }
    cJSON_AddNumberToObject(payload, "api_version", MSW_API_VERSION);
    /* Room for more: Muse backs up (app.source) and removes apps to make it. */
    uint32_t total, free_bytes;
    if (msw_app_space(&total, &free_bytes)) {
        cJSON_AddNumberToObject(payload, "space_bytes", total);
        cJSON_AddNumberToObject(payload, "free_bytes", free_bytes);
    }
    cJSON_AddNumberToObject(payload, "max_apps", MSW_APP_MAX_APPS);
    uint32_t t2, f2, largest = 0;
    msw_pkgs_space(&t2, &f2, &largest);
    /* A package can be as large as the free space; a gap too small is
     * closed up when one is installed. */
    cJSON_AddNumberToObject(payload, "max_app_bytes", (double)f2);
    cJSON_AddNumberToObject(payload, "largest_gap_bytes", (double)largest);
    return r;
}

static cJSON *installed(const char *id, uint16_t ver, size_t n, bool run)
{
    const char *code = NULL;
    char msg[200];
    cJSON *payload;
    cJSON *r = ok_payload(&payload);
    cJSON_AddStringToObject(payload, "id", id);
    cJSON_AddNumberToObject(payload, "version", ver);
    cJSON_AddNumberToObject(payload, "bytes", (double)n);
    if (run) {
        bool started = msw_app_start(id, &code, msg, sizeof(msg));
        cJSON_AddBoolToObject(payload, "started", started);
        if (!started) {
            cJSON_AddStringToObject(payload, "start_error", msg);
        }
    }
    return r;
}

/* app.install as the package arrives from the VM (main/app.c): begin, a
 * write per part (0 go on, 1 send it again from the start, -1 stop), then
 * end with the result. */
typedef struct {
    msw_app_inst_t *in;
    size_t n;
    const char *code;
    char msg[200];
} json_inst_t;

void *msw_app_install_json_begin(cJSON **err)
{
    json_inst_t *j = calloc(1, sizeof(*j));
    if (!j) {
        *err = fail("out_of_memory", "failed to allocate");
        return NULL;
    }
    j->in = msw_app_install_begin(0, &j->code, j->msg, sizeof(j->msg));
    if (!j->in) {
        *err = fail(j->code ? j->code : "install_failed", j->msg);
        free(j);
        return NULL;
    }
    return j;
}

int msw_app_install_json_write(void *ctx, const uint8_t *p, size_t n)
{
    json_inst_t *j = ctx;
    bool restart;
    if (msw_app_install_write(j->in, p, n, &restart)) {
        j->n += n;
        return 0;
    }
    if (restart) {
        j->n = 0;
        return 1;
    }
    return -1;
}

cJSON *msw_app_install_json_end(void *ctx, const char *fetch_code, const char *fetch_msg, bool run)
{
    json_inst_t *j = ctx;
    char id[MSW_APP_ID_MAX] = "";
    uint16_t ver = 0;
    bool ok = msw_app_install_end(j->in, !fetch_code, id, &ver, &j->code, j->msg, sizeof(j->msg));
    cJSON *r;
    if (ok) {
        r = installed(id, ver, j->n, run);
    } else if (fetch_code && strcmp(fetch_code, "stopped")) {
        r = fail(fetch_code, fetch_msg);    /* the download failed */
    } else {
        r = fail(j->code ? j->code : "install_failed", j->msg);
    }
    free(j);
    return r;
}

cJSON *msw_app_install_json(const uint8_t *data, size_t n, bool run)
{
    char id[MSW_APP_ID_MAX] = "";
    uint16_t ver = 0;
    const char *code = NULL;
    char msg[200];
    if (!msw_app_install(data, n, id, &ver, &code, msg, sizeof(msg))) {
        return fail(code ? code : "install_failed", msg);
    }
    return installed(id, ver, n, run);
}

uint8_t *msw_app_screenshot(int quality, int scale, size_t *n, int *w, int *h,
                            const char **source, const char **why, int *ms);

/* Half size (232 x 232, quality 40) unless "size":"full" (466 x 466,
 * quality 70): the base64 reaches Muse as text, so a smaller picture is a
 * shorter read (2026-11-08). */
static cJSON *screenshot(const cJSON *params)
{
    const cJSON *q = params ? cJSON_GetObjectItemCaseSensitive(params, "quality") : NULL;
    const char *size = params ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "size")) : NULL;
    bool full = size && !strcmp(size, "full");
    size_t n;
    int w = 0, h = 0;
    const char *source = "", *why = NULL;
    int ms = 0;
    uint8_t *jpg = msw_app_screenshot(cJSON_IsNumber(q) ? q->valueint : full ? 70 : 40, full ? 1 : 2,
                                      &n, &w, &h, &source, &why, &ms);
    if (!jpg) {
        return fail(why ? why : "failed", why && !strcmp(why, "out_of_memory") ? "out of memory" : "could not take the picture");
    }
    size_t cap = (n + 2) / 3 * 4 + 1, len = 0;
    unsigned char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!b64 || mbedtls_base64_encode(b64, cap, &len, jpg, n) != 0) {
        free(jpg);
        free(b64);
        return fail("out_of_memory", "out of memory");
    }
    free(jpg);
    cJSON *payload;
    cJSON *r = ok_payload(&payload);
    cJSON_AddStringToObject(payload, "format", "jpeg-base64");
    cJSON_AddStringToObject(payload, "data_base64", (const char *)b64);
    free(b64);
    cJSON_AddNumberToObject(payload, "width", w);
    cJSON_AddNumberToObject(payload, "height", h);
    cJSON_AddStringToObject(payload, "size", full ? "full" : "half");
    cJSON_AddNumberToObject(payload, "bytes", (double)n);
    cJSON_AddStringToObject(payload, "source", source);
    const char *running = msw_app_running();
    if (running) {
        cJSON_AddStringToObject(payload, "app", running);
    }
    cJSON_AddNumberToObject(payload, "ms", ms);
    return r;
}

/* NULL: not an app.* command this handles (app.install is main/app.c's). */
cJSON *msw_app_command(const char *command, const cJSON *params)
{
    if (strncmp(command, "app.", 4)) {
        return NULL;
    }
    const char *c = command + 4;
    if (!strcmp(c, "doc")) {
        return page(guide_md_start, guide_md_end, params);
    }
    if (!strcmp(c, "avatar_doc")) {
        return page(avatar_guide_md_start, avatar_guide_md_end, params);
    }
    if (!strcmp(c, "api")) {
        return page(msw_app_h_start, msw_app_h_end, params);
    }
    if (!strcmp(c, "list")) {
        return list();
    }
    if (!strcmp(c, "run")) {
        const char *id = str(params, "id");
        if (!id) {
            return fail("missing_param", "id is required");
        }
        const char *code = NULL;
        char msg[200];
        if (!msw_app_start(id, &code, msg, sizeof(msg))) {
            return fail(code ? code : "load_failed", msg);
        }
        cJSON *payload;
        cJSON *r = ok_payload(&payload);
        cJSON_AddStringToObject(payload, "running", id);
        return r;
    }
    if (!strcmp(c, "stop")) {
        const char *running = msw_app_running();
        if (!running) {
            return fail("not_running", "no app is running");
        }
        cJSON *payload;
        cJSON *r = ok_payload(&payload);
        cJSON_AddStringToObject(payload, "stopped", running);
        int stuck = msw_app_stuck_secs();
        if (stuck) {
            char note[200];
            snprintf(note, sizeof(note),
                     "its code has not returned for %d s: if it does not end within 5 s, the device restarts to end "
                     "it, and app.log then says where it was (STUCK lines)", stuck);
            cJSON_AddStringToObject(payload, "note", note);
        }
        msw_app_stop();
        return r;
    }
    if (!strcmp(c, "remove")) {
        const char *id = str(params, "id");
        if (!id) {
            return fail("missing_param", "id is required");
        }
        if (!msw_app_remove(id)) {
            return fail("not_found", "no app with that id");
        }
        cJSON *payload;
        return ok_payload(&payload);
    }
    if (!strcmp(c, "source")) {
        const char *id = str(params, "id");
        if (!id) {
            return fail("missing_param", "id is required");
        }
        const char *code;
        char *text = msw_app_source(id, &code);
        if (!text) {
            return fail(code, !strcmp(code, "no_source")
                                  ? "this app was installed without its source (built with --no-source, or before build.py kept it)"
                                  : !strcmp(code, "not_found")     ? "no such app"
                                  : !strcmp(code, "broken_source") ? "the source in the package is broken"
                                                                   : "out of memory");
        }
        cJSON *r = page(text, text + strlen(text), params);
        free(text);
        return r;
    }
    if (!strcmp(c, "screenshot")) {
        return screenshot(params);
    }
    if (!strcmp(c, "avatar_list")) {
        static EXT_RAM_BSS_ATTR msw_app_info_t list[MSW_AVATAR_MAX];
        int n = msw_avatar_list(list, MSW_AVATAR_MAX);
        float ms;
        char name[MSW_APP_NAME_MAX + 1];
        const char *cur = msw_avatar_current(name, sizeof(name), &ms);
        cJSON *payload;
        cJSON *r = ok_payload(&payload);
        cJSON_AddStringToObject(payload, "current", cur);
        if (ms >= 0) {
            cJSON_AddNumberToObject(payload, "draw_ms", (int)(ms * 10 + 0.5f) / 10.0);
        }
        cJSON *arr = cJSON_AddArrayToObject(payload, "avatars");
        for (int i = 0; i < n; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "id", list[i].id);
            cJSON_AddStringToObject(a, "name", list[i].name);
            cJSON_AddNumberToObject(a, "version", list[i].ver);
            cJSON_AddNumberToObject(a, "bytes", list[i].bytes);
            cJSON_AddItemToArray(arr, a);
        }
        cJSON_AddNumberToObject(payload, "max_avatars", MSW_AVATAR_MAX);
        return r;
    }
    if (!strcmp(c, "avatar_preview")) {
        msw_avatar_preview_t q = { .id = str(params, "id"), .state = str(params, "state") };
        const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, "frames") : NULL;
        q.frames = cJSON_IsNumber(v) ? v->valueint : 0;
        v = params ? cJSON_GetObjectItemCaseSensitive(params, "seconds") : NULL;
        q.seconds = cJSON_IsNumber(v) ? (float)v->valuedouble : 0;
        v = params ? cJSON_GetObjectItemCaseSensitive(params, "quality") : NULL;
        q.quality = cJSON_IsNumber(v) ? (v->valueint < 10 ? 10 : v->valueint > 95 ? 95 : v->valueint) : 0;
        v = params ? cJSON_GetObjectItemCaseSensitive(params, "scale") : NULL;
        q.scale = cJSON_IsNumber(v) ? v->valueint : 0;
        if (!q.id || !q.state) {
            return fail("missing_param", "id and state are required");
        }
        if (!msw_avatar_preview(&q)) {
            return fail(q.code ? q.code : "failed", q.msg);
        }
        size_t cap = (q.n + 2) / 3 * 4 + 1, len = 0;
        unsigned char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!b64 || mbedtls_base64_encode(b64, cap, &len, q.jpg, q.n) != 0) {
            free(q.jpg);
            free(b64);
            return fail("out_of_memory", "out of memory");
        }
        free(q.jpg);
        cJSON *payload;
        cJSON *r = ok_payload(&payload);
        cJSON_AddStringToObject(payload, "format", "jpeg-base64");
        cJSON_AddStringToObject(payload, "data_base64", (const char *)b64);
        free(b64);
        cJSON_AddNumberToObject(payload, "width", q.w);
        cJSON_AddNumberToObject(payload, "height", q.h);
        cJSON_AddStringToObject(payload, "state", q.state);
        cJSON *times = cJSON_AddArrayToObject(payload, "frame_seconds");
        for (int i = 0; i < q.frames_out; i++) {
            cJSON_AddItemToArray(times, cJSON_CreateNumber((int)(q.times[i] * 100 + 0.5f) / 100.0));
        }
        cJSON_AddNumberToObject(payload, "draw_ms", (int)(q.avg_ms * 10 + 0.5f) / 10.0);
        cJSON_AddNumberToObject(payload, "draw_ms_max", (int)(q.max_ms * 10 + 0.5f) / 10.0);
        return r;
    }
    if (!strcmp(c, "avatar_use")) {
        const char *id = str(params, "id");
        if (!id) {
            return fail("missing_param", "id is required (\"official\" for the official avatar)");
        }
        const char *code = NULL;
        char msg[200];
        if (!msw_avatar_use(id, &code, msg, sizeof(msg))) {
            return fail(code ? code : "load_failed", msg);
        }
        cJSON *payload;
        cJSON *r = ok_payload(&payload);
        cJSON_AddStringToObject(payload, "current", msw_avatar_current(NULL, 0, NULL));
        return r;
    }
    if (!strcmp(c, "log")) {
        char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        if (!buf) {
            return fail("out_of_memory", "out of memory");
        }
        msw_app_log_text(buf, 8192);
        cJSON *payload;
        cJSON *r = ok_payload(&payload);
        cJSON_AddStringToObject(payload, "log", buf);
        free(buf);
        return r;
    }
    return NULL;
}
