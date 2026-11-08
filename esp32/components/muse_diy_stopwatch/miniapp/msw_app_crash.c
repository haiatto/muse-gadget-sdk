/*
 * museStopWatch mini apps: the crash report.
 *
 * When the device panics (a bad pointer, an abort, a watchdog), the panic
 * handler goes through __wrap_esp_panic_handler (CMakeLists.txt wraps it)
 * and this file writes what it can into RTC memory that a restart keeps:
 * the running app, the exception, the backtrace and the last log lines.
 * At the next start msw_crash_boot_report() turns it into app.log lines that
 * Muse can act on: the kind of fault in words, which addresses are the
 * app's (as offsets for addr2line on the app's build/<id>.elf) and which are
 * the firmware's (the nearest exported function), and a hint.
 *
 * The panic side runs with interrupts off and other tasks stopped: no locks,
 * no heap, no printf; only plain copies.
 */
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private/elf_symbol.h"
#include "xtensa_context.h"
#include "msw_app_priv.h"

#define CRASH_MAGIC 0x4D535743u   /* "MSWC" */
#define CRASH_PCS 12
#define CRASH_LOG 4
#define CRASH_LOG_LINE 120

typedef struct {
    uint32_t magic;
    /* set while an app runs */
    char app[MSW_APP_ID_MAX];
    uint32_t code_lo, code_hi, text_vaddr;
    /* set by the panic */
    uint8_t panicked, kind, core, n_pcs;
    uint32_t exccause, excvaddr, pc;
    char reason[96];
    char task[16];
    uint32_t pcs[CRASH_PCS];
    char log[CRASH_LOG][CRASH_LOG_LINE];
    uint32_t check;
} crash_t;

static RTC_NOINIT_ATTR crash_t s_crash;

