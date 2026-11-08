/*
 * museStopWatch: SoftAP mode and the device's small Web server (msw_net.h).
 */
#include "msw_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "muse_ble.h"
#include "muse_link.h"
#include "muse_state.h"
#include "muse_wifi.h"
#include "msw_side.h"

static const char *TAG = "msw_net";

#define NS "msw"
#define AP_IP "192.168.4.1"
#define AP_MAX_CLIENTS 2

static SemaphoreHandle_t s_lock;
static esp_netif_t *s_ap_netif;
static bool s_ap_on;
static httpd_handle_t s_httpd;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_lock;
#endif

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ---------- AP name and password ---------- */

static void make_random(char *out, int len)
{
    /* No 0/o, 1/l/i: it may be typed by hand. */
    static const char ABC[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < len; i++) {
        out[i] = ABC[esp_random() % (sizeof(ABC) - 1)];
    }
    out[len] = '\0';
}

static void make_password(char out[MSW_AP_PASS_MAX + 1])
{
    make_random(out, 10);
}

void msw_ap_credentials(char ssid[MSW_AP_SSID_MAX + 1], char pass[MSW_AP_PASS_MAX + 1])
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, MSW_AP_SSID_MAX + 1, "museSW-%02X%02X", mac[4], mac[5]);

    pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        make_password(pass);   /* still usable this session */
        return;
    }
    size_t n = MSW_AP_PASS_MAX + 1;
    if (nvs_get_str(h, "ap_pass", pass, &n) != ESP_OK || strlen(pass) < 8) {
        make_password(pass);
        nvs_set_str(h, "ap_pass", pass);
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---------- AP ---------- */

bool msw_ap_start(void)
{
    lock();
    if (s_ap_on) {
        unlock();
        return true;
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();   /* 192.168.4.1, DHCP server on */
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    esp_err_t err = esp_wifi_set_mode(mode == WIFI_MODE_AP ? WIFI_MODE_AP : WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP mode refused: %s", esp_err_to_name(err));
        unlock();
        return false;
    }

    char ssid[MSW_AP_SSID_MAX + 1], pass[MSW_AP_PASS_MAX + 1];
    msw_ap_credentials(ssid, pass);
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    strlcpy((char *)cfg.ap.password, pass, sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = AP_MAX_CLIENTS;
    cfg.ap.channel = 1;   /* follows the station's channel once it is connected */
    cfg.ap.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    memset(pass, 0, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config refused: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        unlock();
        return false;
    }

    /* Phones must find the AP: no modem or light sleep while it is up. */
    muse_wifi_keep_awake(true);
#if CONFIG_PM_ENABLE
    if (!s_pm_lock) {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "msw_ap", &s_pm_lock);
    }
    if (s_pm_lock) {
        esp_pm_lock_acquire(s_pm_lock);
    }
#endif
    s_ap_on = true;
    unlock();
    ESP_LOGI(TAG, "AP on: %s", ssid);
    msw_web_start();
    return true;
}

void msw_ap_stop(void)
{
    lock();
    if (!s_ap_on) {
        unlock();
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    muse_wifi_keep_awake(false);
#if CONFIG_PM_ENABLE
    if (s_pm_lock) {
        esp_pm_lock_release(s_pm_lock);
    }
#endif
    s_ap_on = false;
    unlock();
    ESP_LOGI(TAG, "AP off");
}

bool msw_ap_on(void)
{
    return s_ap_on;
}

int msw_ap_clients(void)
{
    if (!s_ap_on) {
        return 0;
    }
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return 0;
    }
    return list.num;
}

/* ---------- addresses ---------- */

static void url_of(const char *ifkey, char out[MSW_URL_MAX])
{
    out[0] = '\0';
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkey);
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, MSW_URL_MAX, "http://" IPSTR "/", IP2STR(&ip.ip));
    }
}

void msw_web_sta_url(char out[MSW_URL_MAX])
{
    if (!muse_wifi_connected()) {
        out[0] = '\0';
        return;
    }
    url_of("WIFI_STA_DEF", out);
}

