/* ESP-IDF allocator for the vendored SpeexDSP subset used by the
 * aec_speexdsp ESPHome component.
 *
 * Based on os_support_custom.h from ESP32-SpeexDSP
 * (https://github.com/rjsachse/ESP32-SpeexDSP). Changes from upstream:
 *  - allocations prefer INTERNAL RAM and fall back to PSRAM: the canceller's
 *    FFT/spectral state is a small, hot working set that pays a large
 *    bandwidth/cache penalty when resident in PSRAM; oversized filters that
 *    do not fit internal RAM still work via the fallback (they run slower);
 *  - every allocation is accounted per memory type; totals are read through
 *    speexdsp_mem_internal_bytes() / speexdsp_mem_psram_bytes() (implemented
 *    in speexdsp_mem.c) and logged by the component at startup;
 *  - the fatal handler logs through ESP_LOG and restarts.
 */
#ifndef OS_SUPPORT_CUSTOM_H
#define OS_SUPPORT_CUSTOM_H

#include "config.h"
#include <string.h>  // For memcpy, memmove, memset
#include <stdio.h>   // For fprintf (fallback)
#include <stdlib.h>  // For malloc, calloc, realloc, free (fallback)
#include <stddef.h>  // For size_t

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>     // For heap_caps_malloc, heap_caps_free, etc.
#include <esp_memory_utils.h>  // For esp_ptr_internal
#include <esp_log.h>           // For ESP_LOG macros
#include <esp_system.h>        // For esp_restart
#include <freertos/FreeRTOS.h> // For pvPortMalloc, vPortFree
#include <freertos/portable.h> // For FreeRTOS portability
#define SPEEXDSP_LOG_TAG "speexdsp"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation accounting (shared state, implemented in speexdsp_mem.c). */
void speexdsp_mem_note_alloc(void *ptr, size_t size);
void speexdsp_mem_note_free(void *ptr);
size_t speexdsp_mem_internal_bytes(void);
size_t speexdsp_mem_psram_bytes(void);

// Core memory allocation functions
#define OVERRIDE_SPEEX_ALLOC
static inline void *speex_alloc(int size) {
   if (size <= 0)
      return NULL;
   void *ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   if (ptr == NULL)
      ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (ptr) {
      memset(ptr, 0, size);
      speexdsp_mem_note_alloc(ptr, (size_t) size);
   }
   return ptr;
}

#define OVERRIDE_SPEEX_ALLOC_SCRATCH
static inline void *speex_alloc_scratch(int size) {
   if (size <= 0)
      return NULL;
   void *ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   if (ptr == NULL)
      ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (ptr)
      speexdsp_mem_note_alloc(ptr, (size_t) size);
   return ptr;
}

#define OVERRIDE_SPEEX_FREE
static inline void speex_free(void *ptr) {
   if (!ptr)
      return;
   speexdsp_mem_note_free(ptr);
   heap_caps_free(ptr);
}

#define OVERRIDE_SPEEX_FREE_SCRATCH
static inline void speex_free_scratch(void *ptr) {
   if (!ptr)
      return;
   speexdsp_mem_note_free(ptr);
   heap_caps_free(ptr);
}

#define OVERRIDE_SPEEX_REALLOC
static inline void *speex_realloc(void *ptr, int size) {
   if (size <= 0) {
      speex_free(ptr);
      return NULL;
   }
   speexdsp_mem_note_free(ptr);
   void *new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
   if (new_ptr == NULL)
      new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (new_ptr)
      speexdsp_mem_note_alloc(new_ptr, (size_t) size);
   return new_ptr;
}

// Memory operations
#define OVERRIDE_SPEEX_COPY
#define SPEEX_COPY(dst, src, n) (memcpy((dst), (src), (n) * sizeof(*(dst)) + 0 * ((dst) - (src))))

#define OVERRIDE_SPEEX_MOVE
#define SPEEX_MOVE(dst, src, n) (memmove((dst), (src), (n) * sizeof(*(dst)) + 0 * ((dst) - (src))))

#define OVERRIDE_SPEEX_MEMSET
#define SPEEX_MEMSET(dst, c, n) (memset((dst), (c), (n) * sizeof(*(dst))))

// Error handling and logging
#ifdef ESP_PLATFORM
#define OVERRIDE_SPEEX_FATAL
static inline void _speex_fatal(const char *str, const char *file, int line) {
   ESP_LOGE(SPEEXDSP_LOG_TAG, "Fatal error in %s, line %d: %s", file, line, str);
   esp_restart();
}

#define OVERRIDE_SPEEX_WARNING
static inline void speex_warning(const char *str) {
#ifndef DISABLE_WARNINGS
   ESP_LOGW(SPEEXDSP_LOG_TAG, "%s", str);
#endif
}

#define OVERRIDE_SPEEX_WARNING_INT
static inline void speex_warning_int(const char *str, int val) {
#ifndef DISABLE_WARNINGS
   ESP_LOGW(SPEEXDSP_LOG_TAG, "%s %d", str, val);
#endif
}

#define OVERRIDE_SPEEX_NOTIFY
static inline void speex_notify(const char *str) {
#ifndef DISABLE_NOTIFICATIONS
   ESP_LOGI(SPEEXDSP_LOG_TAG, "%s", str);
#endif
}
#else
#define OVERRIDE_SPEEX_FATAL
static inline void _speex_fatal(const char *str, const char *file, int line) {
   fprintf(stderr, "Fatal error in %s, line %d: %s\n", file, line, str);
   exit(1);
}

#define OVERRIDE_SPEEX_WARNING
static inline void speex_warning(const char *str) {
#ifndef DISABLE_WARNINGS
   fprintf(stderr, "warning: %s\n", str);
#endif
}

#define OVERRIDE_SPEEX_WARNING_INT
static inline void speex_warning_int(const char *str, int val) {
#ifndef DISABLE_WARNINGS
   fprintf(stderr, "warning: %s %d\n", str, val);
#endif
}

#define OVERRIDE_SPEEX_NOTIFY
static inline void speex_notify(const char *str) {
#ifndef DISABLE_NOTIFICATIONS
   fprintf(stderr, "notification: %s\n", str);
#endif
}
#endif

#define OVERRIDE_SPEEX_PUTC
static inline void _speex_putc(int ch, void *file) {
#ifdef ESP_PLATFORM
   ESP_LOGI(SPEEXDSP_LOG_TAG, "%c", (char) ch);  // Log to ESP32 console
#else
   FILE *f = (FILE *) file;
   fprintf(f, "%c", ch);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* OS_SUPPORT_CUSTOM_H */
