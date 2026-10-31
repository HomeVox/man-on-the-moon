/*
 * Kantoor - LVGL-geheugen uit PSRAM
 *
 * Met vier pagina's tegelijk passen alle LVGL-objecten, stijlen en lettertype-caches
 * niet meer in het interne RAM; dat is nodig voor wifi, DMA van het scherm en de
 * tekenbuffers. LVGL krijgt daarom geheugen uit PSRAM (met intern RAM als reserve).
 * Actief via CONFIG_LV_USE_CUSTOM_MALLOC.
 */
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void* lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

void* lv_realloc_core(void* p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

void lv_free_core(void* p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p)
{
    memset(mon_p, 0, sizeof(lv_mem_monitor_t));
    mon_p->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif
