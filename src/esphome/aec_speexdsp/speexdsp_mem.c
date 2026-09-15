/* Per-memory-type allocation accounting for the vendored SpeexDSP subset
 * used by the aec_speexdsp ESPHome component. The speex_alloc/speex_free
 * overrides in os_support_custom.h report every allocation here so the
 * component can log exactly how much canceller state landed in internal RAM
 * versus PSRAM. */
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <stddef.h>

static size_t internal_bytes = 0;
static size_t psram_bytes = 0;

void speexdsp_mem_note_alloc(void *ptr, size_t size) {
   if (ptr == NULL || size == 0)
      return;
   if (esp_ptr_internal(ptr))
      internal_bytes += size;
   else
      psram_bytes += size;
}

void speexdsp_mem_note_free(void *ptr) {
   if (ptr == NULL)
      return;
   const size_t size = heap_caps_get_allocated_size(ptr);
   if (size == 0)
      return;
   if (esp_ptr_internal(ptr)) {
      if (internal_bytes >= size)
         internal_bytes -= size;
   } else {
      if (psram_bytes >= size)
         psram_bytes -= size;
   }
}

size_t speexdsp_mem_internal_bytes(void) { return internal_bytes; }
size_t speexdsp_mem_psram_bytes(void) { return psram_bytes; }
