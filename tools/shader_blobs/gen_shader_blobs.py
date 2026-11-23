#!/usr/bin/env python3
"""Writes src/engine/pgl/native/shader_blobs.h, the compiled DXBC of every
shader the game compiles at run time on Windows, for the builds that have no
D3DCompile (Linux, DXVK-native): there compile() in pgl_d3d.cpp and gpu.cpp
looks the blob up by the same identity it would compile it with.

    python3 tools/shader_blobs/gen_shader_blobs.py            # regenerate the header
    python3 tools/shader_blobs/gen_shader_blobs.py --verify   # header matches the sources; D3DReflect each blob

What is compiled, read from the sources (never listed here):
  - pgl_d3d.cpp: the HLSL of kHlsl, each compile("SEC_..", entry, target, ..)
    call and each row of d3d_init's pixel shader table (section, entry,
    target, up to three variant macros), with the source name its
    D3DCompile call gives;
  - overlay.cpp: kShader, its gpu::compile_vs / compile_ps calls (name,
    entry), with the targets gpu.cpp's compile_vs / compile_ps pass.

The compiler is D3DCompile of Microsoft's d3dcompiler_47.dll - the one the
game calls on Windows - with the same source bytes, name, macros (each "1",
section first) and flags (D3DCOMPILE_OPTIMIZATION_LEVEL3).  It runs in a Wine
prefix through tools/shader_blobs/shaderc.c, built here with clang-cl and the
xwin CRT/SDK; the prefix's d3dcompiler_47 must be Microsoft's (installed as a
native DLL, e.g. the Bottles dependency "d3dcompiler_47"): the run stops when
the log names another vendor.  Defaults: a Bottles bottle (--bottle, run with
"flatpak run --command=bottles-cli com.usebottles.bottles shell -b <bottle>
-i"), xwin at ~/opt/xwin (--xwin), the work files in ~/.cache/gt3-shader-blobs
(--work; under $HOME, where the flatpak can read).  --wine <command> runs the
exe with another Wine instead of Bottles.

The header is written the same way every time (sources' order, no dates): two
runs with the same sources and compiler give the same bytes."""
import argparse
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PGL = os.path.join(ROOT, 'src', 'engine', 'pgl', 'native', 'pgl_d3d.cpp')
GPU = os.path.join(ROOT, 'src', 'platform', 'gpu.cpp')
OVERLAY = os.path.join(ROOT, 'src', 'platform', 'overlay.cpp')
OUT = os.path.join(ROOT, 'src', 'engine', 'pgl', 'native', 'shader_blobs.h')
SHADERC = os.path.join(ROOT, 'tools', 'shader_blobs', 'shaderc.c')


def fail(msg):
    sys.exit('gen_shader_blobs: ' + msg)


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def raw_string(text, start, delim, path):
    """The body of the raw string literal that follows `start` (R"delim(...)delim")."""
    i = text.find(start)
    if i < 0:
        fail('%s: no %r' % (path, start))
    open_ = 'R"%s(' % delim
    j = text.find(open_, i)
    k = text.find(')%s"' % delim, j)
    if j < 0 or k < 0:
        fail('%s: no raw string after %r' % (path, start))
    return text[j + len(open_):k]


def read(path):
    with open(path, 'r', encoding='utf-8', newline='') as f:
        return f.read()


