/*
 * museStopWatch: voice.play and sound.play (2026-11-13, user decision).
 *
 * Both play an MP3 from the VM, streamed as kantanMuse's kanplay.play_audio
 * does: a single MP3, or an HLS index.m3u8 that lists the segments of one MP3
 * stream (cut anywhere) as Muse writes them, re-read about each second until
 * #EXT-X-ENDLIST. No new segment for 30 s stops it.
 *
 * voice.play: Muse's own voice for the reply of this turn. The chat session
 *   plays it in place of the local voice, through muse_reply_voice (its
 *   say_live()/read()), so the captions follow it, and the turn's replies are
 *   not spoken by the local voice. Only while a voice turn waits for its reply.
 *   cv names the voice ("CV: ..." on the face while it plays).
 * sound.play: a sound over everything else (an alarm, a jingle), mixed in at
 *   the speaker (msw_audio_overlay in muse_audio.c).
 *
 * One stream at a time (one MP3 buffer, the chat session's, which nothing
 * else uses since replies are spoken locally): a new one replaces the old.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_fetch.h"
#include "minimp3.h"
#include "msw_stack.h"
#include "muse_voice.h"
#include "esp_attr.h"

static const char *TAG = "msw_play";

uint8_t *muse_hatch_mp3_buffer(size_t *len);
bool muse_hatch_voice_waiting(void);

#define OUT_RATE 16000
#define LIST_CAP (8 * 1024)          /* index.m3u8 */
#define RING_BYTES (64 * 1024)       /* MP3 bytes downloaded, not yet decoded (power of 2) */
#define WIN_BYTES (16 * 1024)        /* the decoder's window */
#define PCM_SAMPLES (16 * 1024)      /* 1 s of 16 kHz output (power of 2) */
#define MP3_HOLD (1441 + 4)          /* the largest MP3 frame and the next header */
#define LIST_POLL_MS 1000
#define LIST_STALL_MS 30000
#define UNREAD_MS 60000              /* a voice nobody plays (the turn ended): dropped */
#define TASK_STACK (32 * 1024)       /* minimp3 keeps its scratch on the stack */

enum { MODE_NONE, MODE_VOICE, MODE_SOUND };
enum { SINK_DETECT, SINK_LIST, SINK_MP3 };

typedef struct {
    char path[192];
    char cv[48];
    int mode;
    int volume;
} req_t;

/* The request, handed to the task. */
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static req_t s_req;
static bool s_req_new, s_running;
static atomic_uint s_gen;

/* What plays now. */
static atomic_int s_mode;
static atomic_int s_volume;
EXT_RAM_BSS_ATTR static char s_cv[48];
static atomic_bool s_decoding;   /* the task is still making PCM */
static atomic_bool s_heard;      /* the voice was read at least once */
static atomic_llong s_read_us;   /* the reader's last read */
static atomic_bool s_abandon;    /* a voice nobody read for UNREAD_MS: give up */

/* Buffers, carved from the chat session's MP3 buffer. */
static char *s_list;
static uint8_t *s_ring, *s_win;
static int16_t *s_pcm;
static atomic_uint s_ring_w, s_ring_r;
static portMUX_TYPE s_pcm_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pcm_w, s_pcm_r;

/* The download in flight. */
static atomic_bool s_dl_done;
static const char *volatile s_dl_code;
static int s_sink;
static size_t s_list_len;
static bool s_list_over;

/* Decoder. */
EXT_RAM_BSS_ATTR static mp3dec_t s_dec;
EXT_RAM_BSS_ATTR static int16_t s_frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
EXT_RAM_BSS_ATTR static int16_t s_out[MINIMP3_MAX_SAMPLES_PER_FRAME];
static size_t s_win_len;
static uint32_t s_step, s_pos;   /* resampler: Q16 input samples per output sample */
static int16_t s_prev;
static int s_rate;
static uint32_t s_made;          /* output samples made */

/* Plays in place of the local voice for the reply: counted as a "say". */
static atomic_uint s_says;

/* Last result, for >msw.play. */
static char s_last[96];

