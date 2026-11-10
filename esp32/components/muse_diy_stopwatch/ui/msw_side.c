/*
 * museStopWatch: side chats (msw_side.h). Called from the LVGL task and the
 * Web server; NVS namespace "msw", keys side_list (blob), side_cur, side_seq.
 */
#include "msw_side.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "msw_side";

#define NS "msw"

static SemaphoreHandle_t s_lock;
static msw_side_t s_list[MSW_SIDE_MAX];
static int s_n;
static int s_cur = -1;
static uint16_t s_seq;
static bool s_loaded;

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void load_locked(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_list);
    if (nvs_get_blob(h, "side_list", s_list, &len) == ESP_OK && len % sizeof(msw_side_t) == 0) {
        s_n = (int)(len / sizeof(msw_side_t));
    }
    int8_t cur = -1;
    if (nvs_get_i8(h, "side_cur", &cur) == ESP_OK && cur >= 0 && cur < s_n) {
        s_cur = cur;
    }
    nvs_get_u16(h, "side_seq", &s_seq);
    nvs_close(h);
    for (int i = 0; i < s_n; i++) {
        s_list[i].id[MSW_SIDE_ID_LEN] = '\0';
        s_list[i].name[MSW_SIDE_NAME_MAX] = '\0';
    }
}

static bool save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = s_n ? nvs_set_blob(h, "side_list", s_list, s_n * sizeof(msw_side_t)) : nvs_erase_key(h, "side_list");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_set_i8(h, "side_cur", (int8_t)s_cur);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "side_seq", s_seq);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "couldn't save the side chats: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/* Points the chat at the one in use. */
static void apply_locked(void)
{
    muse_hatch_set_session_id(s_cur >= 0 ? s_list[s_cur].id : "");
}

void msw_side_init(void)
{
    lock();
    load_locked();
    apply_locked();
    unlock();
}

int msw_side_count(void)
{
    lock();
    load_locked();
    int n = s_n;
    unlock();
    return n;
}

bool msw_side_get(int i, msw_side_t *out)
{
    lock();
    load_locked();
    bool ok = i >= 0 && i < s_n;
    if (ok) {
        *out = s_list[i];
    }
    unlock();
    return ok;
}

int msw_side_current(void)
{
    lock();
    load_locked();
    int cur = s_cur;
    unlock();
    return cur;
}

int msw_side_find(const char *id)
{
    lock();
    load_locked();
    int found = -1;
    for (int i = 0; id && i < s_n; i++) {
        if (!strcmp(s_list[i].id, id)) {
            found = i;
            break;
        }
    }
    unlock();
    return found;
}

bool msw_side_select(int i)
{
    lock();
    load_locked();
    bool ok = i >= -1 && i < s_n;
    if (ok && i != s_cur) {
        int was = s_cur;
        s_cur = i;
        if (save_locked()) {
            apply_locked();
        } else {
            s_cur = was;   /* not saved: stay where it was, as after a restart */
            ok = false;
        }
    }
    unlock();
    return ok;
}