def collect():
    """[(source name, hlsl bytes, entry, target, [macros])] in the sources' order."""
    jobs = []
    pgl = read(PGL)
    hlsl = raw_string(pgl, 'const char *kHlsl', 'HLSL', PGL).encode('utf-8')
    m = re.search(r'D3DCompile\(kHlsl, strlen\(kHlsl\), "([^"]+)"', pgl)
    if not m:
        fail('pgl_d3d.cpp: no D3DCompile of kHlsl')
    hname = m.group(1)
    if 'shader_blobs::find("%s", entry, target,' % hname not in pgl:
        fail('pgl_d3d.cpp: the lookup does not use the source name %r of D3DCompile' % hname)
    body = pgl[pgl.index('void d3d_init()'):]
    body = body[:body.index('\n}\n')]
    calls = re.findall(r'= compile\("(\w+)", "(\w+)", "(\w+_\d_\d)"\);', body)
    if len(calls) != body.count('= compile(') - body.count('= compile(p.section, p.entry, p.target, p.variant,'):
        fail('pgl_d3d.cpp: a compile() call of d3d_init has an unexpected form')
    for sec, entry, target in calls:
        jobs.append((hname, hlsl, entry, target, [sec]))
    table = body[body.index('} ps[] = {'):]
    table = table[:table.index('\n    };')]
    rows = re.findall(r'\{"(\w+)", "(\w+)", &g_sh\.\w+, "(\w+_\d_\d)"((?:, (?:"\w+"|nullptr))*)(?:, (?:true|false))?\}',
                      table)
    if len(rows) != table.count('&g_sh.'):
        fail('pgl_d3d.cpp: a row of the pixel shader table has an unexpected form')
    for sec, entry, target, rest in rows:
        variants = re.findall(r'"(\w+)"', rest)
        jobs.append((hname, hlsl, entry, target, [sec] + variants))

    gpu = read(GPU)
    targets = {}
    for kind in ('vs', 'ps'):
        m = re.search(r'\*compile_%s\([^)]*\)\s*\{[^}]*?compile\(name, source, entry, "(\w+_\d_\d)"\)' % kind, gpu)
        if not m:
            fail('gpu.cpp: no target in compile_%s' % kind)
        targets[kind] = m.group(1)
    ov = read(OVERLAY)
    shader = raw_string(ov, 'const char kShader[]', '', OVERLAY).encode('utf-8')
    ocalls = re.findall(r'gpu::compile_(vs|ps)\("(\w+)", kShader, "(\w+)"', ov)
    if not ocalls or len(ocalls) != ov.count('gpu::compile_'):
        fail('overlay.cpp: a gpu::compile_vs / compile_ps call has an unexpected form')
    for kind, name, entry in ocalls:
        jobs.append((name, shader, entry, targets[kind], []))
    keys = [(n, e, t, ' '.join(m)) for n, _, e, t, m in jobs]
    if len(set(keys)) != len(keys):
        fail('the same shader is compiled twice')
    return jobs


def winpath(p):
    return 'Z:' + p.replace(os.sep, '/')


def build_shaderc(work, xwin):
    exe = os.path.join(work, 'shaderc.exe')
    inc = []
    for d in ('crt/include', 'sdk/include/ucrt', 'sdk/include/um', 'sdk/include/shared'):
        inc += ['/imsvc', os.path.join(xwin, d)]
    libs = ['/LIBPATH:' + os.path.join(xwin, d) for d in ('crt/lib/x86', 'sdk/lib/um/x86', 'sdk/lib/ucrt/x86')]
    cmd = (['clang-cl-18', '--target=i686-pc-windows-msvc', '/nologo', '/O2', '/MT', '-fuse-ld=lld-link'] + inc +
           [SHADERC, '/Fe' + exe, '/Fo' + os.path.join(work, 'shaderc.obj'), '/link'] + libs +
           ['d3dcompiler.lib', 'dxguid.lib', 'version.lib'])
    subprocess.run(cmd, check=True)
    return exe


def run_shaderc(args, exe, mode, listfile, log):
    if os.path.exists(log):
        os.remove(log)
    line = '%s %s %s %s' % (winpath(exe), mode, winpath(listfile), winpath(log))
    if args.wine:
        cmd = shlex.split(args.wine) + [winpath(exe), mode, winpath(listfile), winpath(log)]
    else:
        cmd = ['flatpak', 'run', '--command=bottles-cli', 'com.usebottles.bottles', 'shell', '-b', args.bottle,
               '-i', line]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(log):
        fail('shaderc wrote no log (%s)' % ' '.join(cmd))
    text = read(log)
    first = text.split('\n', 1)[0]
    if '| Microsoft Corporation |' not in first:
        fail('the d3dcompiler_47.dll of the prefix is not Microsoft\'s: %s' % first)
    if 'result: ok' not in text:
        sys.stderr.write(text)
        fail('shaderc %s failed' % mode)
    return text


