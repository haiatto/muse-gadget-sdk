/*
 * museStopWatch mini apps: the date and time, the battery, and PORT.A (the
 * Grove port: its 5 V and an I2C bus on it). msw_app.h, "time and battery"
 * and "PORT.A".
 *
 * PORT.A is HY2.0-4P: GND, 5V, G10 (SDA, yellow), G11 (SCL, white): M5's C152
 * page and M5Unified's pin table. The bus here is the apps' own (another I2C
 * controller), not the internal one at G47/G48. All of it goes off when the
 * app ends.
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "msw_app_priv.h"
#include "msw_time.h"
#include "muse_state.h"

static const char *TAG = "msw_port";

#define PORT_SDA 10
#define PORT_SCL 11
#define DEVS 8
#define I2C_TIMEOUT_MS 50

extern esp_err_t msw_board_port_power(bool on);

static bool s_power;
static i2c_master_bus_handle_t s_bus;
static int s_khz;
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

bool msw_port_power(bool on)
{
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
    if (!msw_port_power(true)) {
        return false;
    }
    if (s_bus && khz == s_khz) {
        return true;
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
    return true;
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

/* msw_app_core.c: an app ended. */
void msw_port_app_end(void)
{
    msw_i2c_end();
    msw_port_power(false);
}
