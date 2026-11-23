#!/usr/bin/env python3
"""Writes src/platform/os/eucjp_table.inc, the tables of Windows code page
20932 (EUC-JP) that src/platform/os/eucjp.cpp decodes and encodes with, from
a code page file in the NLS format of Windows (c_20932.nls: Wine ships one,
which it builds to give the Windows conversions; a Bottles prefix has it in
drive_c/windows/syswow64).

    python3 tools/platform_tests/gen_cp20932_table.py <c_20932.nls> > src/platform/os/eucjp_table.inc

Checks, before writing, what eucjp.cpp assumes of the file: a double-byte code
page whose lead bytes are 0x8E and 0xA1..0xFE; every WideCharToMultiByte
entry either the default character or a character that decodes back (no
best fit), and equal to the first code that decodes to it in eucjp.cpp's
order - but for the characters it lists as decoding one way only - (single bytes, then 0x8E pairs, then the lead bytes 0xA1..0xFE, each
with the trail bytes 0xA1..0xFE before 0x21..0x7E)."""
import struct
import sys


def load(path):
    data = open(path, 'rb').read()
    w = struct.unpack('<%dH' % (len(data) // 2), data[:len(data) // 2 * 2])
    hdr = w[0]
    info = {'cp': w[1], 'max': w[2], 'def': w[3], 'unidef': w[4], 'transdef': w[5], 'transuni': w[6]}
    p = hdr
    wc_at = p + w[p] + 1
    p += 1
    mb = w[p:p + 256]
    p += 256
    glyphs = w[p]
    p += 1
    if glyphs:
        p += 256
    ranges = w[p]
    p += 1
    offs = w[p:p + 256]
    base = p
    if ranges == 0:
        raise SystemExit('not a double-byte code page')
    dbcs = {}
    for lead in range(256):
        if offs[lead]:
            dbcs[lead] = w[base + offs[lead]:base + offs[lead] + 256]
    wc = w[wc_at:wc_at + 65536]
    if len(wc) != 65536:
        raise SystemExit('short WideCharToMultiByte table')
    return info, mb, dbcs, wc


def main():
    info, mb, dbcs, wc = load(sys.argv[1])
    if info['cp'] != 20932 or info['max'] != 2:
        raise SystemExit('not code page 20932 (%d, max %d)' % (info['cp'], info['max']))
    leads = sorted(dbcs)
    if leads != [0x8E] + list(range(0xA1, 0xFF)):
        raise SystemExit('unexpected lead bytes: %s' % [hex(x) for x in leads])
    unidef = info['unidef']
    transuni = info['transuni']

    def pair(lead, trail):
        """The character of a pair, None when it has none (the default
        character, unless the pair is the default's own: TransUniDefaultChar)."""
        v = dbcs[lead][trail]
        if v == unidef and ((lead << 8) | trail) != transuni:
            return None
        return v

    def single(b):
        if b in dbcs:
            return None
        v = mb[b]
        if v == unidef and b != info['transuni']:
            return None
        return v

    singles = [single(b) for b in range(256)]
    kana = [pair(0x8E, t) for t in range(0xA1, 0xE0)]
    other_kana = [t for t in list(range(0, 0xA1)) + list(range(0xE0, 0x100)) if pair(0x8E, t) is not None]
    if other_kana or None in kana:
        raise SystemExit('0x8E pairs other than 0xA1..0xDF')
    rows = []
    for lead in range(0xA1, 0xFF):
        hi = [pair(lead, t) for t in range(0xA1, 0xFF)]
        lo = [pair(lead, t) for t in range(0x21, 0x7F)]
        stray = [t for t in list(range(0, 0x21)) + list(range(0x7F, 0xA1)) + [0xFF] if pair(lead, t) is not None]
        if stray:
            raise SystemExit('lead 0x%02X has trail bytes outside 0x21..0x7E, 0xA1..0xFE' % lead)
        rows.append(hi + lo)
    if any(v == 0 for r in rows for v in r if v is not None):
        raise SystemExit('a pair decodes to U+0000')

    # characters that decode but do not encode back (WideCharToMultiByte
    # gives the default character: the C1 controls, 0xA0's and 0xFF's)
    decoded = set(v for v in singles + kana + [x for r in rows for x in r] if v is not None)
    one_way = sorted(v for v in decoded if wc[v] == info['def'] and v != info['def'])

    # the encoder eucjp.cpp builds: the first code in its order
    first = {}

    def offer(code, v):
        if v is not None and v not in first and v not in one_way:
            first[v] = code
    for b in range(256):
        offer(b, singles[b])
    for i, v in enumerate(kana):
        offer(0x8E00 | (0xA1 + i), v)
    for r, row in enumerate(rows):
        for i, v in enumerate(row):
            trail = 0xA1 + i if i < 94 else 0x21 + (i - 94)
            offer(((0xA1 + r) << 8) | trail, v)

    def decode(code):
        if code < 0x100:
            if code in dbcs:
                return None
            return singles[code]
        lead, trail = code >> 8, code & 0xFF
        if lead not in dbcs:
            return None
        return pair(lead, trail)
    for u in range(65536):
        code = wc[u]
        if decode(code) == u:
            want = code
        elif code == info['def'] and u != info['def']:
            want = None                       # the default character: no code for u
        else:
            raise SystemExit('U+%04X: a best fit (0x%X), which eucjp.cpp does not have' % (u, code))
        if first.get(u) != want:
            raise SystemExit('U+%04X: the file encodes 0x%X, the first code is %s' %
                             (u, want or 0, hex(first[u]) if u in first else None))

    out = sys.stdout
    out.write('/* eucjp_table.inc - Windows code page 20932 (EUC-JP), for eucjp.cpp.\n'
              ' * Generated by tools/platform_tests/gen_cp20932_table.py from c_20932.nls;\n'
              ' * do not edit.  0: no character. */\n\n')

    def dump(name, values, kind='uint16_t'):
        out.write('static const %s %s[%d] = {\n' % (kind, name, len(values)))
        for i in range(0, len(values), 12):
            out.write('    ' + ' '.join('0x%04X,' % (v or 0) for v in values[i:i + 12]) + '\n')
        out.write('};\n\n')
    out.write('/* characters that decode but do not encode back: WideCharToMultiByte\n'
              ' * gives the default character for them */\n')
    dump('kOneWay', one_way)
    out.write('/* the single bytes (a lead byte: 0) */\n')
    dump('kSingle', [0 if v is None else v for v in singles])
    out.write('/* 0x8E 0xA1..0xDF: half-width katakana */\n')
    dump('kKana', kana)
    out.write('/* lead 0xA1..0xFE: [lead - 0xA1][trail - 0xA1] for the trail bytes 0xA1..0xFE\n'
              ' * (JIS X 0208, then the user-defined rows), [lead - 0xA1][94 + trail - 0x21]\n'
              ' * for 0x21..0x7E (JIS X 0212 as code page 20932 writes it: its second byte\n'
              ' * without the high bit) */\n')
    out.write('static const uint16_t kDouble[94][188] = {\n')
    for r, row in enumerate(rows):
        out.write('    { /* 0x%02X */\n' % (0xA1 + r))
        for i in range(0, len(row), 12):
            out.write('        ' + ' '.join('0x%04X,' % (v or 0) for v in row[i:i + 12]) + '\n')
        out.write('    },\n')
    out.write('};\n')


if __name__ == '__main__':
    main()
