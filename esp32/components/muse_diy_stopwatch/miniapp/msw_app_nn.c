/*
 * museStopWatch mini apps: the int8 network layers (msw_app.h, "neural
 * network layers"). Thin checked wrappers over esp-nn, which the firmware
 * already carries for the wake word's TensorFlow Lite Micro (2026-11-14:
 * opened to apps without adding a library).
 *
 * esp-nn keeps one scratch pointer per kind of layer for the whole firmware
 * (esp_nn_set_conv_scratch_buf), and the wake word's interpreter sets it on
 * every run. So a layer runs under s_lock, and msw_wake.cc takes the same
 * lock (msw_nn_lock) around each Invoke.
 */
#include <stddef.h>
#include <string.h>

#include "esp_nn.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "msw_app_priv.h"

/* The app's structs are esp-nn's, field for field. */
_Static_assert(sizeof(msw_nn_dims_t) == sizeof(data_dims_t), "dims");
_Static_assert(sizeof(msw_nn_conv_t) == sizeof(conv_params_t), "conv");
_Static_assert(offsetof(msw_nn_conv_t, act_min) == offsetof(conv_params_t, activation), "conv act");
_Static_assert(sizeof(msw_nn_dwconv_t) == sizeof(dw_conv_params_t), "dwconv");
_Static_assert(offsetof(msw_nn_dwconv_t, act_min) == offsetof(dw_conv_params_t, activation), "dwconv act");
_Static_assert(sizeof(msw_nn_quant_t) == sizeof(quant_data_t), "quant");

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;

