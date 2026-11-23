#!/usr/bin/env python3
"""Writes src/platform/overlay_cjk.inc, the 12 x 12 bitmaps of the overlay's
wide characters off Windows (platform/overlay.cpp: Windows draws them with
GDI from MS Gothic's 12 px bitmaps, which only Windows has).

    python3 tools/overlay_cjk/gen_overlay_cjk.py [font.pcf.gz]            # regenerate the table
    python3 tools/overlay_cjk/gen_overlay_cjk.py --verify [font.pcf.gz]   # the table matches the font

The font: "Misc Fixed" 12x13ja (-Misc-Fixed-Medium-R-Normal-ja-13-120-75-
75-C-120-ISO10646-1), X.Org's font-misc-misc, "Public domain font.  Share
and enjoy." (its COPYRIGHT property, checked here); Debian / Ubuntu ship it
as /usr/share/fonts/X11/misc/12x13ja.pcf.gz (package xfonts-base), the
default path.  Its cells are 12 x 13 (ascent 11); the CJK glyphs are drawn in
the first 12 rows, which the table keeps; a glyph that reaches the 13th row
(a full-width letter's descender) keeps rows 2 .. 13 when its first is
blank, else its 13th row is dropped (reported).

Taken: every glyph of the font in overlay_font::wide_range's ranges (kana,
CJK punctuation, the CJK ideographs, the full-width forms), and every
character of lang/ja.po in them must be there (the run fails otherwise).

The table is written the same way every time (by code point, no dates): two
runs with the same font give the same bytes."""
import argparse
import gzip
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'src', 'platform', 'overlay_cjk.inc')
JA_PO = os.path.join(ROOT, 'lang', 'ja.po')
DEFAULT_FONT = '/usr/share/fonts/X11/misc/12x13ja.pcf.gz'

PCF_PROPERTIES, PCF_METRICS, PCF_BITMAPS, PCF_BDF_ENCODINGS = 1 << 0, 1 << 2, 1 << 3, 1 << 5


def wide_range(c):
    """overlay_font.cpp's wide_range."""
    return (0x3000 <= c <= 0x30FF or 0x31F0 <= c <= 0x31FF or 0x3400 <= c <= 0x4DBF or
            0x4E00 <= c <= 0x9FFF or 0xFF00 <= c <= 0xFFEF)


