/*
 * museStopWatch: stopwatch.doc, the whole-device guide for Muse
 * (../docs/stopwatch_guide.md, built in). Its second line is "版: <version>";
 * the version goes into the command's description (noise_control.cpp), so
 * Muse reads the guide again only when it changes.
 * Also side.add and side.list, Muse's way to the side chats (msw_side.h).
 */
#include <string.h>

#include "cJSON.h"
#include "msw_side.h"

extern const char guide_start[] asm("_binary_stopwatch_guide_md_start");

#define VERSION_KEY "版: "

const char *msw_doc_version(void)
{
    static char version[32];
    if (!version[0]) {
        const char *p = strstr(guide_start, VERSION_KEY);
        if (p) {
            p += strlen(VERSION_KEY);
            size_t n = strcspn(p, "\r\n");
            if (n >= sizeof(version)) {
                n = sizeof(version) - 1;
            }
            memcpy(version, p, n);
        }
        if (!version[0]) {
            strcpy(version, "?");
        }
    }
    return version;
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

static void add_side(cJSON *to, const msw_side_t *sc, bool current)
{
    cJSON_AddStringToObject(to, "id", sc->id);
    cJSON_AddStringToObject(to, "name", sc->name);
    cJSON_AddBoolToObject(to, "current", current);
}

static cJSON *side_list(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(r, "payload");
    int cur = msw_side_current();
    msw_side_t sc;
    cJSON_AddStringToObject(payload, "current", cur >= 0 && msw_side_get(cur, &sc) ? sc.id : "main");
    cJSON_AddNumberToObject(payload, "max", MSW_SIDE_MAX);
    cJSON *list = cJSON_AddArrayToObject(payload, "sides");
    int n = msw_side_count();
    for (int i = 0; i < n; i++) {
        if (msw_side_get(i, &sc)) {
            cJSON *o = cJSON_CreateObject();
            add_side(o, &sc, i == cur);
            cJSON_AddItemToArray(list, o);
        }
    }
    return r;
}

static cJSON *side_add(const cJSON *params)
{
    const cJSON *id = params ? cJSON_GetObjectItemCaseSensitive(params, "id") : NULL;
    const cJSON *name = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
    if (!cJSON_IsString(id) || !id->valuestring[0]) {
        return fail("missing_param", "id が必要です");
    }
    bool added = false;
    int i = msw_side_put(id->valuestring, cJSON_IsString(name) ? name->valuestring : NULL, &added);
    if (i == MSW_SIDE_BAD_ID) {
        return fail("bad_id", "id は UUID（8-4-4-4-12 の 16 進数）です");
    }
    if (i == MSW_SIDE_FULL) {
        return fail("full", "サイドチャットはすでに 8 個あります。ユーザーが本体で 1 つ消せば足せます");
    }
    msw_side_t sc;
    if (i < 0 || !msw_side_get(i, &sc)) {
        return fail("not_saved", "一覧を保存できませんでした");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(r, "payload");
    cJSON_AddBoolToObject(payload, "added", added);
    add_side(payload, &sc, i == msw_side_current());
    return r;
}

/* NULL: not one of these commands. */
cJSON *msw_doc_command(const char *command, const cJSON *params)
{
    if (!strcmp(command, "side.add")) {
        return side_add(params);
    }
    if (!strcmp(command, "side.list")) {
        return side_list();
    }
    if (strcmp(command, "stopwatch.doc")) {
        return NULL;
    }
    /* A result is {ok, payload}: noise_control.cpp's wrap_result_json sends
     * only those (and error), so anything else is dropped. */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(r, "payload");
    cJSON_AddStringToObject(payload, "version", msw_doc_version());
    cJSON_AddStringToObject(payload, "content", guide_start);
    return r;
}
