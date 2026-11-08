/*
 * museStopWatch: a one-off internal RAM report on the serial log (an
 * investigation aid, 2026-11-07). Prints 30 s and 120 s after boot:
 *   - the internal heap's totals,
 *   - each task, its stack's place (int / psram) and high-water mark,
 *   - with CONFIG_HEAP_TASK_TRACKING, how much each task holds per heap.
 * Allocations are counted against the task that made them (a task's stack
 * against the task that created it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#endif

static const char *TAG = "msw_mem";

static TaskStatus_t *s_st;
static UBaseType_t s_nst;

/* Every used internal block, gathered under the heap lock (no printing there). */
typedef struct {
    void *ptr;
    uint32_t size;
} blk_t;
typedef struct {
    blk_t *b;
    size_t n, cap;
    size_t used, free_sum, free_max;
} walk_t;

static bool walk_cb(walker_heap_into_t heap, walker_block_info_t blk, void *user)
{
    (void)heap;
    walk_t *w = user;
    if (!blk.used) {
        w->free_sum += blk.size;
        w->free_max = blk.size > w->free_max ? blk.size : w->free_max;
        return true;
    }
    w->used += blk.size;
    if (w->n < w->cap) {
        w->b[w->n].ptr = blk.ptr;
        w->b[w->n].size = blk.size;
        w->n++;
    }
    return true;
}

/* A task's stack holds its base; name the block that holds one. */
static const char *stack_of(const blk_t *b)
{
    for (UBaseType_t i = 0; i < s_nst; i++) {
        const uint8_t *base = (const uint8_t *)s_st[i].pxStackBase;
        const uint8_t *p = (const uint8_t *)b->ptr;
        if (base >= p && base < p + b->size) {
            return s_st[i].pcTaskName;
        }
    }
    return NULL;
}

static void walk_internal(void)
{
    walk_t w = { 0 };
    w.cap = 2000;
    w.b = heap_caps_calloc(w.cap, sizeof(blk_t), MALLOC_CAP_SPIRAM);
    if (!w.b) {
        return;
    }
    heap_caps_walk(MALLOC_CAP_INTERNAL, walk_cb, &w);
    /* Size buckets, then each block of 1 KB or more, largest first. */
    size_t bucket[6] = { 0 }, bcount[6] = { 0 };
    static const uint32_t edge[5] = { 64, 256, 1024, 4096, 16384 };
    for (size_t i = 0; i < w.n; i++) {
        int k = 0;
        while (k < 5 && w.b[i].size >= edge[k]) {
            k++;
        }
        bucket[k] += w.b[i].size;
        bcount[k]++;
    }
    printf("msw_mem: internal walk: used %u in %u blocks, free %u (largest %u)\n", (unsigned)w.used,
           (unsigned)w.n, (unsigned)w.free_sum, (unsigned)w.free_max);
    printf("  by size: <64 %u/%u, <256 %u/%u, <1K %u/%u, <4K %u/%u, <16K %u/%u, >=16K %u/%u (bytes/blocks)\n",
           (unsigned)bucket[0], (unsigned)bcount[0], (unsigned)bucket[1], (unsigned)bcount[1], (unsigned)bucket[2],
           (unsigned)bcount[2], (unsigned)bucket[3], (unsigned)bcount[3], (unsigned)bucket[4], (unsigned)bcount[4],
           (unsigned)bucket[5], (unsigned)bcount[5]);
    for (size_t i = 0; i < w.n; i++) {   /* selection by size, only the big ones */
        size_t m = i;
        for (size_t j = i + 1; j < w.n; j++) {
            if (w.b[j].size > w.b[m].size) {
                m = j;
            }
        }
        blk_t t = w.b[i];
        w.b[i] = w.b[m];
        w.b[m] = t;
        if (w.b[i].size < 1024) {
            break;
        }
        const char *st = stack_of(&w.b[i]);
        printf("  blk %p %6u%s%s\n", w.b[i].ptr, (unsigned)w.b[i].size, st ? "  stack of " : "", st ? st : "");
    }
    free(w.b);
}

