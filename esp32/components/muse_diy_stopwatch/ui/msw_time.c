/*
 * museStopWatch: the date and time (msw_time.h). At boot the clock comes
 * from the RTC (it keeps UTC); once Wi-Fi has an address, SNTP sets it from
 * the network and writes it back to the RTC. Local time is Japan's.
 */
#include "msw_time.h"

#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"

static const char *TAG = "msw_time";

#define TZ_JAPAN "JST-9"
#define NTP_SERVER "pool.ntp.org"

/* The StopWatch board (board_m5stack_stopwatch.c); absent on other boards. */
extern bool msw_board_rtc_get(struct tm *t) __attribute__((weak));
extern bool msw_board_rtc_set(const struct tm *t) __attribute__((weak));

static volatile bool s_set;
static bool s_sntp;

/* Days since 1970-01-01 of a UTC date (Howard Hinnant's days_from_civil). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void on_sync(struct timeval *tv)
{
    s_set = true;
    struct tm u;
    gmtime_r(&tv->tv_sec, &u);
    bool saved = msw_board_rtc_set && msw_board_rtc_set(&u);
    ESP_LOGI(TAG, "set from the network: %04d-%02d-%02d %02d:%02d:%02d UTC%s", u.tm_year + 1900, u.tm_mon + 1,
             u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, saved ? ", saved to the RTC" : "");
}

static void start_sntp(void)
{
    if (s_sntp) {
        return;
    }
    s_sntp = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.sync_cb = on_sync;
    cfg.wait_for_sync = false;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP did not start");
        s_sntp = false;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    start_sntp();
}

void msw_time_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    setenv("TZ", TZ_JAPAN, 1);
    tzset();
    struct tm u;
    if (msw_board_rtc_get && msw_board_rtc_get(&u) && u.tm_year >= 125) {
        int64_t secs = days_from_civil(u.tm_year + 1900, u.tm_mon + 1, u.tm_mday) * 86400 + u.tm_hour * 3600 +
                       u.tm_min * 60 + u.tm_sec;
        struct timeval tv = { .tv_sec = (time_t)secs };
        settimeofday(&tv, NULL);
        s_set = true;
        ESP_LOGI(TAG, "from the RTC: %04d-%02d-%02d %02d:%02d UTC", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday,
                 u.tm_hour, u.tm_min);
    } else {
        ESP_LOGI(TAG, "the RTC has no time yet: waiting for the network");
    }
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "no event loop: SNTP starts only if Wi-Fi is up now");
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        start_sntp();
    }
}

bool msw_time_now(struct tm *local)
{
    if (!s_set) {
        return false;
    }
    time_t now = time(NULL);
    return localtime_r(&now, local) != NULL;
}
