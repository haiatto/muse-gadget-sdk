/*
 * museStopWatch: wake words (msw_wake.h).
 *
 * The features and the detection follow ESPHome's micro_wake_word
 * (esphome/components/micro_wake_word: preprocessor_settings.h,
 * micro_wake_word.cpp, streaming_model.cpp), which the models were trained
 * for: a 30 ms window every 10 ms, 40 filterbank channels from 125 to
 * 7500 Hz, noise reduction, PCAN and log scale; features scaled to int8 as
 * (value * 256) / 666 - 128; the model's uint8 output averaged over its
 * sliding window against probability_cutoff; after a detection the model
 * must fall below the cutoff and 100 slices pass before the next.
 *
 * A turn: a wake word presses the app talk control; from then on only the
 * VAD runs, and the control is let go once speech has stopped for
 * END_SILENCE_MS (or no speech came, or the turn is too long).
 *
 * Memory: the models are copied to PSRAM (16-byte aligned for the
 * flatbuffer); their tensor arenas, the ring of mic samples and the tasks'
 * stacks are in PSRAM too.
 */
#include "msw_wake.h"

#include <atomic>
#include <initializer_list>
#include <new>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"

#include "frontend.h"
#include "frontend_util.h"
#include "msw_stack.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

static const char *TAG = "msw_wake";

#define RATE 16000
#define STEP_MS 10
#define FEATURES 40
#define RING_BYTES (RATE * 2)              /* 1 s of samples */
#define VAR_ARENA 1024                     /* the streaming models' state variables */
#define MIN_SLICES_BEFORE_DETECTION 100
#define STAT_US (60 * 1000000LL)
#define POLICY_MS 500                      /* how often the screen and USB are looked at */
#define END_SILENCE_MS 900                 /* speech stopped this long: the turn ends */
#define NO_SPEECH_MS 6000                  /* no speech this long after the wake word */
#define TURN_MAX_MS 14000                  /* under the voice task's 15 s */
#define NVS_NS "msw"
#define NVS_KEY "wake_mode"

extern "C" {
extern const uint8_t hey_muse_start[] asm("_binary_hey_muse_tflite_start");
extern const uint8_t hey_muse_end[] asm("_binary_hey_muse_tflite_end");
extern const uint8_t vad_start[] asm("_binary_vad_tflite_start");
extern const uint8_t vad_end[] asm("_binary_vad_tflite_end");
void msw_console_add(bool (*fn)(const char *line));
void msw_board_vibrate(int pct) __attribute__((weak));
bool msw_mic_wanted(void) __attribute__((weak));   /* a mini app has the mic */
/* miniapp/msw_app_nn.c: mini apps run esp-nn layers too, and esp-nn keeps one
 * scratch pointer per kind of layer; one at a time (2026-11-14). */
void msw_nn_lock(void) __attribute__((weak));
void msw_nn_unlock(void) __attribute__((weak));
/* muse (muse_state.h, muse_menu.h, muse_input.h have no C++ guards) */
bool muse_state_asleep(void);
bool muse_state_on_battery(void);
bool muse_menu_is_open(void);
void muse_input_app_talk(bool pressed);
}

/* Values from each model's manifest (the .json files in models). */
struct Model {
    const char *name;
    const char *word;   /* as shown */
    const uint8_t *start, *end;
    float cutoff;
    int window;
    size_t arena_size;

    uint8_t *flat, *arena, *var_arena;
    tflite::MicroAllocator *ma;
    tflite::MicroResourceVariables *mrv;
    tflite::MicroInterpreter *it;
    int stride, step;
    uint8_t probs[16];
    int idx, ignore;
    uint8_t last;
    uint32_t invokes, detections;
    int64_t us_sum, us_max;
};

static Model s_word = { "hey_muse", "Hey Muse", hey_muse_start, hey_muse_end, 0.97f, 5, 30000 };
static Model s_vad = { "vad", "", vad_start, vad_end, 0.5f, 5, 16772 };

