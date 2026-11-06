// The checksum in a DXBC container header (bytes 4..19), written from its public description: MD5 (RFC 1321) over
// the container from byte 20 on, with the end of the message laid out differently from MD5's own padding. The last,
// partial 64-byte block carries the message length in bits as its first dword, then the remaining bytes and 0x80,
// and (bits >> 2) | 1 as its last dword; when the remaining bytes leave no room for that (56 or more), they and 0x80
// fill one block and the two dwords go into one more block of zeros. The four MD5 state words are the checksum.
// tools/check_placeholders.py has the same in Python and checks it against fxc's output.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxbc_checksum {

inline uint32_t Rotl(uint32_t v, unsigned s) { return (v << s) | (v >> (32 - s)); }

inline void Md5Block(uint32_t st[4], const unsigned char* p)
{
    // the sine table of RFC 1321, computed once
    static const struct Table {
        uint32_t k[64];
        Table() { for (int i = 0; i < 64; ++i) k[i] = (uint32_t)std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0); }
    } t;
    static const unsigned shift[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i)
    {
        const int round = i / 16;
        uint32_t f; int g;
        if (round == 0) { f = (b & c) | (~b & d); g = i; }
        else if (round == 1) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (round == 2) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        const uint32_t x = a + f + t.k[i] + m[g];
        a = d; d = c; c = b;
        b = b + Rotl(x, shift[round][i % 4]);
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

inline void Put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

// checksum of a container of size bytes (size >= 20)
inline void Compute(const unsigned char* blob, size_t size, uint32_t out[4])
{
    const unsigned char* data = blob + 20;
    const size_t n = size - 20, full = n & ~(size_t)63, rem = n - full;
    const uint32_t bits = (uint32_t)(n * 8), tail = (bits >> 2) | 1;
    uint32_t st[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    for (size_t o = 0; o < full; o += 64)
        Md5Block(st, data + o);
    unsigned char last[64] = {};
    if (rem >= 56)
    {
        memcpy(last, data + full, rem);
        last[rem] = 0x80;
        Md5Block(st, last);
        memset(last, 0, sizeof(last));
        Put32(last, bits);
    }
    else
    {
        Put32(last, bits);
        memcpy(last + 4, data + full, rem);
        last[4 + rem] = 0x80;
    }
    Put32(last + 60, tail);
    Md5Block(st, last);
    memcpy(out, st, sizeof(st));
}

// writes the checksum into the container header
inline void Update(unsigned char* blob, size_t size)
{
    uint32_t c[4];
    Compute(blob, size, c);
    for (int i = 0; i < 4; ++i)
        Put32(blob + 4 + 4 * i, c[i]);
}

} // namespace dxbc_checksum
