/* eucjp.h - EUC-JP as Windows' code page 20932 converts it
 * (MultiByteToWideChar / WideCharToMultiByte), for the game's EUC-JP strings
 * (platform/param_db.cpp) off Windows.
 *
 * Code page 20932 is a double-byte code page:
 *   - 0x00..0x7F ASCII; 0x80..0x9F (but 0x8E) the C1 controls, 0xA0 U+F8F0,
 *     0xFF U+F8F3 - which decode, but do not encode back ('?');
 *   - 0x8E + 0xA1..0xDF the half-width katakana;
 *   - 0xA1..0xFE + 0xA1..0xFE JIS X 0208 (its standard mapping: 0xA1C1 the
 *     WAVE DASH U+301C, 0xA1DD the MINUS SIGN U+2212), the rows 0xF5..0xFE
 *     the private use area; NEC's row 13 (0xADA1 ...) has no character;
 *   - JIS X 0212 as two bytes, 0xA1..0xFE + 0x21..0x7E (its second byte
 *     without the high bit; 0xA237 TILDE decodes to U+007E, which encodes as
 *     0x7E).
 * 0x8F is no lead byte there: "0x8F 0xA2 0xB7", the three bytes EUC-JP has for
 * JIS X 0212, decode as U+008F and the JIS X 0208 pair 0xA2B7 - which has no
 * character, so the bytes are not valid.  glibc's iconv "EUC-JP" reads them
 * as JIS X 0212 instead; that is a difference, not this code's.
 *
 * The tables: eucjp_table.inc, generated from the code page's NLS file
 * (tools/platform_tests/gen_cp20932_table.py). */
#pragma once

#include <stddef.h>

#include <string>

namespace eucjp {

/* The bytes as code page 20932 decodes them with MB_ERR_INVALID_CHARS: false
 * (nothing in *out) when a byte or a pair has no character or a lead byte
 * ends the bytes. */
bool decode(const char *bytes, size_t n, std::u32string *out);

/* One character's bytes (1 or 2) into out: 0 when code page 20932 has none
 * for it (WideCharToMultiByte with WC_NO_BEST_FIT_CHARS: the default
 * character '?' and "used default" then). */
int encode_char(char32_t c, char out[2]);

/* The text in code page 20932, '?' for a character it lacks (*lossy then). */
std::string encode(const std::u32string &text, bool *lossy);

} // namespace eucjp