static tflite::MicroMutableOpResolver<20> *s_ops;
static StreamBufferHandle_t s_ring;
static std::atomic<bool> s_on{false};
static std::atomic<bool> s_turn{false};
static std::atomic<bool> s_test{false};   /* ">msw.ww test": a turn as if the word were heard */
static std::atomic<int> s_mode{MSW_WAKE_OFF};
static std::atomic<uint32_t> s_dropped{0};
static bool s_loaded;
static TaskHandle_t s_task, s_policy;
static FrontendConfig s_fe_cfg;
static FrontendState s_fe;

/* The turn (wake task only). */
static int64_t s_turn_t0, s_last_speech;
static bool s_speech;
static uint32_t s_turns;

static bool add_ops(tflite::MicroMutableOpResolver<20> &r)
{
    return r.AddCallOnce() == kTfLiteOk && r.AddVarHandle() == kTfLiteOk && r.AddReshape() == kTfLiteOk &&
           r.AddReadVariable() == kTfLiteOk && r.AddStridedSlice() == kTfLiteOk &&
           r.AddConcatenation() == kTfLiteOk && r.AddAssignVariable() == kTfLiteOk && r.AddConv2D() == kTfLiteOk &&
           r.AddMul() == kTfLiteOk && r.AddAdd() == kTfLiteOk && r.AddMean() == kTfLiteOk &&
           r.AddFullyConnected() == kTfLiteOk && r.AddLogistic() == kTfLiteOk && r.AddQuantize() == kTfLiteOk &&
           r.AddDepthwiseConv2D() == kTfLiteOk && r.AddAveragePool2D() == kTfLiteOk &&
           r.AddMaxPool2D() == kTfLiteOk && r.AddPad() == kTfLiteOk && r.AddPack() == kTfLiteOk &&
           r.AddSplitV() == kTfLiteOk;
}

