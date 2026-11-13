/*
 * museStopWatch mini apps: the date and time, the battery, and PORT.A (the
 * Grove port: its 5 V and an I2C bus on it). msw_app.h, "time and battery"
 * and "PORT.A".
 *
 * PORT.A is HY2.0-4P: GND, 5V, G10 (SDA, yellow), G11 (SCL, white): M5's C152
 * page and M5Unified's pin table. The bus here is the apps' own (another I2C
 * controller), not the internal one at G47/G48. All of it goes off when the
 * app ends.
 *
 * Nothing here works until the user has picked "はじめる" on the start screen
 * (msw_app_core.c, msw_port_allow): an app reaches the port only after the
 * user has read what the app says it does with it. msw_pin_read makes the
 * signal pins inputs only; nothing here drives them.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "msw_app_priv.h"
#include "msw_stack.h"
#include "msw_time.h"
#include "muse_state.h"

static const char *TAG = "msw_port";

#define PORT_SDA 10
#define PORT_SCL 11
#define DEVS 8
#define I2C_TIMEOUT_MS 50

extern esp_err_t msw_board_port_power(bool on);

static bool s_power;
static bool s_allowed;          /* the user picked "はじめる" for this run */
static bool s_pin_in[2];        /* MSW_PIN_YELLOW, MSW_PIN_WHITE made inputs */
static const int s_pin_gpio[2] = { PORT_SDA, PORT_SCL };
static i2c_master_bus_handle_t s_bus;
static int s_khz;

/* UART on PORT.A (msw_uart_begin). UART0 is the console; UART1 is the apps'.
 * The driver's small receive ring is internal RAM (its interrupt fills it);
 * a task moves the bytes on into a larger ring in PSRAM, so an app that reads
 * once a tick (30 a second) loses nothing at camera speeds (1.5 Mbit/s is
 * about 150 KB a second, 5 KB a tick). */
#define APP_UART UART_NUM_1
#define UART_DRV_RX 2048            /* internal: the driver's ring */
#define UART_RING (64 * 1024)       /* PSRAM: what the app reads from */
static uint8_t *s_ring;
static volatile uint32_t s_ring_head, s_ring_tail;   /* written by the task / by the app */
static volatile uint32_t s_ring_lost;
static volatile bool s_uart_on;
static TaskHandle_t s_uart_task;
static struct {
    int addr;
    i2c_master_dev_handle_t dev;
} s_dev[DEVS];

/* ---- time and battery ------------------------------------------------------ */

bool msw_time(msw_time_t *out)
{
    out = msw_api_writable(out, sizeof(*out), "msw_time");
    struct tm t;
    if (!out || !msw_time_now(&t)) {
        return false;
    }
    out->year = t.tm_year + 1900;
    out->month = t.tm_mon + 1;
    out->day = t.tm_mday;
    out->hour = t.tm_hour;
    out->minute = t.tm_min;
    out->second = t.tm_sec;
    out->weekday = t.tm_wday;
    return true;
}

bool msw_battery(msw_battery_t *out)
{
    out = msw_api_writable(out, sizeof(*out), "msw_battery");
    if (!out) {
        return false;
    }
    muse_power_t p = muse_state_power();
    out->percent = p.battery_pct;
    out->mv = p.battery_mv;
    out->charging = p.charging;
    out->usb = p.usb;
    return p.battery_mv > 0 || p.battery_pct >= 0;
}

/* ---- PORT.A ------------------------------------------------------------------ */

/* msw_app_core.c: the user started the app from the start screen (true), or
 * the app ended (false). */
void msw_port_allow(bool on)
{
    s_allowed = on;
}

static bool allowed(const char *fn)
{
    if (!s_allowed) {
        msw_app_logf("%s: PORT.A is not open to this run (no start screen was passed)", fn);
    }
    return s_allowed;
}

bool msw_port_power(bool on)
{
    if (on && !allowed("msw_port_power")) {
        return false;
    }
    if (on == s_power) {
        return true;
    }
    if (msw_board_port_power(on) != ESP_OK) {
        return false;
    }
    s_power = on;
    msw_app_logf("PORT.A 5 V %s", on ? "on" : "off");
    return true;
}

