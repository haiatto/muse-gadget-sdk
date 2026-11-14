/*
 * museStopWatch mini apps: commands from Muse to the running app (app.call;
 * msw_app.h, msw_on_muse_call and msw_arg_*).
 *
 * Muse's command arrives on the task that runs device commands. It leaves
 * the name and a copy of the arguments in one slot and waits; the app's task
 * takes them between ticks (msw_call_deliver, display locked), calls the
 * app, and leaves a copy of the reply. One call at a time.
 *
 * The slot's state says who owns what:
 *   IDLE     nothing in it
 *   WAITING  name and args in it; the caller waits; the caller may take them back
 *   TAKEN    the app's task has them and runs the app's callback
 *   DONE     the reply is in it for the caller
 *   LEFT     the caller gave up while TAKEN: the app's task drops the reply
 * The arguments are freed by whoever has them last, so a call that times out
 * never frees what the app still reads.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "msw_app_priv.h"

#define REPLY_MAX 2000

enum { CALL_IDLE, CALL_WAITING, CALL_TAKEN, CALL_DONE, CALL_LEFT };

static msw_call_cb s_cb;            /* the app's; set and read on the app's task */
static void *s_cb_user;
static volatile bool s_has_cb;      /* read by the caller */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_state;
static char *s_name;
static cJSON *s_args;
static char *s_reply;
static bool s_known;
static bool s_ended;                /* the app ended before it took the call */
static SemaphoreHandle_t s_done;    /* given when the reply (or the end) is there */
static SemaphoreHandle_t s_lock;    /* one caller at a time */

void msw_on_muse_call(msw_call_cb cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
    s_has_cb = cb != NULL;
}

/* msw_api_begin: before app_main, on the app's task. */
void msw_call_begin(void)
{
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
        s_lock = xSemaphoreCreateMutex();
    }
    s_cb = NULL;
    s_cb_user = NULL;
    s_has_cb = false;
}

/* msw_api_end: the app is over. A call it did not take ends now. */
void msw_call_end(void)
{
    s_cb = NULL;
    s_has_cb = false;
    char *name = NULL;
    cJSON *args = NULL;
    bool wake = false;
    portENTER_CRITICAL(&s_mux);
    if (s_state == CALL_WAITING) {
        name = s_name;
        args = s_args;
        s_name = NULL;
        s_args = NULL;
        s_reply = NULL;
        s_known = false;
        s_ended = true;
        s_state = CALL_DONE;
        wake = true;
    }
    portEXIT_CRITICAL(&s_mux);
    free(name);
    cJSON_Delete(args);
    if (wake) {
        xSemaphoreGive(s_done);
    }
}

/* The app's reply, copied into PSRAM (cut at REPLY_MAX, at a whole UTF-8
 * letter). */
static char *copy_reply(const char *text)
{
    text = msw_api_readable(text, 1, "the msw_on_muse_call reply");
    if (!text) {
        return NULL;
    }
    size_t n = strnlen(text, REPLY_MAX + 1);
    if (n > REPLY_MAX) {
        n = REPLY_MAX;
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    char *out = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (out) {
        memcpy(out, text, n);
        out[n] = '\0';
    }
    return out;
}

/* The app's task, display locked, between ticks. */
void msw_call_deliver(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_state != CALL_WAITING) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_state = CALL_TAKEN;
    char *name = s_name;
    cJSON *args = s_args;
    s_name = NULL;
    s_args = NULL;
    portEXIT_CRITICAL(&s_mux);

    const char *out = s_cb ? s_cb(name, (const msw_args_t *)args, s_cb_user) : NULL;
    char *reply = out ? copy_reply(out) : NULL;
    bool known = out != NULL;
    free(name);
    cJSON_Delete(args);

    portENTER_CRITICAL(&s_mux);
    bool left = s_state == CALL_LEFT;
    if (left) {
        s_state = CALL_IDLE;
    } else {
        s_reply = reply;
        s_known = known;
        s_ended = false;
        s_state = CALL_DONE;
    }
    portEXIT_CRITICAL(&s_mux);
    if (left) {
        free(reply);
        msw_app_logf("app.call: the app answered after Muse stopped waiting (keep the callback short)");
    } else {
        xSemaphoreGive(s_done);
    }
}

