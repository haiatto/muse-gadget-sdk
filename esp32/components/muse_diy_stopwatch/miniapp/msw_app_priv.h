/*
 * museStopWatch mini apps: the firmware's side, shared between its files.
 *
 *   msw_app_core.c      store on the "apps" partition, loading, the app task,
 *                       the serial console (>msw.app.*), crash records
 *   msw_app_api.c       the functions apps call (msw_app.h), memory tracking
 *   msw_app_gfx.c       frame drawing, sprites, text
 *   msw_app_sound.c     the 4-channel synth, mixed over the local TTS
 *   msw_app_launcher.c  the launcher tile left of Muse's face
 *   msw_app_cmd.c       Muse's app.* commands
 *   msw_app_exports.c   the symbol table (tools/gen_msw_exports.py)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "msw_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSW_APP_DIR "/apps"
#define MSW_APP_ID_MAX 24
#define MSW_APP_NAME_MAX 32
#define MSW_APP_MAX_APPS 48
#define MSW_APP_MAX_BYTES (1024 * 1024)     /* one package */
#define MSW_APP_ICON 32                     /* icons are 32 x 32 RGB565 */

/* The package: this header, the icon (icon_size bytes, RGB565 little
 * endian, 0 for none), then the ELF. sdk/build.py writes it. */
typedef struct __attribute__((packed)) {
    char magic[4];                  /* "MSWA" */
    uint16_t api;                   /* MSW_API_VERSION the app was built for */
    uint16_t ver;                   /* the app's own version */
    char id[MSW_APP_ID_MAX];        /* file name and store key, [a-z0-9_] */
    char name[MSW_APP_NAME_MAX];    /* shown under the icon, UTF-8 */
    uint16_t icon_w, icon_h;
    uint32_t icon_size;
    uint32_t elf_size;
    uint32_t crc32;                 /* of everything after the header */
} msw_app_pkg_t;

typedef struct {
    char id[MSW_APP_ID_MAX];
    char name[MSW_APP_NAME_MAX];
    uint16_t ver, api;
    uint32_t bytes;
    uint16_t *icon;                 /* MSW_APP_ICON squared, PSRAM; NULL for none */
} msw_app_info_t;

/* ---- core ---- */
void msw_app_core_init(void);                /* once, from the launcher build */
int msw_app_list(msw_app_info_t *out, int max);   /* installed apps, by name */
/* The apps volume's size and free bytes; false when it is not mounted. */
bool msw_app_space(uint32_t *total, uint32_t *free_bytes);
/* Checks and stores a package (data stays the caller's). On failure returns
 * false with *code and a message in msg. */
bool msw_app_install(const uint8_t *data, size_t n, char *id_out, uint16_t *ver_out,
                     const char **code, char *msg, size_t msg_len);
bool msw_app_remove(const char *id);
bool msw_app_start(const char *id, const char **code, char *msg, size_t msg_len);
void msw_app_stop(void);                     /* asks the running app to end; returns at once */
const char *msw_app_running(void);           /* its id, or NULL */
/* The app log: the last lines, oldest first, joined by '\n', into buf. */
size_t msw_app_log_text(char *buf, size_t cap);
void msw_app_logf(const char *fmt, ...);
uint32_t msw_app_changes(void);              /* bumps on install, remove, start, stop */
/* The app's files kept in its package (sdk/build.py adds them): malloc'd
 * text, or NULL with code "not_found", "no_source" or "out_of_memory". */
char *msw_app_source(const char *id, const char **code);

/* Calls fn(arg) on the service task, whose stack is internal RAM: the one
 * that may touch flash (FAT on the apps partition). Waits for it. */
void msw_app_service(void (*fn)(void *arg), void *arg);

/* ---- api (msw_app_api.c) ---- */
/* Before app_main; code_lo..code_hi is the app's loaded code on the
 * instruction bus (timers and animations calling into it are removed when it
 * ends), code_d the same bytes on the data bus (readable byte by byte). */
void msw_api_begin(const char *id, uintptr_t code_lo, uintptr_t code_hi, uintptr_t code_d);
/* An app's pointer the firmware will read n bytes from (for strings, n = 1):
 * one into the app's code is turned into its data-bus copy, one the firmware
 * can't read gives NULL. Either is logged once per fn while the app runs. */
const void *msw_api_readable(const void *p, size_t n, const char *fn);
void *msw_api_writable(void *p, size_t n, const char *fn);
void msw_api_set_rodata(uintptr_t text_vaddr, uintptr_t ro_vaddr, size_t ro_size, uintptr_t ro_addr);

/* ---- crash report (msw_app_crash.c) ---- */
void msw_crash_app(const char *id, uintptr_t code_lo, uintptr_t code_hi, uintptr_t text_vaddr);   /* NULL: none */
void msw_crash_boot_report(void);            /* once at start: a crash record to app.log */
void msw_api_end(void);                      /* after the exit callback: frees all */
void msw_api_tick(uint32_t now_ms);          /* each loop of the app task, locked */
void msw_api_button(int button, bool down);  /* queued from the input task */
bool msw_api_menu_owned(void);               /* msw_menu_own(true) */
void msw_api_pause(bool paused);             /* the app menu opened or closed */
/* ---- the app menu (msw_app_menu.c), on the app task, display locked ---- */
enum { MSW_MENU_NONE, MSW_MENU_GO_ON, MSW_MENU_QUIT };
void msw_menu_open(const char *title);
void msw_menu_close(void);
bool msw_menu_is_open(void);
void msw_menu_poll(void);                    /* while open: Muse's face and caption */
int msw_menu_choice(void);                   /* the button tapped since the last call */
int msw_api_fps(void);
bool msw_api_exit_asked(void);
void msw_api_call_exit_cb(void);
/* Muse's reply text, from the chat session (weak there): handed to the
 * app's msw_on_muse_reply callback on its next tick. */
void msw_app_muse_reply(const char *text);
bool msw_imu_probe(msw_imu_t *out);          /* >msw.imu: works with no app */
void msw_imu_release(void);                  /* the app ended: sensor off */

/* ---- gfx (msw_app_gfx.c) ---- */
void msw_gfx_begin(lv_obj_t *screen);
bool msw_gfx_frame_mode(void);              /* the app made its frame (msw_fb) */
void msw_gfx_end(void);
void msw_gfx_overlay_changed(void);

/* ---- sound (msw_app_sound.c) ---- */
void msw_sound_init(void);
void msw_sound_app(bool running);            /* keeps the speaker open while an app runs */

/* ---- Muse overlay (msw_app_api.c) ---- */
void msw_overlay_draw(uint16_t *row_px, int y, int x1, int x2);   /* frame mode, per row */
bool msw_overlay_active(void);

/* ---- launcher ---- */
void msw_launcher_refresh(void);

#ifdef __cplusplus
}
#endif
