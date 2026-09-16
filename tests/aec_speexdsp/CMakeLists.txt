# PC (Linux/WSL/CI) test suite for the aec_speexdsp component.
#
# The vendored SpeexDSP subset in src/esphome/aec_speexdsp is written for
# ESP-IDF (esp_cpu cycle counter, heap_caps allocator, ESP-DSP FFT). For the
# PC port the C sources are copied into the build directory and compiled with
# a generated config.h that selects the vendored Kiss FFT backend instead of
# the ESP-DSP one, plus small shims:
#   - esp_cpu.h   : esp_cpu_get_cycle_count() via C++ <chrono>
#   - os_support  : the existing non-ESP branch of os_support_custom.h with
#                   allocation accounting implemented in shim/pc_mem.cpp
# The component's beamformer.cpp compiles unmodified against the esp_cpu shim.
#
# Two builds are driven by run_tests.sh:
#   default (Release, -O2)      -> functional correctness
#   -DSANITIZE=ON (ASan+UBSan)  -> memory & undefined-behaviour correctness

cmake_minimum_required(VERSION 3.16)
project(aec_speexdsp_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(COMPONENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/esphome/aec_speexdsp")
set(SPEEX_PC_DIR "${CMAKE_BINARY_DIR}/speexdsp_pc")
set(SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shim")

option(SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
set(ESP_DSP_SOURCE_DIR "" CACHE PATH "Optional ESP-DSP 1.8.2 checkout: test the real wrapper with upstream ANSI kernels")

set(SPEEXDSP_C_SOURCES
    mdf.c
    preprocess.c
    fftwrap.c
    filterbank.c
    kiss_fft.c
    kiss_fftr.c
)
set(SPEEXDSP_HEADERS
    arch.h
    fftwrap.h
    filterbank.h
    kiss_fft.h
    kiss_fft_guts.h
    kiss_fftr.h
    math_approx.h
    os_support.h
    pseudofloat.h
    speex_echo.h
    speex_preprocess.h
    speexdsp_config_types.h
    speexdsp_types.h
)
foreach(FILE ${SPEEXDSP_C_SOURCES} ${SPEEXDSP_HEADERS})
    configure_file("${COMPONENT_DIR}/${FILE}" "${SPEEX_PC_DIR}/${FILE}" COPYONLY)
endforeach()
# PC build configuration: Kiss FFT backend, no ESP-DSP, no ESP platform glue.
configure_file("${SHIM_DIR}/config_pc.h.in" "${SPEEX_PC_DIR}/config.h" @ONLY)
# PC variant of the allocator (libc + accounting hooks, same OVERRIDE macros).
configure_file("${SHIM_DIR}/os_support_custom_pc.h.in" "${SPEEX_PC_DIR}/os_support_custom.h" @ONLY)

add_library(speexdsp_pc STATIC
    "${SPEEX_PC_DIR}/mdf.c"
    "${SPEEX_PC_DIR}/preprocess.c"
    "${SPEEX_PC_DIR}/fftwrap.c"
    "${SPEEX_PC_DIR}/filterbank.c"
    "${SPEEX_PC_DIR}/kiss_fft.c"
    "${SPEEX_PC_DIR}/kiss_fftr.c"
)
target_include_directories(speexdsp_pc PUBLIC ${SPEEX_PC_DIR} ${SHIM_DIR})
target_link_libraries(speexdsp_pc PUBLIC m)
if(ESP_DSP_SOURCE_DIR)
    target_compile_definitions(speexdsp_pc PUBLIC TEST_ESP_DSP=1)
    target_include_directories(speexdsp_pc PUBLIC
        "${SHIM_DIR}/esp_dsp"
        "${ESP_DSP_SOURCE_DIR}/modules/fft/include"
        "${ESP_DSP_SOURCE_DIR}/modules/common/include")
    target_sources(speexdsp_pc PRIVATE
        "${SHIM_DIR}/esp_dsp/common.c"
        "${ESP_DSP_SOURCE_DIR}/modules/fft/float/dsps_fft2r_fc32_ansi.c"
        "${ESP_DSP_SOURCE_DIR}/modules/fft/float/dsps_fft4r_fc32_ansi.c"
        "${ESP_DSP_SOURCE_DIR}/modules/fft/float/dsps_fft2r_bitrev_tables_fc32.c"
        "${ESP_DSP_SOURCE_DIR}/modules/fft/float/dsps_fft4r_bitrev_tables_fc32.c")
endif()
if(SANITIZE)
    target_compile_options(speexdsp_pc PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(speexdsp_pc PRIVATE -fsanitize=address,undefined)
else()
    target_compile_options(speexdsp_pc PRIVATE -O2 -g)
endif()

# The vendored Kiss FFT sources silence strict-aliasing/UB complaints poorly
# under UBSan when optimising; they are exercised as-is (same as on ESP32).
set_source_files_properties(
    "${SPEEX_PC_DIR}/kiss_fft.c" "${SPEEX_PC_DIR}/kiss_fftr.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-strict-aliasing")

# Component beamformer, compiled in place against the esp_cpu shim.
add_library(beamformer_pc STATIC "${COMPONENT_DIR}/beamformer.cpp")
target_include_directories(beamformer_pc PUBLIC ${COMPONENT_DIR} ${SHIM_DIR})
if(SANITIZE)
    target_compile_options(beamformer_pc PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(beamformer_pc PRIVATE -fsanitize=address,undefined)
else()
    target_compile_options(beamformer_pc PRIVATE -O2 -g)
endif()

# Allocation accounting / leak tracking hooked into the speex allocator.
add_library(pc_mem STATIC ${SHIM_DIR}/pc_mem.cpp)
target_include_directories(pc_mem PUBLIC ${SHIM_DIR})
if(SANITIZE)
    target_compile_options(pc_mem PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(pc_mem PRIVATE -fsanitize=address,undefined)
endif()

add_library(test_core STATIC
    dsp/test_framework.cpp
    dsp/audio_io.cpp
    dsp/json.cpp
    dsp/dsp_pipeline.cpp
)
target_include_directories(test_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/dsp ${SHIM_DIR})
target_link_libraries(test_core PUBLIC speexdsp_pc beamformer_pc pc_mem)
if(SANITIZE)
    target_compile_options(test_core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(test_core PRIVATE -fsanitize=address,undefined)
else()
    target_compile_options(test_core PRIVATE -O2 -g)
endif()

add_executable(aec_dsp_tests
    tests/main.cpp
    tests/test_fft.cpp
    tests/test_aec.cpp
    tests/test_noise_suppression.cpp
    tests/test_agc.cpp
    tests/test_vad.cpp
    tests/test_beamformer.cpp
    tests/test_pipeline.cpp
    tests/test_robustness.cpp
    tests/test_memory.cpp
)
target_link_libraries(aec_dsp_tests PRIVATE test_core)
if(SANITIZE)
    target_compile_options(aec_dsp_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(aec_dsp_tests PRIVATE -fsanitize=address,undefined)
else()
    target_compile_options(aec_dsp_tests PRIVATE -O2 -g)
endif()
