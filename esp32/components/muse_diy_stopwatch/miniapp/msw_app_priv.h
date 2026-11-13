/*
 * museStopWatch mini apps: the firmware's side, shared between its files.
 *
 *   msw_app_core.c      installing, loading, the app task, the serial
 *                       console (>msw.app.*), crash records
 *   msw_app_pkgs.c      the packages, each in one piece on "msw_pkgs"
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
#define MSW_APP_MAX_BYTES (1024 * 1024)     /* a package sent whole (not streamed) */
#define MSW_AVATAR_MAX 16
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

/* The package after the ELF: sections, each a 4-byte tag and a 32-bit
 * length of what follows (sdk/build.py):
 *   "MSWD"  data files: u32 count, then count of msw_pkg_file_t, then the
 *           files (offsets from the package's start, 16-aligned)
 *   "MSWZ"  the app's source in zlib: u32 zlib length is the section's,
 *           then u32 text length, then the zlib (the length counts both)
 *   "MSWS"  the source as it is (packages before 2026-11-11)
 */
typedef struct __attribute__((packed)) {
    char name[48];          /* NUL-terminated */
    uint32_t off, size;
} msw_pkg_file_t;

/* ---- the package store (msw_app_pkgs.c), on the service task ---- */
typedef struct {
    uint32_t off, size, seq;
    bool avatar;
    char id[MSW_APP_ID_MAX];
} msw_pkg_entry_t;
typedef struct msw_pkg_writer msw_pkg_writer_t;
bool msw_pkgs_init(void);
bool msw_pkgs_ready(void);
int msw_pkgs_list(msw_pkg_entry_t *out, int max);     /* by place */
int msw_pkgs_count(bool avatar);
/* The package as it lies in flash (mapped), or NULL. Stays valid until the
 * store changes, except for the running app's (compacting leaves it). */
const uint8_t *msw_pkgs_find(const char *id, bool avatar, size_t *n);
void msw_pkgs_space(uint32_t *total, uint32_t *free_bytes, uint32_t *largest);
bool msw_pkgs_remove(const char *id, bool avatar);
int msw_pkgs_compact(const char *pinned_id);           /* bytes moved, -1 on error */
/* Writing: size 0 when not known (then the largest gap; when it overflows,
 * compact and write it again). */
msw_pkg_writer_t *msw_pkgs_begin(size_t size, const char *pinned_id, const char **code, char *msg, size_t msg_len);
bool msw_pkgs_write(msw_pkg_writer_t *w, const void *p, size_t n);
bool msw_pkgs_overflowed(const msw_pkg_writer_t *w, size_t *written);
const uint8_t *msw_pkgs_written(const msw_pkg_writer_t *w, size_t *n);   /* to check before commit */
bool msw_pkgs_commit(msw_pkg_writer_t *w);              /* frees w; replaces the old one of its id */
void msw_pkgs_abort(msw_pkg_writer_t *w);               /* frees w */

/* ---- core ---- */
void msw_app_core_init(void);                /* once, from the launcher build */
int msw_app_list(msw_app_info_t *out, int max);   /* installed apps, by name */
/* The package store's size and free bytes; false when there is none. */
bool msw_app_space(uint32_t *total, uint32_t *free_bytes);
/* Installing a package that arrives in parts (app.install from the VM, the
 * serial put): any task. size 0 when not known. write returns false when it
 * fails; *restart then says the store made room and the package must be
 * sent again from its start. end checks and stores it (or, with ok false,
 * drops it). */
typedef struct msw_app_inst msw_app_inst_t;
msw_app_inst_t *msw_app_install_begin(size_t size, const char **code, char *msg, size_t msg_len);
/* The package's size, if it becomes known before the first write. False:
 * there is no room for it (the reason goes to msw_app_install_end). */
bool msw_app_install_size(msw_app_inst_t *in, size_t size);
bool msw_app_install_write(msw_app_inst_t *in, const uint8_t *p, size_t n, bool *restart);
bool msw_app_install_end(msw_app_inst_t *in, bool ok, char *id_out, uint16_t *ver_out, const char **code,
                         char *msg, size_t msg_len);
/* Checks and stores a package (data stays the caller's). On failure returns
 * false with *code and a message in msg. */
