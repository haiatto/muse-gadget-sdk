/*
 * museStopWatch: stopwatch.doc, the whole-device guide for Muse
 * (../docs/stopwatch_guide.md, built in). Its second line is "版: <version>";
 * the version goes into the command's description (noise_control.cpp), so
 * Muse reads the guide again only when it changes.
 */
#include <string.h>

#include "cJSON.h"

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

/* NULL: not stopwatch.doc. */
cJSON *msw_doc_command(const char *command, const cJSON *params)
{
    (void)params;
    if (strcmp(command, "stopwatch.doc")) {
        return NULL;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "version", msw_doc_version());
    cJSON_AddStringToObject(r, "content", guide_start);
    return r;
}