static bool cancelled(uint32_t gen)
{
    return atomic_load(&s_gen) != gen || atomic_load(&s_abandon);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- PCM ring: the task writes, the chat task (voice) or the speaker (sound) reads ---- */

static size_t pcm_room(void)
{
    portENTER_CRITICAL(&s_pcm_mux);
    size_t n = PCM_SAMPLES - (s_pcm_w - s_pcm_r);
    portEXIT_CRITICAL(&s_pcm_mux);
    return n;
}

static size_t pcm_have(void)
{
    portENTER_CRITICAL(&s_pcm_mux);
    size_t n = s_pcm_w - s_pcm_r;
    portEXIT_CRITICAL(&s_pcm_mux);
    return n;
}

static void pcm_put(const int16_t *p, size_t n)
{
    portENTER_CRITICAL(&s_pcm_mux);
    for (size_t i = 0; i < n && s_pcm_w - s_pcm_r < PCM_SAMPLES; i++) {
        s_pcm[s_pcm_w++ & (PCM_SAMPLES - 1)] = p[i];
    }
    portEXIT_CRITICAL(&s_pcm_mux);
}

static size_t pcm_get(int16_t *p, size_t max)
{
    if (!s_pcm) {
        return 0;
    }
    portENTER_CRITICAL(&s_pcm_mux);
    size_t n = 0;
    while (n < max && s_pcm_r != s_pcm_w) {
        p[n++] = s_pcm[s_pcm_r++ & (PCM_SAMPLES - 1)];
    }
    portEXIT_CRITICAL(&s_pcm_mux);
    return n;
}

static void pcm_clear(void)
{
    portENTER_CRITICAL(&s_pcm_mux);
    s_pcm_r = s_pcm_w = 0;
    portEXIT_CRITICAL(&s_pcm_mux);
}

/* Something still to play: being made, or made and not read yet. */
static bool live(int mode)
{
    return atomic_load(&s_mode) == mode && (atomic_load(&s_decoding) || pcm_have());
}

/* ---- the download: the VM file's parts arrive on the download task ---- */

static int sink(const uint8_t *data, size_t len, void *user)
{
    uint32_t gen = (uint32_t)(uintptr_t)user;
    if (s_sink == SINK_DETECT) {
        size_t skip = len >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3) ? 3 : 0;
        s_sink = len - skip >= 7 && !memcmp(data + skip, "#EXTM3U", 7) ? SINK_LIST : SINK_MP3;
    }
    if (s_sink == SINK_LIST) {
        if (s_list_len + len >= LIST_CAP) {
            s_list_over = true;
            return -1;
        }
        memcpy(s_list + s_list_len, data, len);
        s_list_len += len;
        return 0;
    }
    while (len) {
        if (cancelled(gen)) {
            return -1;
        }
        uint32_t w = atomic_load(&s_ring_w), r = atomic_load(&s_ring_r);
        size_t room = RING_BYTES - (w - r);
        if (!room) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t n = len < room ? len : room;
        size_t at = w & (RING_BYTES - 1);
        size_t first = RING_BYTES - at < n ? RING_BYTES - at : n;
        memcpy(s_ring + at, data, first);
        memcpy(s_ring, data + first, n - first);
        atomic_store(&s_ring_w, w + n);
        data += n;
        len -= n;
    }
    return 0;
}

static void dl_done(size_t total, const char *code, const char *message, void *user)
{
    (void)total;
    (void)message;
    (void)user;
    s_dl_code = code;   /* the codes are string literals */
    atomic_store(&s_dl_done, true);
}

/* ---- decoding: ring -> window -> minimp3 -> 16 kHz mono -> PCM ring ---- */

static void resample_init(int rate)
{
    s_step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
    s_pos = 0;
    s_prev = 0;
    s_rate = rate;
}

static size_t resample(const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    while ((s_pos >> 16) < n) {
        size_t i = s_pos >> 16;
        int32_t a = i ? in[i - 1] : s_prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(s_pos & 0xffff)) >> 16));
        s_pos += s_step;
    }
    s_pos -= n << 16;
    s_prev = in[n - 1];
    return o;
}

#define FRAME_ROOM (MINIMP3_MAX_SAMPLES_PER_FRAME / 2 + 8)

/* A voice is waiting for a reader that does not come (the turn ended). */
static bool unread(void)
{
    if (atomic_load(&s_mode) == MODE_VOICE && pcm_room() < FRAME_ROOM &&
        esp_timer_get_time() - atomic_load(&s_read_us) > UNREAD_MS * 1000LL) {
        atomic_store(&s_abandon, true);
    }
    return atomic_load(&s_abandon);
}

