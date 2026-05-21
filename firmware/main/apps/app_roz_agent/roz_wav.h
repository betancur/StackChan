/*
 * AppRozAgent — WAV header builder (mono 16-bit PCM, no dependencies).
 */
#pragma once
#include <cstdint>
#include <cstring>

namespace roz {

// Write a 44-byte WAV header into `buf`.
// `buf` must be at least 44 bytes long; PCM data follows immediately after.
inline void write_wav_header(uint8_t* buf, uint32_t sample_rate, uint32_t num_samples)
{
    const uint32_t data_bytes  = num_samples * sizeof(int16_t);
    const uint32_t file_size   = data_bytes + 36;
    const uint32_t byte_rate   = sample_rate * sizeof(int16_t);
    const uint16_t channels    = 1;
    const uint16_t bits        = 16;
    const uint16_t block_align = channels * (bits / 8);
    const uint16_t audio_fmt   = 1;   // PCM
    const uint32_t fmt_chunk   = 16;

    uint8_t* p = buf;
    auto w2 = [&](uint16_t v) { memcpy(p, &v, 2); p += 2; };
    auto w4 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
    auto ws = [&](const char* s) { memcpy(p, s, 4); p += 4; };

    ws("RIFF");  w4(file_size);
    ws("WAVE");
    ws("fmt ");  w4(fmt_chunk); w2(audio_fmt); w2(channels);
    w4(sample_rate); w4(byte_rate); w2(block_align); w2(bits);
    ws("data");  w4(data_bytes);
}

}  // namespace roz