def read_pcf(path):
    """The font's properties and glyphs: {code: (width, ascent, descent, rows)},
    each row an int with the left column in the highest of `width' bits."""
    data = gzip.open(path).read() if path.endswith('.gz') else open(path, 'rb').read()
    if data[:4] != b'\x01fcp':
        sys.exit('%s: not a PCF font' % path)
    count = struct.unpack('<I', data[4:8])[0]
    tables = {}
    for i in range(count):
        kind, fmt, size, offset = struct.unpack('<IIII', data[8 + 16 * i:24 + 16 * i])
        tables[kind] = offset

    def head(kind):
        offset = tables[kind]
        fmt = struct.unpack('<I', data[offset:offset + 4])[0]
        return fmt, ('>' if fmt & 4 else '<'), offset + 4

    # properties
    fmt, e, p = head(PCF_PROPERTIES)
    nprops = struct.unpack(e + 'I', data[p:p + 4])[0]
    p += 4
    raw = []
    for _ in range(nprops):
        raw.append(struct.unpack(e + 'IbI', data[p:p + 9]))
        p += 9
    p += (4 - (nprops & 3)) & 3
    strsize = struct.unpack(e + 'I', data[p:p + 4])[0]
    strings = data[p + 4:p + 4 + strsize]

    def string(o):
        return strings[o:strings.index(b'\0', o)].decode('latin-1')

    props = {string(name): (string(value) if is_string else value) for name, is_string, value in raw}

    # metrics
    fmt, e, p = head(PCF_METRICS)
    metrics = []
    if fmt & 0x100:
        n = struct.unpack(e + 'H', data[p:p + 2])[0]
        p += 2
        for _ in range(n):
            metrics.append(tuple(b - 0x80 for b in data[p:p + 5]))
            p += 5
    else:
        n = struct.unpack(e + 'I', data[p:p + 4])[0]
        p += 4
        for _ in range(n):
            metrics.append(struct.unpack(e + 'hhhhhH', data[p:p + 12])[:5])
            p += 12

    # bitmaps
    fmt, e, p = head(PCF_BITMAPS)
    n = struct.unpack(e + 'I', data[p:p + 4])[0]
    p += 4
    offsets = struct.unpack(e + '%dI' % n, data[p:p + 4 * n])
    p += 4 * n
    sizes = struct.unpack(e + '4I', data[p:p + 16])
    p += 16
    pad = 1 << (fmt & 3)
    unit = 1 << ((fmt >> 4) & 3)
    msbit = bool(fmt & 8)
    msbyte = bool(fmt & 4)
    bits = data[p:p + sizes[fmt & 3]]

    # encodings
    fmt, e, p = head(PCF_BDF_ENCODINGS)
    min2, max2, min1, max1, _ = struct.unpack(e + '5H', data[p:p + 10])
    p += 10
    per_row = max2 - min2 + 1
    codes = per_row * (max1 - min1 + 1)
    index = struct.unpack(e + '%dH' % codes, data[p:p + 2 * codes])

    glyphs = {}
    for i, g in enumerate(index):
        if g == 0xFFFF:
            continue
        code = (i // per_row + min1) << 8 | (i % per_row + min2)
        left, right, width, ascent, descent = metrics[g]
        bw = right - left
        stride = ((bw + pad * 8 - 1) // (pad * 8)) * pad if bw > 0 else 0
        rows = []
        for y in range(ascent + descent):
            row = bits[offsets[g] + y * stride:offsets[g] + (y + 1) * stride]
            if unit > 1 and msbyte != msbit:
                row = b''.join(row[k:k + unit][::-1] for k in range(0, len(row), unit))
            v = 0
            for x in range(bw):
                byte = row[x // 8]
                v = v << 1 | (((byte >> (7 - x % 8)) if msbit else (byte >> (x % 8))) & 1)
            rows.append(v << max(0, width - bw - left) if left >= 0 else v)
        glyphs[code] = (width, ascent, descent, rows)
    return props, glyphs


def table(font):
    props, glyphs = read_pcf(font)
    if 'Public domain' not in str(props.get('COPYRIGHT', '')):
        sys.exit('%s: not the public-domain font (COPYRIGHT %r)' % (font, props.get('COPYRIGHT')))
    name = props.get('FONT', '?')
    taken = []
    for code in sorted(glyphs):
        if not wide_range(code):
            continue
        width, ascent, descent, rows = glyphs[code]
        if width != 12 or len(rows) < 12:
            sys.exit('U+%04X: a %d x %d cell, not 12 wide and 12 tall' % (code, width, len(rows)))
        if any(rows[12:]):
            if rows[0] == 0 and not any(rows[13:]):
                rows = rows[1:]
            else:
                print('U+%04X: its rows past the 12th are dropped' % code, file=sys.stderr)
        taken.append((code, rows[:12]))
    have = {c for c, _ in taken}
    with open(JA_PO, encoding='utf-8') as f:
        missing = sorted({ord(ch) for ch in f.read() if wide_range(ord(ch)) and ord(ch) not in have})
    if missing:
        sys.exit('lang/ja.po has characters the font lacks: ' + ' '.join('U+%04X' % c for c in missing))
    out = ['/* overlay_cjk.inc - the overlay\'s wide characters off Windows, for',
           ' * platform/overlay.cpp: %d glyphs of 12 x 12, by code point, each its' % len(taken),
           ' * code then its rows from the top (bit 11 the left column).',
           ' * Generated by tools/overlay_cjk/gen_overlay_cjk.py from the font',
           ' * %s' % name,
           ' * (X.Org font-misc-misc 12x13ja: "Public domain font.  Share and enjoy.");',
           ' * do not edit. */',
           '',
           'static const uint16_t kCjkGlyphs[%d][13] = {' % len(taken)]
    for code, rows in taken:
        out.append('    {0x%04X, %s},' % (code, ', '.join('0x%03X' % r for r in rows)))
    out.append('};')
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('font', nargs='?', default=DEFAULT_FONT)
    ap.add_argument('--verify', action='store_true', help='check the table instead of writing it')
    a = ap.parse_args()
    text = table(a.font)
    if a.verify:
        with open(OUT, encoding='utf-8') as f:
            if f.read() != text:
                sys.exit('%s does not match %s: regenerate it' % (OUT, a.font))
        print('%s matches %s' % (os.path.relpath(OUT, ROOT), a.font))
        return
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('wrote %s (%d bytes)' % (os.path.relpath(OUT, ROOT), len(text)))


if __name__ == '__main__':
    main()