void msw_i2c_end(void)
{
    for (int i = 0; i < DEVS; i++) {
        if (s_dev[i].dev) {
            i2c_master_bus_rm_device(s_dev[i].dev);
            s_dev[i].dev = NULL;
        }
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_khz = 0;
}

bool msw_i2c_begin(int khz)
{
    khz = khz <= 0 ? 100 : khz > 400 ? 400 : khz;
    if (!allowed("msw_i2c_begin") || !msw_port_power(true)) {
        return false;
    }
    if (s_bus && khz == s_khz) {
        return true;
    }
    if (s_uart_on) {
        msw_uart_end();
        msw_app_logf("PORT.A UART ended: msw_i2c_begin takes the pins");
    }
    msw_i2c_end();
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,                     /* any free controller */
        .sda_io_num = PORT_SDA,
        .scl_io_num = PORT_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* weak: most Grove boards have their own */
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PORT.A I2C: %s", esp_err_to_name(err));
        s_bus = NULL;
        return false;
    }
    s_khz = khz;
    s_pin_in[0] = s_pin_in[1] = false;   /* the bus has the pins now */
    return true;
}

int msw_pin_read(int pin)
{
    if (pin != MSW_PIN_YELLOW && pin != MSW_PIN_WHITE) {
        msw_app_logf("msw_pin_read(%d): MSW_PIN_YELLOW or MSW_PIN_WHITE", pin);
        return -1;
    }
    if (!allowed("msw_pin_read")) {
        return -1;
    }
    if (!s_pin_in[pin]) {
        if (s_bus) {
            msw_i2c_end();
            msw_app_logf("PORT.A I2C ended: msw_pin_read takes the pins");
        }
        if (s_uart_on) {
            msw_uart_end();
            msw_app_logf("PORT.A UART ended: msw_pin_read takes the pins");
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_pin_gpio[pin],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK) {
            return -1;
        }
        s_pin_in[pin] = true;
    }
    return gpio_get_level(s_pin_gpio[pin]);
}

static i2c_master_dev_handle_t dev_for(int addr)
{
    if (!s_bus || addr < 0x08 || addr > 0x77) {
        return NULL;
    }
    int free_at = -1;
    for (int i = 0; i < DEVS; i++) {
        if (s_dev[i].dev && s_dev[i].addr == addr) {
            return s_dev[i].dev;
        }
        if (!s_dev[i].dev && free_at < 0) {
            free_at = i;
        }
    }
    if (free_at < 0) {                      /* all in use: drop the first */
        i2c_master_bus_rm_device(s_dev[0].dev);
        s_dev[0].dev = NULL;
        free_at = 0;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = (uint32_t)s_khz * 1000,
    };
    if (i2c_master_bus_add_device(s_bus, &cfg, &s_dev[free_at].dev) != ESP_OK) {
        s_dev[free_at].dev = NULL;
        return NULL;
    }
    s_dev[free_at].addr = addr;
    return s_dev[free_at].dev;
}

int msw_i2c_scan(uint8_t *found, int max)
{
    found = max > 0 ? msw_api_writable(found, (size_t)max, "msw_i2c_scan") : NULL;
    if (!found || !s_bus) {
        return 0;
    }
    int n = 0;
    for (int a = 0x08; a <= 0x77 && n < max; a++) {
        if (i2c_master_probe(s_bus, (uint16_t)a, I2C_TIMEOUT_MS) == ESP_OK) {
            found[n++] = (uint8_t)a;
        }
    }
    return n;
}

bool msw_i2c_write(int addr, const void *data, int n)
{
    data = n > 0 ? msw_api_readable(data, (size_t)n, "msw_i2c_write") : NULL;
    i2c_master_dev_handle_t d = data ? dev_for(addr) : NULL;
    return d && i2c_master_transmit(d, data, (size_t)n, I2C_TIMEOUT_MS) == ESP_OK;
}

bool msw_i2c_read(int addr, void *data, int n)
{
    data = n > 0 ? msw_api_writable(data, (size_t)n, "msw_i2c_read") : NULL;
    i2c_master_dev_handle_t d = data ? dev_for(addr) : NULL;
    return d && i2c_master_receive(d, data, (size_t)n, I2C_TIMEOUT_MS) == ESP_OK;
}

bool msw_i2c_write_read(int addr, const void *out, int n_out, void *in, int n_in)
{
    out = n_out > 0 ? msw_api_readable(out, (size_t)n_out, "msw_i2c_write_read") : NULL;
    in = n_in > 0 ? msw_api_writable(in, (size_t)n_in, "msw_i2c_write_read") : NULL;
    i2c_master_dev_handle_t d = out && in ? dev_for(addr) : NULL;
    return d && i2c_master_transmit_receive(d, out, (size_t)n_out, in, (size_t)n_in, I2C_TIMEOUT_MS) == ESP_OK;
}

/* ---- UART ---------------------------------------------------------------- */

