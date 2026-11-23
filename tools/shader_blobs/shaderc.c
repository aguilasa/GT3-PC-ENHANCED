/* shaderc.c - the Windows side of gen_shader_blobs.py: compiles HLSL with
 * D3DCompile from d3dcompiler_47.dll (the call the game makes at run time)
 * and checks compiled DXBC with D3DReflect.  Built for i686 with clang-cl and
 * the xwin CRT/SDK, run under Wine (a prefix whose d3dcompiler_47 is
 * Microsoft's: gen_shader_blobs.py checks that from the log).
 *
 *   shaderc.exe compile <jobs> <log>    each job line: hlsl \t name \t entry \t target \t out \t macros
 *   shaderc.exe reflect <list> <log>    each line: a .dxbc path
 *
 * Paths are Windows paths (Z:/home/...).  Macros: space separated, each
 * defined to "1" in the given order, as the game's compile() does.  The
 * source is passed as the bytes of the file, with the name given, and the
 * flags the game uses (D3DCOMPILE_OPTIMIZATION_LEVEL3).  Exit 0 only when
 * every line succeeded; the log gets one line per job. */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    fflush(g_log);
}

static char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *size = (size_t)n;
    return buf;
}

/* (the d3dcompiler_47.dll this process loaded: its path and file version) */
static void log_compiler(void)
{
    HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
    char path[MAX_PATH] = "?";
    if (m)
        GetModuleFileNameA(m, path, MAX_PATH);
    char company[256] = "?", version[256] = "?";
    DWORD h = 0, n = GetFileVersionInfoSizeA(path, &h);
    if (n) {
        void *vi = malloc(n);
        if (GetFileVersionInfoA(path, 0, n, vi)) {
            char *s;
            UINT len;
            if (VerQueryValueA(vi, "\\StringFileInfo\\040904B0\\CompanyName", (void **)&s, &len))
                snprintf(company, sizeof company, "%s", s);
            if (VerQueryValueA(vi, "\\StringFileInfo\\040904B0\\FileVersion", (void **)&s, &len))
                snprintf(version, sizeof version, "%s", s);
        }
        free(vi);
    }
    say("compiler: %s | %s | %s\n", path, company, version);
}

static int reflect(const void *code, size_t size, const char *what)
{
    ID3D11ShaderReflection *r = NULL;
    HRESULT hr = D3DReflect(code, size, &IID_ID3D11ShaderReflection, (void **)&r);
    if (FAILED(hr) || !r) {
        say("reflect FAIL %s hr=0x%08lx\n", what, (unsigned long)hr);
        return 0;
    }
    D3D11_SHADER_DESC d;
    hr = r->lpVtbl->GetDesc(r, &d);
    if (FAILED(hr)) {
        say("reflect FAIL %s GetDesc hr=0x%08lx\n", what, (unsigned long)hr);
        r->lpVtbl->Release(r);
        return 0;
    }
    UINT type = D3D11_SHVER_GET_TYPE(d.Version);
    const char *t = type == D3D11_SHVER_PIXEL_SHADER ? "ps" : type == D3D11_SHVER_VERTEX_SHADER ? "vs"
                  : type == D3D11_SHVER_GEOMETRY_SHADER ? "gs" : "??";
    say("reflect ok %s %s_%u_%u bytes=%u instructions=%u cbuffers=%u resources=%u inputs=%u outputs=%u\n", what, t,
        D3D11_SHVER_GET_MAJOR(d.Version), D3D11_SHVER_GET_MINOR(d.Version), (unsigned)size, d.InstructionCount,
        d.ConstantBuffers, d.BoundResources, d.InputParameters, d.OutputParameters);
    r->lpVtbl->Release(r);
    return 1;
}

/* (splits a line at tabs in place; returns the field count) */
static int split(char *line, char **f, int max)
{
    int n = 0;
    line[strcspn(line, "\r\n")] = 0;
    while (n < max) {
        f[n++] = line;
        char *t = strchr(line, '\t');
        if (!t)
            break;
        *t = 0;
        line = t + 1;
    }
    return n;
}

static int do_compile(FILE *jobs)
{
    char line[4096];
    int ok = 0, bad = 0;
    while (fgets(line, sizeof line, jobs)) {
        char *f[6];
        if (line[0] == '\n' || line[0] == '#')
            continue;
        if (split(line, f, 6) != 6) {
            say("bad job line\n");
            bad++;
            continue;
        }
        size_t srcSize = 0;
        char *src = read_file(f[0], &srcSize);
        if (!src) {
            say("compile FAIL %s: cannot read %s\n", f[4], f[0]);
            bad++;
            continue;
        }
        D3D_SHADER_MACRO defs[16];
        int n = 0;
        char macros[1024];
        snprintf(macros, sizeof macros, "%s", f[5]);
        for (char *m = strtok(macros, " "); m && n < 15; m = strtok(NULL, " "))
            defs[n++] = (D3D_SHADER_MACRO){m, "1"};
        defs[n] = (D3D_SHADER_MACRO){NULL, NULL};
        ID3DBlob *code = NULL, *errors = NULL;
        HRESULT hr = D3DCompile(src, srcSize, f[1], defs, NULL, f[2], f[3], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                                &errors);
        free(src);
        if (FAILED(hr) || !code) {
            say("compile FAIL %s hr=0x%08lx\n%s\n", f[4], (unsigned long)hr,
                errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
            bad++;
        } else {
            FILE *o = fopen(f[4], "wb");
            size_t size = ID3D10Blob_GetBufferSize(code);
            if (!o || fwrite(ID3D10Blob_GetBufferPointer(code), 1, size, o) != size) {
                say("compile FAIL %s: cannot write\n", f[4]);
                bad++;
            } else {
                say("compile ok %s %s %s [%s] bytes=%u\n", f[1], f[2], f[3], f[5], (unsigned)size);
                ok++;
            }
            if (o)
                fclose(o);
        }
        if (code)
            ID3D10Blob_Release(code);
        if (errors)
            ID3D10Blob_Release(errors);
    }
    say("compiled %d, failed %d\n", ok, bad);
    return bad == 0 && ok > 0;
}

static int do_reflect(FILE *list)
{
    char line[4096];
    int ok = 0, bad = 0;
    while (fgets(line, sizeof line, list)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        size_t size = 0;
        char *code = read_file(line, &size);
        if (!code) {
            say("reflect FAIL %s: cannot read\n", line);
            bad++;
            continue;
        }
        if (reflect(code, size, line))
            ok++;
        else
            bad++;
        free(code);
    }
    say("reflected %d, failed %d\n", ok, bad);
    return bad == 0 && ok > 0;
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "compile") != 0 && strcmp(argv[1], "reflect") != 0)) {
        fprintf(stderr, "usage: shaderc compile|reflect <list> <log>\n");
        return 2;
    }
    g_log = fopen(argv[3], "w");
    if (!g_log) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 2;
    }
    FILE *in = fopen(argv[2], "r");
    if (!in) {
        say("cannot read %s\n", argv[2]);
        return 2;
    }
    /* (load the compiler first, so the log names it before any job) */
    LoadLibraryA("d3dcompiler_47.dll");
    log_compiler();
    int ok = strcmp(argv[1], "compile") == 0 ? do_compile(in) : do_reflect(in);
    fclose(in);
    say("%s\n", ok ? "result: ok" : "result: FAIL");
    fclose(g_log);
    return ok ? 0 : 1;
}
