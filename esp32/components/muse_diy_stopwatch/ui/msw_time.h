/*
 * museStopWatch: the date and time. The RTC keeps it across restarts; SNTP
 * sets it once Wi-Fi is up. Local time is Japan's (JST).
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the RTC and starts SNTP when Wi-Fi gets an address. Call once. */
void msw_time_init(void);

/* Local time now; false until the RTC or the network has set the clock. */
bool msw_time_now(struct tm *local);

#ifdef __cplusplus
}
#endif