static void uart_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    while (s_uart_on) {
        int n = uart_read_bytes(APP_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0) {
            continue;
        }
        uint32_t head = s_ring_head;
        uint32_t room = UART_RING - (head - __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE));
        uint32_t take = (uint32_t)n < room ? (uint32_t)n : room;
        for (uint32_t i = 0; i < take; i++) {
            s_ring[(head + i) % UART_RING] = buf[i];
        }
        __atomic_store_n(&s_ring_head, head + take, __ATOMIC_RELEASE);
        s_ring_lost += (uint32_t)n - take;
    }
    s_uart_task = NULL;
    msw_task_delete_self();
}

void msw_uart_end(void)
{
    if (!s_uart_on) {
        return;
    }
    s_uart_on = false;
    for (int i = 0; i < 50 && s_uart_task; i++) {   /* it sees the flag within one read */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    uart_driver_delete(APP_UART);
    gpio_reset_pin(PORT_SDA);
    gpio_reset_pin(PORT_SCL);
    if (s_ring_lost) {
        msw_app_logf("PORT.A UART: %u bytes lost (the app read too slowly)", (unsigned)s_ring_lost);
    }
    heap_caps_free(s_ring);
    s_ring = NULL;
}

bool msw_uart_begin(int baud, int rx_pin)
{
    if (!allowed("msw_uart_begin")) {
        return false;
    }
    if (baud < 1200 || baud > 5000000 || (rx_pin != MSW_PIN_YELLOW && rx_pin != MSW_PIN_WHITE)) {
        msw_app_logf("msw_uart_begin(%d, %d): baud 1200..5000000, rx_pin MSW_PIN_YELLOW or MSW_PIN_WHITE",
                     baud, rx_pin);
        return false;
    }
    msw_uart_end();
    msw_i2c_end();
    for (int i = 0; i < 2; i++) {
        if (s_pin_in[i]) {
            gpio_reset_pin(s_pin_gpio[i]);
            s_pin_in[i] = false;
        }
    }
    if (!msw_port_power(true)) {
        return false;
    }
    s_ring = heap_caps_malloc(UART_RING, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        return false;
    }
    s_ring_head = s_ring_tail = s_ring_lost = 0;
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int rx = s_pin_gpio[rx_pin], tx = s_pin_gpio[1 - rx_pin];
    esp_err_t err = uart_driver_install(APP_UART, UART_DRV_RX, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(APP_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(APP_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        s_uart_on = true;
        if (msw_task_create(uart_task, "msw_uart", 3072, NULL, 5, &s_uart_task) != pdPASS) {
            s_uart_on = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PORT.A UART: %s", esp_err_to_name(err));
        uart_driver_delete(APP_UART);
        heap_caps_free(s_ring);
        s_ring = NULL;
        return false;
    }
    msw_app_logf("PORT.A UART %d baud, rx %s, tx %s", baud, rx_pin == MSW_PIN_YELLOW ? "yellow" : "white",
                 rx_pin == MSW_PIN_YELLOW ? "white" : "yellow");
    return true;
}

int msw_uart_available(void)
{
    if (!s_uart_on) {
        return -1;
    }
    return (int)(__atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE) - s_ring_tail);
}

int msw_uart_read(void *data, int max)
{
    data = max > 0 ? msw_api_writable(data, (size_t)max, "msw_uart_read") : NULL;
    if (!s_uart_on || !data) {
        return -1;
    }
    uint32_t tail = s_ring_tail;
    uint32_t have = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE) - tail;
    uint32_t n = have < (uint32_t)max ? have : (uint32_t)max;
    uint32_t at = tail % UART_RING;
    uint32_t first = n < UART_RING - at ? n : UART_RING - at;
    memcpy(data, s_ring + at, first);
    memcpy((uint8_t *)data + first, s_ring, n - first);
    __atomic_store_n(&s_ring_tail, tail + n, __ATOMIC_RELEASE);
    return (int)n;
}

int msw_uart_write(const void *data, int n)
{
    data = n > 0 ? msw_api_readable(data, (size_t)n, "msw_uart_write") : NULL;
    if (!s_uart_on || !data) {
        return -1;
    }
    return uart_write_bytes(APP_UART, data, (size_t)n);
}

/* msw_app_core.c: an app ended. */
void msw_port_app_end(void)
{
    msw_uart_end();
    msw_i2c_end();
    for (int i = 0; i < 2; i++) {
        if (s_pin_in[i]) {
            gpio_reset_pin(s_pin_gpio[i]);
            s_pin_in[i] = false;
        }
    }
    msw_port_power(false);
    s_allowed = false;
}