static void *psram(size_t n, size_t align = 16)
{
    return heap_caps_aligned_alloc(align, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool load_model(Model &m)
{
    size_t n = (size_t)(m.end - m.start);
    m.flat = (uint8_t *)psram(n);
    m.var_arena = (uint8_t *)psram(VAR_ARENA);
    if (!m.flat || !m.var_arena) {
        return false;
    }
    memcpy(m.flat, m.start, n);
    const tflite::Model *model = tflite::GetModel(m.flat);
    /* The manifest's size is a minimum: try it, then 1.5x and 2x, as ESPHome does. */
    static const float grow[] = { 1.0f, 1.5f, 2.0f };
    for (float g : grow) {
        size_t size = ((size_t)(m.arena_size * g) + 15) & ~(size_t)15;
        m.arena = (uint8_t *)psram(size);
        m.ma = tflite::MicroAllocator::Create(m.var_arena, VAR_ARENA);
        m.mrv = m.ma ? tflite::MicroResourceVariables::Create(m.ma, 20) : nullptr;
        void *mem = psram(sizeof(tflite::MicroInterpreter));
        if (m.arena && m.mrv && mem) {
            m.it = new (mem) tflite::MicroInterpreter(model, *s_ops, m.arena, size, m.mrv);
            if (m.it->AllocateTensors() == kTfLiteOk) {
                TfLiteTensor *in = m.it->input(0);
                m.stride = in->dims->data[1];
                m.ignore = -MIN_SLICES_BEFORE_DETECTION;
                ESP_LOGI(TAG, "%s: %u bytes, arena %u (used %u), stride %d", m.name, (unsigned)n, (unsigned)size,
                         (unsigned)m.it->arena_used_bytes(), m.stride);
                return true;
            }
            m.it->~MicroInterpreter();
        }
        heap_caps_free(mem);
        heap_caps_free(m.arena);
        m.arena = nullptr;
        m.it = nullptr;
    }
    ESP_LOGW(TAG, "%s: could not allocate its tensors", m.name);
    return false;
}

/* Forget the recent outputs; the next detection waits MIN_SLICES. */
static void quiet_model(Model &m)
{
    memset(m.probs, 0, sizeof(m.probs));
    m.ignore = -MIN_SLICES_BEFORE_DETECTION;
}

/* One slice of features into the model. Returns 1 when its average is over
 * the cutoff, 0 when under, -1 when it did not run on this slice. */
static int feed_model(Model &m, const int8_t *feat, bool hold_off)
{
    if (!m.it) {
        return -1;
    }
    TfLiteTensor *in = m.it->input(0);
    memcpy(in->data.int8 + FEATURES * m.step, feat, FEATURES);
    if (++m.step < m.stride) {
        return -1;
    }
    m.step = 0;
    int64_t t0 = esp_timer_get_time();
    if (msw_nn_lock) {
        msw_nn_lock();
    }
    TfLiteStatus st = m.it->Invoke();
    if (msw_nn_unlock) {
        msw_nn_unlock();
    }
    if (st != kTfLiteOk) {
        return -1;
    }
    int64_t us = esp_timer_get_time() - t0;
    m.invokes++;
    m.us_sum += us;
    m.us_max = us > m.us_max ? us : m.us_max;
    uint8_t p = m.it->output(0)->data.uint8[0];
    m.last = p;
    m.probs[m.idx] = p;
    m.idx = (m.idx + 1) % m.window;
    uint8_t cut = (uint8_t)(m.cutoff * 255.0f);
    if (hold_off && m.ignore < 0) {
        if (p < cut) {
            m.ignore++;
        }
        return 0;
    }
    int sum = 0;
    for (int i = 0; i < m.window; i++) {
        sum += m.probs[i];
    }
    return sum > cut * m.window ? 1 : 0;
}

static bool load(void)
{
    if (s_loaded) {
        return true;
    }
    void *mem = psram(sizeof(tflite::MicroMutableOpResolver<20>));
    if (!mem) {
        return false;
    }
    s_ops = new (mem) tflite::MicroMutableOpResolver<20>();
    if (!add_ops(*s_ops)) {
        return false;
    }
    FrontendFillConfigWithDefaults(&s_fe_cfg);
    s_fe_cfg.window.size_ms = 30;
    s_fe_cfg.window.step_size_ms = STEP_MS;
    s_fe_cfg.filterbank.num_channels = FEATURES;
    s_fe_cfg.filterbank.lower_band_limit = 125.0f;
    s_fe_cfg.filterbank.upper_band_limit = 7500.0f;
    s_fe_cfg.noise_reduction.smoothing_bits = 10;
    s_fe_cfg.noise_reduction.even_smoothing = 0.025f;
    s_fe_cfg.noise_reduction.odd_smoothing = 0.06f;
    s_fe_cfg.noise_reduction.min_signal_remaining = 0.05f;
    s_fe_cfg.pcan_gain_control.enable_pcan = 1;
    s_fe_cfg.pcan_gain_control.strength = 0.95f;
    s_fe_cfg.pcan_gain_control.offset = 80.0f;
    s_fe_cfg.pcan_gain_control.gain_bits = 21;
    s_fe_cfg.log_scale.enable_log = 1;
    s_fe_cfg.log_scale.scale_shift = 6;
    /* The frontend mallocs its tables (about 11 KB, below the size malloc
     * sends to PSRAM): send them there for this call. */
    heap_caps_malloc_extmem_enable(0);
    bool fe_ok = FrontendPopulateState(&s_fe_cfg, &s_fe, RATE);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (!fe_ok) {
        ESP_LOGW(TAG, "frontend: out of memory");
        return false;
    }
    load_model(s_word);
    load_model(s_vad);
    s_loaded = true;
    return true;
}

static void buzz(int ms)
{
    if (msw_board_vibrate) {
        msw_board_vibrate(60);
        vTaskDelay(pdMS_TO_TICKS(ms));
        msw_board_vibrate(0);
    }
}

static void stats(int64_t secs)
{
    for (Model *m : { &s_word, &s_vad }) {
        if (m->invokes) {
            ESP_LOGI(TAG, "%s: %u runs, %lld us average, %lld us at most, last %d, %u detections", m->name,
                     (unsigned)m->invokes, (long long)(m->us_sum / m->invokes), (long long)m->us_max, m->last,
                     (unsigned)m->detections);
        }
        m->invokes = 0;
        m->us_sum = m->us_max = 0;
    }
    ESP_LOGI(TAG, "%lld s, %u turns, %u bytes dropped, internal free %u, PSRAM free %u", (long long)secs,
             (unsigned)s_turns, (unsigned)s_dropped.exchange(0),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- the turn ---------------------------------------------------------------- */

/* The talk control would mean something else now: the Muse menu takes it
 * as Select, and a mini app holding the mic wants quiet. */
static bool can_start_turn(void)
{
    return !muse_menu_is_open() && !(msw_mic_wanted && msw_mic_wanted());
}

static void start_turn(const char *why)
{
    s_turn_t0 = s_last_speech = esp_timer_get_time();
    s_speech = false;
    quiet_model(s_vad);
    s_turn = true;
    s_turns++;
    muse_input_app_talk(true);
    printf("@msw.ww turn start (%s)\n", why);
    fflush(stdout);
    buzz(120);
}

static void end_turn(const char *why)
{
    muse_input_app_talk(false);
    s_turn = false;
    quiet_model(s_word);
    printf("@msw.ww turn end (%s, %.1f s)\n", why, (double)(esp_timer_get_time() - s_turn_t0) / 1e6);
    fflush(stdout);
}

/* Called after each slice and when no samples came for a while. */
static void check_turn(void)
{
    if (!s_turn) {
        return;
    }
    int64_t ms = (esp_timer_get_time() - s_turn_t0) / 1000;
    int64_t quiet_ms = (esp_timer_get_time() - s_last_speech) / 1000;
    if (!s_on) {
        end_turn("off");
    } else if (s_speech && quiet_ms >= END_SILENCE_MS) {
        end_turn("speech ended");
    } else if (!s_speech && ms >= NO_SPEECH_MS) {
        end_turn("no speech");
    } else if (ms >= TURN_MAX_MS) {
        end_turn("too long");
    }
}

static void wake_task(void *arg)
{
    (void)arg;
    static int16_t buf[RATE * STEP_MS / 1000];
    int8_t feat[FEATURES];
    int64_t stat_at = esp_timer_get_time();
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(s_on || s_turn ? 100 : 2000);
        size_t got = xStreamBufferReceive(s_ring, buf, sizeof(buf), wait) / 2;
        const int16_t *p = buf;
        while (got > 0) {
            size_t used = 0;
            FrontendOutput out = FrontendProcessSamples(&s_fe, p, got, &used);
            p += used;
            got -= used;
            if (out.size != FEATURES) {
                continue;
            }
            for (int i = 0; i < FEATURES; i++) {
                int32_t v = ((int32_t)out.values[i] * 256 + 333) / 666 - 128;
                feat[i] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
            }
            if (s_turn) {
                if (feed_model(s_vad, feat, false) == 1) {
                    s_speech = true;
                    s_last_speech = esp_timer_get_time();
                }
                check_turn();
            } else if (feed_model(s_word, feat, true) == 1) {
                s_word.detections++;
                int avg = 0;
                for (int i = 0; i < s_word.window; i++) {
                    avg += s_word.probs[i];
                }
                quiet_model(s_word);
                printf("@msw.ww detected %s (average %d of 255)\n", s_word.name, avg / s_word.window);
                fflush(stdout);
                if (can_start_turn()) {
                    start_turn(s_word.name);
                } else {
                    buzz(40);
                }
            }
        }
        if (s_test.exchange(false) && !s_turn) {
            start_turn("test");
        }
        check_turn();
        int64_t now = esp_timer_get_time();
        if (s_on && now - stat_at >= STAT_US) {
            stats((now - stat_at) / 1000000);
            stat_at = now;
        }
    }
}

static void push(const int16_t *mono, size_t n)
{
    size_t sent = xStreamBufferSend(s_ring, mono, n * 2, 0);
    if (sent < n * 2) {
        s_dropped += (uint32_t)(n * 2 - sent);
    }
}

extern "C" void msw_wake_feed(const int16_t *mono, size_t n)
{
    if ((s_on || s_turn) && s_ring) {
        push(mono, n);
    }
}

extern "C" void msw_wake_turn_feed(const int16_t *mono, size_t n, bool quiet)
{
    if (s_ring && (s_turn || (quiet && s_on))) {
        push(mono, n);
    }
}

extern "C" bool msw_wake_enabled(void)
{
    return s_on;
}

/* Loads the models on the first start. Policy task only. */
static bool set_on(bool on)
{
    if (on == s_on) {
        return true;
    }
    if (on && !s_task) {
        static EXT_RAM_BSS_ATTR uint8_t ring_mem[RING_BYTES + 1];
        static StaticStreamBuffer_t ring_struct;
        s_ring = xStreamBufferCreateStatic(RING_BYTES, 2 * RATE * STEP_MS / 1000, ring_mem, &ring_struct);
        if (!load() || msw_task_create(wake_task, "msw_wake", 8192, NULL, 3, &s_task) != pdPASS) {
            ESP_LOGW(TAG, "could not start");
            return false;
        }
    }
    if (!on && s_ring) {
        xStreamBufferReset(s_ring);   /* fails in a turn's middle: then the rest is read */
    }
    s_on = on;
    ESP_LOGI(TAG, "%s", on ? "listening" : "off");
    return true;
}

/* ---- the setting ------------------------------------------------------------- */

static bool wanted(void)
{
    switch (s_mode.load()) {
    case MSW_WAKE_SCREEN:
        return !muse_state_asleep();
    case MSW_WAKE_SCREEN_USB:
        return !muse_state_asleep() || !muse_state_on_battery();
    default:
        return false;
    }
}

static void policy_task(void *arg)
{
    (void)arg;
    bool failed = false;
    for (;;) {
        bool want = wanted();
        if (want != s_on && !(want && failed)) {
            failed = !set_on(want);
        }
        if (s_mode == MSW_WAKE_OFF && !s_on) {
            failed = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* until the setting changes */
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLICY_MS));
        }
    }
}

extern "C" int msw_wake_mode(void)
{
    return s_mode;
}

extern "C" const char *msw_wake_word(void)
{
    return s_word.word;
}

extern "C" void msw_wake_set_mode(int mode)
{
    if (mode < MSW_WAKE_OFF || mode > MSW_WAKE_SCREEN_USB) {
        return;
    }
    s_mode = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, NVS_KEY, (uint8_t)mode) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "setting: %d", mode);
    if (s_policy) {
        xTaskNotifyGive(s_policy);
    }
}

