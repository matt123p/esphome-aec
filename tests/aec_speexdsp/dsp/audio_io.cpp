#include "audio_io.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace audio_io {

namespace {

uint32_t read_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16(const uint8_t *p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

void write_u32(std::ofstream &f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    f.write(reinterpret_cast<const char *>(b), 4);
}

void write_u16(std::ofstream &f, uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    f.write(reinterpret_cast<const char *>(b), 2);
}

}  // namespace

bool read_wav(const std::string &path, std::vector<int16_t> &samples, uint32_t &sample_rate, uint16_t &channels) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    uint8_t header[12];
    f.read(reinterpret_cast<char *>(header), 12);
    if (f.gcount() != 12 || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0)
        return false;

    bool have_fmt = false, have_data = false;
    uint16_t bits = 0;
    uint16_t chans = 1;
    uint32_t rate = 16000;
    std::vector<uint8_t> data;
    while (f && !(have_fmt && have_data)) {
        uint8_t chunk[8];
        f.read(reinterpret_cast<char *>(chunk), 8);
        if (f.gcount() != 8)
            break;
        const uint32_t size = read_u32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmt(size < 16 ? 16 : size);
            f.read(reinterpret_cast<char *>(fmt.data()), size);
            if (f.gcount() != static_cast<std::streamsize>(size))
                return false;
            const uint16_t format = read_u16(fmt.data());
            if (format == 0xFFFE && size >= 40) {
                // extensible: real format lives at offset 24
                if (read_u16(fmt.data() + 24) != 1)
                    return false;
            } else if (format != 1) {
                return false;  // PCM only
            }
            chans = read_u16(fmt.data() + 2);
            rate = read_u32(fmt.data() + 4);
            bits = read_u16(fmt.data() + 14);
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data.resize(size);
            f.read(reinterpret_cast<char *>(data.data()), size);
            if (f.gcount() != static_cast<std::streamsize>(size))
                return false;
            have_data = true;
        } else {
            f.seekg(static_cast<std::streamoff>(size + (size & 1)), std::ios::cur);
        }
    }
    if (!have_fmt || !have_data || bits != 16 || chans < 1)
        return false;
    samples.resize(data.size() / 2);
    std::memcpy(samples.data(), data.data(), samples.size() * 2);
    sample_rate = rate;
    channels = chans;
    return true;
}

bool write_wav(const std::string &path, const int16_t *samples, size_t count, uint32_t sample_rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    const uint32_t data_bytes = static_cast<uint32_t>(count * 2);
    f.write("RIFF", 4);
    write_u32(f, 36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write_u32(f, 16);
    write_u16(f, 1);  // PCM
    write_u16(f, 1);  // mono
    write_u32(f, sample_rate);
    write_u32(f, sample_rate * 2);
    write_u16(f, 2);
    write_u16(f, 16);
    f.write("data", 4);
    write_u32(f, data_bytes);
    f.write(reinterpret_cast<const char *>(samples), static_cast<std::streamsize>(count * 2));
    return static_cast<bool>(f);
}

bool read_f32(const std::string &path, std::vector<float> &values) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff bytes = f.tellg();
    f.seekg(0, std::ios::beg);
    values.resize(static_cast<size_t>(bytes) / 4);
    f.read(reinterpret_cast<char *>(values.data()), bytes);
    return static_cast<size_t>(f.gcount()) == static_cast<size_t>(bytes);
}

}  // namespace audio_io