bool msw_app_install(const uint8_t *data, size_t n, char *id_out, uint16_t *ver_out,
                     const char **code, char *msg, size_t msg_len);
bool msw_app_remove(const char *id);
bool msw_app_start(const char *id, const char **code, char *msg, size_t msg_len);
void msw_app_stop(void);                     /* asks the running app to end; returns at once */
const char *msw_app_running(void);           /* its id, or NULL */
int msw_app_stuck_secs(void);                /* how long its code has not returned, if 3 s or more; else 0 */
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
/* The same without waiting (from code that must not block on flash). */
void msw_app_service_async(void (*fn)(void *arg), void *arg);

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
/* The app task (a TaskHandle_t) did not return from `where` when asked to end:
 * suspends it and keeps where it was, for the report after the restart. */
void msw_crash_stuck(void *task, const char *where, unsigned secs);
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
/* The start screen of an app that uses PORT.A: what it says to plug in and
 * what it does with it,
 * "はじめる" (MSW_MENU_GO_ON, counted only with the blue button held) and
 * "やめる" (MSW_MENU_QUIT). Closed with
 * msw_menu_close. Display locked. */
void msw_menu_open_start(const char *title, const char *connect, const char *about);
void msw_menu_start_nudge(void);
void msw_menu_start_ready(bool on);          /* the blue button held: "はじめる" green, else grey */             /* "はじめる" without the blue button held */
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
void msw_mic_app_end(void);
void msw_port_allow(bool on);               /* msw_app_port.c: the user picked "はじめる" */
void msw_port_app_end(void);                /* msw_app_port.c: PORT.A off with the app */                 /* msw_app_mic.c: the mic goes off with the app */
void msw_sound_app(bool running);            /* keeps the speaker open while an app runs */

/* ---- Muse overlay (msw_app_api.c) ---- */
void msw_overlay_draw(uint16_t *row_px, int y, int x1, int x2);   /* frame mode, per row */
bool msw_overlay_active(void);

/* ---- launcher ---- */
void msw_launcher_refresh(void);

/* ---- avatars (msw_avatar.c, idea 009) ---- */
/* Avatars are packages too ("MSWV", sdk/build.py with "kind": "avatar"),
 * stored as <id>.mswava beside the apps; the launcher does not list them. */
typedef struct esp_elf esp_elf_t;
/* On the service task: reads, checks and relocates the avatar's ELF. */
bool msw_app_load_avatar(const char *id, esp_elf_t *elf, char *name, const char **code, char *err, size_t err_len);
void msw_app_unload_avatar(esp_elf_t *elf);
void msw_app_init_arrays(esp_elf_t *elf);
int msw_app_list_avatars(msw_app_info_t *out, int max);   /* by name */
void msw_avatar_boot(void);                 /* once, after the store is mounted */
void msw_avatar_installed(const char *id);  /* reloads it if it is on screen */
void msw_avatar_removing(const char *id);   /* back to the official one if it is on screen */
bool msw_avatar_use(const char *id, const char **code, char *msg, size_t msg_len);   /* "official" too */
/* The avatar on screen ("official" for the official one), its name and its
 * average draw time (-1: not measured yet). */
const char *msw_avatar_current(char *name, size_t name_len, float *avg_ms);
int msw_avatar_list(msw_app_info_t *out, int max);   /* installed avatars, by name */
/* app.avatar_preview: the avatar in one state, frames over time in a grid,
 * as a JPEG (malloc'd in jpg). On failure false with code and msg. */
typedef struct {
    const char *id, *state;     /* state: boot idle listening thinking working speaking happy error off */
    int frames;                 /* 1..9 (0: 4) */
    float seconds;              /* 0: the state's own length */
    int quality;                /* 0: 60 */
    int scale;                  /* 1..4 screen pixels a cell (0: 2) */
    uint8_t *jpg;
    size_t n;
    int w, h, frames_out;
    float times[9];             /* each frame's seconds into the state */
    float avg_ms, max_ms;       /* render time */
    const char *code;
    char msg[160];
    void *done;
} msw_avatar_preview_t;
bool msw_avatar_preview(msw_avatar_preview_t *q);
const char *msw_avatar_state_names(void);

#ifdef __cplusplus
}
#endif