static uint32_t sum(const crash_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(crash_t, check); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void msw_crash_app(const char *id, uintptr_t code_lo, uintptr_t code_hi, uintptr_t text_vaddr)
{
    memset(&s_crash, 0, sizeof(s_crash));
    if (id) {
        strlcpy(s_crash.app, id, sizeof(s_crash.app));
        s_crash.code_lo = code_lo;
        s_crash.code_hi = code_hi;
        s_crash.text_vaddr = text_vaddr;
    }
    s_crash.magic = CRASH_MAGIC;
    s_crash.check = sum(&s_crash);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (src && (esp_ptr_byte_accessible(src) || esp_ptr_in_drom(src))) {
        for (; i + 1 < cap && src[i]; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/* The app log ring (msw_app_core.c), read without its lock. */
extern int msw_app_log_peek(int back, const char **line);

static void record(const panic_info_t *info)
{
    crash_t *c = &s_crash;
    if (c->magic != CRASH_MAGIC || c->check != sum(c)) {
        memset(c, 0, sizeof(*c));
        c->magic = CRASH_MAGIC;
    }
    c->panicked = 1;
    c->kind = (uint8_t)(g_panic_abort ? PANIC_EXCEPTION_ABORT : info->exception);
    c->core = (uint8_t)info->core;
    c->pc = (uint32_t)info->addr;
    const XtExcFrame *f = info->frame;
    if (f) {
        c->exccause = f->exccause;
        c->excvaddr = f->excvaddr;
    }
    copy_str(c->reason, sizeof(c->reason), g_panic_abort && g_panic_abort_details ? g_panic_abort_details : info->reason);
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
    copy_str(c->task, sizeof(c->task), t ? pcTaskGetName(t) : NULL);
    c->n_pcs = 0;
    if (f) {
        esp_backtrace_frame_t fr = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f };
        c->pcs[c->n_pcs++] = esp_cpu_process_stack_pc(fr.pc);
        while (c->n_pcs < CRASH_PCS && fr.next_pc && esp_backtrace_get_next_frame(&fr)) {
            c->pcs[c->n_pcs++] = esp_cpu_process_stack_pc(fr.pc);
        }
    }
    for (int i = 0; i < CRASH_LOG; i++) {
        const char *line = NULL;
        msw_app_log_peek(CRASH_LOG - 1 - i, &line);
        copy_str(c->log[i], CRASH_LOG_LINE, line);
    }
    c->check = sum(c);
}

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info) {
        record(info);
    }
    __real_esp_panic_handler(info);
}

/* ---- at the next start --------------------------------------------------- */

static const char *cause_name(uint32_t cause)
{
    switch (cause) {
    case 0: return "IllegalInstruction";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 6: return "IntegerDivideByZero";
    case 9: return "LoadStoreAlignment";
    case 12: return "InstrPIFDataError";
    case 13: return "LoadStorePIFDataError";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "exception";
    }
}

static const char *cause_hint(const crash_t *c)
{
    if (c->kind == PANIC_EXCEPTION_IWDT || c->kind == PANIC_EXCEPTION_TWDT) {
        return "a watchdog: something ran too long without returning (a loop that waits?)";
    }
    if (strstr(c->reason, "canary") || strstr(c->reason, "overflow")) {
        return "a task's stack overflowed (an app runs on msw_app, 16 KB): make big local arrays static, avoid deep recursion";
    }
    if (c->kind == PANIC_EXCEPTION_ABORT || c->kind == PANIC_EXCEPTION_DEBUG) {
        return "the firmware stopped itself (abort or assert): see the reason";
    }
    switch (c->exccause) {
    case 0: return "ran bad code: a bad function pointer, or a broken stack (an array written past its end)";
    case 2: case 20: return "called a bad function pointer (NULL, freed, or not set)";
    case 3: return "read bytes from code memory: a pointer into the app's code used as data, or a bad pointer";
    case 6: return "an integer divided by zero";
    case 9: return "an unaligned read or write: a pointer cast to a wider type at an odd address";
    case 28: case 29:
        return c->excvaddr < 0x1000 ? "a NULL pointer (plus a small offset): check pointers that can be NULL (msw_fb, msw_gfx_new, malloc, msw_sprite_from_art)"
               : "a bad pointer: freed memory, out of an array's bounds, or never set";
    default: return "";
    }
}

/* The firmware's function at or below pc, from the app symbol table. */
extern const struct esp_elfsym msw_app_symbols[];

static const char *near_export(uint32_t pc, uint32_t *off)
{
    const char *best = NULL;
    uint32_t best_at = 0;
    for (const struct esp_elfsym *s = msw_app_symbols; s->name; s++) {
        uint32_t a = (uint32_t)(uintptr_t)s->sym;
        if (a <= pc && a > best_at && pc - a < 8 * 1024) {
            best_at = a;
            best = s->name;
        }
    }
    *off = pc - best_at;
    return best;
}

void msw_crash_boot_report(void)
{
    crash_t *c = &s_crash;
    bool valid = c->magic == CRASH_MAGIC && c->check == sum(c);
    esp_reset_reason_t why = esp_reset_reason();
    bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
    if (valid && c->panicked && crash) {
        c->app[sizeof(c->app) - 1] = c->reason[sizeof(c->reason) - 1] = c->task[sizeof(c->task) - 1] = '\0';
        const char *who = c->app[0] ? c->app : "the firmware (no app running)";
        if (c->kind == PANIC_EXCEPTION_FAULT) {
            msw_app_logf("CRASH: %s crashed the device: %s (EXCCAUSE %u) in task %s, core %u",
                         who, cause_name(c->exccause), (unsigned)c->exccause, c->task, c->core);
            msw_app_logf("CRASH: at pc 0x%08x, address 0x%08x", (unsigned)c->pc, (unsigned)c->excvaddr);
        } else {
            msw_app_logf("CRASH: %s crashed the device: %s in task %s, core %u", who,
                         c->kind == PANIC_EXCEPTION_ABORT ? "abort" : c->kind == PANIC_EXCEPTION_IWDT ? "interrupt watchdog"
                         : c->kind == PANIC_EXCEPTION_TWDT ? "task watchdog" : "debug exception", c->task, c->core);
        }
        if (c->reason[0]) {
            msw_app_logf("CRASH: reason: %s", c->reason);
        }
        const char *hint = cause_hint(c);
        if (hint[0]) {
            msw_app_logf("CRASH: hint: %s", hint);
        }
        if (c->app[0] && c->excvaddr >= c->code_lo && c->excvaddr < c->code_hi) {
            msw_app_logf("CRASH: the address is in %s's code, at ELF 0x%x", c->app,
                         (unsigned)(c->text_vaddr + c->excvaddr - c->code_lo));
        }
        msw_app_logf("CRASH: backtrace, newest first (app lines: addr2line -pfiaC -e build/%s.elf <ELF address>):",
                     c->app[0] ? c->app : "<id>");
        for (int i = 0; i < c->n_pcs && i < CRASH_PCS; i++) {
            uint32_t pc = c->pcs[i];
            if (c->app[0] && pc >= c->code_lo && pc < c->code_hi) {
                msw_app_logf("CRASH:  #%d app %s, ELF 0x%x", i, c->app, (unsigned)(c->text_vaddr + pc - c->code_lo));
            } else {
                uint32_t off;
                const char *name = near_export(pc, &off);
                if (name) {
                    msw_app_logf("CRASH:  #%d firmware 0x%08x, in or after %s (+0x%x)", i, (unsigned)pc, name, (unsigned)off);
                } else {
                    msw_app_logf("CRASH:  #%d firmware 0x%08x", i, (unsigned)pc);
                }
            }
        }
        for (int i = 0; i < CRASH_LOG; i++) {
            c->log[i][CRASH_LOG_LINE - 1] = '\0';
            if (c->log[i][0]) {
                msw_app_logf("CRASH: log before it: %s", c->log[i]);
            }
        }
        char sha[17];
        esp_app_get_elf_sha256(sha, sizeof(sha));
        msw_app_logf("CRASH: firmware %s, ELF sha256 %s (end of the report)", esp_app_get_description()->version, sha);
    }
    msw_crash_app(NULL, 0, 0, 0);
}