BLOB_REF = '''#ifdef __ID3D10Blob_INTERFACE_DEFINED__
/* an ID3DBlob over a blob of the table, as D3DCompile would return it (one
 * reference; the bytes are not copied) */
class BlobRef final : public ID3D10Blob {
public:
    explicit BlobRef(const Blob &b) : m_blob(b) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --m_refs;
        if (n == 0)
            delete this;
        return n;
    }
    void *STDMETHODCALLTYPE GetBufferPointer() override { return const_cast<unsigned char *>(m_blob.code); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return m_blob.size; }

private:
    const Blob &m_blob;
    std::atomic<ULONG> m_refs{1};
};
#endif

'''


def write_header(jobs, blobs, compiler):
    out = []
    w = out.append
    w('/* shader_blobs.h - the compiled DXBC of the shaders pgl_d3d.cpp and overlay.cpp\n')
    w(' * compile at run time on Windows, for builds without D3DCompile.  Generated by\n')
    w(' * tools/shader_blobs/gen_shader_blobs.py; do not edit.  Regenerate after any\n')
    w(' * change to the HLSL, the entries, the targets or the macros.\n')
    w(' * Compiler: %s */\n' % compiler)
    w('#pragma once\n\n#include <atomic>\n#include <cstddef>\n#include <cstdint>\n#include <cstring>\n\n')
    w('namespace shader_blobs {\n\n')
    w('struct Blob {\n')
    w('    const char *name;               /* the source name given to D3DCompile */\n')
    w('    const char *entry;\n    const char *target;\n')
    w('    const char *macros;             /* each defined to "1", in this order, space separated */\n')
    w('    uint64_t source_hash;           /* FNV-1a 64 of the HLSL source */\n')
    w('    const unsigned char *code;\n    size_t size;\n};\n\n')
    for i, ((name, src, entry, target, macros), code) in enumerate(zip(jobs, blobs)):
        w('/* %s %s %s [%s] */\n' % (name, entry, target, ' '.join(macros)))
        w('inline constexpr unsigned char kCode%d[%d] = {\n' % (i, len(code)))
        for k in range(0, len(code), 20):
            w('    ' + ','.join('0x%02x' % b for b in code[k:k + 20]) + ',\n')
        w('};\n')
    w('\ninline constexpr Blob kBlobs[%d] = {\n' % len(jobs))
    for i, (name, src, entry, target, macros) in enumerate(jobs):
        w('    {"%s", "%s", "%s", "%s", 0x%016xull, kCode%d, sizeof kCode%d},\n' %
          (name, entry, target, ' '.join(macros), fnv1a64(src), i, i))
    w('};\n\n')
    w('inline uint64_t source_hash(const char *source, size_t size)\n{\n')
    w('    uint64_t h = 0xcbf29ce484222325ull;\n')
    w('    for (size_t i = 0; i < size; i++)\n')
    w('        h = (h ^ (unsigned char)source[i]) * 0x100000001b3ull;\n')
    w('    return h;\n}\n\n')
    w('/* the blob compiled from (name, entry, target, macros), or null */\n')
    w('inline const Blob *find(const char *name, const char *entry, const char *target, const char *macros)\n{\n')
    w('    for (const Blob &b : kBlobs)\n')
    w('        if (strcmp(b.name, name) == 0 && strcmp(b.entry, entry) == 0 && strcmp(b.target, target) == 0 &&\n')
    w('            strcmp(b.macros, macros) == 0)\n')
    w('            return &b;\n')
    w('    return nullptr;\n}\n\n')
    w(BLOB_REF)
    w('}  /* namespace shader_blobs */\n')
    return ''.join(out)