/* Decodes what it can. ended: no more bytes will come (decode to the end). */
static void decode_some(bool ended)
{
    for (;;) {
        /* Top up the window from the ring. */
        uint32_t w = atomic_load(&s_ring_w), r = atomic_load(&s_ring_r);
        size_t n = w - r;
        if (n > WIN_BYTES - s_win_len) {
            n = WIN_BYTES - s_win_len;
        }
        for (size_t i = 0; i < n; i++) {
            s_win[s_win_len + i] = s_ring[(r + i) & (RING_BYTES - 1)];
        }
        s_win_len += n;
        atomic_store(&s_ring_r, r + n);
        bool ring_empty = atomic_load(&s_ring_w) == atomic_load(&s_ring_r);
        bool last = ended && ring_empty;
        /* minimp3 takes a frame only once it sees the next header: until the
         * end, leave the last MP3_HOLD bytes for more to arrive. */
        size_t hold = last ? 0 : MP3_HOLD;
        size_t off = 0;
        bool full = false;
        while (s_win_len - off > hold) {
            if (pcm_room() < FRAME_ROOM) {
                full = true;
                break;
            }
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&s_dec, s_win + off, (int)(s_win_len - off), s_frame, &info);
            if (!info.frame_bytes) {
                if (last) {
                    off = s_win_len;   /* trailing junk */
                }
                break;
            }
            off += info.frame_bytes;
            if (!samples) {
                continue;   /* ID3 or a skipped frame */
            }
            if (info.channels == 2) {
                for (int i = 0; i < samples; i++) {
                    s_frame[i] = (int16_t)(((int32_t)s_frame[2 * i] + s_frame[2 * i + 1]) / 2);
                }
            }
            if (s_rate != info.hz) {
                resample_init(info.hz);
            }
            size_t m = resample(s_frame, (size_t)samples, s_out);
            pcm_put(s_out, m);
            s_made += m;
        }
        if (off) {
            memmove(s_win, s_win + off, s_win_len - off);
            s_win_len -= off;
        } else if (!full && s_win_len == WIN_BYTES) {
            /* A full window and no frame in it: not MP3. Not while the PCM is
             * full: then nothing was tried (it dropped 5 s of a 7 s voice,
             * 2026-11-13). */
            s_win_len = 0;
        }
        if (full || ring_empty) {
            return;
        }
    }
}

