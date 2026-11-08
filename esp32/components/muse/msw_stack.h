/*
 * museStopWatch: task stacks in PSRAM, to leave internal RAM for what must be
 * there (DMA, Wi-Fi, and later the TTS arena).
 *
 * With code and read-only data in PSRAM (CONFIG_SPIRAM_FETCH_INSTRUCTIONS and
 * CONFIG_SPIRAM_RODATA) flash writes leave the cache on
 * (SPI_FLASH_CACHE_NO_DISABLE in esp-idf spi_flash_os_func_app.c), so a task
 * with a PSRAM stack may write NVS (main/helper_task.h says the same).
 * Elsewhere these fall back to internal RAM.
 *
 * Not for a task that maps flash (esp_partition_mmap, and OTA state reads
 * such as esp_ota_get_state_partition): mapping freezes the cache and IDF
 * asserts the caller's stack is internal (esp_cache_utils.c).
 *
 * A task made with msw_task_create() ends with msw_task_delete_self(): a plain
 * vTaskDelete() would leak a WithCaps task's stack and TCB.
 */
#pragma once

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
#define MSW_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define MSW_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

static inline BaseType_t msw_task_create(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                         UBaseType_t prio, TaskHandle_t *out)
{
    return xTaskCreateWithCaps(fn, name, stack_bytes, arg, prio, out, MSW_STACK_CAPS);
}

static inline BaseType_t msw_task_create_pinned(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                                void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, prio, out, core, MSW_STACK_CAPS);
}

static inline void msw_task_delete_self(void)
{
    vTaskDeleteWithCaps(NULL);
}