def parse_header(text):
    """{(name, entry, target, macros): (hash, code bytes)} of a written header."""
    codes = {}
    for m in re.finditer(r'inline constexpr unsigned char kCode(\d+)\[(\d+)\] = \{\n(.*?)\};', text, re.S):
        data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(3)))
        if len(data) != int(m.group(2)):
            fail('shader_blobs.h: kCode%s has %d bytes, not %s' % (m.group(1), len(data), m.group(2)))
        codes[int(m.group(1))] = data
    table = {}
    for m in re.finditer(r'\{"([^"]*)", "([^"]*)", "([^"]*)", "([^"]*)", 0x([0-9a-f]{16})ull, kCode(\d+),', text):
        table[(m.group(1), m.group(2), m.group(3), m.group(4))] = (int(m.group(5), 16), codes[int(m.group(6))])
    return table


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--verify', action='store_true', help='check the header against the sources and D3DReflect it')
    ap.add_argument('--bottle', default='gt3pc')
    ap.add_argument('--wine', help='run shaderc.exe with this command instead of bottles-cli')
    ap.add_argument('--xwin', default=os.path.expanduser('~/opt/xwin'))
    ap.add_argument('--work', default=os.path.expanduser('~/.cache/gt3-shader-blobs'))
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    jobs = collect()
    exe = build_shaderc(work, args.xwin)

    if args.verify:
        table = parse_header(read(args.out))
        want = {(n, e, t, ' '.join(m)): fnv1a64(s) for n, s, e, t, m in jobs}
        if set(table) != set(want):
            fail('shader_blobs.h does not list the shaders the sources compile (missing %s, extra %s)' %
                 (sorted(set(want) - set(table)), sorted(set(table) - set(want))))
        stale = [k for k in want if table[k][0] != want[k]]
        if stale:
            fail('shader_blobs.h is stale for %s: regenerate it' % stale)
        paths = []
        for i, k in enumerate(want):
            p = os.path.join(work, 'verify%02d.dxbc' % i)
            with open(p, 'wb') as f:
                f.write(table[k][1])
            paths.append(p)
        lst = os.path.join(work, 'verify.txt')
        with open(lst, 'w', newline='\n') as f:
            f.write(''.join(winpath(p) + '\n' for p in paths))
        log = run_shaderc(args, exe, 'reflect', lst, os.path.join(work, 'verify.log'))
        sys.stdout.write(log)
        print('shader_blobs.h: %d blobs match the sources and pass D3DReflect' % len(want))
        return

    sources = {}
    lines = []
    outs = []
    for i, (name, src, entry, target, macros) in enumerate(jobs):
        if src not in sources:
            p = os.path.join(work, 'source%d.hlsl' % len(sources))
            with open(p, 'wb') as f:
                f.write(src)
            sources[src] = p
        o = os.path.join(work, 'blob%02d.dxbc' % i)
        if os.path.exists(o):
            os.remove(o)
        outs.append(o)
        lines.append('\t'.join([winpath(sources[src]), name, entry, target, winpath(o), ' '.join(macros)]) + '\n')
    lst = os.path.join(work, 'jobs.txt')
    with open(lst, 'w', newline='\n') as f:
        f.write(''.join(lines))
    log = run_shaderc(args, exe, 'compile', lst, os.path.join(work, 'compile.log'))
    compiler = log.split('\n', 1)[0].split('|', 1)[1].strip()
    blobs = []
    for o in outs:
        with open(o, 'rb') as f:
            blobs.append(f.read())
    text = write_header(jobs, blobs, 'd3dcompiler_47.dll ' + compiler + ', D3DCOMPILE_OPTIMIZATION_LEVEL3')
    with open(args.out, 'w', newline='\n') as f:
        f.write(text)
    print('%s: %d blobs (%d bytes of DXBC)' % (os.path.relpath(args.out, ROOT), len(blobs), sum(map(len, blobs))))


if __name__ == '__main__':
    main()
