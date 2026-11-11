/*
 * museStopWatch mini apps: the package store (2026-11-11, user decision).
 *
 * Packages (apps and avatars) sit in the "msw_pkgs" partition, each in one
 * piece, so the whole partition is mapped once and a package is used where
 * it lies: the ELF is relocated from the mapping and an app's data files
 * (msw_app_data) are read in place, with no copy in PSRAM.
 *
 * Layout, in 4 KB sectors (the flash's erase unit):
 *   sector 0     record 0: the format mark ("MSWP", version); records 1..15:
 *                the move journal (compacting, below)
 *   sector 1..   entries, each from a sector start: a 32-byte entry header,
 *                then the package. Space that holds no entry is erased.
 *
 * An entry header is written first with size 0xFFFFFFFF; the size is
 * written when the whole package is in and checked (commit), and dead is
 * cleared when it is removed. Flash only turns 1s into 0s without an erase,
 * so each of these is one small write. At boot the store finishes what a
 * power cut left: an entry without its size, or a dead one, is erased, and
 * of two entries with one id the newer (seq) stays.
 *
 * Compacting moves entries down into the gaps, so that the free space is in
 * one piece. A move goes sector by sector from the lowest, and each sector
 * done clears a bit in the journal. The target is at least a sector below
 * the source, so a sector's source is still there when it is copied again
 * after a power cut: the boot finishes the move from the journal.
 *
 * Everything here runs on the service task (msw_app_core.c), whose stack is
 * in internal RAM, as flash writes require.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

#include "msw_app_priv.h"

static const char *TAG = "msw_pkgs";

#define SEC 4096u
#define ENTRY_MAGIC 0x4557534Du   /* "MSWE" */
#define JREC_MAGIC 0x4A57534Du    /* "MSWJ" */
#define FORMAT_MAGIC 0x5057534Du  /* "MSWP" */
#define FORMAT_VERSION 1u
#define ERASED 0xFFFFFFFFu
#define MAX_ENTRIES (MSW_APP_MAX_APPS + MSW_AVATAR_MAX + 8)

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t size;      /* package bytes; ERASED until committed */
    uint32_t dead;      /* ERASED while alive */
    uint32_t reserved[4];
} entry_hdr_t;          /* 32 bytes, then the package */

#define JREC_BITS 1792u
typedef struct {
    uint32_t magic;
    uint32_t from, to;  /* entry offsets in the partition */
    uint32_t sectors;
    uint32_t done;      /* ERASED until the move and its clean-up are done */
    uint8_t reserved[0x0C];
    uint8_t progress[JREC_BITS / 8];   /* bit k cleared: sector k copied */
} jrec_t;               /* 256 bytes, 16 in the journal sector */

_Static_assert(sizeof(entry_hdr_t) == 32, "entry header");
_Static_assert(sizeof(jrec_t) == 256, "journal record");

static const esp_partition_t *s_part;
static const uint8_t *s_map;            /* the whole partition, read-only */
static esp_partition_mmap_handle_t s_map_h;
static msw_pkg_entry_t *s_ent;          /* PSRAM, MAX_ENTRIES, by offset */
static int s_n;
static uint32_t s_seq;
static uint8_t *s_buf;                  /* PSRAM, one sector, for moves */

static uint32_t up(uint32_t n)
{
    return (n + SEC - 1) & ~(SEC - 1);
}

static uint32_t entry_bytes(const msw_pkg_entry_t *e)
{
    return up(sizeof(entry_hdr_t) + e->size);
}

static const entry_hdr_t *hdr_at(uint32_t off)
{
    return (const entry_hdr_t *)(s_map + off);
}

static bool erase(uint32_t off, uint32_t n)
{
    esp_err_t e = esp_partition_erase_range(s_part, off, n);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "erase %08x+%u: %s", (unsigned)off, (unsigned)n, esp_err_to_name(e));
    }
    return e == ESP_OK;
}

static bool write(uint32_t off, const void *p, size_t n)
{
    esp_err_t e = esp_partition_write(s_part, off, p, n);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "write %08x+%u: %s", (unsigned)off, (unsigned)n, esp_err_to_name(e));
    }
    return e == ESP_OK;
}

/* Erases n bytes of an entry from its end, so that its header goes last: a
 * cut leaves the header, and the next boot erases the rest again. */
