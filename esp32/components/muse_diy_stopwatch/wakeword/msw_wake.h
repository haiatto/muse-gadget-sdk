/*
 * museStopWatch: wake words on the device (microWakeWord models on
 * TensorFlow Lite Micro). The voice task hands its mic reads to
 * msw_wake_feed and msw_wake_turn_feed; a task of its own makes the features
 * and runs the models.
 *
 * A wake word starts a turn as if the talk button were pressed (the app talk
 * control, muse_input_app_talk); the VAD model then lets it go when the
 * speech ends.
 *
 * The setting (Settings > Wake word, saved in NVS) says when it listens:
 * never (the default), while the screen is on, or while the screen is on or
 * USB powers the device. Never on battery with the screen off.
 * Serial: ">msw.ww off|screen|usb|test|stat".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MSW_WAKE_OFF = 0,
    MSW_WAKE_SCREEN = 1,       /* while the screen is on */
    MSW_WAKE_SCREEN_USB = 2,   /* ... or while USB powers it */
} msw_wake_mode_t;

/* muse_settings_ui.c, at boot: reads the setting and starts to follow it. */
void msw_wake_init(void);
int msw_wake_mode(void);
void msw_wake_set_mode(int mode);
/* The name of the wake word, for the settings page ("Hey Muse"). */
const char *msw_wake_word(void);
/* Listening now (the setting and the screen and power allow it). */
bool msw_wake_enabled(void);

/* muse_voice.c, through weak references. Idle reads: */
void msw_wake_feed(const int16_t *mono, size_t n);
/* Reads in a turn or a call. `quiet`: no one talks (the call waits), so a
 * wake word can start the next turn. */
void msw_wake_turn_feed(const int16_t *mono, size_t n, bool quiet);

#ifdef __cplusplus
}
#endif