static void report(void)
{
    multi_heap_info_t in;
    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "internal: free %u, used %u, largest %u, min free %u, blocks %u used / %u free",
             (unsigned)in.total_free_bytes, (unsigned)in.total_allocated_bytes, (unsigned)in.largest_free_block,
             (unsigned)in.minimum_free_bytes, (unsigned)in.allocated_blocks, (unsigned)in.free_blocks);
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *st = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (st) {
        n = uxTaskGetSystemState(st, n, NULL);
        printf("msw_mem: tasks (name, stack place, min free stack bytes, prio)\n");
        for (UBaseType_t i = 0; i < n; i++) {
            const void *base = st[i].pxStackBase;
            printf("  %-16s %-5s %6u  %2u\n", st[i].pcTaskName,
                   esp_ptr_external_ram(base) ? "psram" : esp_ptr_internal(base) ? "int" : "?",
                   (unsigned)st[i].usStackHighWaterMark, (unsigned)st[i].uxCurrentPriority);
        }
        s_st = st;
        s_nst = n;
    }
#endif
#if CONFIG_HEAP_TASK_TRACKING
    /* The same tracking lists the overview table reads (one mutex, no heap walk). */
    enum { MAX_TASKS = 64, MAX_HEAPS = 64 * 8, MAX_ALLOCS = 4000 };
    heap_all_tasks_stat_t a = { 0 };
    a.task_count = MAX_TASKS;
    a.heap_count = MAX_HEAPS;
    a.alloc_count = MAX_ALLOCS;
    a.stat_arr = heap_caps_calloc(MAX_TASKS, sizeof(task_stat_t), MALLOC_CAP_SPIRAM);
    a.heap_stat_start = heap_caps_calloc(MAX_HEAPS, sizeof(heap_stat_t), MALLOC_CAP_SPIRAM);
    a.alloc_stat_start = heap_caps_calloc(MAX_ALLOCS, sizeof(heap_task_block_t), MALLOC_CAP_SPIRAM);
    if (a.stat_arr && a.heap_stat_start && a.alloc_stat_start && heap_caps_get_all_task_stat(&a) == ESP_OK) {
        printf("msw_mem: heap held per task (name, alive, internal now / peak, psram now)\n");
        size_t sum = 0;
        for (size_t i = 0; i < MAX_TASKS && a.stat_arr[i].heap_count; i++) {
            const task_stat_t *t = &a.stat_arr[i];
            size_t in_now = 0, in_peak = 0, ps_now = 0;
            for (size_t h = 0; t->heap_stat && h < t->heap_count; h++) {
                const heap_stat_t *hs = &t->heap_stat[h];
                if (hs->caps & MALLOC_CAP_SPIRAM) {
                    ps_now += hs->current_usage;
                } else {
                    in_now += hs->current_usage;
                    in_peak += hs->peak_usage;
                }
            }
            printf("  %-16s %-5s int %7u / %7u   psram %8u\n", t->name, t->is_alive ? "alive" : "gone",
                   (unsigned)in_now, (unsigned)in_peak, (unsigned)ps_now);
            sum += in_now;
        }
        printf("  internal total held by tracked tasks: %u\n", (unsigned)sum);
        printf("msw_mem: internal blocks >= 1024 bytes (task, size)\n");
        for (size_t i = 0; i < MAX_TASKS && a.stat_arr[i].heap_count; i++) {
            const task_stat_t *t = &a.stat_arr[i];
            for (size_t h = 0; t->heap_stat && h < t->heap_count; h++) {
                const heap_stat_t *hs = &t->heap_stat[h];
                if ((hs->caps & MALLOC_CAP_SPIRAM) || !hs->alloc_stat) {
                    continue;
                }
                for (size_t k = 0; k < hs->alloc_count; k++) {
                    if (hs->alloc_stat[k].size >= 1024) {
                        printf("  %-16s %6u\n", t->name, (unsigned)hs->alloc_stat[k].size);
                    }
                }
            }
        }
    }
    free(a.stat_arr);
    free(a.heap_stat_start);
    free(a.alloc_stat_start);
#endif
    walk_internal();
    free(s_st);
    s_st = NULL;
    s_nst = 0;
    fflush(stdout);
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* CPU use per task over a few seconds: two snapshots of the run-time counters. */
static int cmp_handle(const void *a, const void *b)
{
    const TaskStatus_t *x = a, *y = b;
    return (x->xHandle > y->xHandle) - (x->xHandle < y->xHandle);
}

