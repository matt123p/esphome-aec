/* PC shim for ESP-IDF's esp_cpu.h (C-compatible: included from vendored .c
 * files). preprocess.c, fftwrap.c and beamformer.cpp use
 * esp_cpu_get_cycle_count() for profiling only; on the PC this maps to a
 * monotonic nanosecond counter (wrapping at 32 bits like the ESP register). */
#ifndef ESP_CPU_SHIM_H
#define ESP_CPU_SHIM_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t esp_cpu_get_cycle_count(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

#ifdef __cplusplus
}
#endif

#endif /* ESP_CPU_SHIM_H */
