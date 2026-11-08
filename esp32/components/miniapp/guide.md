# museStopWatch mini apps: read this first

You can make small apps (games, tools, toys) for this watch. An app is C code
you build on your VM into a `.mswapp` file; `app.install` copies it to the
watch, where it shows as a round icon on the launcher (swipe right from the
face). The user starts it with a tap, or you with `app.run`.

The watch: round 466 x 466 touch screen (only the circle shows: centre
(233, 233), radius 233), a speaker, vibration, a motion sensor, two rim
buttons: yellow (upper left, the app menu) and blue (upper right, the
app's own). ESP32-S3; your app runs from PSRAM with about 4 MB free.

## Steps

1. **Get the SDK once** on your VM:
   `git clone --depth 1 https://github.com/haiatto/museStopWatch` (use its
   `sdk/` and `apps/`), and the compiler: Espressif's xtensa-esp-elf GCC 15.2
   for Linux (crosstool-NG release `esp-15.2.0_20251204`), unpacked; put its
   `bin/` on PATH or set `MSW_GCC` to `xtensa-esp32s3-elf-gcc`.
2. **Write the app**: a folder with `app.json` and `.c` files.
3. **Build**: `python3 sdk/build.py apps/<name>` gives
   `apps/<name>/build/<id>.mswapp`. It fails if you call anything the
   firmware does not give, and names it.
4. **Install**: `app.install` with `path` = the file's path on the VM
   (`run: true` starts it). Same `id` = update.
5. **Check**: `app.log` (your `msw_log` lines, load errors, crashes, leaks
   the firmware cleaned up). Ask the user what they see and hear; you cannot
   see the screen.
6. `app.list`, `app.stop`, `app.remove` as needed.

## app.json

```json
{ "id": "blocks", "name": "おちものブロック", "version": 1,
  "icon": ["..RR..", ".RRRR.", "RRRRRR"], "palette": {"R": "#e53935"} }
```
id: a-z 0-9 _ (≤23). name ≤31 bytes (a Japanese letter is 3). icon: text
art up to 32 x 32, '.' transparent; 16 x 16 or less is drawn twice the size.

## The shape of an app

```c
#include "msw_app.h"
static msw_gfx_t *fb;
static void tick(uint32_t dt_ms, void *user) {
    int x, y;
    bool down = msw_touch(&x, &y);
    msw_gfx_fill(fb, MSW_RGB(0, 0, 40));
    if (down) msw_gfx_fill_circle(fb, x, y, 20, MSW_RGB(255, 200, 0));
    msw_fb_present();
}
int app_main(int argc, char **argv) {
    fb = msw_fb();                 /* frame drawing; NULL: out of memory */
    if (!fb) return 1;
    msw_set_fps(30);
    msw_on_tick(tick, NULL);
    return 0;                      /* return at once; tick runs after */
}
```

`app_main` sets things up and returns. Then the watch calls your tick
callback up to fps times a second, and the button callback for the blue
button. Everything the app took (memory, screen, sounds, timers) is freed
when it ends.

**Quitting** (the standard way): the yellow button (upper left) pauses the
app and opens the app menu with "つづける" (go on) and "おわる" (quit). You
write nothing for it, and need no pause screen of your own. `msw_on_pause`
tells you when the menu opens and closes. Other ways are allowed
(`msw_menu_own(true)` gives you the yellow button, and holding it 1.5 s
then quits; `msw_exit()` quits), but then the app must show the user how
to quit.

Read the full API with `app.api` (msw_app.h). Summary:

- **life**: msw_on_tick, msw_set_fps (1..60), msw_on_button, msw_on_exit,
  msw_exit, msw_on_pause, msw_menu_own, msw_millis, msw_random, msw_log
- **touch**: msw_touch(&x, &y) — true while a finger is down. One finger
  only: see "No two touches at once" below
- **frame drawing** (games): msw_fb, msw_fb_present, msw_fb_present_rect,
  msw_gfx_fill / pixel / rect / fill_rect / line / circle / fill_circle /
  blit / text, msw_text_width, msw_gfx_new (off-screen buffers), MSW_RGB
- **sprites**: msw_sprite_from_art(rows, n, palette) — text art like the
  icon; msw_sprite_draw(fb, s, x, y, scale 1..8, MSW_FLIP_X/Y)
- **lvgl drawing** (menus, tools): msw_screen() and the lvgl widgets in
  the SDK's export list; fonts from msw_font(14/16/20/28, or 0 = Japanese)
- **sound**: msw_sound(ch 0..3, wave, hz, ms, volume, fade),
  msw_sound_slide, msw_sound_stop, msw_pcm_write (16 kHz), msw_say(text)
  (Japanese TTS, on the watch)
- **vibration**: msw_vibrate(ms, strength)
- **motion**: msw_imu(&m) — acceleration in g in screen axes (x right,
  y down); gravity in the screen's plane is (-m.ax, -m.ay)
- **storage**: msw_store_put / msw_store_get (≤4 KB a key; high scores)
- **Muse**: msw_muse_online, msw_avatar_show/hide, msw_bubble,
  msw_on_muse_reply, msw_muse_talk(down) — only if the app wants Muse in
  it. Using Muse is the app's choice: in an app the blue button is the
  app's, not "talk". To let the user talk to Muse, give a control (the
  blue button or a screen button) that calls msw_muse_talk(true) on press
  and msw_muse_talk(false) on release, and show what it is.