void msw_nn_lock(void)
{
    if (!s_lock) {
        taskENTER_CRITICAL(&s_lock_mux);
        if (!s_lock) {
            s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        }
        taskEXIT_CRITICAL(&s_lock_mux);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void msw_nn_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* w * h * c bytes, or 0 if a size is out of range (also > 16 MB). */
static size_t bytes3(int32_t w, int32_t h, int32_t c)
{
    if (w <= 0 || h <= 0 || c <= 0 || w > 4096 || h > 4096 || c > 65536) {
        return 0;
    }
    uint64_t n = (uint64_t)w * h * c;
    return n > (16u << 20) ? 0 : (size_t)n;
}

static bool dims_ok(const msw_nn_dims_t *d, const char *fn)
{
    return msw_api_readable(d, sizeof(*d), fn) && bytes3(d->width, d->height, d->channels);
}

static bool quant_ok(const msw_nn_quant_t *q, int ch, const char *fn)
{
    return msw_api_readable(q, sizeof(*q), fn) && msw_api_readable(q->shift, (size_t)ch * 4, fn)
           && msw_api_readable(q->mult, (size_t)ch * 4, fn);
}

static bool scratch_ok(void *scratch, int need, const char *fn)
{
    if (need <= 0) {
        return true;
    }
    if (!msw_api_writable(scratch, (size_t)need, fn)) {
        return false;
    }
    if ((uintptr_t)scratch & 15) {
        msw_app_logf("%s: scratch must be 16-byte aligned", fn);
        return false;
    }
    return true;
}

int msw_nn_conv_scratch(const msw_nn_dims_t *in, const msw_nn_dims_t *filter, const msw_nn_dims_t *out,
                        const msw_nn_conv_t *p)
{
    const char *fn = "msw_nn_conv_scratch";
    if (!dims_ok(in, fn) || !dims_ok(filter, fn) || !dims_ok(out, fn) || !msw_api_readable(p, sizeof(*p), fn)) {
        return -1;
    }
    return esp_nn_get_conv_scratch_size((const data_dims_t *)in, (const data_dims_t *)filter,
                                        (const data_dims_t *)out, (const conv_params_t *)p);
}

bool msw_nn_conv(const msw_nn_dims_t *in, const int8_t *in_data, const msw_nn_dims_t *filter,
                 const int8_t *filter_data, const int32_t *bias, const msw_nn_dims_t *out, int8_t *out_data,
                 const msw_nn_conv_t *p, const msw_nn_quant_t *quant, void *scratch)
{
    const char *fn = "msw_nn_conv";
    if (!dims_ok(in, fn) || !dims_ok(filter, fn) || !dims_ok(out, fn) || !msw_api_readable(p, sizeof(*p), fn)) {
        return false;
    }
    int oc = out->channels;
    size_t fbytes = bytes3(filter->width, filter->height, in->channels);
    if (!fbytes || filter->channels != in->channels || (uint64_t)fbytes * oc > (16u << 20)
        || !msw_api_readable(in_data, bytes3(in->width, in->height, in->channels), fn)
        || !msw_api_readable(filter_data, fbytes * oc, fn) || (bias && !msw_api_readable(bias, (size_t)oc * 4, fn))
        || !msw_api_writable(out_data, bytes3(out->width, out->height, oc), fn) || !quant_ok(quant, oc, fn)) {
        return false;
    }
    int need = esp_nn_get_conv_scratch_size((const data_dims_t *)in, (const data_dims_t *)filter,
                                            (const data_dims_t *)out, (const conv_params_t *)p);
    if (!scratch_ok(scratch, need, fn)) {
        return false;
    }
    msw_nn_lock();
    esp_nn_set_conv_scratch_buf(scratch);
    esp_nn_conv_s8((const data_dims_t *)in, in_data, (const data_dims_t *)filter, filter_data, bias,
                   (const data_dims_t *)out, out_data, (const conv_params_t *)p, (const quant_data_t *)quant);
    esp_nn_set_conv_scratch_buf(NULL);
    msw_nn_unlock();
    return true;
}

int msw_nn_dwconv_scratch(const msw_nn_dims_t *in, const msw_nn_dims_t *filter, const msw_nn_dims_t *out,
                          const msw_nn_dwconv_t *p)
{
    const char *fn = "msw_nn_dwconv_scratch";
    if (!dims_ok(in, fn) || !dims_ok(filter, fn) || !dims_ok(out, fn) || !msw_api_readable(p, sizeof(*p), fn)) {
        return -1;
    }
    return esp_nn_get_depthwise_conv_scratch_size((const data_dims_t *)in, (const data_dims_t *)filter,
                                                  (const data_dims_t *)out, (const dw_conv_params_t *)p);
}

bool msw_nn_dwconv(const msw_nn_dims_t *in, const int8_t *in_data, const msw_nn_dims_t *filter,
                   const int8_t *filter_data, const int32_t *bias, const msw_nn_dims_t *out, int8_t *out_data,
                   const msw_nn_dwconv_t *p, const msw_nn_quant_t *quant, void *scratch)
{
    const char *fn = "msw_nn_dwconv";
    if (!dims_ok(in, fn) || !dims_ok(filter, fn) || !dims_ok(out, fn) || !msw_api_readable(p, sizeof(*p), fn)) {
        return false;
    }
    int oc = out->channels;
    if (p->ch_mult <= 0 || in->channels * p->ch_mult != oc
        || !msw_api_readable(in_data, bytes3(in->width, in->height, in->channels), fn)
        || !msw_api_readable(filter_data, bytes3(filter->width, filter->height, oc), fn)
        || (bias && !msw_api_readable(bias, (size_t)oc * 4, fn))
        || !msw_api_writable(out_data, bytes3(out->width, out->height, oc), fn) || !quant_ok(quant, oc, fn)) {
        return false;
    }
    int need = esp_nn_get_depthwise_conv_scratch_size((const data_dims_t *)in, (const data_dims_t *)filter,
                                                      (const data_dims_t *)out, (const dw_conv_params_t *)p);
    if (!scratch_ok(scratch, need, fn)) {
        return false;
    }
    msw_nn_lock();
    esp_nn_set_depthwise_conv_scratch_buf(scratch);
    esp_nn_depthwise_conv_s8((const data_dims_t *)in, in_data, (const data_dims_t *)filter, filter_data, bias,
                             (const data_dims_t *)out, out_data, (const dw_conv_params_t *)p,
                             (const quant_data_t *)quant);
    esp_nn_set_depthwise_conv_scratch_buf(NULL);
    msw_nn_unlock();
    return true;
}

bool msw_nn_fc(const int8_t *in, int32_t in_offset, int row_len, const int8_t *filter, const int32_t *bias,
               int8_t *out, int out_channels, int32_t out_offset, const msw_nn_quant_t *quant,
               int32_t act_min, int32_t act_max)
{
    const char *fn = "msw_nn_fc";
    if (row_len <= 0 || row_len > 65535 || out_channels <= 0 || out_channels > 65535
        || !msw_api_readable(in, (size_t)row_len, fn)
        || !msw_api_readable(filter, (size_t)row_len * out_channels, fn)
        || (bias && !msw_api_readable(bias, (size_t)out_channels * 4, fn))
        || !msw_api_writable(out, (size_t)out_channels, fn) || !quant_ok(quant, out_channels, fn)) {
        return false;
    }
    msw_nn_lock();
    esp_nn_fully_connected_per_ch_s8(in, in_offset, (uint16_t)row_len, filter, 0, bias, out,
                                     (uint16_t)out_channels, out_offset, quant->shift, quant->mult, act_min,
                                     act_max);
    msw_nn_unlock();
    return true;
}

static bool pool_ok(const int8_t *in, int in_w, int in_h, int8_t *out, int out_w, int out_h, int channels,
                    const char *fn)
{
    return bytes3(in_w, in_h, channels) && bytes3(out_w, out_h, channels) && channels <= 65535
           && msw_api_readable(in, bytes3(in_w, in_h, channels), fn)
           && msw_api_writable(out, bytes3(out_w, out_h, channels), fn);
}

bool msw_nn_max_pool(const int8_t *in, int in_w, int in_h, int8_t *out, int out_w, int out_h, int stride_w,
                     int stride_h, int filter_w, int filter_h, int pad_w, int pad_h, int act_min, int act_max,
                     int channels)
{
    if (!pool_ok(in, in_w, in_h, out, out_w, out_h, channels, "msw_nn_max_pool")) {
        return false;
    }
    msw_nn_lock();
    esp_nn_max_pool_s8(in, in_w, in_h, out, out_w, out_h, stride_w, stride_h, filter_w, filter_h, pad_w, pad_h,
                       act_min, act_max, channels);
    msw_nn_unlock();
    return true;
}

bool msw_nn_avg_pool(const int8_t *in, int in_w, int in_h, int8_t *out, int out_w, int out_h, int stride_w,
                     int stride_h, int filter_w, int filter_h, int pad_w, int pad_h, int act_min, int act_max,
                     int channels)
{
    if (!pool_ok(in, in_w, in_h, out, out_w, out_h, channels, "msw_nn_avg_pool")) {
        return false;
    }
    msw_nn_lock();
    esp_nn_avg_pool_s8(in, in_w, in_h, out, out_w, out_h, stride_w, stride_h, filter_w, filter_h, pad_w, pad_h,
                       act_min, act_max, channels);
    msw_nn_unlock();
    return true;
}
