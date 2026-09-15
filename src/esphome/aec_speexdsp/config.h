/* Build configuration for the vendored SpeexDSP subset used by the
 * aec_speexdsp ESPHome component.
 *
 * Based on ESP32-SpeexDSP (https://github.com/rjsachse/ESP32-SpeexDSP),
 * which packages SpeexDSP (https://github.com/xiph/speexdsp, BSD license).
 *
 * Floating point is used, matching the upstream ESP32 port. All ESP32
 * variants with an FPU (ESP32, ESP32-S2/S3, ESP32-P4) run this efficiently;
 * RISC-V variants (ESP32-C3/C5/C6) lack an FPU and will spend significant
 * CPU in soft-float.
 */
#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_CONFIG_H 1        /* Enable config.h inclusion */
#define OS_SUPPORT_CUSTOM 1    /* Enable os_support_custom.h */
#define FLOATING_POINT 1       /* Use floating-point arithmetic */
#define USE_ESP_DSP_FFT 1      /* SIMD-accelerated ESP-DSP real FFT */
/* The per-bin Ephraim-Malah pass is dominated by scalar sqrt/exp/divide.
 * Interpolate the already-computed Bark-band gains instead: this removes the
 * measured preprocessing hotspot and retains frequency-shaped suppression. */
#define SPEEX_PREPROCESS_LINEAR_GAIN 0
#define EXPORT                 /* Empty EXPORT for no DLL exports */

#define ESP_PLATFORM 1         /* ESP-IDF: ESP_LOG/heap_caps in the allocator */

/* Allocation policy lives in os_support_custom.h: internal RAM first with a
 * PSRAM fallback, so the hot FFT state stays in fast memory whenever it
 * fits. */


#endif /* CONFIG_H */
