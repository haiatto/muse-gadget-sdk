/*
 * museStopWatch mini-app API, version 1.
 *
 * A mini app is a small ELF file that the StopWatch loads into PSRAM and
 * runs. It can call only the functions in this header, the lvgl functions
 * listed in the SDK (sdk/exports.list) and a small part of the C library.
 *
 * The screen is round: 466 x 466 pixels, centre (233, 233), radius 233.
 * Corners of the square are not visible.
 *
 * Life of an app:
 *   1. int app_main(int argc, char **argv) runs once. Build the screen and
 *      set the callbacks here, then return 0. (Return non-zero to give up.)
 *   2. The firmware calls the tick callback up to `fps` times a second, and
 *      the button callback on each press and release of the blue button
 *      (upper right; the talk button outside apps): it is the app's.
 *   3. The yellow button (upper left) opens the app menu: the app pauses (no
 *      ticks) and the user picks "つづける" (go on) or "おわる" (quit). This
 *      is how users quit apps. An app can take the yellow button with
 *      msw_menu_own(true); then it must give its own way to quit and tell
 *      the user what it is (holding yellow 1.5 s still quits).
 *   4. The app ends when it calls msw_exit(), when the user picks "おわる",
 *      or when Muse stops it. The exit callback runs then; the firmware frees the app's
 *      screen, frame buffer, sounds and timers after it.
 *
 * All callbacks (and app_main) run on the app's own task with the display
 * locked: lvgl calls are safe there. Keep each call short (a few ms), or the
 * screen stops updating. Never call these functions from another task.
 *
 * Two ways to draw (pick one per app):
 *   - lvgl: build widgets on msw_screen(). Good for buttons, labels, menus.
 *   - frame: msw_fb() gives a 466 x 466 RGB565 buffer. Draw into it with the
 *     msw_gfx_* functions (or your own code) and call msw_fb_present() once
 *     per tick. Good for games. lvgl is not drawn while the frame is used.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef MSW_HOST
/* sdk/build.py running an avatar on the build machine: no lvgl there. */
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_font_t lv_font_t;
#else
#include "lvgl.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MSW_API_VERSION 1
#define MSW_SCREEN_W 466
#define MSW_SCREEN_H 466

/* ---- life ---------------------------------------------------------------- */

typedef void (*msw_tick_cb)(uint32_t dt_ms, void *user);     /* dt: ms since the last tick */
typedef void (*msw_button_cb)(int button, bool down, void *user);
typedef void (*msw_exit_cb)(void *user);
typedef void (*msw_pause_cb)(bool paused, void *user);

#define MSW_BUTTON_BLUE 0   /* the blue rim button (upper right): the app's */
#define MSW_BUTTON_YELLOW 1 /* the yellow one (upper left): only with msw_menu_own(true) */

void msw_on_tick(msw_tick_cb cb, void *user);
void msw_set_fps(int fps);                       /* 1..60, default 30 */
void msw_on_button(msw_button_cb cb, void *user);
void msw_on_exit(msw_exit_cb cb, void *user);
void msw_exit(void);                             /* ends after the current callback */
/* The app menu (yellow button) opened (paused = true) or closed. The firmware
 * stops the sounds when it opens; start the music again here if needed. */
void msw_on_pause(msw_pause_cb cb, void *user);
/* true: the yellow button goes to the button callback instead of opening
 * the app menu. Then show the user how to quit (a quit button of your own,
 * or "hold the yellow button"). */
void msw_menu_own(bool own);

uint32_t msw_millis(void);                       /* ms since the app started */
uint32_t msw_random(void);                       /* hardware random, 32 bits */
void msw_log(const char *fmt, ...);              /* one line; Muse reads it with app.log */

/* ---- touch --------------------------------------------------------------- */

/* The finger now: true and its position while the screen is touched. Works
 * in both drawing ways. The screen reads ONE finger: two at once are not
 * reliable (it can lose a held finger), so never ask the user to hold one
 * screen button and press another. Put the second action on the blue
 * button (msw_on_button) instead: run with a finger, jump with blue. */
bool msw_touch(int *x, int *y);

/* ---- lvgl drawing -------------------------------------------------------- */

lv_obj_t *msw_screen(void);                      /* the app's lvgl screen, black */
/* A font for labels: size 14, 16, 20 or 28 (Latin letters, digits, symbols),
 * or 0 for 16 px Japanese (kana, kanji and Latin). */
const lv_font_t *msw_font(int size);

/* ---- frame drawing ------------------------------------------------------- */

typedef struct {
    uint16_t *px;        /* RGB565, native byte order: px[y * stride + x] */
    int w, h;            /* size in pixels */
    int stride;          /* pixels per row */
} msw_gfx_t;