void msw_web_ap_url(char out[MSW_URL_MAX])
{
    if (!s_ap_on) {
        out[0] = '\0';
        return;
    }
    url_of("WIFI_AP_DEF", out);
    if (!out[0]) {
        strlcpy(out, "http://" AP_IP "/", MSW_URL_MAX);
    }
}

/* ---------- access key ---------- */

#define KEY_LEN 12
#define COOKIE "msw"

static char s_key[KEY_LEN + 1];

static const char *web_key(void)
{
    if (s_key[0]) {
        return s_key;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t n = sizeof(s_key);
        if (nvs_get_str(h, "web_key", s_key, &n) != ESP_OK || strlen(s_key) != KEY_LEN) {
            make_random(s_key, KEY_LEN);
            nvs_set_str(h, "web_key", s_key);
            nvs_commit(h);
        }
        nvs_close(h);
    } else {
        make_random(s_key, KEY_LEN);   /* this boot only */
    }
    return s_key;
}

static bool key_ok(const char *k)
{
    const char *want = web_key();
    if (strlen(k) != KEY_LEN) {
        return false;
    }
    uint8_t diff = 0;
    for (int i = 0; i < KEY_LEN; i++) {
        diff |= (uint8_t)(k[i] ^ want[i]);
    }
    return diff == 0;
}

void msw_web_key_url(const char *url, char out[MSW_URL_MAX])
{
    if (!url[0]) {
        out[0] = '\0';
        return;
    }
    snprintf(out, MSW_URL_MAX, "%s?k=%s", url, web_key());
}

static bool authed(httpd_req_t *req)
{
    char v[KEY_LEN + 2];
    size_t n = sizeof(v);
    return httpd_req_get_cookie_val(req, COOKIE, v, &n) == ESP_OK && key_ok(v);
}

/* ---------- Web server ---------- */

extern const uint8_t msw_web_html_start[] asm("_binary_msw_web_html_start");
extern const uint8_t msw_web_html_end[] asm("_binary_msw_web_html_end");

static void send_locked(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req,
        "<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>museStopWatch</title><style>"
        "body{font-family:system-ui,sans-serif;background:#0f0c1d;color:#f2efff;margin:0;padding:16px;line-height:1.6}"
        "h1{font-size:1.3rem;color:#a77dff}section{background:#1a1530;border-radius:12px;padding:12px 16px}"
        "</style></head><body><h1>museStopWatch</h1><section>"
        "設定ページを開くには、本体の <b>SETTINGS &rsaquo; QR &rsaquo; Web QR</b> に出る QR をスマホで読み取ってください。"
        "一度開いたブラウザは、次からこのアドレスだけで開けます。"
        "</section></body></html>");
}

static esp_err_t on_root(httpd_req_t *req)
{
    char q[48], k[KEY_LEN + 2];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "k", k, sizeof(k)) == ESP_OK && key_ok(k)) {
        /* Keep the key in a cookie and drop it from the address bar. */
        char cookie[96];
        snprintf(cookie, sizeof(cookie), COOKIE "=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict", web_key());
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t err = httpd_resp_send(req, NULL, 0);
        memset(cookie, 0, sizeof(cookie));
        return err;
    }
    if (!authed(req)) {
        send_locked(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)msw_web_html_start, msw_web_html_end - msw_web_html_start);
}

static esp_err_t forbid(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "forbidden");
}

/* Appends to a JSON buffer; *o stays below cap. */
static void put(char *buf, size_t cap, size_t *o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *buf, size_t cap, size_t *o, const char *fmt, ...)
{
    if (*o >= cap - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *o, cap - *o, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *o = *o + (size_t)n < cap - 1 ? *o + (size_t)n : cap - 1;
    }
}