## Patterns that work

- **Games**: frame drawing; redraw and present once a tick. Measured on the
  watch: sending a whole frame takes 27 ms (0.12 µs a pixel), and every
  pixel you write costs about 25 ns (the frame is in PSRAM). A full-screen
  redraw with scrolling layers runs about 18-20 fps; set fps to match
  (`msw_set_fps(20)`), and move things by dt. Faster: present only what
  changed (`msw_fb_present_rect`), redraw only when something changed, and
  do not paint the same pixel several times (sky, then hills, then tiles).
- **Per-pixel effects** (water, light): work in 2 x 2 blocks, step values
  along a row instead of computing each pixel, and use lookup tables. That
  took the fish bowl from 9 to 20 fps.
- `app.log` shows, when an app ends, its ticks a second and how long a tick
  and a present took. Use it to find what is slow.
- **Buttons on the screen**: draw circles; each tick, test msw_touch's point
  against them (allow ~15 px extra). Edge-detect presses yourself; repeat
  held moves on a timer. Put them along the rim at the lower left and
  lower right, where a thumb hides the least: about 190 px from the centre
  at 115-155 degrees (left) and 25-65 degrees (right), 0 = right, 90 =
  down: x = 233 + 190·cos(a), y = 233 + 190·sin(a). Moves on the left
  (← (70, 327), → (154, 403)), other buttons on the right ((364, 364)).
  Keep the play area and text in the upper middle.
- **No two touches at once (rule).** The screen cannot do it: it is
  self-capacitive, and with two fingers it can lose the held one for good
  or see a finger that is not there. So never design "hold one screen
  button and tap another" (run + jump, move + fire). Put the second action
  on the blue button: hold an arrow on the screen, press blue to jump
  (apps/pyoko). Or make it one finger (tap to fire, auto-run). Show the
  user what blue does (a blue mark by the upper-right rim, a line on the
  title). Avoid gestures on a held button too (such as "slide up to
  jump"): the finger drifts while held, so they fire by mistake.
- **Layout for a round screen**: at height y, the visible x range is
  233 ± sqrt(233² − (y − 233)²). Keep text and buttons away from the rim.
- **Sound**: short effects on channels 1-3, music on channel 0 at a low
  volume (15-25). Write tunes as (MIDI note, length) tables;
  hz = 440 · 2^((n − 69) / 12).
- **Timing**: use the dt passed to tick, not frame counts.
- **Saving**: load in app_main, save when a score beats the best.
- **Text**: msw_gfx_text(fb, x, y, "…", color, size). size 0 is the only
  Japanese font (16 px); 14/16/20/28 are Latin only. LV_SYMBOL_LEFT/RIGHT/
  UP/DOWN/OK/CLOSE work with the Latin sizes.

## Pitfalls

- No threads, no sleeping, no blocking loops: keep each tick under a few ms
  (the screen and sound wait for you). Never wait for a touch in a loop.
- No files, sockets or OS calls: only msw_*, the listed lvgl calls and a
  small C library (memcpy, snprintf, math…). The build tells you if you
  use anything else.
- Prefer float over double (`sinf`, `1.0f`): the chip has no double
  hardware.
- Large static arrays are fine (PSRAM), but keep the package under 1 MB.
- Colours are RGB565: use MSW_RGB(r, g, b).
- Every text art row must have the same length; every letter must be in
  the palette (else NULL and a line in app.log).
- A crash restarts the watch. app.log then says which app was running.
- Do not draw with lvgl and the frame at the same time.

## Rules

- Make your own art, characters, music and names. Do not copy characters,
  sprites, tunes or logos from existing games or works. Genre words are fine
  ("a falling-block puzzle", "a jump-and-run game").
- Quit with the yellow button's app menu. If the app uses another way
  (msw_menu_own, its own quit button), show the user how to quit.