/* Switches to frame drawing and returns the screen buffer (in PSRAM, black
 * at first). NULL if out of memory. Call once, in app_main. */
msw_gfx_t *msw_fb(void);
void msw_fb_present(void);                       /* sends the whole buffer to the screen */
void msw_fb_present_rect(int x, int y, int w, int h);   /* only this part */

/* An off-screen buffer of your own (for a background or a scaled scene);
 * freed with msw_gfx_free or when the app ends. */
msw_gfx_t *msw_gfx_new(int w, int h);
void msw_gfx_free(msw_gfx_t *g);

#define MSW_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void msw_gfx_fill(msw_gfx_t *g, uint16_t color);
void msw_gfx_pixel(msw_gfx_t *g, int x, int y, uint16_t color);
void msw_gfx_rect(msw_gfx_t *g, int x, int y, int w, int h, uint16_t color);       /* outline */
void msw_gfx_fill_rect(msw_gfx_t *g, int x, int y, int w, int h, uint16_t color);
void msw_gfx_line(msw_gfx_t *g, int x0, int y0, int x1, int y1, uint16_t color);
void msw_gfx_circle(msw_gfx_t *g, int cx, int cy, int r, uint16_t color);           /* outline */
void msw_gfx_fill_circle(msw_gfx_t *g, int cx, int cy, int r, uint16_t color);
/* Copies src onto g at (x, y), each pixel `scale` times (1..8). */
void msw_gfx_blit(msw_gfx_t *g, int x, int y, const msw_gfx_t *src, int scale);

/* Text with the firmware's fonts. size: 14, 16, 20 or 28 (Latin), or 0 for
 * the 16 px Japanese font (kana and kanji). Returns the width drawn. */
int msw_gfx_text(msw_gfx_t *g, int x, int y, const char *utf8, uint16_t color, int size);
int msw_text_width(const char *utf8, int size);

/* ---- sprites from text art ----------------------------------------------- */

/* A palette entry: a character in the art and its colour. */
typedef struct {
    char ch;
    uint16_t color;
} msw_pal_t;

/* A sprite: pixels plus a transparent colour. */
typedef struct {
    int w, h;
    uint16_t *px;
    uint16_t key;        /* pixels of this colour are not drawn */
} msw_sprite_t;

/* Builds a sprite from rows of characters, one character a pixel, all rows
 * the same length. '.' and ' ' are transparent; other characters take their
 * colour from pal (ended by an entry with ch 0). NULL if out of memory or a
 * character is not in pal (msw_log tells which). Freed when the app ends. */
msw_sprite_t *msw_sprite_from_art(const char *const *rows, int nrows, const msw_pal_t *pal);
/* A blank sprite (all transparent) to draw into yourself. */
msw_sprite_t *msw_sprite_new(int w, int h);
void msw_sprite_free(msw_sprite_t *s);

#define MSW_FLIP_X 1
#define MSW_FLIP_Y 2
/* Draws s at (x, y) (its top left), each pixel `scale` times (1..8). Parts
 * outside g are clipped. */
void msw_sprite_draw(msw_gfx_t *g, const msw_sprite_t *s, int x, int y, int scale, int flip);

/* ---- sound --------------------------------------------------------------- */

#define MSW_WAVE_SQUARE 0
#define MSW_WAVE_TRIANGLE 1
#define MSW_WAVE_SAW 2
#define MSW_WAVE_NOISE 3
#define MSW_WAVE_SINE 4
#define MSW_SOUND_CHANNELS 4

/* Plays a note on channel 0..3 at once (a note already on it stops): `wave`,
 * frequency hz (noise: its pitch), ms long, volume 0..100. It fades out
 * linearly over its length when fade is true. Returns at once. */
void msw_sound(int ch, int wave, int hz, int ms, int volume, bool fade);
/* Like msw_sound, with the frequency sliding from hz to hz_end (jumps, lasers). */
void msw_sound_slide(int ch, int wave, int hz, int hz_end, int ms, int volume);
void msw_sound_stop(int ch);                    /* -1: all channels */
bool msw_sound_busy(int ch);                    /* the channel still plays */
/* Your own 16 kHz mono samples, mixed over the channels. Returns how many
 * were taken (the rest did not fit; try again next tick). */
int msw_pcm_write(const int16_t *mono, int n);

/* Reads Japanese text aloud with the built-in voice (Sano TTS). Returns
 * NULL when started, or why not: "speaker_off", "busy", "empty". */
const char *msw_say(const char *text);

/* ---- vibration ----------------------------------------------------------- */

