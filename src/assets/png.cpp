#include "assets/png.h"

#include <cstddef>
#include <fstream>

#include "core/crc32.h"

namespace vette::assets {

// Deflate with LZ77 (hash chains, 32 KB window) and the fixed Huffman codes.
std::vector<uint8_t> deflate_fixed(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int nbits = 0;
    const auto bits = [&](uint32_t v, int count) {
        acc |= v << nbits;
        nbits += count;
        while (nbits >= 8) {
            out.push_back(static_cast<uint8_t>(acc));
            acc >>= 8;
            nbits -= 8;
        }
    };
    const auto huff = [&](uint32_t code, int len) {  // Huffman codes go most significant bit first
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((code >> i) & 1u) << (len - 1 - i);
        bits(r, len);
    };
    const auto lit = [&](int v) {
        if (v < 144) huff(0x30u + static_cast<uint32_t>(v), 8);
        else if (v < 256) huff(0x190u + static_cast<uint32_t>(v - 144), 9);
        else if (v < 280) huff(static_cast<uint32_t>(v - 256), 7);
        else huff(0xC0u + static_cast<uint32_t>(v - 280), 8);
    };
    static constexpr int kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                          33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                          1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    bits(1, 1);  // final block
    bits(1, 2);  // fixed Huffman
    constexpr int kWindow = 32768, kHash = 1 << 15, kChain = 48;
    std::vector<int32_t> head(kHash, -1), prev(kWindow, -1);
    const auto hash3 = [&](size_t i) {
        return static_cast<int>(((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & (kHash - 1));
    };
    size_t i = 0;
    const size_t n = in.size();
    const auto insert = [&](size_t at) {
        if (at + 2 >= n) return;
        const int h = hash3(at);
        prev[at % kWindow] = head[static_cast<size_t>(h)];
        head[static_cast<size_t>(h)] = static_cast<int32_t>(at);
    };
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < n) {
            int cand = head[static_cast<size_t>(hash3(i))];
            for (int chain = 0; chain < kChain && cand >= 0 && i - static_cast<size_t>(cand) <= kWindow - 1; ++chain) {
                const size_t c = static_cast<size_t>(cand);
                int len = 0;
                while (len < 258 && i + static_cast<size_t>(len) < n &&
                       in[c + static_cast<size_t>(len)] == in[i + static_cast<size_t>(len)])
                    ++len;
                if (len > best_len) {
                    best_len = len;
                    best_dist = static_cast<int>(i - c);
                    if (len == 258) break;
                }
                const int32_t p = prev[c % kWindow];
                if (p >= cand) break;
                cand = p;
            }
        }
        if (best_len >= 3) {
            int lc = 28;
            while (kLenBase[lc] > best_len) --lc;
            lit(257 + lc);
            bits(static_cast<uint32_t>(best_len - kLenBase[lc]), kLenExtra[lc]);
            int dc = 29;
            while (kDistBase[dc] > best_dist) --dc;
            huff(static_cast<uint32_t>(dc), 5);
            bits(static_cast<uint32_t>(best_dist - kDistBase[dc]), kDistExtra[dc]);
            for (int k = 0; k < best_len; ++k) insert(i + static_cast<size_t>(k));
            i += static_cast<size_t>(best_len);
        } else {
            lit(in[i]);
            insert(i);
            ++i;
        }
    }
    lit(256);
    if (nbits > 0) out.push_back(static_cast<uint8_t>(acc));
    return out;
}

std::vector<uint8_t> encode_png(int w, int h, const uint32_t* pixels, bool alpha) {
    const size_t channels = alpha ? 4 : 3;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * channels + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < w; ++x) {
            const uint32_t c = pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            raw.push_back(static_cast<uint8_t>(c >> 16));
            raw.push_back(static_cast<uint8_t>(c >> 8));
            raw.push_back(static_cast<uint8_t>(c));
            if (alpha) raw.push_back(static_cast<uint8_t>(c >> 24));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (const uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    const std::vector<uint8_t> d = deflate_fixed(raw);
    z.insert(z.end(), d.begin(), d.end());
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(adler >> s));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto be32 = [&](uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(v >> s));
    };
    const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(static_cast<uint32_t>(data.size()));
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        be32(crc32(std::span<const uint8_t>(png.data() + start, png.size() - start)));
    };
    std::vector<uint8_t> ihdr;
    for (const uint32_t v : {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}) {
        for (int s = 24; s >= 0; s -= 8) ihdr.push_back(static_cast<uint8_t>(v >> s));
    }
    ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(alpha ? 6 : 2), 0, 0, 0});  // 8-bit RGB(A)
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

bool write_png(const std::filesystem::path& path, int w, int h, const std::vector<uint32_t>& pixels, bool alpha) {
    const std::vector<uint8_t> png = encode_png(w, h, pixels.data(), alpha);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}

}  // namespace vette::assets