extern "C" void msw_wake_init(void)
{
    if (s_policy) {
        return;
    }
    nvs_handle_t h;
    uint8_t mode = MSW_WAKE_OFF;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &mode);
        nvs_close(h);
    }
    s_mode = mode <= (uint8_t)MSW_WAKE_SCREEN_USB ? (int)mode : (int)MSW_WAKE_OFF;
    /* Loading the models runs on this stack too. */
    if (msw_task_create(policy_task, "msw_wake_ctl", 6144, NULL, 2, &s_policy) != pdPASS) {
        ESP_LOGW(TAG, "could not start its control task");
    }
}

/* ---- serial ------------------------------------------------------------------ */

static bool console(const char *line)
{
    if (strncmp(line, "msw.ww", 6)) {
        return false;
    }
    const char *arg = line[6] == ' ' ? line + 7 : "";
    static const char *const MODES[] = { "off", "screen", "usb" };
    bool known = false;
    for (int i = 0; i < 3; i++) {
        if (!strcmp(arg, MODES[i])) {
            msw_wake_set_mode(i);
            known = true;
        }
    }
    if (!strcmp(arg, "test")) {
        s_test = true;
        if (!s_task) {
            printf("@msw.ww test: not listening yet (set screen or usb first)\n");
        }
    } else if (!known) {
        stats(0);
    }
    printf("@msw.ww mode %s, %s%s\n", MODES[s_mode], s_on ? "listening" : "off", s_turn ? ", in a turn" : "");
    fflush(stdout);
    return true;
}

/* Registers the serial command at boot (before app_main). */
__attribute__((constructor)) static void register_console(void)
{
    msw_console_add(console);
}
