// Chronos — tools/WavIo.h
//
// Minimal RIFF/WAV reader + float32 writer shared by the file-based tools.
// Reader parses chunks (skips JUNK etc.), handles PCM 16/24/32 and IEEE float 32,
// and preserves the source channel count (1 or 2; >2 keeps the first two).

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wavio
{

using Vec = std::vector<float>;

inline bool read(const std::string& path, std::vector<Vec>& out, double& sampleRate)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    auto rd32 = [&](uint32_t& v){ return std::fread(&v, 4, 1, f) == 1; };
    auto rd16 = [&](uint16_t& v){ return std::fread(&v, 2, 1, f) == 1; };
    char tag[4]; uint32_t sz;
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4)) { std::fclose(f); return false; }
    rd32(sz);
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4)) { std::fclose(f); return false; }
    uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0; std::vector<uint8_t> data;
    while (std::fread(tag, 1, 4, f) == 4)
    {
        if (!rd32(sz)) break;
        if (!std::memcmp(tag, "fmt ", 4))
        {
            rd16(fmt); rd16(ch); rd32(rate); uint32_t br; rd32(br); uint16_t ba; rd16(ba); rd16(bits);
            for (uint32_t i = 16; i < sz; ++i) std::fgetc(f);
        }
        else if (!std::memcmp(tag, "data", 4))
        {
            // Sample packs sometimes declare more data than the file holds
            // (truncated exports) — accept whatever is actually present.
            data.resize(sz);
            const size_t got = std::fread(data.data(), 1, sz, f);
            data.resize(got);
        }
        else
        {
            for (uint32_t i = 0; i < sz + (sz & 1); ++i) std::fgetc(f);
        }
    }
    std::fclose(f);
    if (ch == 0 || data.empty() || bits == 0) return false;

    sampleRate = rate;
    const int bytes = bits / 8;
    const int frames = static_cast<int>(data.size() / (static_cast<size_t>(bytes) * ch));
    const int outCh = ch >= 2 ? 2 : 1;
    out.assign(static_cast<size_t>(outCh), Vec(static_cast<size_t>(frames)));
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < outCh; ++c)
        {
            const uint8_t* p = data.data() + (static_cast<size_t>(i) * ch + static_cast<size_t>(c)) * bytes;
            float s = 0.0f;
            if (fmt == 3 && bits == 32)      { std::memcpy(&s, p, 4); }
            else if (bits == 16) { int16_t v; std::memcpy(&v, p, 2); s = v / 32768.0f; }
            else if (bits == 24) { int32_t v = (p[0] | (p[1] << 8) | (p[2] << 16)); if (v & 0x800000) v |= ~0xFFFFFF; s = v / 8388608.0f; }
            else if (bits == 32) { int32_t v; std::memcpy(&v, p, 4); s = v / 2147483648.0f; }
            out[static_cast<size_t>(c)][static_cast<size_t>(i)] = s;
        }
    return true;
}

inline void write(const std::string& path, const std::vector<Vec>& planar, double sampleRate)
{
    const int ch = static_cast<int>(planar.size());
    const int n  = ch > 0 ? static_cast<int>(planar[0].size()) : 0;
    const uint32_t dataBytes = static_cast<uint32_t>(n) * static_cast<uint32_t>(ch) * 4u;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }

    auto u32 = [&](uint32_t v){ std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v){ std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(3); u16(static_cast<uint16_t>(ch));
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate) * static_cast<uint32_t>(ch) * 4u);
    u16(static_cast<uint16_t>(ch * 4)); u16(32);
    std::fwrite("data", 1, 4, f); u32(dataBytes);

    std::vector<float> inter(static_cast<size_t>(n) * static_cast<size_t>(ch));
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < ch; ++c)
            inter[static_cast<size_t>(i) * ch + static_cast<size_t>(c)] = planar[static_cast<size_t>(c)][static_cast<size_t>(i)];
    std::fwrite(inter.data(), 4, inter.size(), f);
    std::fclose(f);
}

} // namespace wavio
