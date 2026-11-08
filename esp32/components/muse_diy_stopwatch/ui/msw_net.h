/*
 * museStopWatch: SoftAP mode and the device's small Web server.
 *
 * The AP runs beside the station (APSTA), so the home Wi-Fi and Muse stay
 * connected. Its name is museSW-XXXX (MAC suffix) and its WPA2 password is
 * made once at random and kept in NVS (namespace "msw").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSW_AP_SSID_MAX 32
#define MSW_AP_PASS_MAX 16
#define MSW_URL_MAX 64

/* Turns the AP on (and the Web server). Returns false if Wi-Fi refused it. */
bool msw_ap_start(void);
void msw_ap_stop(void);
bool msw_ap_on(void);
/* Phones joined to the AP now. */
int msw_ap_clients(void);
void msw_ap_credentials(char ssid[MSW_AP_SSID_MAX + 1], char pass[MSW_AP_PASS_MAX + 1]);

/* Starts the Web server if it isn't running. */
bool msw_web_start(void);
/* Starts it once Wi-Fi is up (or the AP is on), from boot. */
void msw_web_autostart(void);
bool msw_web_running(void);

/* The Web page's address over the home Wi-Fi ("" when not connected) and
 * over the AP ("" when the AP is off). */
void msw_web_sta_url(char out[MSW_URL_MAX]);
void msw_web_ap_url(char out[MSW_URL_MAX]);

/* The address with the access key in it, for the Web QR: the page only takes
 * changes from a browser that opened it this way (it keeps the key in a
 * cookie). The key is made once at random and kept in NVS. */
void msw_web_key_url(const char *url, char out[MSW_URL_MAX]);

#ifdef __cplusplus
}
#endif