static void make_uuid(char out[MSW_SIDE_ID_LEN + 1])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;   /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80;   /* RFC 4122 variant */
    snprintf(out, MSW_SIDE_ID_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* A UUID in any case, with spaces around it, to lowercase: false if it isn't one. */
static bool clean_id(const char *in, char out[MSW_SIDE_ID_LEN + 1])
{
    while (*in == ' ') {
        in++;
    }
    size_t n = 0;
    for (; in[n] && in[n] != ' '; n++) {
        if (n >= MSW_SIDE_ID_LEN) {
            return false;
        }
        char c = in[n];
        bool dash = n == 8 || n == 13 || n == 18 || n == 23;
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
        out[n] = c;
    }
    for (const char *t = in + n; *t; t++) {
        if (*t != ' ') {
            return false;
        }
    }
    out[n] = '\0';
    return n == MSW_SIDE_ID_LEN;
}

int msw_side_add(const char *id)
{
    char want[MSW_SIDE_ID_LEN + 1] = "";
    if (id && id[0] && !clean_id(id, want)) {
        return MSW_SIDE_BAD_ID;
    }
    if (want[0]) {
        int at = msw_side_find(want);
        if (at >= 0) {
            return msw_side_select(at) ? at : MSW_SIDE_FULL;
        }
    }
    lock();
    load_locked();
    int i = MSW_SIDE_FULL;
    if (s_n < MSW_SIDE_MAX) {
        i = s_n;
        if (want[0]) {
            memcpy(s_list[i].id, want, sizeof(want));
        } else {
            make_uuid(s_list[i].id);
        }
        s_seq++;
        snprintf(s_list[i].name, sizeof(s_list[i].name), "Side %u", (unsigned)s_seq);
        int was = s_cur;
        s_n++;
        s_cur = i;
        if (save_locked()) {
            apply_locked();
        } else {
            s_n--;
            s_seq--;
            s_cur = was;
            i = MSW_SIDE_FULL;
        }
    }
    unlock();
    return i;
}

/* Copies name, cut to fit on a UTF-8 character boundary. */
static void copy_name(char out[MSW_SIDE_NAME_MAX + 1], const char *name)
{
    size_t n = strnlen(name, MSW_SIDE_NAME_MAX + 1);
    if (n > MSW_SIDE_NAME_MAX) {
        n = MSW_SIDE_NAME_MAX;
        while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) {
            n--;   /* name[n] continues a character: cut before that character */
        }
    }
    memcpy(out, name, n);
    out[n] = '\0';
}

int msw_side_put(const char *id, const char *name, bool *added)
{
    char want[MSW_SIDE_ID_LEN + 1];
    *added = false;
    if (!id || !clean_id(id, want)) {
        return MSW_SIDE_BAD_ID;
    }
    bool rename = name && name[0];
    lock();
    load_locked();
    int i = -1;
    for (int k = 0; k < s_n; k++) {
        if (!strcmp(s_list[k].id, want)) {
            i = k;
            break;
        }
    }
    if (i >= 0 && !rename) {
        unlock();
        return i;
    }
    if (i < 0 && s_n >= MSW_SIDE_MAX) {
        unlock();
        return MSW_SIDE_FULL;
    }
    msw_side_t was_entry = i >= 0 ? s_list[i] : (msw_side_t){0};
    uint16_t was_seq = s_seq;
    bool fresh = i < 0;
    if (fresh) {
        i = s_n++;
        memcpy(s_list[i].id, want, sizeof(want));
        s_seq++;
        snprintf(s_list[i].name, sizeof(s_list[i].name), "Side %u", (unsigned)s_seq);
    }
    if (rename) {
        copy_name(s_list[i].name, name);
    }
    if (save_locked()) {
        *added = fresh;
    } else if (fresh) {
        s_n--;
        s_seq = was_seq;
        i = MSW_SIDE_NOT_SAVED;
    } else {
        s_list[i] = was_entry;
        i = MSW_SIDE_NOT_SAVED;
    }
    unlock();
    return i;
}

bool msw_side_delete(int i)
{
    lock();
    load_locked();
    bool ok = i >= 0 && i < s_n;
    if (ok) {
        msw_side_t gone = s_list[i];
        int was = s_cur;
        memmove(&s_list[i], &s_list[i + 1], (s_n - i - 1) * sizeof(msw_side_t));
        s_n--;
        s_cur = s_cur == i ? -1 : s_cur > i ? s_cur - 1 : s_cur;
        if (save_locked()) {
            apply_locked();
        } else {
            memmove(&s_list[i + 1], &s_list[i], (s_n - i) * sizeof(msw_side_t));
            s_list[i] = gone;
            s_n++;
            s_cur = was;
            ok = false;
        }
    }
    unlock();
    return ok;
}