static void cpu_task(void *arg)
{
    const int secs = (int)(intptr_t)arg;
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    TaskStatus_t *a = heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    TaskStatus_t *b = heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (a && b) {
        configRUN_TIME_COUNTER_TYPE ta, tb;
        UBaseType_t na = uxTaskGetSystemState(a, cap, &ta);
        vTaskDelay(pdMS_TO_TICKS(secs * 1000));
        UBaseType_t nb = uxTaskGetSystemState(b, cap, &tb);
        qsort(a, na, sizeof *a, cmp_handle);
        const double span = (double)(tb - ta);   /* per core */
        printf("msw_cpu: %d s, %% of one core (name, core, prio, cpu%%)\n", secs);
        for (UBaseType_t i = 0; i < nb; i++) {
            TaskStatus_t key = { .xHandle = b[i].xHandle };
            TaskStatus_t *o = bsearch(&key, a, na, sizeof *a, cmp_handle);
            const double used = (double)(b[i].ulRunTimeCounter - (o ? o->ulRunTimeCounter : 0));
            const BaseType_t core = xTaskGetCoreID(b[i].xHandle);
            if (used * 1000 >= span) {   /* 0.1 % or more */
                printf("  %-16s %-3s %2u %5.1f\n", b[i].pcTaskName, core == tskNO_AFFINITY ? "any" : core ? "1" : "0",
                       (unsigned)b[i].uxCurrentPriority, span > 0 ? 100.0 * used / span : 0.0);
            }
        }
    }
    free(a);
    free(b);
    printf("@msw.cpu done\n");
    fflush(stdout);
    vTaskDeleteWithCaps(NULL);
}
#endif

static void mem_task(void *arg)
{
    if (arg) {   /* at boot: 30 s and 120 s */
        vTaskDelay(pdMS_TO_TICKS(30000));
        report();
        vTaskDelay(pdMS_TO_TICKS(90000));
    }
    report();
    printf("@msw.mem done\n");
    fflush(stdout);
    vTaskDeleteWithCaps(NULL);
}

void msw_mem_start(void)
{
    xTaskCreateWithCaps(mem_task, "msw_mem", 4096, (void *)1, 1, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* museStopWatch serial commands, ">msw.<name>" (muse_input.c). Other components
 * add theirs with msw_console_add() (a weak symbol would lose to this archive's). */
#define MORE_MAX 4   /* sanotts and miniapp today; one slot used to let the last one win */
static bool (*s_more[MORE_MAX])(const char *line);
static int s_more_n;

void msw_console_add(bool (*fn)(const char *line))
{
    if (s_more_n < MORE_MAX) {
        s_more[s_more_n++] = fn;
    }
}

bool msw_console(const char *line)
{
    if (!strcmp(line, "msw.mem")) {
        xTaskCreateWithCaps(mem_task, "msw_mem", 4096, NULL, 1, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return true;
    }
    if (!strncmp(line, "msw.caption ", 12)) {
        /* bench: a caption on the face and the app menu's bubble, to check
         * where long text goes (agent.status lines, idea 008) */
        extern void muse_state_set_caption(const char *fmt, ...);
        static char text[160];   /* "\n" in the line: a line break */
        size_t o = 0;
        for (const char *p = line + 12; *p && o < sizeof(text) - 1; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                text[o++] = '\n';
                p++;
            } else {
                text[o++] = *p;
            }
        }
        text[o] = '\0';
        muse_state_set_caption("%s", text);
        printf("@msw.caption ok\n");
        fflush(stdout);
        return true;
    }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (!strncmp(line, "msw.cpu", 7)) {
        int secs = atoi(line + 7);
        secs = secs < 1 ? 3 : secs > 60 ? 60 : secs;
        xTaskCreateWithCaps(cpu_task, "msw_cpu", 4096, (void *)(intptr_t)secs, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return true;
    }
#endif
    for (int i = 0; i < s_more_n; i++) {
        if (s_more[i](line)) {
            return true;
        }
    }
    printf("@msw.error unknown command \"%s\"\n", line);
    fflush(stdout);
    return true;
}