void msw_vibrate(int ms, int strength);         /* strength 1..100; returns at once */

/* ---- motion sensor (IMU) ------------------------------------------------- */

typedef struct {
    float ax, ay, az;    /* acceleration in g, screen axes: x right, y down, z out of the screen */
    float gx, gy, gz;    /* rotation in degrees per second around the same axes */
} msw_imu_t;

/* Lying face up and still: az = +1, ax = ay = 0. Tilting so the bottom of
 * the screen goes down makes ay negative: gravity in screen axes is
 * (-ax, -ay). Shaking shows as large changes. false if the sensor failed. */
bool msw_imu(msw_imu_t *out);

/* ---- storage ------------------------------------------------------------- */

/* Small data that stays across restarts (high scores, settings): up to 4 KB
 * per key, keys of letters, digits and '_' up to 15 characters. */
bool msw_store_put(const char *key, const void *data, size_t n);
int msw_store_get(const char *key, void *data, size_t cap);   /* bytes read, -1 if none */

/* ---- Muse ---------------------------------------------------------------- */

/* The app decides whether Muse appears. Nothing shows unless it asks. */
bool msw_muse_online(void);                      /* connected to Muse now */
/* A small Muse face at (x, y) (its centre) over the app; mood 0 calm, 1 happy,
 * 2 surprised. In frame drawing it is drawn at each msw_fb_present. */
void msw_avatar_show(int x, int y, int mood);
void msw_avatar_hide(void);
/* A speech bubble by the avatar (or at the bottom if none) for ms (0: until
 * msw_bubble(NULL, 0)). UTF-8, Japanese is fine. */
void msw_bubble(const char *text, int ms);
/* Muse's reply text as it arrives, while the app runs. */
typedef void (*msw_reply_cb)(const char *text, void *user);
void msw_on_muse_reply(msw_reply_cb cb, void *user);
/* Talking to Muse is the app's to offer: the blue button is the app's, not
 * the talk button, while an app runs. true starts recording the user's voice
 * (as holding the talk button does outside apps), false ends it and sends
 * it; the reply comes to msw_on_muse_reply. Show the user what to press. */
void msw_muse_talk(bool down);

/* ---- avatars (app.json "kind": "avatar") --------------------------------- */

/* An avatar is not an app: it draws Muse on the watch's face screen (and the
 * small Muse of the app menu) in place of the official avatar, every frame,
 * on the screen's task. It is built like an app, with "kind": "avatar" in
 * app.json; its app_main only calls msw_avatar_define and returns 0. It
 * must not call other msw_* functions, malloc, or wait: keep its data in
 * static arrays. Draw on a 64 x 64 grid of colour numbers; the watch blows
 * the grid up to the screen. render() runs about 25 times a second: keep it
 * under 5 ms (app.avatar_list says how long it takes). If it crashes or is
 * too slow, the watch goes back to the official avatar and says why in
 * app.log. */

#define MSW_AVATAR_W 64
#define MSW_AVATAR_H 64
#define MSW_AVATAR_COLORS 256

/* Muse's state, as the face shows it. */
#define MSW_MODE_BOOT 0
#define MSW_MODE_IDLE 1
#define MSW_MODE_LISTENING 2     /* the user is talking */
#define MSW_MODE_THINKING 3      /* waiting for Muse's reply */
#define MSW_MODE_SPEAKING 4      /* Muse's reply is playing */
#define MSW_MODE_ERROR 5
#define MSW_MODE_OFF 6           /* powering down */

typedef struct {
    int mode;        /* MSW_MODE_* */
    float t;         /* seconds since boot */
    float mode_t;    /* seconds in this mode */
    float level;     /* 0..1: the voice's loudness (listening and speaking) */
    float happy;     /* 0..1: just petted (the face was tapped) */
    float working;   /* 0 or 1 in THINKING: Muse busy on work it was asked for */
} msw_avatar_pose_t;

typedef struct {
    /* Fill grid[y * MSW_AVATAR_W + x] with colour numbers and set the
     * colours used in palette (RGB565, MSW_RGB). Number 0 should be the
     * background (black). The grid keeps the last frame's picture. */
    void (*render)(const msw_avatar_pose_t *pose, uint8_t *grid, uint16_t *palette);
    /* Optional (NULL: the official colours): the colour of the ring and the
     * state's accents around the face for a mode, as 0xRRGGBB. */
    uint32_t (*accent)(int mode);
} msw_avatar_def_t;

/* Call once from an avatar's app_main. def must stay (make it static). */
void msw_avatar_define(const msw_avatar_def_t *def);

#ifdef __cplusplus
}
#endif
