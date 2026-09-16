/* Tiny PCM16 WAV reader/writer for the test suite. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audio_io {

// Reads a PCM16 WAV (mono or stereo). Returns interleaved samples; false on error.
bool read_wav(const std::string &path, std::vector<int16_t> &samples, uint32_t &sample_rate, uint16_t &channels);

// Writes a mono PCM16 WAV.
bool write_wav(const std::string &path, const int16_t *samples, size_t count, uint32_t sample_rate);

// Reads a raw float32 file (ground-truth label tracks from the generator).
bool read_f32(const std::string &path, std::vector<float> &values);

}  // namespace audio_io