/* Waits for the download while decoding. NULL when it finished, else the error. */
static const char *fetch(const char *path, int sink_mode, uint32_t gen)
{
    s_sink = sink_mode;
    s_list_over = false;
    if (sink_mode == SINK_LIST || sink_mode == SINK_DETECT) {
        s_list_len = 0;
    }
    atomic_store(&s_dl_done, false);
    s_dl_code = NULL;
    const char *code = NULL, *message = NULL;
    /* The one download slot may be taken (an image, an app): wait a little. */
    for (int tries = 0;; tries++) {
        if (image_fetch_vm_stream(path, 16 * 1024 * 1024, sink, dl_done, (void *)(uintptr_t)gen, &code, &message)) {
            break;
        }
        if (strcmp(code, "busy") || tries >= 50 || cancelled(gen)) {
            return code;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    while (!atomic_load(&s_dl_done)) {
        decode_some(false);
        unread();   /* the sink stops on it */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (atomic_load(&s_abandon)) {
        return "not_played";
    }
    if (s_list_over) {
        return "too_large";
    }
    return s_dl_code;
}

/* Decodes while waiting ms, or until cancelled. False if a voice went unread too long. */
static bool pump(int ms, uint32_t gen)
{
    for (int t = 0; t < ms && !cancelled(gen); t += 5) {
        decode_some(false);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return !unread();
}

/* HLS (RFC 8216) as kanplay.play_audio reads it: segment URIs in order and the end tag. */
static size_t list_scan(size_t want, const char **uri, size_t *uri_len, bool *ended)
{
    size_t count = 0, i = s_list_len >= 3 && !memcmp(s_list, "\xEF\xBB\xBF", 3) ? 3 : 0;
    *ended = false;
    while (i < s_list_len) {
        size_t end = i;
        while (end < s_list_len && s_list[end] != '\n' && s_list[end] != '\r') {
            end++;
        }
        size_t b = i, e = end;
        while (b < e && (s_list[b] == ' ' || s_list[b] == '\t')) {
            b++;
        }
        while (e > b && (s_list[e - 1] == ' ' || s_list[e - 1] == '\t')) {
            e--;
        }
        if (e > b) {
            if (s_list[b] != '#') {
                if (count == want && uri) {
                    *uri = s_list + b;
                    *uri_len = e - b;
                }
                count++;
            } else if (e - b == 14 && !memcmp(s_list + b, "#EXT-X-ENDLIST", 14)) {
                *ended = true;
            }
        }
        i = end;
        while (i < s_list_len && (s_list[i] == '\n' || s_list[i] == '\r')) {
            i++;
        }
    }
    return count;
}

/* The segment's path, next to the playlist. NULL when fine, else the error. */
static const char *segment_path(const char *list_path, const char *uri, size_t len, char *out, size_t cap)
{
    for (size_t k = 0; k + 2 < len; k++) {
        if (!memcmp(uri + k, "://", 3)) {
            return "unsupported_uri";
        }
    }
    if (uri[0] == '/' || memchr(uri, '?', len)) {
        return "unsupported_uri";
    }
    const char *slash = strrchr(list_path, '/');
    size_t dir = slash ? (size_t)(slash - list_path) + 1 : 0;
    if (dir + len + 1 > cap) {
        return "invalid_path";
    }
    memcpy(out, list_path, dir);
    memcpy(out + dir, uri, len);
    out[dir + len] = 0;
    return NULL;
}

static const char *play_list(const char *path, uint32_t gen, int *segments)
{
    size_t index = 0;
    int64_t waiting_since = now_ms();
    for (;;) {
        if (cancelled(gen)) {
            return "stopped";
        }
        const char *uri = NULL;
        size_t uri_len = 0;
        bool ended;
        size_t count = list_scan(index, &uri, &uri_len, &ended);
        if (index < count) {
            char seg[224];
            const char *err = segment_path(path, uri, uri_len, seg, sizeof(seg));
            if (!err) {
                /* The playlist is kept apart: a segment goes to the ring. */
                err = fetch(seg, SINK_MP3, gen);
            }
            if (err) {
                return err;
            }
            *segments = (int)++index;
            waiting_since = now_ms();
            continue;
        }
        if (ended) {
            return index ? NULL : "empty_playlist";
        }
        if (now_ms() - waiting_since > LIST_STALL_MS) {
            return "playlist_stalled";
        }
        if (!pump(LIST_POLL_MS, gen)) {
            return "not_played";
        }
        if (cancelled(gen)) {
            return "stopped";
        }
        const char *err = fetch(path, SINK_LIST, gen);
        if (err) {
            return err;
        }
        if (s_list_len < 7 || memcmp(s_list + (s_list_len >= 3 && !memcmp(s_list, "\xEF\xBB\xBF", 3) ? 3 : 0), "#EXTM3U", 7)) {
            return "not_playlist";
        }
    }
}

static void play(const req_t *req, uint32_t gen)
{
    pcm_clear();
    atomic_store(&s_abandon, false);
    atomic_store(&s_ring_w, 0);
    atomic_store(&s_ring_r, 0);
    s_win_len = 0;
    s_made = 0;
    s_rate = 0;
    mp3dec_init(&s_dec);
    strlcpy(s_cv, req->cv, sizeof(s_cv));
    atomic_store(&s_volume, req->volume);
    atomic_store(&s_heard, false);
    atomic_store(&s_read_us, esp_timer_get_time());
    atomic_store(&s_decoding, true);
    atomic_store(&s_mode, req->mode);
    if (req->mode == MODE_SOUND) {
        muse_voice_player_kick();
    }
    int64_t t0 = now_ms();
    int segments = 0;
    const char *err = fetch(req->path, SINK_DETECT, gen);
    bool list = s_sink == SINK_LIST;
    if (!err && list) {
        err = play_list(req->path, gen, &segments);
    }
    /* Decode the rest (also after a failed segment: what came still plays). */
    while (!cancelled(gen) && (atomic_load(&s_ring_w) != atomic_load(&s_ring_r) || s_win_len > 0)) {
        decode_some(true);
        if (!pump(5, gen)) {
            err = err ? err : "not_played";
            break;
        }
    }
    if (!err && !s_made) {
        err = "not_mp3";
    }
    bool replaced = atomic_load(&s_gen) != gen;
    if (cancelled(gen) && !err) {
        err = replaced ? "stopped" : "not_played";
    }
    if (req->mode == MODE_VOICE && !s_made) {
        atomic_fetch_sub(&s_says, 1);   /* nothing to say: the local voice speaks the reply */
    }
    if (!replaced) {
        /* A newer request owns these now (the command set them for it). */
        atomic_store(&s_decoding, false);
        if (atomic_load(&s_abandon)) {
            pcm_clear();
        }
    }
    snprintf(s_last, sizeof(s_last), "%s %s: %s, %.1fs of audio, %d segment(s), %lld ms",
             req->mode == MODE_VOICE ? "voice" : "sound", list ? "hls" : "mp3", err ? err : "done",
             s_made / (double)OUT_RATE, segments, (long long)(now_ms() - t0));
    printf("@msw.play %s\n", s_last);
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        req_t req;
        portENTER_CRITICAL(&s_req_mux);
        if (!s_req_new) {
            s_running = false;
            portEXIT_CRITICAL(&s_req_mux);
            break;
        }
        req = s_req;
        s_req_new = false;
        uint32_t gen = atomic_load(&s_gen);
        portEXIT_CRITICAL(&s_req_mux);
        if (req.mode != MODE_NONE) {
            play(&req, gen);
        } else if (atomic_load(&s_gen) == gen) {
            atomic_store(&s_decoding, false);   /* stopped */
        }
    }
    msw_task_delete_self();
}

/* ---- the reply voice: the local voice, with voice.play in its place ---- */

static const muse_reply_voice_t *s_local;
static atomic_bool s_ext;   /* this message plays voice.play's audio */

static bool rv_start(const char *text)
{
    atomic_store(&s_ext, false);
    return s_local && s_local->start(text);
}

static int rv_read(int16_t *mono, size_t max)
{
    if (!atomic_load(&s_ext)) {
        return s_local ? s_local->read(mono, max) : -1;
    }
    atomic_store(&s_read_us, esp_timer_get_time());
    atomic_store(&s_heard, true);
    size_t n = pcm_get(mono, max);
    if (n) {
        return (int)n;
    }
    if (!live(MODE_VOICE)) {
        atomic_store(&s_ext, false);
        return -1;
    }
    return 0;
}

static uint32_t rv_says(void)
{
    return (s_local ? s_local->says() : 0) + atomic_load(&s_says);
}

static bool rv_say_live(void)
{
    if (live(MODE_VOICE)) {
        atomic_store(&s_ext, true);
        return true;
    }
    atomic_store(&s_ext, false);
    return s_local && s_local->say_live();
}

static void stop_all(void);

static void rv_cancel(void)
{
    if (atomic_load(&s_ext) || atomic_load(&s_mode) == MODE_VOICE) {
        stop_all();
    }
    atomic_store(&s_ext, false);
    if (s_local) {
        s_local->cancel();
    }
}

static const muse_reply_voice_t s_voice = { rv_start, rv_read, rv_says, rv_say_live, rv_cancel };

/* ---- hooks for the speaker, the idle player and the face ---- */

/* muse_audio_write: sound.play's audio over what is played. */
void msw_audio_overlay(int16_t *mono, size_t n)
{
    if (atomic_load(&s_mode) != MODE_SOUND) {
        return;
    }
    static int16_t buf[256];
    int vol = atomic_load(&s_volume);
    while (n) {
        size_t want = n < 256 ? n : 256;
        size_t got = pcm_get(buf, want);
        if (!got) {
            return;
        }
        for (size_t i = 0; i < got; i++) {
            int32_t s = mono[i] + buf[i] * vol / 100;
            mono[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        mono += got;
        n -= got;
    }
}

/* msw_app_sound.c: keep the idle player going while a sound plays. */
bool msw_play_sound_active(void)
{
    return live(MODE_SOUND);
}

/* msw_app_sound.c: a press stopped the idle player: the sound stops too. */
void msw_play_stop(void)
{
    if (atomic_load(&s_mode) != MODE_NONE) {
        stop_all();
    }
}

/* muse_ui.c: the voice's name while voice.play's audio is heard, else NULL. */
const char *msw_voice_credit(void)
{
    return s_cv[0] && atomic_load(&s_heard) && atomic_load(&s_ext) && live(MODE_VOICE) ? s_cv : NULL;
}

/* ---- requests ---- */

static bool buffers(void)
{
    if (s_pcm) {
        return true;
    }
    size_t cap = 0;
    uint8_t *b = muse_hatch_mp3_buffer(&cap);
    if (!b || cap < LIST_CAP + RING_BYTES + WIN_BYTES + PCM_SAMPLES * 2) {
        return false;
    }
    s_list = (char *)b;
    s_ring = b + LIST_CAP;
    s_win = s_ring + RING_BYTES;
    s_pcm = (int16_t *)(s_win + WIN_BYTES);
    return true;
}

/* Hands req to the task (starting it if none runs). */
static const char *submit(const req_t *req)
{
    portENTER_CRITICAL(&s_req_mux);
    s_req = *req;
    s_req_new = true;
    atomic_fetch_add(&s_gen, 1);
    bool start = !s_running;
    s_running = true;
    portEXIT_CRITICAL(&s_req_mux);
    if (start && msw_task_create(task, "msw_play", TASK_STACK, NULL, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_req_mux);
        s_running = false;
        s_req_new = false;
        portEXIT_CRITICAL(&s_req_mux);
        return "out_of_memory";
    }
    return NULL;
}

static void stop_all(void)
{
    req_t none = { .mode = MODE_NONE };
    atomic_store(&s_mode, MODE_NONE);
    pcm_clear();
    portENTER_CRITICAL(&s_req_mux);
    bool running = s_running;
    portEXIT_CRITICAL(&s_req_mux);
    if (running) {
        submit(&none);
    }
}

static cJSON *error(const char *code, const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "code", code);
    cJSON_AddStringToObject(r, "message", message);
    return r;
}

cJSON *msw_play_command(const char *command, const cJSON *params)
{
    bool voice = !strcmp(command, "voice.play");
    if (!voice && strcmp(command, "sound.play")) {
        return NULL;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(params, "stop"))) {
        stop_all();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "stopped", true);
        return r;
    }
    const cJSON *path = cJSON_GetObjectItem(params, "path");
    if (!cJSON_IsString(path) || !path->valuestring[0]) {
        return error("missing_param", "path is required");
    }
    const char *p = path->valuestring + strspn(path->valuestring, "/");
    req_t req = { .mode = voice ? MODE_VOICE : MODE_SOUND, .volume = 100 };
    if (strlen(p) >= sizeof(req.path)) {
        return error("invalid_path", "path is too long");
    }
    strlcpy(req.path, p, sizeof(req.path));
    if (voice) {
        if (!muse_hatch_voice_waiting()) {
            return error("no_turn", "voice.play only works while the user's voice turn waits for your reply");
        }
        const cJSON *cv = cJSON_GetObjectItem(params, "cv");
        if (cJSON_IsString(cv)) {
            strlcpy(req.cv, cv->valuestring, sizeof(req.cv));
        }
    } else {
        const cJSON *vol = cJSON_GetObjectItem(params, "volume");
        if (cJSON_IsNumber(vol)) {
            req.volume = vol->valueint < 0 ? 0 : vol->valueint > 100 ? 100 : vol->valueint;
        }
    }
    if (!buffers()) {
        return error("out_of_memory", "no audio buffer");
    }
    if (voice) {
        const muse_reply_voice_t *cur = muse_reply_voice();
        if (cur != &s_voice) {
            s_local = cur;
            muse_reply_voice_set(&s_voice);
        }
        atomic_fetch_add(&s_says, 1);   /* before the reply arrives: the turn plays this in its place */
    }
    /* The old one stops being heard at once. */
    atomic_store(&s_mode, MODE_NONE);
    pcm_clear();
    const char *err = submit(&req);
    if (err) {
        if (voice) {
            atomic_fetch_sub(&s_says, 1);
        }
        return error(err, "failed to start the player");
    }
    if (voice) {
        /* live() from now: the reply may come before the task starts. */
        atomic_store(&s_decoding, true);
        atomic_store(&s_mode, MODE_VOICE);
    }
    ESP_LOGI(TAG, "%s %s%s%s", command, req.path, req.cv[0] ? " cv " : "", req.cv);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "started", true);
    return r;
}

void msw_console_add(bool (*fn)(const char *line));

/* >msw.play: what plays and the last result. */
static bool msw_play_console(const char *line)
{
    if (strncmp(line, "msw.play", 8)) {
        return false;
    }
    printf("@msw.play mode=%d decoding=%d pcm=%u last=%s\n", atomic_load(&s_mode), atomic_load(&s_decoding),
           (unsigned)pcm_have(), s_last[0] ? s_last : "-");
    return true;
}

__attribute__((constructor)) static void msw_play_register(void)
{
    msw_console_add(msw_play_console);
}