const char *msw_app_call(const char *name, const cJSON *args, int timeout_ms, char **reply)
{
    *reply = NULL;
    if (!msw_app_running() || !s_lock) {
        return "not_running";
    }
    if (!s_has_cb) {
        return "no_commands";
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return "busy";
    }
    char *n = strdup(name);
    cJSON *a = cJSON_IsObject(args) ? cJSON_Duplicate(args, true) : cJSON_CreateObject();
    if (!n || !a) {
        free(n);
        cJSON_Delete(a);
        xSemaphoreGive(s_lock);
        return "out_of_memory";
    }
    xSemaphoreTake(s_done, 0);      /* nothing old in it */
    portENTER_CRITICAL(&s_mux);
    bool idle = s_state == CALL_IDLE;
    if (idle) {
        s_name = n;
        s_args = a;
        s_ended = false;
        s_state = CALL_WAITING;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!idle) {                    /* the app still runs a call Muse gave up on */
        free(n);
        cJSON_Delete(a);
        xSemaphoreGive(s_lock);
        return "busy";
    }

    const char *err = NULL;
    bool got = xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    char *back_name = NULL;
    cJSON *back_args = NULL;
    portENTER_CRITICAL(&s_mux);
    if (s_state == CALL_DONE) {     /* also when it came just after the wait */
        *reply = s_reply;
        err = s_ended ? "not_running" : s_known ? NULL : "unknown_command";
        s_reply = NULL;
        s_state = CALL_IDLE;
    } else if (s_state == CALL_WAITING) {   /* never taken: no ticks (the app menu open?) */
        back_name = s_name;
        back_args = s_args;
        s_name = NULL;
        s_args = NULL;
        s_state = CALL_IDLE;
        err = "not_taken";
    } else {                        /* TAKEN: the app's callback has not returned */
        s_state = CALL_LEFT;
        err = "timeout";
    }
    portEXIT_CRITICAL(&s_mux);
    (void)got;
    free(back_name);
    cJSON_Delete(back_args);
    xSemaphoreGive(s_lock);
    return err;
}

/* ---- the app reads its arguments ------------------------------------------ */

static const cJSON *item(const msw_args_t *args, const char *key, const char *fn)
{
    const cJSON *o = (const cJSON *)args;
    key = msw_api_readable(key, 1, fn);
    return o && key ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}

static const cJSON *at(const msw_args_t *args, const char *key, int i, const char *fn)
{
    const cJSON *v = item(args, key, fn);
    return cJSON_IsArray(v) && i >= 0 ? cJSON_GetArrayItem(v, i) : NULL;
}

static int to_int(double d)
{
    return d >= 2147483647.0 ? 2147483647 : d <= -2147483648.0 ? (int)-2147483647 - 1 : (int)d;
}

bool msw_arg_has(const msw_args_t *args, const char *key)
{
    return item(args, key, "msw_arg_has") != NULL;
}

int msw_arg_int(const msw_args_t *args, const char *key, int def)
{
    const cJSON *v = item(args, key, "msw_arg_int");
    return cJSON_IsNumber(v) ? to_int(v->valuedouble) : def;
}

float msw_arg_float(const msw_args_t *args, const char *key, float def)
{
    const cJSON *v = item(args, key, "msw_arg_float");
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

bool msw_arg_bool(const msw_args_t *args, const char *key, bool def)
{
    const cJSON *v = item(args, key, "msw_arg_bool");
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

const char *msw_arg_str(const msw_args_t *args, const char *key, const char *def)
{
    const cJSON *v = item(args, key, "msw_arg_str");
    return cJSON_IsString(v) ? v->valuestring : def;
}

int msw_arg_len(const msw_args_t *args, const char *key)
{
    const cJSON *v = item(args, key, "msw_arg_len");
    return cJSON_IsArray(v) ? cJSON_GetArraySize(v) : -1;
}

int msw_arg_int_at(const msw_args_t *args, const char *key, int i, int def)
{
    const cJSON *v = at(args, key, i, "msw_arg_int_at");
    return cJSON_IsNumber(v) ? to_int(v->valuedouble) : def;
}

float msw_arg_float_at(const msw_args_t *args, const char *key, int i, float def)
{
    const cJSON *v = at(args, key, i, "msw_arg_float_at");
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