static void erase_back(uint32_t off, uint32_t n)
{
    for (uint32_t end = off + up(n); end > off; end -= SEC) {
        erase(end - SEC, SEC);
    }
}

/* Erases from an uncommitted entry up to the next entry header or the end. */
static uint32_t erase_uncommitted(uint32_t off)
{
    uint32_t end = off + SEC;
    while (end < s_part->size && hdr_at(end)->magic != ENTRY_MAGIC) {
        end += SEC;
    }
    erase_back(off, end - off);
    return end;
}

static void pkg_id(const uint8_t *pkg, char *id, bool *avatar)
{
    const msw_app_pkg_t *h = (const msw_app_pkg_t *)pkg;
    memcpy(id, h->id, MSW_APP_ID_MAX);
    id[MSW_APP_ID_MAX - 1] = '\0';
    *avatar = !memcmp(h->magic, "MSWV", 4);
}

/* ---- the journal ---------------------------------------------------------- */

static const jrec_t *jrec(int i)
{
    return (const jrec_t *)(s_map + (size_t)i * sizeof(jrec_t));
}

static bool bit_done(const jrec_t *j, uint32_t k)
{
    return !(j->progress[k / 8] & (1u << (k % 8)));
}

static bool mark_bit(int i, uint32_t k)
{
    uint8_t b = jrec(i)->progress[k / 8] & (uint8_t)~(1u << (k % 8));
    return write((uint32_t)(i * sizeof(jrec_t) + offsetof(jrec_t, progress) + k / 8), &b, 1);
}

/* Copies the sectors not done yet, erases what is left of the source, and
 * marks the record done. */
static bool finish_move(int i)
{
    const jrec_t *j = jrec(i);
    uint32_t from = j->from, to = j->to, n = j->sectors;
    for (uint32_t k = 0; k < n; k++) {
        if (bit_done(j, k)) {
            continue;
        }
        if (esp_partition_read(s_part, from + k * SEC, s_buf, SEC) != ESP_OK || !erase(to + k * SEC, SEC)
            || !write(to + k * SEC, s_buf, SEC) || !mark_bit(i, k)) {
            return false;
        }
    }
    /* The source's sectors that the copy did not cover. */
    uint32_t end = from + n * SEC, keep = to + n * SEC;
    uint32_t start = from > keep ? from : keep;
    if (end > start) {
        erase_back(start, end - start);
    }
    uint32_t zero = 0;
    return write((uint32_t)(i * sizeof(jrec_t) + offsetof(jrec_t, done)), &zero, 4);
}

static bool write_mark(void)
{
    uint32_t mark[2] = { FORMAT_MAGIC, FORMAT_VERSION };
    return write(0, mark, sizeof(mark));
}

static int free_jrec(void)
{
    for (int i = 1; i < (int)(SEC / sizeof(jrec_t)); i++) {
        if (jrec(i)->magic == ERASED) {
            return i;
        }
    }
    /* Full: every record is done (an unfinished one is finished at boot). */
    return erase(0, SEC) && write_mark() ? 1 : -1;
}

static void resume_journal(void)
{
    for (int i = 1; i < (int)(SEC / sizeof(jrec_t)); i++) {
        const jrec_t *j = jrec(i);
        if (j->magic == JREC_MAGIC && j->done == ERASED) {
            if (j->sectors > JREC_BITS || j->to >= j->from || j->from - j->to < SEC
                || j->from + j->sectors * SEC > s_part->size) {
                ESP_LOGE(TAG, "journal record %d is broken: left as it is", i);
                continue;
            }
            msw_app_logf("finishing a package move cut off at the last restart");
            finish_move(i);
        }
    }
}

/* ---- scanning ------------------------------------------------------------- */

static int by_off(const void *a, const void *b)
{
    uint32_t x = ((const msw_pkg_entry_t *)a)->off, y = ((const msw_pkg_entry_t *)b)->off;
    return x < y ? -1 : x > y;
}

static void kill(uint32_t off, uint32_t size)
{
    uint32_t zero = 0;
    write(off + offsetof(entry_hdr_t, dead), &zero, 4);
    erase_back(off, sizeof(entry_hdr_t) + size);
}

static void scan(void)
{
    s_n = 0;
    uint32_t off = SEC;
    while (off < s_part->size) {
        const entry_hdr_t *h = hdr_at(off);
        if (h->magic == ENTRY_MAGIC) {
            bool fits = h->size != ERASED && h->size >= sizeof(msw_app_pkg_t)
                        && h->size <= s_part->size - off - sizeof(*h);
            if (h->size == ERASED || !fits) {
                ESP_LOGW(TAG, "%08x: an unfinished package: erased", (unsigned)off);
                off = erase_uncommitted(off);
                continue;
            }
            uint32_t next = off + up(sizeof(*h) + h->size);
            const uint8_t *pkg = s_map + off + sizeof(*h);
            const msw_app_pkg_t *p = (const msw_app_pkg_t *)pkg;
            bool ok = h->dead == ERASED && (!memcmp(p->magic, "MSWA", 4) || !memcmp(p->magic, "MSWV", 4))
                      && esp_rom_crc32_le(0, pkg + sizeof(*p), h->size - sizeof(*p)) == p->crc32;
            if (!ok || s_n == MAX_ENTRIES) {
                if (h->dead == ERASED) {
                    ESP_LOGW(TAG, "%08x: a broken package: erased", (unsigned)off);
                }
                kill(off, h->size);
            } else {
                msw_pkg_entry_t *e = &s_ent[s_n++];
                e->off = off;
                e->size = h->size;
                e->seq = h->seq;
                pkg_id(pkg, e->id, &e->avatar);
                s_seq = h->seq > s_seq ? h->seq : s_seq;
            }
            off = next;
        } else if (h->magic == ERASED) {
            off += SEC;
        } else {
            ESP_LOGW(TAG, "%08x: a sector with no package: erased", (unsigned)off);
            erase(off, SEC);
            off += SEC;
        }
    }
    /* Two of one id (a cut during an update): the newer stays. */
    for (int i = 0; i < s_n; i++) {
        for (int k = i + 1; k < s_n; k++) {
            msw_pkg_entry_t *a = &s_ent[i], *b = &s_ent[k];
            if (a->avatar == b->avatar && !strcmp(a->id, b->id)) {
                msw_pkg_entry_t *old = a->seq < b->seq ? a : b;
                kill(old->off, old->size);
                *old = s_ent[--s_n];
                i = -1;   /* again from the start */
                break;
            }
        }
    }
    qsort(s_ent, s_n, sizeof(*s_ent), by_off);
}

/* ---- the public part ------------------------------------------------------ */