static void put_str(char *buf, size_t cap, size_t *o, const char *s)
{
    put(buf, cap, o, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            put(buf, cap, o, "\\%c", c);
        } else if (c < 0x20) {
            put(buf, cap, o, "\\u%04x", c);
        } else {
            put(buf, cap, o, "%c", c);
        }
    }
    put(buf, cap, o, "\"");
}

static volatile bool s_sdk_token_bad;
static volatile int s_side_result;   /* the last side.add: 0, MSW_SIDE_FULL or MSW_SIDE_BAD_ID */

/* A test for other sites (a Muse Space page): open to every origin, no key,
 * and nothing secret in it. The serial log shows each call and its Origin,
 * so a call the browser blocked after sending is still seen here. */
static void cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static void log_origin(httpd_req_t *req, const char *what)
{
    char origin[96] = "-";
    httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    ESP_LOGI(TAG, "@web.ping %s origin=%s", what, origin);
}

static esp_err_t on_ping(httpd_req_t *req)
{
    log_origin(req, "get");
    cors(req);
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"device\":\"museStopWatch\",\"uptime_ms\":%lld}",
             (long long)(esp_timer_get_time() / 1000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t on_ping_options(httpd_req_t *req)
{
    log_origin(req, "preflight");
    cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t on_status(httpd_req_t *req)
{
    if (!authed(req)) {
        return forbid(req);
    }
    muse_state_poke();   /* an open settings page keeps auto-sleep away (not a sleeping screen awake) */
    const size_t cap = 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t o = 0;
    put(buf, cap, &o, "{\"device\":");
    int n = muse_ble_status_json(buf + o, 1024);
    if (n > 0) {
        o += (size_t)n < 1023 ? (size_t)n : 1023;
    } else {
        put(buf, cap, &o, "null");
    }
    put(buf, cap, &o, ",\"sdk_token\":%s,\"sdk_token_bad\":%s,\"ap\":{\"on\":%s,\"clients\":%d}",
        muse_link_sdk_token() ? "true" : "false", s_sdk_token_bad ? "true" : "false",
        s_ap_on ? "true" : "false", msw_ap_clients());

    muse_wifi_saved_t saved[MUSE_WIFI_SAVED_MAX];
    int ns = muse_wifi_saved(saved, MUSE_WIFI_SAVED_MAX);
    put(buf, cap, &o, ",\"saved\":[");
    for (int i = 0; i < ns; i++) {
        put(buf, cap, &o, "%s{\"ssid\":", i ? "," : "");
        put_str(buf, cap, &o, saved[i].ssid);
        put(buf, cap, &o, ",\"hidden\":%s}", saved[i].hidden ? "true" : "false");
    }
    muse_wifi_ap_t aps[12];
    uint32_t gen = 0;
    int na = muse_wifi_scan_results(aps, 12, &gen);
    put(buf, cap, &o, "],\"scanning\":%s,\"scan\":[", muse_wifi_scanning() ? "true" : "false");
    for (int i = 0; i < na; i++) {
        put(buf, cap, &o, "%s{\"ssid\":", i ? "," : "");
        put_str(buf, cap, &o, aps[i].ssid);
        put(buf, cap, &o, ",\"rssi\":%d,\"secure\":%s}", aps[i].rssi, aps[i].secure ? "true" : "false");
    }
    put(buf, cap, &o, "],\"side\":{\"cur\":%d,\"max\":%d,\"bad\":%s,\"list\":[", msw_side_current(), MSW_SIDE_MAX,
        s_side_result == MSW_SIDE_BAD_ID ? "true" : "false");
    int nside = msw_side_count();
    for (int i = 0; i < nside; i++) {
        msw_side_t sc;
        if (!msw_side_get(i, &sc)) {
            break;
        }
        put(buf, cap, &o, "%s{\"name\":", i ? "," : "");
        put_str(buf, cap, &o, sc.name);
        put(buf, cap, &o, ",\"id\":");
        put_str(buf, cap, &o, sc.id);
        put(buf, cap, &o, "}");
    }
    put(buf, cap, &o, "]}}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, o);
    free(buf);
    return err;
}

/* The SDK token restarts the device: answer the request first. */
static void sdk_token_task(void *arg)
{
    char *token = arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    s_sdk_token_bad = !muse_link_sdk_token_set(token);   /* restarts when it's good */
    memset(token, 0, strlen(token));
    free(token);
    vTaskDelete(NULL);
}

#define CMD_BODY_MAX 1600

static void run_line(char *line)
{
    if (!strcmp(line, "wifi.scan")) {
        muse_wifi_scan();
    } else if (!strncmp(line, "wifi.choose=", 12)) {
        muse_wifi_choose(line + 12);   /* a saved network, with its saved password */
    } else if (!strcmp(line, "side.main")) {   /* side chats (msw_side.h), by ID */
        msw_side_select(-1);
    } else if (!strcmp(line, "side.new")) {
        msw_side_add(NULL);
    } else if (!strncmp(line, "side.add=", 9)) {
        int i = msw_side_add(line + 9);
        s_side_result = i >= 0 ? 0 : i;
    } else if (!strncmp(line, "side.select=", 12)) {
        int i = msw_side_find(line + 12);
        if (i >= 0) {
            msw_side_select(i);
        }
    } else if (!strncmp(line, "side.delete=", 12)) {
        int i = msw_side_find(line + 12);
        if (i >= 0) {
            msw_side_delete(i);
        }
    } else if (!strcmp(line, "ap.off")) {
        msw_ap_stop();
    } else if (!strncmp(line, "sdk_token=", 10)) {
        char *token = strdup(line + 10);
        if (token && xTaskCreate(sdk_token_task, "msw_sdk", 4096, token, 5, NULL) != pdPASS) {
            memset(token, 0, strlen(token));
            free(token);
        }
    } else if (line[0]) {
        muse_ble_command(line);   /* the official setup commands: wifi.*, hatch.*, volume, ... */
    }
}

static esp_err_t on_cmd(httpd_req_t *req)
{
    /* The cookie, and a header a page on another site can't send without
     * asking first (CORS), so other pages can't post here. */
    if (!authed(req) || httpd_req_get_hdr_value_len(req, "X-MSW") == 0) {
        return forbid(req);
    }
    muse_state_poke();
    if (req->content_len > CMD_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "too long");
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            memset(body, 0, req->content_len);
            free(body);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    for (char *line = body, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        run_line(line);
    }
    memset(body, 0, req->content_len);   /* passwords and tokens */
    free(body);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

bool msw_web_start(void)
{
    lock();
    if (s_httpd) {
        unlock();
        return true;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
    /* Flash writes leave the cache on here, so the NVS saves the handlers make
     * are safe on a PSRAM stack; internal RAM is short (Muse couldn't connect). */
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        unlock();
        return false;
    }
    static const httpd_uri_t URIS[] = {
        { .uri = "/", .method = HTTP_GET, .handler = on_root },
        { .uri = "/api/status", .method = HTTP_GET, .handler = on_status },
        { .uri = "/api/cmd", .method = HTTP_POST, .handler = on_cmd },
        { .uri = "/api/ping", .method = HTTP_GET, .handler = on_ping },
        { .uri = "/api/ping", .method = HTTP_OPTIONS, .handler = on_ping_options },
    };
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_register_uri_handler(s_httpd, &URIS[i]);
    }
    unlock();
    ESP_LOGI(TAG, "Web server on port %d", cfg.server_port);
    return true;
}

bool msw_web_running(void)
{
    return s_httpd != NULL;
}

/* Waits for the network, then starts the server and ends. */
static void autostart_task(void *arg)
{
    (void)arg;
    while (!muse_wifi_connected() && !s_ap_on) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    web_key();   /* read from NVS here, on an internal stack */
    msw_web_start();
    vTaskDelete(NULL);
}

void msw_web_autostart(void)
{
    static bool started;
    if (!started) {
        started = true;
        xTaskCreate(autostart_task, "msw_web_auto", 3072, NULL, 2, NULL);
    }
}
