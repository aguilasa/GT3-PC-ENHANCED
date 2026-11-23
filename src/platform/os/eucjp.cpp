/* eucjp.cpp - EUC-JP as Windows' code page 20932 converts it (eucjp.h). */
#include "platform/os/eucjp.h"

#include <stdint.h>

#include <mutex>
#include <vector>

namespace eucjp {
namespace {

#include "platform/os/eucjp_table.inc"

bool lead(uint8_t b)
{
    return b == 0x8E || (b >= 0xA1 && b <= 0xFE);
}

/* The character of a pair, 0 when it has none. */
char32_t pair(uint8_t l, uint8_t t)
{
    if (l == 0x8E)
        return t >= 0xA1 && t <= 0xDF ? kKana[t - 0xA1] : 0;
    if (t >= 0xA1 && t <= 0xFE)
        return kDouble[l - 0xA1][t - 0xA1];
    if (t >= 0x21 && t <= 0x7E)
        return kDouble[l - 0xA1][94 + t - 0x21];
    return 0;
}

/* U+0000..U+FFFF -> its code (a byte, or lead << 8 | trail); 0xFFFF none.
 * The first code that decodes to the character, in the table's order (the
 * single bytes, the 0x8E pairs, then by lead and trail, 0xA1..0xFE before
 * 0x21..0x7E): what WideCharToMultiByte gives (the generator checked it), but
 * for the characters that decode one way only. */
std::vector<uint16_t> g_encode;
std::once_flag g_encodeOnce;

void build_encode()
{
    g_encode.assign(0x10000, 0xFFFF);
    auto offer = [](uint16_t code, char32_t c) {
        if (c < 0x10000 && g_encode[c] == 0xFFFF)
            g_encode[c] = code;
    };
    for (int b = 0; b < 0x100; b++)
        if (!lead((uint8_t)b))
            offer((uint16_t)b, kSingle[b]);
    for (int t = 0xA1; t <= 0xDF; t++)
        offer((uint16_t)(0x8E00 | t), kKana[t - 0xA1]);
    for (int l = 0xA1; l <= 0xFE; l++) {
        for (int t = 0xA1; t <= 0xFE; t++)
            if (char32_t c = kDouble[l - 0xA1][t - 0xA1])
                offer((uint16_t)(l << 8 | t), c);
        for (int t = 0x21; t <= 0x7E; t++)
            if (char32_t c = kDouble[l - 0xA1][94 + t - 0x21])
                offer((uint16_t)(l << 8 | t), c);
    }
    for (uint16_t c : kOneWay)
        g_encode[c] = 0xFFFF;
}

} // namespace

bool decode(const char *bytes, size_t n, std::u32string *out)
{
    out->clear();
    std::u32string s;
    s.reserve(n);
    const uint8_t *p = (const uint8_t *)bytes;
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i];
        if (!lead(b)) {
            s.push_back(kSingle[b]);
            i++;
            continue;
        }
        if (i + 1 >= n)
            return false;                   /* (a lead byte at the end) */
        const char32_t c = pair(b, p[i + 1]);
        if (c == 0)
            return false;
        s.push_back(c);
        i += 2;
    }
    out->swap(s);
    return true;
}

int encode_char(char32_t c, char out[2])
{
    std::call_once(g_encodeOnce, build_encode);
    if (c >= 0x10000)
        return 0;
    const uint16_t code = g_encode[c];
    if (code == 0xFFFF)
        return 0;
    if (code < 0x100) {
        out[0] = (char)code;
        return 1;
    }
    out[0] = (char)(code >> 8);
    out[1] = (char)(code & 0xFF);
    return 2;
}

std::string encode(const std::u32string &text, bool *lossy)
{
    std::string s;
    s.reserve(text.size() * 2);
    *lossy = false;
    for (char32_t c : text) {
        char b[2];
        const int k = encode_char(c, b);
        if (k == 0) {
            s += '?';
            *lossy = true;
        } else {
            s.append(b, (size_t)k);
        }
    }
    return s;
}

} // namespace eucjp