bool msw_pkgs_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "msw_pkgs");
    if (!s_part) {
        ESP_LOGE(TAG, "no msw_pkgs partition: mini apps need the partition table of 2026-11-11 (write over USB)");
        return false;
    }
    s_ent = heap_caps_calloc(MAX_ENTRIES, sizeof(*s_ent), MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_malloc(SEC, MALLOC_CAP_SPIRAM);
    const void *p = NULL;
    esp_err_t e = s_ent && s_buf ? esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA, &p, &s_map_h)
                                 : ESP_ERR_NO_MEM;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "map msw_pkgs: %s", esp_err_to_name(e));
        s_part = NULL;
        return false;
    }
    s_map = p;
    int64_t t0 = esp_timer_get_time();
    const uint32_t *mark = (const uint32_t *)s_map;
    if (mark[0] != FORMAT_MAGIC || mark[1] != FORMAT_VERSION) {
        /* New (after the partition table changed, the old FAT volume's bytes
         * are here) or of another version: erase it all, once. */
        msw_app_logf("package store: formatting %u KB (once)", (unsigned)(s_part->size / 1024));
        if (!erase(0, s_part->size) || !write_mark()) {
            s_map = NULL;
            return false;
        }
        ESP_LOGI(TAG, "formatted in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    }
    resume_journal();
    scan();
    uint32_t total, free_b, largest;
    msw_pkgs_space(&total, &free_b, &largest);
    ESP_LOGI(TAG, "%d packages, %u KB free of %u KB (largest gap %u KB), scanned in %d ms", s_n,
             (unsigned)(free_b / 1024), (unsigned)(total / 1024), (unsigned)(largest / 1024),
             (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

bool msw_pkgs_ready(void)
{
    return s_map != NULL;
}

int msw_pkgs_list(msw_pkg_entry_t *out, int max)
{
    int n = s_n < max ? s_n : max;
    memcpy(out, s_ent, (size_t)n * sizeof(*out));
    return n;
}

static msw_pkg_entry_t *find(const char *id, bool avatar)
{
    for (int i = 0; i < s_n; i++) {
        if (s_ent[i].avatar == avatar && !strcmp(s_ent[i].id, id)) {
            return &s_ent[i];
        }
    }
    return NULL;
}

const uint8_t *msw_pkgs_find(const char *id, bool avatar, size_t *n)
{
    msw_pkg_entry_t *e = s_map ? find(id, avatar) : NULL;
    if (!e) {
        return NULL;
    }
    *n = e->size;
    return s_map + e->off + sizeof(entry_hdr_t);
}

int msw_pkgs_count(bool avatar)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) {
        n += s_ent[i].avatar == avatar;
    }
    return n;
}

/* The gap before entry i (i == s_n: the one after the last). */
static void gap(int i, uint32_t *start, uint32_t *len)
{
    uint32_t a = i == 0 ? SEC : s_ent[i - 1].off + entry_bytes(&s_ent[i - 1]);
    uint32_t b = i == s_n ? (uint32_t)s_part->size & ~(SEC - 1) : s_ent[i].off;
    *start = a;
    *len = b > a ? b - a : 0;
}

void msw_pkgs_space(uint32_t *total, uint32_t *free_bytes, uint32_t *largest)
{
    *total = *free_bytes = *largest = 0;
    if (!s_map) {
        return;
    }
    *total = (uint32_t)s_part->size - SEC;
    for (int i = 0; i <= s_n; i++) {
        uint32_t a, len;
        gap(i, &a, &len);
        *free_bytes += len;
        *largest = len > *largest ? len : *largest;
    }
}

bool msw_pkgs_remove(const char *id, bool avatar)
{
    msw_pkg_entry_t *e = s_map ? find(id, avatar) : NULL;
    if (!e) {
        return false;
    }
    kill(e->off, e->size);
    int i = (int)(e - s_ent);
    memmove(&s_ent[i], &s_ent[i + 1], (size_t)(s_n - i - 1) * sizeof(*s_ent));
    s_n--;
    return true;
}

/* Moves entries down into the gaps; the one with pinned_id (an app now
 * running, whose data it reads in place) stays. Returns the bytes moved, or
 * -1 on a flash error. */
int msw_pkgs_compact(const char *pinned_id)
{
    if (!s_map) {
        return -1;
    }
    int64_t t0 = esp_timer_get_time();
    int moved = 0;
    uint32_t cursor = SEC;
    for (int i = 0; i < s_n; i++) {
        msw_pkg_entry_t *e = &s_ent[i];
        uint32_t bytes = entry_bytes(e);
        bool pinned = pinned_id && !e->avatar && !strcmp(e->id, pinned_id);
        if (pinned || e->off == cursor) {
            cursor = e->off + bytes;    /* stays where it is */
            continue;
        }
        /* Down to cursor: below it are only entries already in place, and
         * the next entry starts after this one ends. */
        int j = free_jrec();
        if (j < 0) {
            return -1;
        }
        jrec_t rec;
        memset(&rec, 0xFF, sizeof(rec));
        rec.magic = JREC_MAGIC;
        rec.from = e->off;
        rec.to = cursor;
        rec.sectors = bytes / SEC;
        if (rec.sectors > JREC_BITS
            || !write((uint32_t)(j * sizeof(jrec_t)), &rec, offsetof(jrec_t, done))
            || !finish_move(j)) {
            return -1;
        }
        e->off = cursor;
        cursor += bytes;
        moved += (int)bytes;
    }
    if (moved) {
        msw_app_logf("packages compacted: %u KB moved in %d ms", (unsigned)(moved / 1024),
                     (int)((esp_timer_get_time() - t0) / 1000));
    }
    return moved;
}

/* ---- writing a package ---------------------------------------------------- */

struct msw_pkg_writer {
    uint32_t off;       /* the entry */
    uint32_t room;      /* package bytes that fit */
    uint32_t pos;       /* package bytes written */
    bool known;         /* size given to begin */
    bool overflow;
    bool failed;
};

/* The smallest gap that holds need bytes, or the largest when need is 0. */
static bool pick_gap(uint32_t need, uint32_t *start, uint32_t *len)
{
    bool found = false;
    for (int i = 0; i <= s_n; i++) {
        uint32_t a, l;
        gap(i, &a, &l);
        if (need ? (l >= need && (!found || l < *len)) : (l > (found ? *len : 0))) {
            *start = a;
            *len = l;
            found = true;
        }
    }
    return found;
}

msw_pkg_writer_t *msw_pkgs_begin(size_t size, const char *pinned_id, const char **code, char *msg, size_t msg_len)
{
    *code = "no_space";
    if (!s_map) {
        *code = "no_store";
        snprintf(msg, msg_len, "no msw_pkgs partition: this needs the partition table of 2026-11-11 (write over USB)");
        return NULL;
    }
    uint32_t need = size ? up(sizeof(entry_hdr_t) + (uint32_t)size) : 0;
    uint32_t total, free_b, largest;
    msw_pkgs_space(&total, &free_b, &largest);
    if (need > free_b) {
        snprintf(msg, msg_len, "not enough space: needs %u KB, %u KB free of %u KB", (unsigned)(need / 1024),
                 (unsigned)(free_b / 1024), (unsigned)(total / 1024));
        return NULL;
    }
    uint32_t a = 0, len = 0;
    if (!pick_gap(need, &a, &len) && need) {
        /* Enough free space, but in pieces. */
        if (msw_pkgs_compact(pinned_id) < 0 || !pick_gap(need, &a, &len)) {
            snprintf(msg, msg_len, "%u KB free, but not in one piece around the running app", (unsigned)(free_b / 1024));
            return NULL;
        }
    }
    if (len < up(sizeof(entry_hdr_t) + sizeof(msw_app_pkg_t))) {
        snprintf(msg, msg_len, "no space: %u KB free of %u KB", (unsigned)(free_b / 1024), (unsigned)(total / 1024));
        return NULL;
    }
    msw_pkg_writer_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM);
    entry_hdr_t h;
    memset(&h, 0xFF, sizeof(h));
    h.magic = ENTRY_MAGIC;
    h.seq = s_seq + 1;
    if (!w || !write(a, &h, sizeof(h))) {
        free(w);
        *code = "write_failed";
        snprintf(msg, msg_len, "flash write failed");
        return NULL;
    }
    s_seq++;
    w->off = a;
    w->room = len - sizeof(entry_hdr_t);
    w->known = size != 0;
    return w;
}

bool msw_pkgs_write(msw_pkg_writer_t *w, const void *p, size_t n)
{
    if (w->failed || w->overflow) {
        return false;
    }
    if (n > w->room - w->pos) {
        w->overflow = true;
        return false;
    }
    if (!write(w->off + sizeof(entry_hdr_t) + w->pos, p, n)) {
        w->failed = true;
        return false;
    }
    w->pos += (uint32_t)n;
    return true;
}

bool msw_pkgs_overflowed(const msw_pkg_writer_t *w, size_t *written)
{
    *written = w->pos;
    return w->overflow;
}

const uint8_t *msw_pkgs_written(const msw_pkg_writer_t *w, size_t *n)
{
    *n = w->pos;
    return s_map + w->off + sizeof(entry_hdr_t);
}

void msw_pkgs_abort(msw_pkg_writer_t *w)
{
    if (w) {
        erase_back(w->off, sizeof(entry_hdr_t) + (w->pos ? w->pos : 1));
        free(w);
    }
}

bool msw_pkgs_commit(msw_pkg_writer_t *w)
{
    uint32_t size = w->pos;
    msw_pkg_entry_t e = { .off = w->off, .size = size, .seq = hdr_at(w->off)->seq };
    pkg_id(s_map + w->off + sizeof(entry_hdr_t), e.id, &e.avatar);
    msw_pkg_entry_t *old = find(e.id, e.avatar);
    if (w->failed || w->overflow || (s_n == MAX_ENTRIES && !old) || !write(w->off + offsetof(entry_hdr_t, size), &size, 4)) {
        msw_pkgs_abort(w);
        return false;
    }
    free(w);
    /* The old one with this id goes after the new one is in. */
    if (old) {
        msw_pkgs_remove(e.id, e.avatar);
    }
    s_ent[s_n++] = e;
    qsort(s_ent, s_n, sizeof(*s_ent), by_off);
    return true;
}
