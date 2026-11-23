/* pgl_d3d.cpp - the Direct3D 11 objects of the pgl renderer: the shaders
 * (HLSL below), input layouts, state-object caches and the dynamic vertex /
 * index streams.
 *
 * Shader conventions (docs/renderer.md, docs/pgl.md 4-5): colours travel in
 * GS units (bytes 0..255, 0x80 = 1.0 for modulation and alpha); the pixel
 * shader computes the GS source colour / alpha (texture function, fog,
 * alpha test) and writes o0 = colour / 255 and, for the blend, o1.a = C /
 * 128 (dual-source: C = As or FIX).  Texture coordinates travel as the GS's
 * S T Q (texels x Q, Q), interpolated linearly in screen space and divided
 * per pixel, as the GS does; the bound native texture holds the GS texels
 * (x0, y0, w, h) of the texture's 2^TW x 2^TH space.
 *
 * PC (CPU time): the VS / PS constants (bind_vs_cb, bind_ps_cb) go into a
 * ring - one large dynamic buffer written forward in 256-byte slots with
 * NO_OVERWRITE (DISCARD when it wraps), bound at their offset (Direct3D
 * 11.1 constant buffer offsets) - instead of renaming one small buffer with
 * DISCARD at each change (a model part's constants nearly every draw: some
 * 3500 renames a race frame).  Without the driver's support: as before. */
#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_win32.h"   /* (the Win32 calls off Windows too) */
#include "engine/pgl/native/pgl_capture.h"  /* PC (2026-11-06): the frame capture's bind counts and GPU queries */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <emmintrin.h>
#include <memory>
#include <unordered_map>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#ifndef _WIN32
#include <string>
#include "engine/pgl/native/shader_blobs.h"   /* (no D3DCompile: the shaders compiled offline) */
#endif

#include "platform/gpu.h"
#include "platform/log.h"

namespace pglr {

ID3D11Device *dev() { return gpu::device(); }
/* (PC, 2026-11-06: the frame capture's bind counts and GPU queries, pgl_capture.h) */
ID3D11DeviceContext *dc() { return gpu::context(); }

namespace {
ID3D11DeviceContext *g_qdc;                 /* (d3d_init: the context never changes) */
}  /* namespace */

ID3D11DeviceContext *qdc() { return g_qdc; }

namespace {

/* One source, compiled per entry point with the SEC_* macro of its section
 * (each section declares its own b0). */
const char *kHlsl = R"HLSL(
/* PC (2026-11-15): AGSS (settings.h; SAMPLE_PS, ps_4_1) - the varyings taken
 * at each sample's own place, so the pixel shader runs once per sample: the
 * texture, the colour, the fog and the alpha test at every sample of the
 * GPU's own pattern (supersampling), the resolve their mean.  Empty for
 * every other shader (their code as it was). */
#ifdef SAMPLE_PS
#define SMP sample
#else
#define SMP
#endif
struct PsIn {
    float4 pos : SV_Position;
    noperspective SMP float3 stq : TEXCOORD0;   /* S x 2^TW, T x 2^TH, Q */
    noperspective SMP float4 color : COLOR0;    /* GS bytes */
    noperspective SMP float fog : FOG;          /* F, 0..255 */
    nointerpolation float4 tclamp : TEXCOORD1;  /* PC: a sprite's texel range (GsVtx tu0..tv1) */
    nointerpolation float4 rect : TEXCOORD2;    /* PC: the VU1 draw's texture rectangle (PSF_INST_RECT) */
    /* PC: a merged VU1 draw's part (pgl_d3d.h CbParts): its texture slot and
     * PS texture flags; x 0xFFFF: not merged (tex0, the constants' flags) */
    nointerpolation uint2 tsel : TEXCOORD3;
    /* PC (2026-11-05): the instance (0 / 1) of a C = Ad draw made of two
     * (PSF_AD_INST: pgl_d3d.h) */
    nointerpolation uint rep : TEXCOORD4;
    /* PC (2026-11-07): 1 at a vertex inside the hide box (CbVu1 hideC /
     * hideH: the cockpit camera's driver's head), 0 outside; a pixel where it
     * interpolates above 0.5 is discarded (ps_body).  2 on every vertex of a
     * glass shell: its translucent texels are discarded */
    SMP float hide : TEXCOORD5;
};
struct PsInFlat {
    float4 pos : SV_Position;
    noperspective SMP float3 stq : TEXCOORD0;
    nointerpolation float4 color : COLOR0;  /* the leading (kicking) vertex's */
    noperspective SMP float fog : FOG;
    nointerpolation float4 tclamp : TEXCOORD1;
    nointerpolation float4 rect : TEXCOORD2;
    nointerpolation uint2 tsel : TEXCOORD3;
    nointerpolation uint rep : TEXCOORD4;
    SMP float hide : TEXCOORD5;
};
struct FullOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

/* ---- GS-space vertices ---------------------------------------------- */
#ifdef SEC_GS
cbuffer CbGs : register(b0) {
    float4 vp;                      /* native target width, height */
};
struct GsIn {
    float4 pos : POSITION;          /* native x, y, depth, Q */
    float2 tc : TEXCOORD0;          /* texels x Q (FST: texels, Q = 1) */
    uint4 color : COLOR0;
    float fog : FOG;
    float4 tclamp : TEXCOORD1;      /* PC: a sprite's texel range */
    uint inst : SV_InstanceID;      /* PC: (a C = Ad draw's two instances, PsIn rep) */
};
PsIn vs_gs(GsIn i)
{
    PsIn o;
    o.pos = float4(i.pos.x / vp.x * 2.0 - 1.0, 1.0 - i.pos.y / vp.y * 2.0, i.pos.z, 1.0);
    o.stq = float3(i.tc, i.pos.w);
    o.color = float4(i.color);
    o.fog = i.fog;
    o.tclamp = i.tclamp;
    o.rect = float4(0, 0, 0, 0);
    o.tsel = uint2(0xFFFF, 0);
    o.rep = i.inst;                         /* PC (2026-11-05): 0 / 1, and the mirror's instance (FOLD_PS) */
    o.hide = 0;
    return o;
}
#endif

/* ---- VU1 programs (docs/models.md 5) --------------------------------- */
#ifdef SEC_VU1
cbuffer CbVu1 : register(b0) {
    float4 C[4];
    float4 viewScale;
    float4 viewCenter;
    float4 limits;
    float4 ndcMap;
    float morph; float zNorm; float flareO; float flareGpu;
    int format; float flareX; float flareY; int pad0;
    /* PC (2026-11-07): the hide box (pgl_d3d.h CbVu1): centre, w 1 on; half sizes */
    float4 hideC; float4 hideH;
    /* PC (2026-11-04): the program, the exponent and the block are read only
     * without the lights' ring (a record's lightBase -1: light_supported) -
     * the draw's record has them (Vu1In inst3, lightBlocks) */
    int program; int specExp; int pad1; int pad2;
    float4 L[16];
};
/* PC (2026-11-03): the records' ring (pgl_d3d.h Vu1Inst, vu1_inst) as a
 * buffer: a merged draw's parts (pgl_d3d.h Vu1Part, inst0.w of them from
 * record inst2.x): each one's first vertex (ascending), texture slot and PS
 * texture flags; 2^TW and 2^TH; texture rectangle; its lighting block,
 * program and exponent - 4 words of 16 bytes */
Buffer<uint4> instRecs : register(t1);
/* PC (2026-11-04): the lighting blocks' ring (pgl_d3d.h light_block): 16
 * float4 a block, a draw's (or a merged draw's part's) from its record */
Buffer<float4> lightBlocks : register(t2);
#define LB(k) (lightBase >= 0 ? lightBlocks.Load(lightBase * 16 + (k)) : L[k])
/* PC: the flare queries' result strip (flareGpu: the glow's texel read
 * here, at (flareX, flareY), instead of flareO: docs/renderer.md 8) */
Texture2D flareStrip : register(t0);
/* A lit vertex in the VU1's clip space, before the projection: what the
 * microcode's polygon clipper works on. */
struct Vu1Clip {
    float4 pc : CLIPPOS;            /* (P, 1) x C */
    float2 st : TEXCOORD0;          /* S x 2^TW, T x 2^TH (not yet x Q) */
    float4 color : COLOR0;          /* GS bytes */
    float4 zw : ZFROMW;             /* PC: the draw's depth from w (gs_zw) */
    float flat : FLATSHADE;         /* PC: the draw's flat shading (1) */
    float4 rect : TEXRECT;          /* PC: the draw's texture rectangle (PsIn rect) */
    uint2 tsel : TEXSEL;            /* PC: its part's texture slot and flags (PsIn tsel) */
    uint rep : REP;                 /* PC: the instance of a C = Ad draw of two (PsIn rep) */
    float hide : HIDE;              /* PC (2026-11-07): inside the hide box (PsIn hide) */
};
struct Vu1In {
    float3 pos : POSITION0;  float3 pos2 : POSITION1;
    float3 nrm : NORMAL0;    float3 nrm2 : NORMAL1;
    float2 st  : TEXCOORD0;  float2 st2  : TEXCOORD1;
    uint4  rgba : COLOR0;    uint4  rgba2 : COLOR1;
    /* PC: the draw's record (instance data, slot 1: Vu1Inst) - 2^TW, 2^TH,
     * flat shading, the parts of a merged draw; the depth from w; the
     * texture's rectangle */
    float4 inst0 : INST0;    float4 inst1 : INST1;    float4 inst2 : INST2;
    float4 inst3 : INST3;    /* PC: its lighting block (lightBlocks; -1: L), program, exponent; w: the record's
                              * place among the draw's copies (PsIn rep: a C = Ad draw's instance, or the pass of
                              * a pair of draws - vu1_inst's copies) */
    uint batch : BATCH;      /* PC: its batch in the mesh (a merged draw's part: slot 2) */
    uint vid : SV_VertexID;  /* PC (2026-11-06): the vertex's number - past the mesh's count, the doubled stream's
                              * second copy: the second pass (rep) */
};
/* PC (2026-11-06): the pass a vertex is drawn for (PsIn rep): the record's
 * copy (bits 0..1 of inst3.w: a C = Ad draw's instance, a pair's draw, the
 * fold's), plus one for a vertex of the doubled index stream's second half
 * (inst3.w holds 4 x the mesh's vertices above the copy's place) */
uint vu1_rep(float padw, uint vid)
{
    uint p = (uint)padw;
    uint nv = p >> 2;
    return (p & 3) + ((nv != 0 && vid >= nv) ? 1 : 0);
}
float pow_e(float x, int e)
{
    float x2 = x * x;
    float x4 = x2 * x2;
    if (e <= 4) return x4;
    float x8 = x4 * x4;
    if (e <= 8) return x8;
    return x8 * x8;
}
float4 pow_e4(float4 x, int e)
{
    return float4(pow_e(x.x, e), pow_e(x.y, e), pow_e(x.z, e), pow_e(x.w, e));
}
Vu1Clip vs_vu1(Vu1In i)
{
    Vu1Clip o;
    /* PC: a merged draw's part by the vertex (CbParts, first vertices
     * ascending), else the draw's own record */
    float2 texWH = i.inst0.xy;
    float4 rect = i.inst2;
    float4 lp = i.inst3;
    uint2 tsel = uint2(0xFFFF, 0);
    uint nParts = (uint)i.inst0.w;          /* (Vu1Inst parts) */
    if (nParts != 0) {
        /* (the part of its batch: the draw's parts by batch from inst2.y,
         * their records from inst2.x) */
        uint at = 4 * ((uint)i.inst2.x + (i.batch - (uint)i.inst2.y));
        uint4 a = instRecs.Load(at);
        texWH = asfloat(instRecs.Load(at + 1).xy);
        rect = asfloat(instRecs.Load(at + 2));
        lp = asfloat(instRecs.Load(at + 3));
        tsel = a.yz;
    }
    /* PC (2026-11-04): the lighting from the record: its block (LB: the
     * lights' ring, or the constants' L), program and exponent */
    int lightBase = (int)lp.x;
    int prog = (int)lp.y;
    int e = (int)lp.z;
    if (lightBase < 0) {
        prog = program;
        e = specExp;
    }
    float3 P = i.pos;
    float3 N = i.nrm;
    float2 ST = i.st;
    float4 c = float4(i.rgba);
    if (format == 1 || format == 2) {
        P = lerp(i.pos, i.pos2, morph);
        N = lerp(i.nrm, i.nrm2, morph);
        ST = lerp(i.st, i.st2, morph);
        if (format == 1)
            c = trunc(lerp(float4(i.rgba), float4(i.rgba2), morph));
    }
    /* PC (2026-11-07): 1 inside the hide box (the model's space); 2 the
     * whole part's translucent texels (hideC.w 2: a glass shell) */
    const float hide = hideC.w == 2 ? 2.0 : (hideC.w != 0 && all(abs(P - hideC.xyz) <= hideH.xyz)) ? 1.0 : 0.0;
    if (prog == 1) {                /* DIFFUSE */
        float4 d = max(LB(0) * N.x + LB(1) * N.y + LB(2) * N.z, 0);
        c = clamp(LB(3) + LB(4) * d.x + LB(5) * d.y + LB(6) * d.z + LB(7) * d.w, 0, LB(8));
    } else if (prog == 2) {         /* SPEC2* */
        float4 t = max(LB(0) * N.x + LB(1) * N.y + LB(2) * N.z, 0);
        c = clamp(LB(3) + LB(4) * t.x + LB(5) * t.y + LB(6) * pow_e(t.z, e) + LB(7) * pow_e(t.w, e), 0, LB(8));
    } else if (prog == 3) {         /* SPEC4* */
        float4 dl = max(LB(0) * N.x + LB(1) * N.y + LB(2) * N.z, 0);
        float4 dh = pow_e4(max(LB(8) * N.x + LB(9) * N.y + LB(10) * N.z, 0), e);
        c = clamp(LB(3) + LB(4) * dl.x + LB(5) * dl.y + LB(6) * dl.z + LB(7) * dl.w
                  + LB(11) * dh.x + LB(12) * dh.y + LB(13) * dh.z + LB(14) * dh.w, 0, LB(15));
    } else if (prog == 4) {         /* LOCAL* */
        float4 L0 = LB(0), L1 = LB(1), L2 = LB(2), L3 = LB(3), L4 = LB(4);
        float3 V = P - L3.xyz;
        float3 R = V - 2 * dot(V, N) * N;
        float r = max(dot(float3(L0.x, L1.x, L2.x), R) * rsqrt(max(dot(V, V), 1e-30)), 0);
        /* alpha 0x80: the program's vf09 = vf00 + I (127), w = 1 + 127, and
         * FTOI0.xyz replaces only the colour (0x1BF / 0x22C, 0x1D6 / 0x24A) */
        c = float4(trunc(L4.rgb * pow_e(r, e)), 128);
    } else if (prog == 5) {         /* ENVMTX */
        float4 L0 = LB(0), L1 = LB(1), L2 = LB(2), L3 = LB(3), L4 = LB(4), L5 = LB(5);
        float3 V = P - L3.xyz;
        float iv = rsqrt(max(dot(V, V), 1e-30));
        float3 R = (V - 2 * dot(V, N) * N) * iv;
        float3 Rv = R.x * L0.xyz + R.y * L1.xyz + R.z * L2.xyz;
        float m = 0.5 * rsqrt(max(Rv.x * Rv.x + Rv.y * Rv.y + (Rv.z + 1) * (Rv.z + 1), 1e-30));
        ST = float2(0.5 + Rv.x * m, 0.5 - Rv.y * m);
        c = trunc(c * (L4 + L5 * min(dot(V, N) * iv, 0)));
    } else if (prog == 6) {         /* ENVMAP */
        float4 L0 = LB(0), L1 = LB(1), L2 = LB(2), L3 = LB(3), L4 = LB(4), L5 = LB(5);
        float3 V = P - L3.xyz;
        float iv = rsqrt(max(dot(V, V), 1e-30));
        float3 Re = V - 2 * dot(V, N) * N;
        Re = Re.x * L0.xyz + Re.y * L1.xyz + Re.z * L2.xyz;
        float th;
        if (Re.x >= Re.z)
            th = (Re.x >= -Re.z) ? Re.z / Re.x : 6 - Re.x / Re.z;
        else
            th = (Re.x >= -Re.z) ? 2 - Re.x / Re.z : 4 + Re.z / Re.x;
        float y = Re.y * iv;
        ST = float2(L0.w * th + L1.w, L2.w * (y * y * y + 2 * y) + L3.w);
        c = trunc(c * (L4 + L5 * min(dot(V, N) * iv, 0)));
    } else if (prog == 7) {         /* CONST */
        c = clamp(c * LB(0) + LB(2), 0, LB(1));
    } else if (prog == 8) {         /* the flare program */
        /* the occlusion texel's R byte: from the strip on the GPU (the
         * queries just before wrote it), or flareO; 0: not drawn (beyond the
         * far plane: gs_vu1 drops the triangle) */
        float occ = flareO;
        if (flareGpu != 0)
            occ = round(flareStrip.Load(int3(flareX, flareY, 0)).r * 255.0);
        c = min(c * float4(LB(0).rgb, occ / 255.0), 255);
        if (occ == 0) {
            o.pc = float4(0, 0, 2, 1);
            o.st = float2(0, 0);
            o.color = float4(0, 0, 0, 0);
            o.zw = i.inst1;
            o.flat = i.inst0.z;
            o.rect = rect;
            o.tsel = tsel;
            o.rep = vu1_rep(i.inst3.w, i.vid);
            o.hide = hide;
            return o;
        }
    }
    o.pc = P.x * C[0] + P.y * C[1] + P.z * C[2] + C[3];
    o.st = ST * texWH;
    o.color = c;
    o.zw = i.inst1;
    o.flat = i.inst0.z;
    o.rect = rect;
    o.tsel = tsel;
    o.rep = vu1_rep(i.inst3.w, i.vid);      /* PC (2026-11-05): the record's copy: 0 / 1 (C = Ad), 2 the fold's;
                                             * PC (2026-11-06): or the doubled stream's half */
    o.hide = hide;
    return o;
}

/* The transform programs' projection of a clip-space vertex (docs/models.md
 * 3.3): q = 1 / w, STQ = (st q, q), XYZ through viewScale / viewCenter (here
 * to the target's NDC), F = min(w, fog start) q viewScale.w + viewCenter.w. */
/* PC: the GS Z times w.  The microcode's is pc.z through the viewport:
 * pc.z / w, scaled by about 2^23, kept the rounding of a course vertex's
 * products (hundreds of metres from the origin) as centimetres of depth a
 * few tens of metres away - a sign's arm through the sign, flickering as
 * the view moved.  A perspective projection makes Z a function of w, as
 * exact as the eye distance: zw.z != 0, Z = zw.x + zw.y / w (vu1_depth;
 * docs/renderer.md 4, "Precision"); else pc.z as the microcode takes it,
 * an overlay's offset zw.w on top (docs/renderer.md "Overlays").  (zw: the
 * draw's record, Vu1Clip) */
float gs_zw(float4 pc, float4 zw)
{
    if (zw.z != 0)
        return zw.y + zw.x * pc.w;
    return pc.z * viewScale.z + pc.w * viewCenter.z + pc.w * zw.w;
}
PsIn project_vu1(Vu1Clip v)
{
    PsIn o;
    float4 pc = v.pc;
    o.pos.x = ndcMap.x * pc.x + ndcMap.y * pc.w - pc.w;
    o.pos.y = pc.w - (ndcMap.z * pc.y + ndcMap.w * pc.w);
    o.pos.z = gs_zw(pc, v.zw) * zNorm;
    o.pos.w = pc.w;
    float q = 1.0 / pc.w;
    o.fog = clamp(viewCenter.w + viewScale.w * min(pc.w, limits.w) * q, 0, 255);
    o.color = v.color;
    o.stq = float3(v.st * q, q);
    o.tclamp = float4(-1e30, -1e30, 1e30, 1e30);
    o.rect = v.rect;
    o.tsel = v.tsel;
    o.rep = v.rep;
    o.hide = v.hide;
    return o;
}

/* The clipper's planes, as distances (inside >= 0): the guard band |x|,
 * |y| <= 2w (x, y x vBB0 = 0.5 against w) and |z| <= w. */
float clip_dist(float4 p, int k)
{
    if (k == 0) return 2 * p.w - p.x;
    if (k == 1) return 2 * p.w + p.x;
    if (k == 2) return 2 * p.w - p.y;
    if (k == 3) return 2 * p.w + p.y;
    if (k == 4) return p.w - p.z;
    return p.w + p.z;
}

Vu1Clip clip_lerp(Vu1Clip a, Vu1Clip b, float t)
{
    Vu1Clip o;
    o.pc = lerp(a.pc, b.pc, t);
    o.st = lerp(a.st, b.st, t);
    o.color = lerp(a.color, b.color, t);
    o.zw = a.zw;
    o.flat = a.flat;
    o.rect = a.rect;
    o.tsel = a.tsel;
    o.rep = a.rep;
    o.hide = lerp(a.hide, b.hide, t);
    return o;
}

/* PC: the VU1 microcode's polygon clipper (0x57A..0x762, Sutherland-Hodgman
 * over the six planes, docs/models.md 3.3).  The GS gets STQ = ST x Q and
 * Gouraud colours / fog interpolated linearly on the screen (noperspective
 * here); a triangle reaching past the camera (w <= 0) has no meaningful
 * screen-space values, and Direct3D's own clipping of such already
 * projected attributes gave the road under the bumper camera garbage
 * texture coordinates.  As on the PS2 the clipping is done first, in clip
 * space on the raw ST and colours, and every vertex is projected after it.
 * A triangle inside all six planes is projected unchanged; one outside one
 * plane entirely is dropped (the microcode's ADC). */
[maxvertexcount(21)]
void gs_vu1(triangle Vu1Clip t[3], inout TriangleStream<PsIn> outStream)
{
    bool inside = true;
    [unroll] for (int k = 0; k < 6; k++) {
        float d0 = clip_dist(t[0].pc, k), d1 = clip_dist(t[1].pc, k), d2 = clip_dist(t[2].pc, k);
        if (d0 < 0 && d1 < 0 && d2 < 0)
            return;
        if (d0 < 0 || d1 < 0 || d2 < 0)
            inside = false;
    }
    if (inside) {
        outStream.Append(project_vu1(t[0]));
        outStream.Append(project_vu1(t[1]));
        outStream.Append(project_vu1(t[2]));
        return;
    }

    Vu1Clip poly[9], next[9];
    [unroll] for (int z = 0; z < 9; z++) {
        poly[z] = t[0];
        next[z] = t[0];
    }
    poly[1] = t[1];
    poly[2] = t[2];
    int n = 3;
    [unroll] for (int p = 0; p < 6; p++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            Vu1Clip a = poly[i];
            Vu1Clip b = poly[i + 1 < n ? i + 1 : 0];
            float da = clip_dist(a.pc, p), db = clip_dist(b.pc, p);
            if (da >= 0)
                next[m++] = a;
            if ((da >= 0) != (db >= 0))
                next[m++] = clip_lerp(a, b, da / (da - db));
        }
        n = m;
        for (int j = 0; j < n; j++)
            poly[j] = next[j];
        if (n < 3)
            return;
    }
    /* flat shading: the triangle's provoking (kicking) colour on every piece */
    if (t[0].flat != 0)
        for (int f = 0; f < n; f++)
            poly[f].color = t[0].color;
    for (int v = 1; v + 1 < n; v++) {
        outStream.Append(project_vu1(poly[0]));
        outStream.Append(project_vu1(poly[v]));
        outStream.Append(project_vu1(poly[v + 1]));
        outStream.RestartStrip();
    }
}
/* PC (GPU time, 2026-11-03): a batch whose box lies inside all six planes
 * (vu1_draw: its corners through C) needs no clipper - every triangle takes
 * gs_vu1's inside path, its vertices projected unchanged: here, in the
 * vertex stage (a geometry stage of 21 vertices out ran for every triangle
 * of every model part) */
PsIn vs_vu1_direct(Vu1In i)
{
    return project_vu1(vs_vu1(i));
}
#endif

/* ---- Full-screen passes ------------------------------------------------- */
#ifdef SEC_FULL
FullOut vs_full(uint id : SV_VertexID)
{
    FullOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
#endif

/* ---- The GS pixel pipeline ---------------------------------------------- */
#ifdef SEC_PS
Texture2D tex0 : register(t0);
Texture2D dstCopy : register(t1);           /* DATE, C = Ad: the target as the draw state found it */
SamplerState samp0 : register(s0);
cbuffer CbPs : register(b0) {
    float4 texRect;                 /* x0, y0, 1 / w, 1 / h (GS texels) */
    uint flags; float aref; float fix; float rowScale;
    float4 fogTa0;                  /* FOGCOL, TEXA.TA0 */
    float4 ta1;                     /* TEXA.TA1; y: PSF_BLEND_AD's selectors (A | B << 2 | D << 6) */
};
/* PC (2026-11-04): the mirror pass (MIRROR_PS: pgl_d3d.h mirror_ps) - the
 * bit the mirror already holds where this draw wrote: a pixel whose alpha
 * has it is discarded, the stencil op marks the others */
cbuffer CbMirror : register(b1) {
    uint4 mirrorKeep;
};
struct PsOut {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
#ifdef FOLD_PS
    float depth : SV_Depth;                 /* PC (2026-11-05): the mirror folded in (ps_body, pgl_d3d.h PSF_MIRROR) */
#endif
};
#ifdef FOLD_PS
Texture2D<float> zBuf : register(t17);      /* the depth buffer (bound read-only as the depth-stencil view) */
#endif
bool compare(float a, float ref, uint f)
{
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a < ref;
    if (f == 3) return a <= ref;
    if (f == 4) return a == ref;
    if (f == 5) return a >= ref;
    if (f == 6) return a > ref;
    return a != ref;
}
/* PC (2026-11-03): a merged VU1 draw's textures (pgl_d3d.h CbParts): its
 * part's slot - 0 tex0 / samp0 as ever, 1..15 these */
Texture2D mt1 : register(t2);   Texture2D mt2 : register(t3);   Texture2D mt3 : register(t4);
Texture2D mt4 : register(t5);   Texture2D mt5 : register(t6);   Texture2D mt6 : register(t7);
Texture2D mt7 : register(t8);   Texture2D mt8 : register(t9);   Texture2D mt9 : register(t10);
Texture2D mt10 : register(t11); Texture2D mt11 : register(t12); Texture2D mt12 : register(t13);
Texture2D mt13 : register(t14); Texture2D mt14 : register(t15); Texture2D mt15 : register(t16);
SamplerState ms1 : register(s1);   SamplerState ms2 : register(s2);   SamplerState ms3 : register(s3);
SamplerState ms4 : register(s4);   SamplerState ms5 : register(s5);   SamplerState ms6 : register(s6);
SamplerState ms7 : register(s7);   SamplerState ms8 : register(s8);   SamplerState ms9 : register(s9);
SamplerState ms10 : register(s10); SamplerState ms11 : register(s11); SamplerState ms12 : register(s12);
SamplerState ms13 : register(s13); SamplerState ms14 : register(s14); SamplerState ms15 : register(s15);
#ifdef MERGED_PS
float4 sample_slot(uint slot, float2 uv)
{
    [forcecase] switch (slot) {
    case 1: return mt1.SampleLevel(ms1, uv, 0);
    case 2: return mt2.SampleLevel(ms2, uv, 0);
    case 3: return mt3.SampleLevel(ms3, uv, 0);
    case 4: return mt4.SampleLevel(ms4, uv, 0);
    case 5: return mt5.SampleLevel(ms5, uv, 0);
    case 6: return mt6.SampleLevel(ms6, uv, 0);
    case 7: return mt7.SampleLevel(ms7, uv, 0);
    case 8: return mt8.SampleLevel(ms8, uv, 0);
    case 9: return mt9.SampleLevel(ms9, uv, 0);
    case 10: return mt10.SampleLevel(ms10, uv, 0);
    case 11: return mt11.SampleLevel(ms11, uv, 0);
    case 12: return mt12.SampleLevel(ms12, uv, 0);
    case 13: return mt13.SampleLevel(ms13, uv, 0);
    case 14: return mt14.SampleLevel(ms14, uv, 0);
    case 15: return mt15.SampleLevel(ms15, uv, 0);
    default: return tex0.SampleLevel(samp0, uv, 0);
    }
}
/* PC (2026-11-13): a replacement texture's sample (pgl_texrepl.h; the alpha
 * mode 3): through its mip levels, by the footprint's derivatives */
float4 sample_slot_grad(uint slot, float2 uv, float2 dx, float2 dy)
{
    [forcecase] switch (slot) {
    case 1: return mt1.SampleGrad(ms1, uv, dx, dy);
    case 2: return mt2.SampleGrad(ms2, uv, dx, dy);
    case 3: return mt3.SampleGrad(ms3, uv, dx, dy);
    case 4: return mt4.SampleGrad(ms4, uv, dx, dy);
    case 5: return mt5.SampleGrad(ms5, uv, dx, dy);
    case 6: return mt6.SampleGrad(ms6, uv, dx, dy);
    case 7: return mt7.SampleGrad(ms7, uv, dx, dy);
    case 8: return mt8.SampleGrad(ms8, uv, dx, dy);
    case 9: return mt9.SampleGrad(ms9, uv, dx, dy);
    case 10: return mt10.SampleGrad(ms10, uv, dx, dy);
    case 11: return mt11.SampleGrad(ms11, uv, dx, dy);
    case 12: return mt12.SampleGrad(ms12, uv, dx, dy);
    case 13: return mt13.SampleGrad(ms13, uv, dx, dy);
    case 14: return mt14.SampleGrad(ms14, uv, dx, dy);
    case 15: return mt15.SampleGrad(ms15, uv, dx, dy);
    default: return tex0.SampleGrad(samp0, uv, dx, dy);
    }
}
#else
float4 sample_slot(uint slot, float2 uv)
{
    return tex0.SampleLevel(samp0, uv, 0);
}
float4 sample_slot_grad(uint slot, float2 uv, float2 dx, float2 dy)
{
    return tex0.SampleGrad(samp0, uv, dx, dy);
}
#endif
/* PC (2026-11-10): [Graphics] AnisotropicFiltering (pgl_d3d.h PSF_ANISO): the
 * colour over the pixel's footprint along its long axis - up to ta1.w
 * (CbPs::pad2) taps across it, each the GS's own sample (the draw's sampler
 * at level 0: bilinear), weighted by its alpha, so a texel the alpha hides
 * adds nothing and its colour never shows (user: "a texture might have
 * negative space that's transparent, but it has latent RGB values that
 * suddenly become visible while anisotropic filtering is active" - PCSX2's,
 * whose hardware filter averages the colours alone).  The textures have one
 * level (no mipmaps), so the footprint's short axis stays the GS's.  None
 * taken - the footprint no longer than a texel across, the long axis less
 * than twice the short, or the check (ta1.z 1: CbPs::pad1, GT3PC_PGL_ANISO_ZERO;
 * 2 the taps unweighted, a comparison) - the centre's colour.  tcU: the unclamped GS texel coordinates; gxT, gyT:
 * their screen derivatives (ps_body, before any branch). */
float3 aniso_rgb(uint slot, uint fl, float2 tcU, float2 gxT, float2 gyT, float4 tclamp, float4 tr, float3 centre)
{
    const float lx = length(gxT), ly = length(gyT);
    const float minor = max(min(lx, ly), 1.0);
    const int n = (int)min(ceil(max(lx, ly) / minor), ta1.w);
    if (n <= 1 || ta1.z == 1)
        return centre;
    const float2 axis = lx >= ly ? gxT : gyT;
    const float2 across = lx >= ly ? gyT : gxT;     /* PC (2026-11-13): (a replacement's taps: their footprint) */
    const uint am = (fl >> 12) & 3;
    const bool tcc = (fl & 8) != 0;
    float3 sum = 0;
    float wsum = 0;
    [loop] for (int i = 0; i < 16; i++) {
        if (i >= n)
            break;
        const float2 tc = clamp(tcU + axis * ((i + 0.5) / n - 0.5), tclamp.xy, tclamp.zw);
        /* PC (2026-11-13): a replacement texture's tap (the alpha mode 3)
         * through its mip levels, its footprint a tap's share of the long
         * axis by the short one */
        float4 s;
        [branch] if (am == 3)
            s = sample_slot_grad(slot, (tc - tr.xy) * tr.zw, axis / n * tr.zw, across * tr.zw);
        else
            s = sample_slot(slot, (tc - tr.xy) * tr.zw);
        /* (the texel's alpha as ps_body takes it: the stored one, CT16's bit
         * 7 choosing TA1 / TA0, CT24's TA0 alike everywhere; TCC 0 none) */
        float w = 1;
        if (tcc && (am == 0 || am == 3))
            w = s.a;
        else if (tcc && am == 2)
            w = s.a >= 0.5 ? ta1.x : fogTa0.w;
        if (ta1.z == 2)
            w = 1;                          /* (GT3PC_PGL_ANISO_ZERO=2, a comparison: the colours alone averaged,
                                             * as a hardware anisotropic filter does - the latent colours shown) */
        sum += s.rgb * w;
        wsum += w;
    }
    return wsum > 0 ? sum / wsum : centre;
}
PsOut ps_body(float4 pos, float3 stq, float4 col, float fog, float4 tclamp, float4 rect, uint2 tsel, uint rep,
              float hide)
{
    PsOut o;
    /* PC (2026-11-07): inside the hide box (vs_vu1: the cockpit camera's
     * driver's head): a triangle wholly inside goes, one across it is cut
     * where the flag interpolates past a half.  (2: a glass shell's
     * translucent texels, below, once the texture has given the alpha) */
    if (hide > 0.5 && hide < 1.5)
        discard;
#ifdef MERGED_PS
    /* PC: a merged draw's part has its own texture flags (TME, TFX, TCC,
     * the alpha mode, full colours: pgl_d3d.h kPsTexFlags) - the merged
     * draws' variant only (ps_main_merged: the others keep the shader of
     * one texture, the slots' switch cost every pixel ~5 % of the GPU) */
    uint fl = tsel.x != 0xFFFF ? ((flags & ~0x80302Fu) | tsel.y) : flags;     /* (kPsTexFlags; PC 2026-11-10 PSF_ANISO) */
#else
    uint fl = flags;
#endif
    /* PC (2026-11-10): (PSF_ANISO: aniso_rgb) the GS texel coordinates and
     * their screen derivatives, here before any discard or branch
     * (unclamped: the clamp's edge is no edge of the footprint) */
    const float2 tcU = stq.xy / stq.z;
    const float2 gxT = ddx(tcU), gyT = ddy(tcU);
    uint scan = (fl >> 18) & 3;
    if (scan >= 2) {
        uint row = (uint)floor(pos.y / rowScale);
        if ((row & 1) == (scan & 1))
            discard;
    }
    if (fl & (1u << 20)) {
        /* DATE: bit 7 of the destination alpha must equal DATM, else the
         * pixel is not written (colour nor depth) */
        bool set = round(dstCopy.Load(int3(pos.xy, 0)).a * 255.0) >= 128.0;
        if (set != ((fl & (1u << 21)) != 0))
            discard;
    }
    float4 c = floor(col + 0.001);
    if (fl & 1) {
        float2 tc = clamp(stq.xy / stq.z, tclamp.xy, tclamp.zw);
        /* PC: a VU1 draw's rectangle from its record (PSF_INST_RECT) */
        float4 tr = (fl & (1u << 24)) ? rect : texRect;
        float2 uv = (tc - tr.xy) * tr.zw;
        uint am = (fl >> 12) & 3;
        /* PC (2026-11-13): a replacement texture (the alpha mode 3,
         * pgl_texrepl.h): its colours through its mip levels, its alpha
         * from the first level as the game's own textures give theirs - the
         * alpha is a mask as often as a transparency here (the reflection
         * test reads its bit 7), and a mip level's mean of 128 and 125 is
         * neither.  The game's own have one level, as ever */
        float4 t0;
        [branch] if (am == 3) {
            t0 = sample_slot_grad(tsel.x, uv, gxT * tr.zw, gyT * tr.zw);
            t0.a = sample_slot(tsel.x, uv).a;
        } else {
            t0 = sample_slot(tsel.x, uv);
        }
        float4 t = round(t0 * 255.0);
        /* PC (2026-11-10): (PSF_ANISO) the colour over the footprint; the
         * alpha the centre's, the GS's own */
        if (fl & 32u)
            t.rgb = round(aniso_rgb(tsel.x, fl, tcU, gxT, gyT, tclamp, tr, t0.rgb) * 255.0);
        if (am == 1) t.a = fogTa0.w;
        else if (am == 2) t.a = t.a >= 128 ? ta1.x : fogTa0.w;
        else if (am == 3) t.a = round(t0.a * 128.0);    /* PC (2026-11-13): the usual alpha -> the GS's (255 -> 0x80) */
        uint tfx = (fl >> 1) & 3;
        float3 rgb;
        float a;
        if (tfx == 1) rgb = t.rgb;
        else if (fl & (1u << 23)) rgb = floor(t.rgb * c.rgb / 255.0 + 0.5);
        else rgb = floor(t.rgb * c.rgb / 128.0);
        if (tfx >= 2) rgb += c.a;
        if ((fl & 8) == 0) a = c.a;
        else if (tfx == 0) a = floor(t.a * c.a / 128.0);
        else if (tfx == 2) a = t.a + c.a;
        else a = t.a;
        c = min(float4(rgb, a), 255.0);
    }
    /* PC (2026-11-07): a glass shell wound to face the eye (hide 2, the
     * cockpit camera): its translucent texels - the tint - go, its opaque
     * ones (the pillars drawn in the same batch) stay */
    if (hide > 1.5 && c.a < 112.0)
        discard;
    if (fl & 16) {
        float f = clamp(floor(fog), 0, 255);
        c.rgb = floor((f * c.rgb + (255.0 - f) * fogTa0.rgb) / 255.0);
    }
    c = clamp(c, 0, 255);
    uint gate = (fl >> 10) & 3;
    if (gate != 0) {
        bool ok = compare(c.a, aref, (fl >> 6) & 7);
        if (gate == 1 && !ok) discard;
        if (gate == 2 && ok) discard;
    }
    float cf = (fl & (1u << 17)) ? fix / 128.0 : c.a / 128.0;
    float aOut = c.a;
    if ((fl & (1u << 9)) && aOut < 128) aOut += 128;
    uint premul = (fl >> 14) & 7;
    float3 rgbOut = c.rgb / 255.0;
    if (premul == 1) rgbOut = saturate(c.rgb * (1.0 + min(cf, 1.0)) / 255.0);
    else if (premul == 2) rgbOut = saturate(c.rgb / 128.0);
    else if (premul == 3) rgbOut = min(cf, 1.0).xxx;
    else if (premul == 4) rgbOut = 0;
    else if (premul == 5) rgbOut = min(floor(c.rgb * cf), 255.0) / 255.0;   /* (Cs C) >> 7, C up to 2.0 */
    if (fl & (1u << 22)) {
        /* C = Ad (AdMode::Shader, blending off): the GS's ((A - B) Ad >> 7)
         * + D, clamped, with Cd and Ad from the copy of the target (t1) */
        float4 d = round(dstCopy.Load(int3(pos.xy, 0)) * 255.0);
        uint s = (uint)ta1.y;
        uint sa = s & 3, sb = (s >> 2) & 3, sd = (s >> 6) & 3;
        float3 A = sa == 0 ? c.rgb : (sa == 1 ? d.rgb : 0);
        float3 B = sb == 0 ? c.rgb : (sb == 1 ? d.rgb : 0);
        float3 D = sd == 0 ? c.rgb : (sd == 1 ? d.rgb : 0);
        rgbOut = clamp(floor((A - B) * d.a / 128.0) + D, 0, 255) / 255.0;
    }
#ifdef MIRROR_PS
    /* (the mirror pass: nothing written but the stencil, where the alpha's
     * bit 7 is not the one held) */
    if ((aOut >= 128) == (mirrorKeep.x != 0))
        discard;
#endif
#ifdef FOLD_PS
    /* PC (2026-11-05): the mirror pass as the draw's last instance (pgl_d3d.h
     * PSF_MIRROR): the depth test done here against the depth buffer's own
     * value (the same floats and function as the fixed test: a draw with no
     * depth writes), a fragment that fails it discarded; a colour instance's
     * fragment then writes that value as its depth (the fixed test is EQUAL:
     * it passes, its stencil op as ever); the mirror instance's fragment,
     * where the alpha's bit 7 is not the one held, writes another value,
     * fails it, and the depth-fail stencil op marks the mirror (the rest
     * discarded).  The same fragments, tests and marks as the draw and its
     * mirror pass made, in one draw. */
    {
        const float stored = zBuf.Load(int3(pos.xy, 0));
        if ((fl & (1u << 31)) && !compare(pos.z, stored, (fl >> 28) & 7))
            discard;
        const uint mrep = (fl & (1u << 25)) ? 2 : 1;
        if (rep == mrep) {
            if ((aOut >= 128) == ((fl & (1u << 27)) != 0))
                discard;
            o.depth = stored == 1.0 ? 0.0 : 1.0;
        } else {
            o.depth = stored;
        }
    }
#endif
    float a1 = saturate(cf);
    if (fl & (1u << 25)) {
        /* PC (2026-11-05): a C = Ad draw as two instances (PSF_AD_INST): both
         * add Cs x DEST_ALPHA (Ad / 255 each, Ad / 128 together); the alpha
         * equation (o0.a + Ad (1 - o1.a)) leaves Ad for the first instance
         * and writes As for the second - as the two draws did.  (Or as two
         * draws of one instance each with the same state, the second drawn
         * from the record's second copy: rep 1 from it, draw_passes's pairs) */
        if (rep == 0) {
            aOut = 0;
            a1 = 0;
        } else {
            a1 = 1;
        }
    }
    o.c0 = float4(rgbOut, aOut / 255.0);
    o.c1 = float4(0, 0, 0, a1);
    return o;
}
PsOut ps_main(PsIn i) { return ps_body(i.pos, i.stq, i.color, i.fog, i.tclamp, i.rect, i.tsel, i.rep, i.hide); }
PsOut ps_main_flat(PsInFlat i)
{
    return ps_body(i.pos, i.stq, i.color, i.fog, i.tclamp, i.rect, i.tsel, i.rep, i.hide);
}
/* PC (2026-11-04): the alpha mirror brought up to date from the target's
 * copy (t1): a pixel whose alpha has bit 7 set is kept (the stencil op
 * writes 1 there), the others discarded (the stencil cleared to 0 before) */
float4 ps_mirror_sync(float4 pos : SV_Position) : SV_Target0
{
    if (round(dstCopy.Load(int3(pos.xy, 0)).a * 255.0) < 128.0)
        discard;
    return float4(0, 0, 0, 0);
}
/* A Z-format frame drawn from a depth texture (graphics.md 8.3). */
float ps_depth_copy(PsIn i) : SV_Depth
{
    float2 tc = clamp(i.stq.xy / i.stq.z, i.tclamp.xy, i.tclamp.zw);
    float2 uv = (tc - texRect.xy) * texRect.zw;
    return tex0.SampleLevel(samp0, uv, 0).r;
}
#endif

/* ---- Presentation ---------------------------------------------------------- */
#ifdef SEC_PRESENT
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
cbuffer CbPresent : register(b0) {
    float4 srcRect;                 /* u0, v0, u1, v1 */
    float4 taps;                    /* the step between taps (u, v), the taps a side (x, y; 0 / 1: one) */
};
float4 ps_present(FullOut i) : SV_Target
{
    float2 uv = lerp(srcRect.xy, srcRect.zw, i.uv);
    int nx = max(1, (int)taps.z), ny = max(1, (int)taps.w);
    if (nx == 1 && ny == 1)
        return float4(tex0.SampleLevel(samp0, uv, 0).rgb, 1);
    /* the screen pixel's part of a larger picture, averaged (supersampling) */
    float2 o = uv - taps.xy * (float2(nx, ny) - 1.0) * 0.5;
    float3 sum = 0;
    [loop] for (int y = 0; y < ny; y++)
        [loop] for (int x = 0; x < nx; x++)
            sum += tex0.SampleLevel(samp0, o + taps.xy * float2(x, y), 0).rgb;
    return float4(sum / (nx * ny), 1);
}
#endif

/* ---- pfx_color_correction (graphics.md 8.5) ----------------------------- */
#ifdef SEC_PFX
Texture2D tex0 : register(t0);
Texture2D curveTex : register(t1);
cbuffer CbPfx : register(b0) {
    float4 colR;                    /* the matrix column of the R input (bytes) */
    float4 colG;
    float4 colB;
    float4 srcScale;                /* source pixels per destination pixel */
};
float3 curve(float i) { return round(curveTex.Load(int3((int)i, 0, 0)).rgb * 255.0); }
float4 ps_pfx_color(FullOut i) : SV_Target
{
    int2 sp = int2(i.pos.xy * srcScale.xy);
    float3 s = round(tex0.Load(int3(sp, 0)).rgb * 255.0);
    /* three sprite passes, each clamped (R, then + B, then + G) */
    float3 p1 = min(floor(curve(s.r) * colR.rgb / 128.0), 255.0);
    float3 p2 = min(p1 + floor(curve(s.b) * colB.rgb / 128.0), 255.0);
    float3 p3 = min(p2 + floor(curve(s.g) * colG.rgb / 128.0), 255.0);
    return float4(p3 / 255.0, 0);
}
#endif

/* ---- A byte plane read through a palette (graphics.md 8.6) -------------- */
#ifdef SEC_PLANE
Texture2D capTex : register(t0);            /* the captured frame (GS bytes) */
Texture2D palTex : register(t1);            /* the palette, n x 1 */
cbuffer CbPlane : register(b0) {
    int4 sel;                               /* channel, shift, mask */
};
float4 ps_plane(FullOut i) : SV_Target
{
    float4 s = capTex.Load(int3(i.pos.xy, 0));
    float b = sel.x == 0 ? s.r : sel.x == 1 ? s.g : s.b;
    uint idx = ((uint)round(b * 255.0) >> (uint)sel.y) & (uint)sel.z;
    return palTex.Load(int3((int)idx, 0, 0));
}
#endif

/* ---- A lens-flare occlusion query (pgl_flare.cpp, docs/renderer.md 8) --- */
#ifdef SEC_FLARE
Texture2D zTex : register(t0);              /* the screen's depth target (Z x zNorm) */
cbuffer CbFlare : register(b0) {
    int4 box;                               /* the copy's origin x0, y0; columns, rows */
    int4 rect[5];                           /* the shape's rectangles, x1 / y1 exclusive */
    int4 misc;                              /* nRects, ztest, Cs, halfY */
    float4 zs;                              /* native per GS pixel x / y, the shape's Z, - */
    int4 scr;                               /* the screen's GS width, height */
    float4 wx4[17];                         /* each copied column's weight in the final texel */
    float4 wy4[17];                         /* each copied row's */
};
/* The shape's alpha at a screen pixel after its depth-tested draw: 255
 * where it passed, 0 where it did not or outside it (the cleared box). */
float shape_alpha(int gx, int gy)
{
    gx = clamp(gx, 0, scr.x - 1);
    gy = clamp(gy, 0, scr.y - 1);
    bool inside = false;
    for (int r = 0; r < misc.x; r++)
        inside = inside || (gx >= rect[r].x && gx < rect[r].z && gy >= rect[r].y && gy < rect[r].w);
    if (!inside)
        return 0;
    int fy = misc.w != 0 ? gy / 2 : gy;
    float sz = zTex.Load(int3(int((gx + 0.5) * zs.x), int((fy + 0.5) * zs.y), 0)).r;
    bool ok = misc.y == 1 || (misc.y == 2 && zs.z >= sz) || (misc.y == 3 && zs.z > sz);
    return ok ? 255 : 0;
}
/* PC: the query as the GS computed it, without its emulation.  On the PS2 the
 * box's alpha is cleared, the shape drawn depth-tested at alpha 2.0 (0xFF),
 * the box copied 1:1 into the work buffer, halved in place to one texel by
 * bilinear sprites, and Cs x As >> 7 stored into the result texel.  The
 * halvings are linear: the final alpha is a weighted sum of the copied
 * pixels, the weights (exec_flare_query, the GS's own sequence, odd sizes and
 * in-place reads included) per axis.  Here each pixel's alpha is the depth
 * test at its GS pixel centre in the native depth, whatever the output
 * scale.  RGB = (Cs A) >> 7, alpha A, the texel the game reads back. */
float4 ps_flare(FullOut i) : SV_Target
{
    float sum = 0;
    for (int y = 0; y < box.w; y++) {
        float wy = wy4[y >> 2][y & 3];
        if (wy == 0)
            continue;
        for (int x = 0; x < box.z; x++) {
            float wx = wx4[x >> 2][x & 3];
            if (wx != 0)
                sum += wx * wy * shape_alpha(box.x + x, box.y + y);
        }
    }
    uint a = (uint)clamp(floor(sum + 0.5), 0, 255);
    uint c = min((uint)misc.z * a >> 7, 255u);
    return float4(c, c, c, a) / 255.0;
}

/* PC (2026-11-05): a frame's queries as one batch (pgl_d3d.h FlareRec,
 * flare_batch_draw; pgl_exec.cpp flare_queries_flush) instead of ps_flare's
 * loop in one pixel a query: a point per GS pixel of each query's box, its
 * weighted term added into the query's texel of a float scratch (the points
 * blend in primitive order - the loop's order: the same sums), then a point
 * per query converting its sum into the strip's texel as ps_flare does.
 * (ps_flare took 37 us of a GTX 1660 SUPER for one 10 x 10 lamp query,
 * serial in one thread with the next sprite's depth test waiting for it:
 * 5.7 ms of a night race's frame; the batch of the same queries 0.06 ms.) */
cbuffer CbFlareBatch : register(b1) {
    float4 fzs;                             /* native depth pixels per GS pixel x, y */
    int4 fscr;                              /* the screen's GS width, height, halfY; the draw's first record */
    int4 fstrip;                            /* the strip's width, height */
};
Texture2D flareZ : register(t3);            /* the screen's depth target */
Buffer<float4> flareWts : register(t4);     /* FlareWeights: 16 float4 of wx, 16 of wy a set */
Buffer<float4> flareRecs : register(t5);    /* FlareRec: 8 float4 a query */
Texture2D<float> flareSum : register(t6);   /* the sums */
struct FlarePt { float4 pos : SV_Position; float term : TEXCOORD0; };
struct FlareConv { float4 pos : SV_Position; float4 color : COLOR0; };
float4 flare_texel_pos(float2 t)
{
    return float4((t + 0.5) / float2(fstrip.xy) * float2(2, -2) + float2(-1, 1), 0, 1);
}
FlarePt vs_flare_pt(uint id : SV_VertexID, uint inst : SV_InstanceID)
{
    const int at = (fscr.w + (int)inst) * 8;
    const int4 box = asint(flareRecs.Load(at));
    const int4 m = asint(flareRecs.Load(at + 6));          /* nRects, ztest, Cs, weights */
    const float4 zt = flareRecs.Load(at + 7);               /* Z, texel x, texel y */
    const int x = (int)id % box.z, y = (int)id / box.z;     /* row by row, as the loop */
    FlarePt o;
    o.pos = flare_texel_pos(zt.yz);
    o.term = 0;
    const int gx = clamp(box.x + x, 0, fscr.x - 1), gy = clamp(box.y + y, 0, fscr.y - 1);
    bool inside = false;
    for (int r = 0; r < m.x; r++) {
        const int4 rc = asint(flareRecs.Load(at + 1 + r));
        inside = inside || (gx >= rc.x && gx < rc.z && gy >= rc.y && gy < rc.w);
    }
    if (!inside)
        return o;
    const int fy = fscr.z != 0 ? gy / 2 : gy;
    const float sz = flareZ.Load(int3(int((gx + 0.5) * fzs.x), int((fy + 0.5) * fzs.y), 0)).r;
    const bool ok = m.y == 1 || (m.y == 2 && zt.x >= sz) || (m.y == 3 && zt.x > sz);
    const float wx = flareWts.Load(m.w * 32 + (x >> 2))[x & 3];
    const float wy = flareWts.Load(m.w * 32 + 16 + (y >> 2))[y & 3];
    o.term = wx * wy * (ok ? 255.0 : 0.0);
    return o;
}
float4 ps_flare_pt(FlarePt i) : SV_Target
{
    return float4(i.term, 0, 0, 0);
}
FlareConv vs_flare_conv(uint id : SV_VertexID)
{
    const int at = (fscr.w + (int)id) * 8;
    const int4 m = asint(flareRecs.Load(at + 6));
    const float4 zt = flareRecs.Load(at + 7);
    FlareConv o;
    o.pos = flare_texel_pos(zt.yz);
    const float sum = flareSum.Load(int3(int2(zt.yz), 0));
    const uint a = (uint)clamp(floor(sum + 0.5), 0, 255);
    const uint c = min((uint)m.z * a >> 7, 255u);
    o.color = float4(c, c, c, a) / 255.0;
    return o;
}
float4 ps_flare_conv(FlareConv i) : SV_Target
{
    return i.color;
}
#endif

/* ---- A multisampled depth target's resolve (docs/renderer.md "Anti-aliasing") */
#ifdef SEC_DRESOLVE
Texture2DMS<float> zMs : register(t0);
float ps_depth_resolve(FullOut i) : SV_Depth
{
    return zMs.Load(int2(i.pos.xy), 0);     /* sample 0 */
}
#endif

/* ---- A multisampled colour target's resolve where the scissor lets it (the
 * part written since its copy: target_copy_srv): the samples' mean, as
 * ResolveSubresource gives it */
#ifdef SEC_CRESOLVE
Texture2DMS<float4> cMs : register(t0);
float4 ps_color_resolve(FullOut i) : SV_Target
{
    uint w, h, n;
    cMs.GetDimensions(w, h, n);
    float4 s = 0;
    for (uint k = 0; k < n; k++)
        s += cMs.Load(int2(i.pos.xy), k);
    return s / (float)n;
}
#endif

/* ---- FXAA over a race view's 3D picture (docs/renderer.md "Anti-aliasing") --
 * FXAA 3.11's quality algorithm (Timothy Lottes, NVIDIA): luma edges found
 * from the 3 x 3 neighbourhood, the edge's span searched both ways along it
 * (steps 1, 1.5, 2, 2, 2, 2, 4, 8 pixels), the pixel resampled across the
 * edge by its place in the span; sub-pixel aliasing blended by 1/2 (FXAA's
 * default 3/4 softens textures more).  Luma from the colour (the alpha byte
 * is the GS's own: kept). */
#ifdef SEC_FXAA
Texture2D tex0 : register(t0);              /* a copy of the picture */
SamplerState samp0 : register(s0);          /* linear, clamped */
cbuffer CbFxaa : register(b0) {
    float4 rcpFrame;                        /* 1 / width, 1 / height (native pixels) */
};
static const float3 kLuma = float3(0.299, 0.587, 0.114);
float fxaa_luma(float2 uv) { return dot(tex0.SampleLevel(samp0, uv, 0).rgb, kLuma); }
float4 ps_fxaa(FullOut i) : SV_Target
{
    const float subpix = 0.5, edgeThreshold = 0.125, edgeThresholdMin = 0.0625;
    float2 posM = i.uv;
    float4 rgbaM = tex0.SampleLevel(samp0, posM, 0);
    float lumaM = dot(rgbaM.rgb, kLuma);
    float lumaS = dot(tex0.SampleLevel(samp0, posM, 0, int2(0, 1)).rgb, kLuma);
    float lumaE = dot(tex0.SampleLevel(samp0, posM, 0, int2(1, 0)).rgb, kLuma);
    float lumaN = dot(tex0.SampleLevel(samp0, posM, 0, int2(0, -1)).rgb, kLuma);
    float lumaW = dot(tex0.SampleLevel(samp0, posM, 0, int2(-1, 0)).rgb, kLuma);
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(edgeThresholdMin, rangeMax * edgeThreshold))
        return rgbaM;
    float lumaNW = dot(tex0.SampleLevel(samp0, posM, 0, int2(-1, -1)).rgb, kLuma);
    float lumaSE = dot(tex0.SampleLevel(samp0, posM, 0, int2(1, 1)).rgb, kLuma);
    float lumaNE = dot(tex0.SampleLevel(samp0, posM, 0, int2(1, -1)).rgb, kLuma);
    float lumaSW = dot(tex0.SampleLevel(samp0, posM, 0, int2(-1, 1)).rgb, kLuma);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcpFrame.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) lumaN = lumaW;
    if (!horzSpan) lumaS = lumaE;
    if (horzSpan) lengthSign = rcpFrame.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = posM;
    float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float subpixE = subpixC * subpixC;
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    float lumaEndN = fxaa_luma(posN) - lumaNN * 0.5;
    float lumaEndP = fxaa_luma(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    const float steps[7] = {1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    [loop] for (int s = 0; s < 7 && !(doneN && doneP); s++) {
        if (!doneN) {
            posN -= offNP * steps[s];
            lumaEndN = fxaa_luma(posN) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP) {
            posP += offNP * steps[s];
            lumaEndP = fxaa_luma(posP) - lumaNN * 0.5;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-1.0 / spanLength) + 0.5;
    float subpixH = subpixG * subpix;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    return float4(tex0.SampleLevel(samp0, posM, 0).rgb, rgbaM.a);
}
#endif
)HLSL";

struct Shaders {
    Com<ID3D11VertexShader> vsGs, vsVu1, vsFull;
    Com<ID3D11VertexShader> vsVu1Direct;    /* PC: vs_vu1 and the projection, no clipper (Vs::Vu1Direct) */
    Com<ID3D11GeometryShader> gsVu1;        /* the VU1 clipper */
    Com<ID3D11PixelShader> psMain, psMainFlat, psDepthCopy, psPresent, psPfxColor, psPlane, psFlare;
    Com<ID3D11PixelShader> psMainMerged, psMainFlatMerged;     /* PC: a merged VU1 draw's (draw_merged) */
    Com<ID3D11PixelShader> psDepthResolve, psFxaa, psColorResolve;
    /* PC (2026-11-04): the alpha mirror's (mirror_ps, Ps::MirrorSync) and
     * its two constant buffers (mirror_cb) */
    Com<ID3D11PixelShader> psMainMirror, psMainFlatMirror, psMainMirrorMerged, psMainFlatMirrorMerged, psMirrorSync;
    Com<ID3D11Buffer> cbMirror[2];
    /* PC (2026-11-05): the mirror pass folded into the draw (FOLD_PS: Ps::MainFold / MainFlatFold, draw_merged) */
    Com<ID3D11PixelShader> psMainFold, psMainFlatFold, psMainFoldMerged, psMainFlatFoldMerged;
    /* PC (2026-11-15): AGSS - the same shaded at every sample (SAMPLE_PS: Ps::MainSs / MainFlatSs, their merged
     * and mirror pass variants; null where the device has no ps_4_1) */
    Com<ID3D11PixelShader> psMainSs, psMainFlatSs, psMainSsMerged, psMainFlatSsMerged;
    Com<ID3D11PixelShader> psMainSsMirror, psMainFlatSsMirror, psMainSsMirrorMerged, psMainFlatSsMirrorMerged;
    /* PC (2026-11-05): the flare queries' batch (flare_batch_draw) */
    Com<ID3D11VertexShader> vsFlarePt, vsFlareConv;
    Com<ID3D11PixelShader> psFlarePt, psFlareConv;
    Com<ID3D11InputLayout> ilGs, ilVu1;
    Com<ID3D11Buffer> cbVs, cbPs;
    Com<ID3D11Buffer> cbFlare;              /* ps_flare's (larger than cbPs) */
    size_t cbVsSize = 0, cbPsSize = 0;
} g_sh;

/* PC (2026-11-05): the binding cache and the rings below are the submitting
 * thread's own (thread_local): the executor's when it submits itself, the
 * submit thread's, the submit workers' (pgl_exec.cpp run_work) */
thread_local Vs g_curVs = Vs::Full;
thread_local bool g_vsBound = false;

/* What is bound (d3d_invalidate forgets it). */
struct Bound {
    bool valid = false;
    ID3D11PixelShader *ps = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    int vpW = 0, vpH = 0;
    D3D11_RECT scissor = {-1, -1, -1, -1};
    ID3D11RasterizerState *raster = nullptr;
    ID3D11BlendState *blend = nullptr;
    ID3D11DepthStencilState *depth = nullptr;
    uint8_t stencilRef = 0;                 /* PC: (the alpha mirror: set_depth) */
    ID3D11ShaderResourceView *srv0 = nullptr;
    ID3D11ShaderResourceView *srv1 = nullptr;
    ID3D11ShaderResourceView *srvZ = nullptr;   /* PC (2026-11-05): slot 17, the depth buffer (set_srv_z) */
    ID3D11SamplerState *samp0 = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer *vb = nullptr;
    UINT vbStride = 0;
    ID3D11Buffer *ib = nullptr;
    bool inst = false;                      /* PC: the records' stream on slot 1 (vu1_inst) */
    ID3D11Buffer *bvb = nullptr;            /* PC: slot 2 (set_batch_vb) */
    uint8_t vsCb[sizeof(CbVu1)];
    size_t vsCbBytes = 0;
    uint8_t psCb[256];
    size_t psCbBytes = 0;
};
thread_local Bound g_bound;

/* PC (2026-11-03): the VU1 draws' records (pgl_d3d.h Vu1Inst, vu1_inst).
 * The executor writes record `next' at cpu[next % kVu1Insts] and publishes
 * the count (release: the D3D thread reads the records below it); the D3D
 * thread copies them to the GPU's ring as draws need them (uploaded: the
 * records there, by number; cycle: the ring's cycle it holds).  The executor
 * reuses a place only after the D3D thread copied what was there
 * (uploadedSeen: its last look). */
struct InstRing {
    Com<ID3D11Buffer> buf;
    Com<ID3D11ShaderResourceView> srv;      /* PC: (a merged draw's parts: vs_vu1's instRecs) */
    std::unique_ptr<Vu1Inst[]> cpu;
    uint32_t next = 0;                      /* (the executor's) */
    uint32_t uploadedSeen = 0;
    Vu1Inst last = {};
    bool lastValid = false;
    uint32_t lastCopies = 1;                /* PC (2026-11-05): the last record's places (vu1_inst copies) */
    std::atomic<uint32_t> published{0};
    std::atomic<uint32_t> uploaded{0};
    uint32_t up = 0;                        /* (the D3D thread's) */
    uint32_t cycle = ~0u;
};

/* PC (2026-11-04): the lighting blocks' ring (pgl_d3d.h light_block), run as
 * the records' ring is: the executor writes block `next' at cpu[next %
 * kLightBlocks] and publishes the count; the D3D thread copies the blocks a
 * draw may read to the GPU's ring before the draw (light_upload: up to the
 * draw's mark), DISCARD at a new cycle.  A block is written once while it is
 * the last, or found in the memo (the material of a car's part comes round
 * every few parts): memo - the block's hash and number, checked against the
 * ring's copy. */
struct LightRing {
    Com<ID3D11Buffer> buf;
    Com<ID3D11ShaderResourceView> srv;
    std::unique_ptr<float[]> cpu;           /* kLightBlocks x 64 floats */
    uint32_t next = 0;                      /* (the executor's) */
    uint32_t uploadedSeen = 0;
    struct Memo { uint32_t hash, seq; } memo[256];
    std::atomic<uint32_t> published{0};
    std::atomic<uint32_t> uploaded{0};
    uint32_t up = 0;                        /* (the D3D thread's) */
};

/* PC (2026-11-03): what slots 1.. of the merged draws hold (PS t2.. / s1..:
 * draw_merged; the D3D thread's - cleared by d3d_invalidate / d3d_forget) */
struct MergedSlots {
    ID3D11ShaderResourceView *srv[kMergeSlots] = {};
    ID3D11SamplerState *samp[kMergeSlots] = {};
};
MergedSlots g_mergedSlots;

/* The constants' ring (see the file comment); vs / ps: where the bound
 * constants lie in it (first constant, count: 16 bytes each). */
struct CbRing {
    Com<ID3D11Buffer> buf;
    Com<ID3D11DeviceContext1> dc1;
    uint32_t size = 0, pos = 0;
    UINT vsFirst = 0, vsNum = 0, psFirst = 0, psNum = 0;
    bool on = false;
};
constexpr uint32_t kRingBytes = 4u << 20;

}  /* namespace */

/* PC (2026-11-05): a submitting thread's rings (pgl_d3d.h thread_rings): the
 * records', the lighting blocks' and the constants' - made as the thread
 * starts (rings_init_thread), the D3D thread writing the GPU's copies through
 * the pointer the draws' calls carry */
struct Rings {
    InstRing inst;
    LightRing lights;
    CbRing cb;
};

namespace {
thread_local Rings t_rings;
/* (what the device allows, found once: d3d_init) */
bool g_instSrvOk = false, g_lightRingOn = false, g_cbRingOk = false;
Com<ID3D11DeviceContext1> g_dc1;
Com<ID3D11Buffer> make_cb(size_t bytes);    /* (below) */
Com<ID3D11Buffer> g_bvbZeros;               /* (set_batch_vb: the stream of zeros for no batches; made in d3d_init) */
}  /* namespace */

Rings *thread_rings()
{
    return &t_rings;
}

namespace {

/* The calling thread's rings made (once) */
void rings_init_thread()
{
    Rings &r = t_rings;
    if (r.inst.cpu)
        return;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = kVu1Insts * (UINT)sizeof(Vu1Inst);
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER | (g_instSrvOk ? D3D11_BIND_SHADER_RESOURCE : 0u);
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev()->CreateBuffer(&d, nullptr, r.inst.buf.put());
    if (g_instSrvOk && r.inst.buf) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.FirstElement = 0;
        v.Buffer.NumElements = kVu1Insts * (UINT)(sizeof(Vu1Inst) / 16);
        dev()->CreateShaderResourceView(r.inst.buf.get(), &v, r.inst.srv.put());
    }
    r.inst.cpu.reset(new Vu1Inst[kVu1Insts]);
    if (g_lightRingOn) {
        D3D11_BUFFER_DESC ld = {};
        ld.ByteWidth = kLightBlocks * 256u;
        ld.Usage = D3D11_USAGE_DYNAMIC;
        ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ld.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(dev()->CreateBuffer(&ld, nullptr, r.lights.buf.put()))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
            v.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.FirstElement = 0;
            v.Buffer.NumElements = kLightBlocks * 16u;
            if (FAILED(dev()->CreateShaderResourceView(r.lights.buf.get(), &v, r.lights.srv.put())))
                r.lights.buf.reset();
        }
        if (r.lights.srv) {
            r.lights.cpu.reset(new float[(size_t)kLightBlocks * 64]);
            memset(r.lights.memo, 0xFF, sizeof(r.lights.memo));
        }
    }
    if (g_cbRingOk && g_dc1) {
        r.cb.dc1 = g_dc1;
        r.cb.buf = make_cb(kRingBytes);
        r.cb.size = kRingBytes;
        r.cb.pos = kRingBytes;              /* (the first write discards) */
        r.cb.on = (bool)r.cb.buf;
    }
}

}  /* namespace */

void d3d_thread_init()
{
    rings_init_thread();
}

namespace {

/* (variants: up to three more macros; fatal: the game stops when it does not
 * compile, else null is returned and the reason logged) */
Com<ID3DBlob> compile(const char *section, const char *entry, const char *target, const char *v1 = nullptr,
                      const char *v2 = nullptr, const char *v3 = nullptr, bool fatal = true)
{
#ifdef _WIN32
    Com<ID3DBlob> code, errors;
    D3D_SHADER_MACRO defs[5] = {{section, "1"}};
    int n = 1;
    for (const char *v : {v1, v2, v3})
        if (v != nullptr)
            defs[n++] = D3D_SHADER_MACRO{v, "1"};
    defs[n] = D3D_SHADER_MACRO{nullptr, nullptr};
    HRESULT hr = D3DCompile(kHlsl, strlen(kHlsl), "pgl_shaders.hlsl", defs, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr)) {
        if (fatal)
            pclog::fatal("pgl: shader %s does not compile:\n%s", entry,
                         errors ? (const char *)errors->GetBufferPointer() : "?");
        pclog::write("pgl: shader %s (%s%s%s) does not compile:\n%s", entry, v1 ? v1 : "", v2 ? " " : "", v2 ? v2 : "",
                     errors ? (const char *)errors->GetBufferPointer() : "?");
        code.reset();
    }
    return code;
#else
    /* (the blob compiled offline from this kHlsl with the same name, section,
     * entry, target and macros: shader_blobs.h, tools/shader_blobs) */
    static const uint64_t kHash = shader_blobs::source_hash(kHlsl, strlen(kHlsl));
    std::string macros = section;
    for (const char *v : {v1, v2, v3})
        if (v != nullptr)
            macros.append(" ").append(v);
    Com<ID3DBlob> code;
    const shader_blobs::Blob *b = shader_blobs::find("pgl_shaders.hlsl", entry, target, macros.c_str());
    const char *missing = b == nullptr ? "not in shader_blobs.h"
                        : b->source_hash != kHash ? "shader_blobs.h was made from another HLSL (regenerate it)"
                                                  : nullptr;
    if (missing != nullptr) {
        if (fatal)
            pclog::fatal("pgl: shader %s (%s %s): %s", entry, target, macros.c_str(), missing);
        pclog::write("pgl: shader %s (%s %s): %s", entry, target, macros.c_str(), missing);
        return code;
    }
    *code.put() = new shader_blobs::BlobRef(*b);
    return code;
#endif
}

Com<ID3D11Buffer> make_cb(size_t bytes)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (UINT)((bytes + 15) & ~(size_t)15);
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Buffer> b;
    dev()->CreateBuffer(&d, nullptr, b.put());
    return b;
}

void update_cb(ID3D11Buffer *b, const void *data, size_t bytes)
{
    /* (queued: the D3D thread) */
    q::run_with(data, bytes, [b, bytes](const uint8_t *p) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = qdc()->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (SUCCEEDED(hr)) {
            memcpy(m.pData, p, bytes);
            qdc()->Unmap(b, 0);
        } else {
            log_once("pgl-cb-map", "pgl: a constant buffer could not be mapped (0x%08lX): the draw used its old"
                     " contents", (unsigned long)hr);
        }
    });
}

/* GT3PC_FLICKER: the CPU's copy of what the streams hold (stream_put),
 * for d3d_verify_copy. */
std::vector<uint8_t> g_vshadow, g_ishadow;

/* State caches.  PC (2026-11-05): used by two stages at once (the executor's
 * passes and the submit thread's draws): the maps under a lock, the fast
 * tables in front of them written with the one value a key has (a torn read
 * is impossible: aligned pointers), the last-blend memo per thread. */
std::unordered_map<uint64_t, Com<ID3D11BlendState>> g_blend;
std::unordered_map<uint32_t, Com<ID3D11DepthStencilState>> g_depth;
std::unordered_map<uint32_t, Com<ID3D11RasterizerState>> g_raster;
std::unordered_map<uint32_t, Com<ID3D11SamplerState>> g_samplers;
SRWLOCK g_stateLock = SRWLOCK_INIT;
struct StateLock {
    StateLock() { AcquireSRWLockExclusive(&g_stateLock); }
    ~StateLock() { ReleaseSRWLockExclusive(&g_stateLock); }
};
/* PC (CPU time, 2026-11-03): the states of the small keys by index too, and
 * the last blend state (a map lookup hashed its key for every draw state) */
ID3D11DepthStencilState *g_depthFast[512];  /* PC: (with the stencil's part of the key) */
ID3D11SamplerState *g_samplerFast[64];
ID3D11RasterizerState *g_rasterFast[8];     /* PC (2026-11-05): (cull | scissor << 2) */
thread_local uint64_t t_blendLastKey = ~0ull;
thread_local ID3D11BlendState *t_blendLast;

/* Dynamic streams: one buffer each, written forward with NO_OVERWRITE and
 * restarted with DISCARD when full. */
struct Stream {
    Com<ID3D11Buffer> buf;
    uint32_t size = 0;
    uint32_t pos = 0;
    bool index = false;
};
Stream g_vstream, g_istream;

void stream_grow(Stream &s, uint32_t bytes)
{
    uint32_t size = s.size ? s.size : (s.index ? (1u << 20) : (4u << 20));
    while (size < bytes)
        size *= 2;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = s.index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s.buf.reset();
    dev()->CreateBuffer(&d, nullptr, s.buf.put());
    s.size = size;
    s.pos = 0;
}

uint32_t stream_put(Stream &s, const void *data, uint32_t bytes, uint32_t align)
{
    uint32_t at = (s.pos + align - 1) / align * align;
    bool discard = false;
    if (!s.buf || bytes > s.size) {
        stream_grow(s, bytes);
        at = 0;
        discard = true;
        if (flicker_on())
            flicker_event("the %s stream made: %u bytes", s.index ? "index" : "vertex", s.size);
    } else if (at + bytes > s.size) {
        if (flicker_on())
            flicker_event("the %s stream restarted (DISCARD) at %u of %u bytes, for %u", s.index ? "index" : "vertex",
                          at, s.size, bytes);
        at = 0;
        discard = true;
    }
    /* (queued: the D3D thread maps the stream and copies the data along) */
    const D3D11_MAP mode = discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
    ID3D11Buffer *buf = s.buf.get();
    const bool index = s.index;
    q::run_with(data, bytes, [buf, mode, at, bytes, index](const uint8_t *p) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = qdc()->Map(buf, 0, mode, 0, &m);
        if (FAILED(hr)) {
            log_once("pgl-stream-map", "pgl: a %s stream could not be mapped (0x%08lX)", index ? "index" : "vertex",
                     (unsigned long)hr);
            return;
        }
        memcpy((uint8_t *)m.pData + at, p, bytes);
        qdc()->Unmap(buf, 0);
    });
    if (flicker_on()) {
        std::vector<uint8_t> &sh = s.index ? g_ishadow : g_vshadow;
        if (sh.size() != s.size)
            sh.assign(s.size, 0);
        memcpy(sh.data() + at, data, bytes);
    }
    s.pos = at + bytes;
    return at;
}

}  /* namespace */

void d3d_init()
{
    Com<ID3DBlob> vsGs = compile("SEC_GS", "vs_gs", "vs_4_0");
    Com<ID3DBlob> vsVu1 = compile("SEC_VU1", "vs_vu1", "vs_4_0");
    Com<ID3DBlob> vsFull = compile("SEC_FULL", "vs_full", "vs_4_0");
    Com<ID3DBlob> gsVu1 = compile("SEC_VU1", "gs_vu1", "gs_4_0");
    Com<ID3DBlob> vsVu1Direct = compile("SEC_VU1", "vs_vu1_direct", "vs_4_0");
    {
        /* PC (2026-11-05): the flare queries' batch (flare_batch_draw) */
        Com<ID3DBlob> pt = compile("SEC_FLARE", "vs_flare_pt", "vs_4_0");
        Com<ID3DBlob> cv = compile("SEC_FLARE", "vs_flare_conv", "vs_4_0");
        dev()->CreateVertexShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, g_sh.vsFlarePt.put());
        dev()->CreateVertexShader(cv->GetBufferPointer(), cv->GetBufferSize(), nullptr, g_sh.vsFlareConv.put());
    }
    dev()->CreateVertexShader(vsGs->GetBufferPointer(), vsGs->GetBufferSize(), nullptr, g_sh.vsGs.put());
    dev()->CreateVertexShader(vsVu1->GetBufferPointer(), vsVu1->GetBufferSize(), nullptr, g_sh.vsVu1.put());
    dev()->CreateVertexShader(vsVu1Direct->GetBufferPointer(), vsVu1Direct->GetBufferSize(), nullptr,
                              g_sh.vsVu1Direct.put());
    dev()->CreateVertexShader(vsFull->GetBufferPointer(), vsFull->GetBufferSize(), nullptr, g_sh.vsFull.put());
    dev()->CreateGeometryShader(gsVu1->GetBufferPointer(), gsVu1->GetBufferSize(), nullptr, g_sh.gsVu1.put());
    struct {
        const char *section;
        const char *entry;
        Com<ID3D11PixelShader> *out;
        const char *target;
        const char *variant;
        const char *variant2;
        const char *variant3;
        bool optional;                      /* PC (2026-11-15): (not made on a device without its target: no stop) */
    } ps[] = {
        {"SEC_PS", "ps_main", &g_sh.psMain, "ps_4_0"},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlat, "ps_4_0"},
        {"SEC_PS", "ps_main", &g_sh.psMainMerged, "ps_4_0", "MERGED_PS"},     /* PC: (draw_merged) */
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatMerged, "ps_4_0", "MERGED_PS"},
        {"SEC_PS", "ps_main", &g_sh.psMainMirror, "ps_4_0", "MIRROR_PS"},     /* PC: (mirror_ps) */
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatMirror, "ps_4_0", "MIRROR_PS"},
        {"SEC_PS", "ps_main", &g_sh.psMainMirrorMerged, "ps_4_0", "MIRROR_PS", "MERGED_PS"},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatMirrorMerged, "ps_4_0", "MIRROR_PS", "MERGED_PS"},
        {"SEC_PS", "ps_mirror_sync", &g_sh.psMirrorSync, "ps_4_0"},
        {"SEC_PS", "ps_main", &g_sh.psMainFold, "ps_4_0", "FOLD_PS"},         /* PC (2026-11-05): the mirror folded in */
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatFold, "ps_4_0", "FOLD_PS"},
        {"SEC_PS", "ps_main", &g_sh.psMainFoldMerged, "ps_4_0", "FOLD_PS", "MERGED_PS"},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatFoldMerged, "ps_4_0", "FOLD_PS", "MERGED_PS"},
        {"SEC_PS", "ps_depth_copy", &g_sh.psDepthCopy, "ps_4_0"},
        {"SEC_PRESENT", "ps_present", &g_sh.psPresent, "ps_4_0"},
        {"SEC_PFX", "ps_pfx_color", &g_sh.psPfxColor, "ps_4_0"},
        {"SEC_PLANE", "ps_plane", &g_sh.psPlane, "ps_4_0"},
        {"SEC_FLARE", "ps_flare", &g_sh.psFlare, "ps_4_0"},
        {"SEC_FLARE", "ps_flare_pt", &g_sh.psFlarePt, "ps_4_0"},        /* PC (2026-11-05): the queries' batch */
        {"SEC_FLARE", "ps_flare_conv", &g_sh.psFlareConv, "ps_4_0"},
        {"SEC_DRESOLVE", "ps_depth_resolve", &g_sh.psDepthResolve, "ps_4_1"},    /* (a Texture2DMS of any count) */
        {"SEC_FXAA", "ps_fxaa", &g_sh.psFxaa, "ps_4_0"},
        {"SEC_CRESOLVE", "ps_color_resolve", &g_sh.psColorResolve, "ps_4_1"},    /* (a Texture2DMS of any count) */
        /* PC (2026-11-15): AGSS's (SAMPLE_PS; ps_4_1: the sample interpolation) */
        {"SEC_PS", "ps_main", &g_sh.psMainSs, "ps_4_1", "SAMPLE_PS", nullptr, nullptr, true},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatSs, "ps_4_1", "SAMPLE_PS", nullptr, nullptr, true},
        {"SEC_PS", "ps_main", &g_sh.psMainSsMerged, "ps_4_1", "SAMPLE_PS", "MERGED_PS", nullptr, true},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatSsMerged, "ps_4_1", "SAMPLE_PS", "MERGED_PS", nullptr, true},
        {"SEC_PS", "ps_main", &g_sh.psMainSsMirror, "ps_4_1", "SAMPLE_PS", "MIRROR_PS", nullptr, true},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatSsMirror, "ps_4_1", "SAMPLE_PS", "MIRROR_PS", nullptr, true},
        {"SEC_PS", "ps_main", &g_sh.psMainSsMirrorMerged, "ps_4_1", "SAMPLE_PS", "MIRROR_PS", "MERGED_PS", true},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlatSsMirrorMerged, "ps_4_1", "SAMPLE_PS", "MIRROR_PS", "MERGED_PS", true},
    };
    for (auto &p : ps) {
        /* (PC 2026-11-15: an optional one that does not compile is logged and
         * left out - AGSS then not offered - instead of stopping the game) */
        Com<ID3DBlob> b = compile(p.section, p.entry, p.target, p.variant, p.variant2, p.variant3, !p.optional);
        if (!b)
            continue;
        const HRESULT hr = dev()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, p.out->put());
        if (FAILED(hr) && p.optional)
            p.out->reset();
    }
    pclog::write("pgl: AGSS (per-sample shading) %s", per_sample_shaders() ? "available"
                                                                            : "not available (no ps_4_1: feature level"
                                                                              " below 10.1)");
    D3D11_INPUT_ELEMENT_DESC gsLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev()->CreateInputLayout(gsLayout, 5, vsGs->GetBufferPointer(), vsGs->GetBufferSize(), g_sh.ilGs.put());
    D3D11_INPUT_ELEMENT_DESC vu1Layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UINT, 0, 68, D3D11_INPUT_PER_VERTEX_DATA, 0},
        /* PC: the draw's record (Vu1Inst, vu1_inst) */
        {"INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        /* PC: the vertex's batch (set_batch_vb) */
        {"BATCH", 0, DXGI_FORMAT_R16_UINT, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev()->CreateInputLayout(vu1Layout, 13, vsVu1->GetBufferPointer(), vsVu1->GetBufferSize(), g_sh.ilVu1.put());
    {
        /* PC: the VU1 draws' records (vu1_inst); a merged draw's parts read
         * from them as a buffer (vs_vu1's instRecs, t1), where a buffer a
         * shader reads may be mapped NO_OVERWRITE (Direct3D 11.1) - else no
         * draws are merged (merge_supported) */
        D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
        g_instSrvOk = SUCCEEDED(dev()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))) &&
                      o.MapNoOverwriteOnDynamicBufferSRV;
        static_assert(sizeof(Vu1Inst) == 64, "Vu1Inst");
        /* PC (2026-11-04): the lighting blocks' ring (light_block), a buffer
         * the vertex stage reads: the same maps.  GT3PC_PGL_LIGHT_RING=0:
         * without it, the blocks in the constants (to compare) */
        const char *lr = getenv("GT3PC_PGL_LIGHT_RING");
        g_lightRingOn = g_instSrvOk && !(lr != nullptr && atoi(lr) == 0);
        /* PC: the constants' ring, where the driver takes constant buffer
         * offsets and NO_OVERWRITE maps of constant buffers (Direct3D 11.1) */
        D3D11_FEATURE_DATA_D3D11_OPTIONS opt = {};
        g_cbRingOk = SUCCEEDED(dev()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt))) &&
                     opt.ConstantBufferOffsetting && opt.MapNoOverwriteOnDynamicConstantBuffer &&
                     SUCCEEDED(dc()->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                                    reinterpret_cast<void **>(g_dc1.put())));
        /* PC (2026-11-05): the executor's rings (Rings: a submitting thread's
         * own; the submit thread's and the workers' as they start) */
        rings_init_thread();
        pclog::write("pgl: VU1 draws %s", t_rings.inst.srv ? "merged where they may be (their parts read as a buffer)"
                                                           : "not merged (no NO_OVERWRITE maps of a buffer a shader reads)");
        pclog::write("pgl: the VU1 draws' lighting blocks %s",
                     t_rings.lights.srv ? "in a ring the vertex stage reads (a part's own: parts with materials of their"
                                          " own merge)" : "in the constants (no ring)");
    }
    g_sh.cbVsSize = sizeof(CbVu1);
    g_sh.cbPsSize = 256;
    g_sh.cbVs = make_cb(g_sh.cbVsSize);
    g_sh.cbPs = make_cb(g_sh.cbPsSize);
    g_sh.cbFlare = make_cb(sizeof(CbFlare));
    static_assert(sizeof(CbFlare) % 16 == 0, "CbFlare");
    for (int k = 0; k < 2; k++) {
        /* PC (2026-11-04): the mirror pass's constants (mirror_cb: keep 0 / 1) */
        const uint32_t keep[4] = {(uint32_t)k, 0, 0, 0};
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(keep);
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA init = {keep, 0, 0};
        dev()->CreateBuffer(&d, &init, g_sh.cbMirror[k].put());
    }
    static_assert(sizeof(Vu1Part) == sizeof(Vu1Inst), "Vu1Part");
    static_assert(sizeof(GsVtx) == 48, "GsVtx");
    static_assert(sizeof(gt3vif::Vertex) == 72, "Vertex");
    static_assert(sizeof(CbVu1) % 16 == 0 && sizeof(CbPs) % 16 == 0, "constant buffers");
    if (!g_sh.ilGs || !g_sh.ilVu1 || !g_sh.cbVs || !g_sh.cbPs || !g_sh.gsVu1 || !g_sh.vsVu1Direct || !t_rings.inst.buf)
        pclog::fatal("pgl: the renderer's Direct3D 11 objects could not be created");

    pclog::write("pgl: shader constants %s", t_rings.cb.on ? "in a 4 MB ring (Direct3D 11.1 constant buffer offsets)"
                                                           : "in their own buffers (no constant buffer offsets)");
    /* PC (2026-11-05): the stream of zeros for draws without batches (set_batch_vb), made once here (two
     * submitting threads must not race to make it) */
    {
        const uint32_t z[4] = {};
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(z);
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = {z, 0, 0};
        dev()->CreateBuffer(&d, &init, g_bvbZeros.put());
    }
    g_qdc = dc();
}

/* (Below: the binding cache and the ring's positions are the executor's -
 * what has been queued; the calls themselves are queued for the D3D thread,
 * pgl_queue.h, with the values they need.) */
namespace {

/* The ring's bindings: the VS constants (and the VU1 clipper's, the same:
 * gs) and the PS constants at their offsets (queued). */
void ring_bind_vs()
{
    ID3D11Buffer *b = t_rings.cb.buf.get();
    ID3D11DeviceContext1 *c1 = t_rings.cb.dc1.get();
    const UINT first = t_rings.cb.vsFirst, num = t_rings.cb.vsNum;
    const bool gs = g_curVs == Vs::Vu1;
    q::run([b, c1, first, num, gs] {
        c1->VSSetConstantBuffers1(0, 1, &b, &first, &num);
        if (gs)
            c1->GSSetConstantBuffers1(0, 1, &b, &first, &num);
    });
}

void ring_bind_ps()
{
    ID3D11Buffer *b = t_rings.cb.buf.get();
    ID3D11DeviceContext1 *c1 = t_rings.cb.dc1.get();
    const UINT first = t_rings.cb.psFirst, num = t_rings.cb.psNum;
    q::run([b, c1, first, num] { c1->PSSetConstantBuffers1(0, 1, &b, &first, &num); });
}

/* bytes of data, a stage's constants (vs, else ps), into the ring's next
 * slots and bound there.  At a wrap the buffer is discarded: the other
 * stage's bound constants are written into it again first and bound anew
 * (a draw may still use them). */
bool ring_write(bool vs, const void *data, size_t bytes)
{
    const uint32_t need = (uint32_t)((bytes + 255) & ~(size_t)255);
    const uint32_t other = vs ? (uint32_t)g_bound.psCbBytes : (g_vsBound ? (uint32_t)g_bound.vsCbBytes : 0u);
    const uint32_t otherNeed = (other + 255) & ~255u;
    const bool wrap = t_rings.cb.pos + need > t_rings.cb.size;
    const uint32_t keep = wrap ? other : 0;  /* the other stage's constants written again */
    uint32_t at = t_rings.cb.pos;
    if (wrap) {
        at = 0;
        if (keep != 0) {
            (vs ? t_rings.cb.psFirst : t_rings.cb.vsFirst) = 0;
            (vs ? t_rings.cb.psNum : t_rings.cb.vsNum) = otherNeed / 16;
            at = otherNeed;
        }
    }
    (vs ? t_rings.cb.vsFirst : t_rings.cb.psFirst) = at / 16;
    (vs ? t_rings.cb.vsNum : t_rings.cb.psNum) = need / 16;
    t_rings.cb.pos = at + need;
    /* the data along: the kept constants first, then the new */
    uint8_t blob[2 * 512];
    if (keep != 0)
        memcpy(blob, vs ? g_bound.psCb : g_bound.vsCb, keep);
    memcpy(blob + keep, data, bytes);
    ID3D11Buffer *buf = t_rings.cb.buf.get();
    const uint32_t n = (uint32_t)bytes;
    q::run_with(blob, keep + bytes, [buf, wrap, keep, at, n](const uint8_t *p) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = qdc()->Map(buf, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
        if (FAILED(hr)) {
            log_once("pgl-ring-map", "pgl: the constants' ring could not be mapped (0x%08lX): draws used old"
                     " constants", (unsigned long)hr);
            return;
        }
        uint8_t *dst = static_cast<uint8_t *>(m.pData);
        if (keep != 0)
            memcpy(dst, p, keep);
        memcpy(dst + at, p + keep, n);
        qdc()->Unmap(buf, 0);
    });
    if (vs || keep != 0)
        ring_bind_vs();
    if (!vs || keep != 0)
        ring_bind_ps();
    return true;
}

}  /* namespace */

/* (g_bvbZeros: with the rings above) */

/* PC (2026-11-05): the binding cache below is the submit stage's (pgl_d3d.h):
 * a call from the executor's thread while a frame pipelines is queued for
 * the submit thread, which makes it there, in order with the draws' records
 * (the passes' bindings before the draws that follow them).  (Until
 * 2026-11-05 a pass inside a draw's submission had to save and restore the
 * draw's vertex stage; now every pass comes before the draw's record.) */
#define PGL_FORWARD(...)                                                    \
    if (q::t_forward) {                                                     \
        sq::run([=] { __VA_ARGS__; });                                      \
        return;                                                             \
    }

void d3d_invalidate()
{
    PGL_FORWARD(d3d_invalidate())
    /* the VU1 clipper only in front of vs_vu1: the other modules' draws run
     * without a geometry stage */
    q::run([] {
        qdc()->GSSetShader(nullptr, nullptr, 0);
        g_mergedSlots = MergedSlots();
        ID3D11ShaderResourceView *none = nullptr;   /* PC (2026-11-05): (the depth buffer's slot, set_srv_z) */
        qdc()->PSSetShaderResources(17, 1, &none);
    });
    g_vsBound = false;
    g_bound = Bound();
}

void d3d_forget()
{
    PGL_FORWARD(d3d_forget())
    g_vsBound = false;
    g_bound = Bound();
    q::run([] { g_mergedSlots = MergedSlots(); });
}

void set_targets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, int w, int h)
{
    PGL_FORWARD(set_targets(rtv, dsv, w, h))
    if (!g_bound.valid || rtv != g_bound.rtv || dsv != g_bound.dsv) {
        /* PC (2026-11-05): the depth buffer's texture (set_srv_z) goes with a
         * change of the depth-stencil view - Direct3D drops it itself when a
         * view of the buffer is bound for writing (quietly: the next folded
         * draw read zeros) */
        const bool dropZ = g_bound.srvZ != nullptr && dsv != g_bound.dsv;
        cap::bind(D3DB_RTV | D3DB_DSV);
        q::run([rtv, dsv, dropZ] {
            ID3D11ShaderResourceView *none = nullptr;
            qdc()->PSSetShaderResources(0, 1, &none);
            if (dropZ)
                qdc()->PSSetShaderResources(17, 1, &none);
            qdc()->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
        });
        g_bound.srv0 = nullptr;
        if (dropZ)
            g_bound.srvZ = nullptr;
        g_bound.rtv = rtv;
        g_bound.dsv = dsv;
        g_bound.valid = true;
    }
    if (w != g_bound.vpW || h != g_bound.vpH) {
        const D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        cap::bind(D3DB_VIEWPORT);
        q::run([vp] { qdc()->RSSetViewports(1, &vp); });
        g_bound.vpW = w;
        g_bound.vpH = h;
    }
}

void set_srv_z(ID3D11ShaderResourceView *srv)
{
    PGL_FORWARD(set_srv_z(srv))
    if (srv == g_bound.srvZ)
        return;
    q::run([srv] { qdc()->PSSetShaderResources(17, 1, &srv); });
    g_bound.srvZ = srv;
}

void set_scissor(const D3D11_RECT &r)
{
    PGL_FORWARD(set_scissor(r))
    if (memcmp(&r, &g_bound.scissor, sizeof(r)) != 0) {
        const D3D11_RECT c = r;
        cap::bind(D3DB_SCISSOR);
        q::run([c] { qdc()->RSSetScissorRects(1, &c); });
        g_bound.scissor = r;
    }
}

void set_raster(ID3D11RasterizerState *s)
{
    PGL_FORWARD(set_raster(s))
    if (s != g_bound.raster) {
        cap::bind(D3DB_RASTER);
        q::run([s] { qdc()->RSSetState(s); });
        g_bound.raster = s;
    }
}

void set_blend(ID3D11BlendState *s)
{
    PGL_FORWARD(set_blend(s))
    if (s != g_bound.blend) {
        cap::bind(D3DB_BLEND);
        q::run([s] { qdc()->OMSetBlendState(s, nullptr, 0xFFFFFFFFu); });
        g_bound.blend = s;
    }
}

void set_depth(ID3D11DepthStencilState *s, uint8_t stencilRef)
{
    PGL_FORWARD(set_depth(s, stencilRef))
    if (s != g_bound.depth || stencilRef != g_bound.stencilRef) {
        cap::bind(D3DB_DEPTH);
        const uint8_t byte = stencil_ref_byte(stencilRef);  /* PC (2026-11-06): (the mirror's bit) */
        q::run([s, byte] { qdc()->OMSetDepthStencilState(s, byte); });
        g_bound.depth = s;
        g_bound.stencilRef = stencilRef;
    }
}

/* PC (2026-11-04): the alpha mirror's shaders and constants, and what the
 * cache holds bound (the mirror pass puts it back: pgl_exec.cpp
 * queue_mirror_pass) */
ID3D11PixelShader *mirror_ps(bool flat, bool merged, bool perSample)
{
    if (perSample) {                        /* PC (2026-11-15): AGSS */
        if (merged)
            return flat ? g_sh.psMainFlatSsMirrorMerged.get() : g_sh.psMainSsMirrorMerged.get();
        return flat ? g_sh.psMainFlatSsMirror.get() : g_sh.psMainSsMirror.get();
    }
    if (merged)
        return flat ? g_sh.psMainFlatMirrorMerged.get() : g_sh.psMainMirrorMerged.get();
    return flat ? g_sh.psMainFlatMirror.get() : g_sh.psMainMirror.get();
}

bool per_sample_shaders()
{
    for (const Com<ID3D11PixelShader> *s : {&g_sh.psMainSs, &g_sh.psMainFlatSs, &g_sh.psMainSsMerged,
                                            &g_sh.psMainFlatSsMerged, &g_sh.psMainSsMirror, &g_sh.psMainFlatSsMirror,
                                            &g_sh.psMainSsMirrorMerged, &g_sh.psMainFlatSsMirrorMerged})
        if (!*s)
            return false;
    return true;
}

ID3D11Buffer *mirror_cb(bool keep1)
{
    return g_sh.cbMirror[keep1 ? 1 : 0].get();
}

ID3D11PixelShader *bound_ps()
{
    return g_bound.ps;
}

ID3D11DepthStencilState *bound_depth(uint8_t *stencilRef)
{
    *stencilRef = g_bound.stencilRef;
    return g_bound.depth;
}

ID3D11BlendState *bound_blend()
{
    return g_bound.blend;
}

void set_srv0(ID3D11ShaderResourceView *srv)
{
    PGL_FORWARD(set_srv0(srv))
    if (srv != g_bound.srv0) {
        cap::bind(D3DB_SRV0);
        q::run([srv] { qdc()->PSSetShaderResources(0, 1, &srv); });
        g_bound.srv0 = srv;
    }
}

void set_srv1(ID3D11ShaderResourceView *srv)
{
    PGL_FORWARD(set_srv1(srv))
    if (srv != g_bound.srv1) {
        cap::bind(D3DB_SRV1);
        q::run([srv] { qdc()->PSSetShaderResources(1, 1, &srv); });
        g_bound.srv1 = srv;
    }
}

void set_sampler0(ID3D11SamplerState *s)
{
    PGL_FORWARD(set_sampler0(s))
    if (s != g_bound.samp0) {
        cap::bind(D3DB_SAMP0);
        q::run([s] { qdc()->PSSetSamplers(0, 1, &s); });
        g_bound.samp0 = s;
    }
}

void set_topology(D3D11_PRIMITIVE_TOPOLOGY t)
{
    PGL_FORWARD(set_topology(t))
    if (t != g_bound.topo) {
        cap::bind(D3DB_TOPO);
        q::run([t] { qdc()->IASetPrimitiveTopology(t); });
        g_bound.topo = t;
    }
}

void set_vb(ID3D11Buffer *vb, UINT stride)
{
    PGL_FORWARD(set_vb(vb, stride))
    if (vb != g_bound.vb || stride != g_bound.vbStride) {
        cap::bind(D3DB_VB);
        q::run([vb, stride] {
            const UINT offset = 0;
            qdc()->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        });
        g_bound.vb = vb;
        g_bound.vbStride = stride;
    }
}

void set_ib(ID3D11Buffer *ib)
{
    PGL_FORWARD(set_ib(ib))
    if (ib != g_bound.ib) {
        cap::bind(D3DB_IB);
        q::run([ib] { qdc()->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0); });
        g_bound.ib = ib;
    }
}

void set_batch_vb(ID3D11Buffer *bvb)
{
    PGL_FORWARD(set_batch_vb(bvb))
    Com<ID3D11Buffer> &zeros = g_bvbZeros;
    if (bvb == nullptr) {
        if (!zeros) {
            const uint32_t z[4] = {};
            D3D11_BUFFER_DESC d = {};
            d.ByteWidth = sizeof(z);
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init = {z, 0, 0};
            dev()->CreateBuffer(&d, &init, zeros.put());
        }
        bvb = zeros.get();
    }
    if (bvb != g_bound.bvb) {
        const UINT stride = bvb == zeros.get() ? 0u : (UINT)sizeof(uint16_t);
        q::run([bvb, stride] {
            const UINT offset = 0;
            qdc()->IASetVertexBuffers(2, 1, &bvb, &stride, &offset);
        });
        g_bound.bvb = bvb;
    }
}

/* (the records' stream on input slot 1 and, for merged draws, its buffer
 * view on VS t1) */
static void inst_bind()
{
    InstRing &r = t_rings.inst;
    if (g_bound.inst)
        return;
    ID3D11Buffer *b = r.buf.get();
    ID3D11ShaderResourceView *v = r.srv.get();
    ID3D11ShaderResourceView *lv = t_rings.lights.srv.get();     /* PC: (the lighting blocks, t2) */
    q::run([b, v, lv] {
        const UINT stride = sizeof(Vu1Inst), offset = 0;
        qdc()->IASetVertexBuffers(1, 1, &b, &stride, &offset);
        if (v != nullptr)
            qdc()->VSSetShaderResources(1, 1, &v);
        if (lv != nullptr)
            qdc()->VSSetShaderResources(2, 1, &lv);
    });
    g_bound.inst = true;
}

bool merge_supported()
{
    return (bool)t_rings.inst.srv;
}

/* PC (2026-11-05): a ring full - the D3D thread must copy the place's record
 * first.  The executor and the submit thread wait for everything queued
 * (q::sync); a submit worker (q::t_capture: its calls are captured, not
 * queued) waits for the D3D thread's uploads of this ring to reach `need' -
 * the runs before its own are published in order by the submit thread, so
 * they come. */
static void ring_catch_up(std::atomic<uint32_t> &uploaded, uint32_t need)
{
    if (q::t_capture == nullptr) {
        q::sync();
        return;
    }
    int spins = 0;
    while ((int32_t)(uploaded.load(std::memory_order_acquire) - need) < 0) {
        if (++spins < 4000) {
            _mm_pause();
        } else {
            SwitchToThread();
            spins = 0;
        }
    }
}

uint32_t vu1_inst_parts(const Vu1Part *parts, uint32_t n, Vu1Inst rec, uint32_t copies)
{
    InstRing &r = t_rings.inst;
    inst_bind();
    /* (n + copies places in one cycle: to the next cycle's start if not) */
    const uint32_t need = n + copies;
    uint32_t seq = r.next;
    if (vu1_inst_slot(seq) + need > kVu1Insts)
        seq += kVu1Insts - vu1_inst_slot(seq);
    if (seq + need - r.uploadedSeen > kVu1Insts) {
        r.uploadedSeen = r.uploaded.load(std::memory_order_acquire);
        if (seq + need - r.uploadedSeen > kVu1Insts) {
            ring_catch_up(r.uploaded, seq + need - kVu1Insts);  /* (as vu1_inst) */
            r.uploadedSeen = seq + need - kVu1Insts;
        }
    }
    for (uint32_t i = 0; i < n; i++)
        memcpy(&r.cpu[vu1_inst_slot(seq + i)], &parts[i], sizeof(Vu1Inst));
    rec.texRect[0] = (float)vu1_inst_slot(seq);
    for (uint32_t k = 0; k < copies; k++) {   /* PC (2026-11-05): (a C = Ad draw of two instances: twice) */
        Vu1Inst &c = r.cpu[vu1_inst_slot(seq + n + k)];
        c = rec;
        c.pad = rec.pad + (float)k;         /* (its place among the copies in bits 0..1: vs_vu1's rep; PC 2026-11-06:
                                             * over 4 x the mesh's vertices, the doubled stream's) */
    }
    r.next = seq + need;
    r.published.store(r.next, std::memory_order_release);
    r.last = rec;
    r.lastValid = true;
    r.lastCopies = copies;
    return seq + n;
}

uint32_t vu1_inst(const Vu1Inst &rec, uint32_t copies)
{
    InstRing &r = t_rings.inst;
    inst_bind();
    /* (the draw before's record again, when it has places enough) */
    if (r.lastValid && r.lastCopies >= copies && memcmp(&rec, &r.last, sizeof(rec)) == 0)
        return r.next - r.lastCopies;
    /* (copies places in one cycle: to the next cycle's start if not) */
    uint32_t seq = r.next;
    if (vu1_inst_slot(seq) + copies > kVu1Insts)
        seq += kVu1Insts - vu1_inst_slot(seq);
    if (seq + copies - r.uploadedSeen > kVu1Insts) {
        r.uploadedSeen = r.uploaded.load(std::memory_order_acquire);
        if (seq + copies - r.uploadedSeen > kVu1Insts) {
            /* (the place's record may not be copied yet: every queued call
             * run - no draw left that reads one this old) */
            ring_catch_up(r.uploaded, seq + copies - kVu1Insts);
            r.uploadedSeen = seq + copies - kVu1Insts;
        }
    }
    for (uint32_t k = 0; k < copies; k++) {   /* PC (2026-11-05): (a C = Ad draw of two instances: twice) */
        Vu1Inst &c = r.cpu[vu1_inst_slot(seq + k)];
        c = rec;
        c.pad = rec.pad + (float)k;         /* (its place among the copies in bits 0..1: vs_vu1's rep - an instance's,
                                             * or a draw's that starts at a later copy, draw_passes's pairs; PC
                                             * 2026-11-06: over 4 x the mesh's vertices, the doubled stream's) */
    }
    r.next = seq + copies;
    r.published.store(r.next, std::memory_order_release);
    r.last = rec;
    r.lastValid = true;
    r.lastCopies = copies;
    return seq;
}

void vu1_inst_upload(Rings *rg, uint32_t seq)
{
    InstRing &r = rg->inst;                 /* (the submitting thread's, named by the call: not this thread's) */
    const uint32_t cycle = seq / kVu1Insts;
    D3D11_MAP mode;
    uint32_t from;
    if (cycle != r.cycle) {
        /* (a new cycle of the ring: the draws before it read the old one) */
        mode = D3D11_MAP_WRITE_DISCARD;
        from = cycle * kVu1Insts;
        r.cycle = cycle;
    } else if ((int32_t)(seq - r.up) >= 0) {
        mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        from = r.up;
    } else {
        return;                             /* (copied with the ones before) */
    }
    /* everything written so far, to the end of the cycle */
    uint32_t to = r.published.load(std::memory_order_acquire);
    const uint32_t end = (cycle + 1) * kVu1Insts;
    if (to - from > end - from)
        to = end;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *b = r.buf.get();
    if (FAILED(qdc()->Map(b, 0, mode, 0, &m))) {
        log_once("pgl-inst-map", "pgl: the VU1 draws' records could not be mapped: draws read old ones");
        return;
    }
    memcpy(static_cast<Vu1Inst *>(m.pData) + vu1_inst_slot(from), &r.cpu[vu1_inst_slot(from)],
           (size_t)(to - from) * sizeof(Vu1Inst));
    qdc()->Unmap(b, 0);
    r.up = to;
    r.uploaded.store(to, std::memory_order_release);
}

uint32_t d3d_check_bound(bool texture, bool date, bool indexed, char *what, size_t n)
{
    uint32_t bits = 0;
    size_t used = 0;
    if (n > 0)
        what[0] = 0;
    auto note = [&](uint32_t bit, const char *name, const void *actual, const void *cached) {
        bits |= bit;
        if (used < n) {
            const int k = snprintf(what + used, n - used, "%s%s %p (cached %p)", used ? ", " : "", name, actual,
                                   cached);
            used = k > 0 ? std::min(n - 1, used + (size_t)k) : used;
        }
    };
    auto release = [](IUnknown *p) {
        if (p != nullptr)
            p->Release();
    };
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    dc()->OMGetRenderTargets(1, &rtv, &dsv);
    if (rtv != g_bound.rtv)
        note(D3DB_RTV, "render target", rtv, g_bound.rtv);
    if (dsv != g_bound.dsv)
        note(D3DB_DSV, "depth target", dsv, g_bound.dsv);
    release(rtv);
    release(dsv);
    UINT nv = 1;
    D3D11_VIEWPORT vp = {};
    dc()->RSGetViewports(&nv, &vp);
    if (nv < 1 || (int)vp.Width != g_bound.vpW || (int)vp.Height != g_bound.vpH || vp.TopLeftX != 0 ||
        vp.TopLeftY != 0)
        note(D3DB_VIEWPORT, "viewport", (const void *)(uintptr_t)(int)vp.Width, (const void *)(uintptr_t)g_bound.vpW);
    UINT nr = 1;
    D3D11_RECT sr = {};
    dc()->RSGetScissorRects(&nr, &sr);
    if (nr < 1 || memcmp(&sr, &g_bound.scissor, sizeof(sr)) != 0)
        note(D3DB_SCISSOR, "scissor (left)", (const void *)(uintptr_t)sr.left,
             (const void *)(uintptr_t)g_bound.scissor.left);
    ID3D11RasterizerState *rs = nullptr;
    dc()->RSGetState(&rs);
    if (rs != g_bound.raster)
        note(D3DB_RASTER, "raster state", rs, g_bound.raster);
    release(rs);
    ID3D11BlendState *bs = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    dc()->OMGetBlendState(&bs, factor, &mask);
    if (bs != g_bound.blend || mask != 0xFFFFFFFFu)
        note(D3DB_BLEND, "blend state", bs, g_bound.blend);
    release(bs);
    ID3D11DepthStencilState *ds = nullptr;
    UINT ref = 0;
    dc()->OMGetDepthStencilState(&ds, &ref);
    if (ds != g_bound.depth)
        note(D3DB_DEPTH, "depth state", ds, g_bound.depth);
    release(ds);
    ID3D11PixelShader *ps = nullptr;
    dc()->PSGetShader(&ps, nullptr, nullptr);
    if (ps != g_bound.ps)
        note(D3DB_PS, "pixel shader", ps, g_bound.ps);
    release(ps);
    if (g_vsBound) {
        ID3D11VertexShader *vs = nullptr;
        dc()->VSGetShader(&vs, nullptr, nullptr);
        ID3D11VertexShader *want = g_curVs == Vs::Gs ? g_sh.vsGs.get() : g_curVs == Vs::Vu1 ? g_sh.vsVu1.get()
                                   : g_curVs == Vs::Vu1Direct ? g_sh.vsVu1Direct.get() : g_sh.vsFull.get();
        if (vs != want)
            note(D3DB_VS, "vertex shader", vs, want);
        release(vs);
        ID3D11Buffer *cb = nullptr;
        ID3D11Buffer *wantCb = t_rings.cb.on ? t_rings.cb.buf.get() : g_sh.cbVs.get();
        dc()->VSGetConstantBuffers(0, 1, &cb);
        if (cb != wantCb)
            note(D3DB_VSCB, "VS constants", cb, wantCb);
        release(cb);
    }
    if (g_bound.psCbBytes != 0) {
        ID3D11Buffer *cb = nullptr;
        ID3D11Buffer *want = t_rings.cb.on ? t_rings.cb.buf.get() : g_sh.cbPs.get();
        dc()->PSGetConstantBuffers(0, 1, &cb);
        if (cb != want)
            note(D3DB_PSCB, "PS constants", cb, want);
        release(cb);
    }
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    dc()->IAGetPrimitiveTopology(&topo);
    if (topo != g_bound.topo)
        note(D3DB_TOPO, "topology", (const void *)(uintptr_t)topo, (const void *)(uintptr_t)g_bound.topo);
    ID3D11Buffer *vb = nullptr;
    UINT stride = 0, offset = 0;
    dc()->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb != g_bound.vb || stride != g_bound.vbStride || offset != 0)
        note(D3DB_VB, "vertex buffer", vb, g_bound.vb);
    release(vb);
    if (indexed) {
        ID3D11Buffer *ib = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        dc()->IAGetIndexBuffer(&ib, &fmt, &offset);
        if (ib != g_bound.ib || fmt != DXGI_FORMAT_R32_UINT || offset != 0)
            note(D3DB_IB, "index buffer", ib, g_bound.ib);
        release(ib);
    }
    if (texture) {
        ID3D11ShaderResourceView *srv = nullptr;
        dc()->PSGetShaderResources(0, 1, &srv);
        if (srv != g_bound.srv0)
            note(D3DB_SRV0, "texture 0", srv, g_bound.srv0);
        release(srv);
        ID3D11SamplerState *smp = nullptr;
        dc()->PSGetSamplers(0, 1, &smp);
        if (smp != g_bound.samp0)
            note(D3DB_SAMP0, "sampler 0", smp, g_bound.samp0);
        release(smp);
    }
    if (date) {
        ID3D11ShaderResourceView *srv = nullptr;
        dc()->PSGetShaderResources(1, 1, &srv);
        if (srv != g_bound.srv1)
            note(D3DB_SRV1, "texture 1", srv, g_bound.srv1);
        release(srv);
    }
    return bits;
}

/* GT3PC_FLICKER: what the GPU holds for a draw - its constant buffers and
 * its vertex / index ranges, copied into staging memory right after it -
 * against what was written (the binding cache's copy of the constants, the
 * streams' CPU shadows).  A set per frame in flight, a slot per draw. */
namespace {

constexpr uint32_t kVerifyVb = 2048, kVerifyIb = 512, kVerifySlots = 512;

uint32_t verify_hash(const void *p, size_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

struct VerifySlot {
    Com<ID3D11Buffer> vsCb, psCb;           /* staging, the constant buffers' sizes */
    uint32_t vsBytes = 0, psBytes = 0, vsHash = 0, psHash = 0;
    uint32_t vbBytes = 0, vbHash = 0, ibBytes = 0, ibHash = 0;
};
struct VerifySet {
    std::vector<VerifySlot> slots;
    Com<ID3D11Buffer> vb, ib;               /* staging: kVerifyVb / kVerifyIb bytes a slot */
    bool failed = false;
};
VerifySet g_verify[4];

Com<ID3D11Buffer> staging_like(ID3D11Buffer *src, UINT bytes)
{
    D3D11_BUFFER_DESC d = {};
    if (src != nullptr)
        src->GetDesc(&d);
    d.ByteWidth = src != nullptr ? d.ByteWidth : bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    d.StructureByteStride = 0;
    Com<ID3D11Buffer> b;
    dev()->CreateBuffer(&d, nullptr, b.put());
    return b;
}

}  /* namespace */

void d3d_verify_copy(int set, uint32_t slot, bool indexed, uint32_t first, uint32_t count, int32_t baseVertex)
{
    VerifySet &vs = g_verify[set & 3];
    if (vs.failed || slot >= kVerifySlots)
        return;
    if (!vs.vb) {
        vs.vb = staging_like(nullptr, kVerifyVb * kVerifySlots);
        vs.ib = staging_like(nullptr, kVerifyIb * kVerifySlots);
        if (!vs.vb || !vs.ib) {
            vs.failed = true;
            return;
        }
    }
    if (vs.slots.size() <= slot)
        vs.slots.resize(slot + 1);
    VerifySlot &s = vs.slots[slot];
    if (!s.vsCb) {
        s.vsCb = staging_like(g_sh.cbVs.get(), 0);
        s.psCb = staging_like(g_sh.cbPs.get(), 0);
    }
    s.vsBytes = s.psBytes = s.vbBytes = s.ibBytes = 0;
    /* (the constants: their buffer, or their slots of the ring) */
    auto copy_cb = [](ID3D11Buffer *dst, ID3D11Buffer *own, UINT first, size_t bytes) {
        if (!t_rings.cb.on) {
            dc()->CopyResource(dst, own);
            return;
        }
        const D3D11_BOX box = {first * 16, 0, 0, first * 16 + (UINT)bytes, 1, 1};
        dc()->CopySubresourceRegion(dst, 0, 0, 0, 0, t_rings.cb.buf.get(), 0, &box);
    };
    if (s.vsCb && g_vsBound && g_bound.vsCbBytes != 0) {
        copy_cb(s.vsCb.get(), g_sh.cbVs.get(), t_rings.cb.vsFirst, g_bound.vsCbBytes);
        s.vsBytes = (uint32_t)g_bound.vsCbBytes;
        s.vsHash = verify_hash(g_bound.vsCb, s.vsBytes);
    }
    if (s.psCb && g_bound.psCbBytes != 0) {
        copy_cb(s.psCb.get(), g_sh.cbPs.get(), t_rings.cb.psFirst, g_bound.psCbBytes);
        s.psBytes = (uint32_t)g_bound.psCbBytes;
        s.psHash = verify_hash(g_bound.psCb, s.psBytes);
    }
    /* the vertex range (an indexed draw's: its first `count' vertices) */
    const uint32_t stride = g_bound.vbStride;
    const uint64_t v0 = (uint64_t)(indexed ? (uint32_t)std::max(0, baseVertex) : first) * stride;
    uint64_t vbytes = std::min<uint64_t>((uint64_t)count * stride, kVerifyVb);
    if (g_bound.vb == g_vstream.buf.get() && v0 + vbytes <= g_vshadow.size() && vbytes > 0) {
        const D3D11_BOX box = {(UINT)v0, 0, 0, (UINT)(v0 + vbytes), 1, 1};
        dc()->CopySubresourceRegion(vs.vb.get(), 0, slot * kVerifyVb, 0, 0, g_vstream.buf.get(), 0, &box);
        s.vbBytes = (uint32_t)vbytes;
        s.vbHash = verify_hash(g_vshadow.data() + v0, (size_t)vbytes);
    }
    if (indexed) {
        const uint64_t i0 = (uint64_t)first * 4;
        const uint64_t ibytes = std::min<uint64_t>((uint64_t)count * 4, kVerifyIb);
        if (g_bound.ib == g_istream.buf.get() && i0 + ibytes <= g_ishadow.size() && ibytes > 0) {
            const D3D11_BOX box = {(UINT)i0, 0, 0, (UINT)(i0 + ibytes), 1, 1};
            dc()->CopySubresourceRegion(vs.ib.get(), 0, slot * kVerifyIb, 0, 0, g_istream.buf.get(), 0, &box);
            s.ibBytes = (uint32_t)ibytes;
            s.ibHash = verify_hash(g_ishadow.data() + i0, (size_t)ibytes);
        }
    }
}

void d3d_verify_resolve(int set, uint32_t used, uint32_t *bits)
{
    VerifySet &vs = g_verify[set & 3];
    for (uint32_t i = 0; i < used; i++)
        bits[i] = 0;
    if (vs.failed || !vs.vb)
        return;
    D3D11_MAPPED_SUBRESOURCE mv = {}, mi = {};
    const bool okV = SUCCEEDED(dc()->Map(vs.vb.get(), 0, D3D11_MAP_READ, 0, &mv));
    const bool okI = SUCCEEDED(dc()->Map(vs.ib.get(), 0, D3D11_MAP_READ, 0, &mi));
    for (uint32_t i = 0; i < used && i < vs.slots.size() && i < kVerifySlots; i++) {
        VerifySlot &s = vs.slots[i];
        uint32_t b = 0;
        D3D11_MAPPED_SUBRESOURCE m;
        if (s.vsBytes != 0 && SUCCEEDED(dc()->Map(s.vsCb.get(), 0, D3D11_MAP_READ, 0, &m))) {
            if (verify_hash(m.pData, s.vsBytes) != s.vsHash)
                b |= 1;
            dc()->Unmap(s.vsCb.get(), 0);
        }
        if (s.psBytes != 0 && SUCCEEDED(dc()->Map(s.psCb.get(), 0, D3D11_MAP_READ, 0, &m))) {
            if (verify_hash(m.pData, s.psBytes) != s.psHash)
                b |= 2;
            dc()->Unmap(s.psCb.get(), 0);
        }
        if (okV && s.vbBytes != 0 &&
            verify_hash((const uint8_t *)mv.pData + (size_t)i * kVerifyVb, s.vbBytes) != s.vbHash)
            b |= 4;
        if (okI && s.ibBytes != 0 &&
            verify_hash((const uint8_t *)mi.pData + (size_t)i * kVerifyIb, s.ibBytes) != s.ibHash)
            b |= 8;
        bits[i] = b;
    }
    if (okV)
        dc()->Unmap(vs.vb.get(), 0);
    if (okI)
        dc()->Unmap(vs.ib.get(), 0);
}

void bind_vs(Vs vs)
{
    PGL_FORWARD(bind_vs(vs))
    if (g_vsBound && vs == g_curVs)
        return;
    g_vsBound = true;
    g_curVs = vs;
    ID3D11Buffer *cb = t_rings.cb.on ? nullptr : g_sh.cbVs.get();     /* (the ring: ring_bind_vs) */
    ID3D11VertexShader *v = vs == Vs::Gs ? g_sh.vsGs.get() : vs == Vs::Vu1 ? g_sh.vsVu1.get()
                            : vs == Vs::Vu1Direct ? g_sh.vsVu1Direct.get() : g_sh.vsFull.get();
    ID3D11GeometryShader *g = vs == Vs::Vu1 ? g_sh.gsVu1.get() : nullptr;     /* the VU1 clipper */
    ID3D11InputLayout *il = vs == Vs::Gs ? g_sh.ilGs.get()
                            : (vs == Vs::Vu1 || vs == Vs::Vu1Direct) ? g_sh.ilVu1.get() : nullptr;
    cap::bind(D3DB_VS);
    q::run([v, g, il, cb] {
        qdc()->VSSetShader(v, nullptr, 0);
        qdc()->GSSetShader(g, nullptr, 0);
        qdc()->IASetInputLayout(il);
        if (cb != nullptr) {
            qdc()->VSSetConstantBuffers(0, 1, &cb);
            if (g != nullptr)
                qdc()->GSSetConstantBuffers(0, 1, &cb);
        }
    });
    /* (the ring's constants where they are; bind_vs_cb binds new ones) */
    if (t_rings.cb.on && g_bound.vsCbBytes != 0)
        ring_bind_vs();
}

void bind_ps(Ps ps)
{
    PGL_FORWARD(bind_ps(ps))
    ID3D11PixelShader *s = nullptr;
    switch (ps) {
    case Ps::Main: s = g_sh.psMain.get(); break;
    case Ps::MainFlat: s = g_sh.psMainFlat.get(); break;
    case Ps::DepthCopy: s = g_sh.psDepthCopy.get(); break;
    case Ps::Present: s = g_sh.psPresent.get(); break;
    case Ps::PfxColor: s = g_sh.psPfxColor.get(); break;
    case Ps::Plane: s = g_sh.psPlane.get(); break;
    case Ps::Flare: s = g_sh.psFlare.get(); break;
    case Ps::DepthResolve: s = g_sh.psDepthResolve.get(); break;
    case Ps::Fxaa: s = g_sh.psFxaa.get(); break;
    case Ps::ColorResolve: s = g_sh.psColorResolve.get(); break;
    case Ps::MirrorSync: s = g_sh.psMirrorSync.get(); break;
    case Ps::MainFold: s = g_sh.psMainFold.get(); break;            /* PC (2026-11-05): the mirror folded in */
    case Ps::MainFlatFold: s = g_sh.psMainFlatFold.get(); break;
    case Ps::MainSs: s = g_sh.psMainSs.get(); break;                /* PC (2026-11-15): AGSS */
    case Ps::MainFlatSs: s = g_sh.psMainFlatSs.get(); break;
    }
    if (s == g_bound.ps)
        return;
    const bool first = g_bound.ps == nullptr;
    ID3D11Buffer *cb = first && !t_rings.cb.on ? g_sh.cbPs.get() : nullptr;
    cap::bind(D3DB_PS);
    q::run([s, cb] {
        qdc()->PSSetShader(s, nullptr, 0);
        if (cb != nullptr)
            qdc()->PSSetConstantBuffers(0, 1, &cb);
    });
    g_bound.ps = s;
}

void bind_flare_cb(const CbFlare &cb)
{
    PGL_FORWARD(bind_flare_cb(cb))
    update_cb(g_sh.cbFlare.get(), &cb, sizeof(cb));
    ID3D11Buffer *b = g_sh.cbFlare.get();
    q::run([b] { qdc()->PSSetConstantBuffers(0, 1, &b); });
    g_bound.psCbBytes = 0;          /* slot 0 no longer holds cbPs's contents */
}

/* PC (2026-11-05): the flare queries' batch (pgl_d3d.h flare_batch_draw):
 * its objects - the records' and weights' buffers the vertex stage reads
 * (Buffer<float4>, dynamic, grown as a frame needs), the sums' texture
 * (R32_FLOAT, the strip's size), its additive blend state, the constants -
 * made and used on the D3D thread alone (device calls are free-threaded; the
 * buffers are written by the thread that draws with them). */
namespace {

struct FlareGpu {
    Com<ID3D11Buffer> recs, weights, cb;
    Com<ID3D11ShaderResourceView> recsSrv, weightsSrv;
    uint32_t recsCap = 0, weightsCap = 0;   /* records / sets the buffers hold */
    Com<ID3D11Texture2D> sum;
    Com<ID3D11RenderTargetView> sumRtv;
    Com<ID3D11ShaderResourceView> sumSrv;
    int sumW = 0, sumH = 0;
    Com<ID3D11BlendState> add;
    bool failed = false;
} g_flareGpu;

struct CbFlareBatch {
    float   zs[4];
    int32_t scr[4];
    int32_t strip[4];
};

/* a buffer of `need' elements of `stride' bytes at least (grown by doubling
 * from `least'), read as float4 */
bool flare_buffer(Com<ID3D11Buffer> &b, Com<ID3D11ShaderResourceView> &srv, uint32_t &cap, uint32_t need,
                  uint32_t stride, uint32_t least)
{
    if (need <= cap)
        return true;
    uint32_t n = cap != 0 ? cap : least;
    while (n < need)
        n *= 2;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = n * stride;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    srv.reset();
    if (FAILED(dev()->CreateBuffer(&d, nullptr, b.put())))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.FirstElement = 0;
    v.Buffer.NumElements = n * stride / 16;
    if (FAILED(dev()->CreateShaderResourceView(b.get(), &v, srv.put())))
        return false;
    cap = n;
    return true;
}

bool flare_objects(FlareGpu &g, const FlareBatch &b)
{
    if (!flare_buffer(g.recs, g.recsSrv, g.recsCap, b.nRecs, sizeof(FlareRec), 1024) ||
        !flare_buffer(g.weights, g.weightsSrv, g.weightsCap, b.nWeights, sizeof(FlareWeights), 64))
        return false;
    if (!g.sum || g.sumW != b.stripW || g.sumH != b.stripH) {
        g.sumSrv.reset();
        g.sumRtv.reset();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)b.stripW;
        d.Height = (UINT)b.stripH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, g.sum.put())) ||
            FAILED(dev()->CreateRenderTargetView(g.sum.get(), nullptr, g.sumRtv.put())) ||
            FAILED(dev()->CreateShaderResourceView(g.sum.get(), nullptr, g.sumSrv.put())))
            return false;
        g.sumW = b.stripW;
        g.sumH = b.stripH;
    }
    if (!g.add) {
        D3D11_BLEND_DESC d = {};
        D3D11_RENDER_TARGET_BLEND_DESC &r = d.RenderTarget[0];
        r.BlendEnable = TRUE;
        r.SrcBlend = D3D11_BLEND_ONE;
        r.DestBlend = D3D11_BLEND_ONE;
        r.BlendOp = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = D3D11_BLEND_ONE;
        r.DestBlendAlpha = D3D11_BLEND_ZERO;
        r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        if (FAILED(dev()->CreateBlendState(&d, g.add.put())))
            return false;
    }
    if (!g.cb) {
        g.cb = make_cb(sizeof(CbFlareBatch));
        if (!g.cb)
            return false;
    }
    return true;
}

bool flare_write(ID3D11DeviceContext *c, ID3D11Buffer *b, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;
    memcpy(m.pData, data, bytes);
    c->Unmap(b, 0);
    return true;
}

/* (on the D3D thread) */
void flare_batch_run(const FlareBatch &b, const FlareRec *recs, const FlareWeights *weights,
                     ID3D11ShaderResourceView *zSrv, ID3D11RenderTargetView *stripRtv, uint32_t capId)
{
    FlareGpu &g = g_flareGpu;
    ID3D11DeviceContext *c = qdc();
    cap::gpu_begin(capId);                  /* PC (2026-11-06): (the frame capture: the batch as one) */
    struct CapEnd {
        uint32_t id;
        ~CapEnd() { cap::gpu_end(id); }
    } capEnd = {capId};
    if (g.failed || b.nRecs == 0 || b.nWeights == 0 || stripRtv == nullptr)
        return;
    if (!flare_objects(g, b)) {
        g.failed = true;
        log_once("pgl-flare-batch", "pgl: the flare queries' batch has no Direct3D objects: the glows read stale"
                 " results (GT3PC_PGL_FLARE_BATCH=0 draws each query alone)");
        return;
    }
    if (!flare_write(c, g.recs.get(), recs, (size_t)b.nRecs * sizeof(FlareRec)) ||
        !flare_write(c, g.weights.get(), weights, (size_t)b.nWeights * sizeof(FlareWeights))) {
        log_once("pgl-flare-batch-map", "pgl: the flare queries' buffers could not be mapped: the glows read stale"
                 " results");
        return;
    }
    ID3D11Buffer *cb = g.cb.get();
    auto constants = [&](int32_t first) {
        const CbFlareBatch k = {{b.scale[0], b.scale[1], 0.0f, 0.0f},
                                {b.screen[0], b.screen[1], b.halfY, first},
                                {b.stripW, b.stripH, 0, 0}};
        flare_write(c, cb, &k, sizeof(k));
    };
    /* the points: each query's terms into its texel of the sums */
    const float zero[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(g.sumRtv.get(), zero);
    ID3D11RenderTargetView *rtv = g.sumRtv.get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp = {0, 0, (float)b.stripW, (float)b.stripH, 0, 1};
    c->RSSetViewports(1, &vp);
    c->RSSetState(raster_state(0, false));
    c->OMSetBlendState(g.add.get(), nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(depth_state(false, pgl::gs::Compare::Always, false), 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    c->VSSetShader(g_sh.vsFlarePt.get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(g_sh.psFlarePt.get(), nullptr, 0);
    ID3D11ShaderResourceView *srvs[3] = {zSrv, g.weightsSrv.get(), g.recsSrv.get()};
    c->VSSetShaderResources(3, 3, srvs);
    c->VSSetConstantBuffers(1, 1, &cb);
    /* (one instanced draw per run of queries of one box size: the box's
     * pixels are the instance's points) */
    for (uint32_t i = 0; i < b.nRecs;) {
        const uint32_t area = (uint32_t)(recs[i].box[2] * recs[i].box[3]);
        uint32_t j = i + 1;
        while (j < b.nRecs && (uint32_t)(recs[j].box[2] * recs[j].box[3]) == area)
            j++;
        constants((int32_t)i);
        if (area != 0)
            c->DrawInstanced(area, j - i, 0, 0);
        i = j;
    }
    /* the sums into the strip's texels */
    c->OMSetRenderTargets(1, &stripRtv, nullptr);
    c->OMSetBlendState(blend_plain(15), nullptr, 0xFFFFFFFFu);
    ID3D11ShaderResourceView *sum = g.sumSrv.get();
    c->VSSetShaderResources(6, 1, &sum);
    c->VSSetShader(g_sh.vsFlareConv.get(), nullptr, 0);
    c->PSSetShader(g_sh.psFlareConv.get(), nullptr, 0);
    constants(0);
    c->Draw(b.nRecs, 0);
    ID3D11ShaderResourceView *none[4] = {nullptr, nullptr, nullptr, nullptr};
    c->VSSetShaderResources(3, 4, none);
    c->OMSetRenderTargets(0, nullptr, nullptr);
}

}  /* namespace */

void flare_batch_draw(const FlareBatch &b, const FlareRec *recs, const FlareWeights *weights,
                      ID3D11ShaderResourceView *zSrv, ID3D11RenderTargetView *stripRtv, uint32_t capId)
{
    /* (the records then the weights, copied along into the ring: q::run_with
     * forwards through the submit thread while the frame pipelines) */
    const size_t rb = (size_t)b.nRecs * sizeof(FlareRec), wb = (size_t)b.nWeights * sizeof(FlareWeights);
    static thread_local std::vector<uint8_t> stage;
    stage.resize(rb + wb);
    if (rb != 0)
        memcpy(stage.data(), recs, rb);
    if (wb != 0)
        memcpy(stage.data() + rb, weights, wb);
    q::run_with(stage.data(), rb + wb, [b, rb, zSrv, stripRtv, capId](const uint8_t *p) {
        flare_batch_run(b, reinterpret_cast<const FlareRec *>(p), reinterpret_cast<const FlareWeights *>(p + rb),
                        zSrv, stripRtv, capId);
    });
}

/* (same_bytes: pgl_render.h) */

bool vs_cb_bound(const void *data, size_t bytes)
{
    return g_vsBound && bytes == g_bound.vsCbBytes && same_bytes(data, g_bound.vsCb, bytes);
}

/* PC (2026-11-04): the lighting blocks' ring (pgl_d3d.h) */
bool light_supported()
{
    return (bool)t_rings.lights.srv;
}

static uint32_t light_hash(const float (*L)[4])
{
    /* (the 64 words folded: SSE, a few cycles) */
    const __m128i *p = reinterpret_cast<const __m128i *>(L);
    __m128i h = _mm_setzero_si128();
    for (int i = 0; i < 16; i++)
        h = _mm_add_epi32(_mm_xor_si128(h, _mm_loadu_si128(p + i)), _mm_set1_epi32(0x9E3779B9 * (i + 1)));
    alignas(16) uint32_t w[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(w), h);
    uint32_t x = w[0] ^ (w[1] * 0x85EBCA6Bu) ^ (w[2] * 0xC2B2AE35u) ^ (w[3] * 0x27D4EB2Fu);
    x ^= x >> 15;
    return x;
}

int32_t light_block(const float (*L)[4])
{
    LightRing &r = t_rings.lights;
    if (!r.srv)
        return -1;
    /* (the block of the part before, most often: no hash) */
    if (r.next != 0 && same_bytes(L, &r.cpu[(size_t)((r.next - 1) & (kLightBlocks - 1)) * 64], 256))
        return (int32_t)((r.next - 1) & (kLightBlocks - 1));
    const uint32_t hash = light_hash(L);
    const uint32_t slot = hash & 255u;
    LightRing::Memo &m = r.memo[slot];
    if (m.hash == hash && m.seq < r.next && r.next - m.seq <= kLightBlocks &&
        same_bytes(L, &r.cpu[(size_t)(m.seq & (kLightBlocks - 1)) * 64], 256))
        return (int32_t)(m.seq & (kLightBlocks - 1));
    const uint32_t seq = r.next;
    if ((seq & (kLightBlocks - 1)) == 0) {
        /* a new cycle of the ring: the D3D thread discards the buffer when
         * it reaches it - a draw still held must go before (with the blocks
         * it reads in the cycle before), and nothing found in the memo is
         * read across it */
        q::release_held();
        memset(r.memo, 0xFF, sizeof(r.memo));   /* (seq ~0: never before next) */
    }
    if (seq - r.uploadedSeen >= kLightBlocks) {
        r.uploadedSeen = r.uploaded.load(std::memory_order_acquire);
        if (seq - r.uploadedSeen >= kLightBlocks) {
            ring_catch_up(r.uploaded, seq + 1 - kLightBlocks);  /* (as vu1_inst) */
            r.uploadedSeen = seq + 1 - kLightBlocks;
        }
    }
    memcpy(&r.cpu[(size_t)(seq & (kLightBlocks - 1)) * 64], L, 256);
    r.next = seq + 1;
    r.published.store(seq + 1, std::memory_order_release);
    m.hash = hash;
    m.seq = seq;
    return (int32_t)(seq & (kLightBlocks - 1));
}

uint32_t light_mark()
{
    return t_rings.lights.next;
}

void light_upload(Rings *rg, uint32_t upTo)
{
    LightRing &r = rg->lights;              /* (the submitting thread's, named by the call: not this thread's) */
    if (!r.srv || (int32_t)(upTo - r.up) <= 0)
        return;
    ID3D11Buffer *b = r.buf.get();
    while ((int32_t)(upTo - r.up) > 0) {
        const uint32_t from = r.up;
        const uint32_t end = (from | (kLightBlocks - 1)) + 1;    /* (this cycle's end) */
        const uint32_t to = (int32_t)(upTo - end) >= 0 ? end : upTo;
        const D3D11_MAP mode = (from & (kLightBlocks - 1)) == 0 ? D3D11_MAP_WRITE_DISCARD
                                                                  : D3D11_MAP_WRITE_NO_OVERWRITE;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(qdc()->Map(b, 0, mode, 0, &m))) {
            log_once("pgl-light-map", "pgl: the lighting blocks could not be mapped: draws read old ones");
            r.up = upTo;
            r.uploaded.store(upTo, std::memory_order_release);
            return;
        }
        const size_t s = (size_t)(from & (kLightBlocks - 1)) * 64;
        memcpy(static_cast<float *>(m.pData) + s, &r.cpu[s], (size_t)(to - from) * 256);
        qdc()->Unmap(b, 0);
        r.up = to;
    }
    r.uploaded.store(r.up, std::memory_order_release);
}

void draw_merged(ID3D11ShaderResourceView *const *srv, ID3D11SamplerState *const *samp, uint32_t nSlots, Ps ps,
                 uint32_t count, uint32_t first, int32_t base, uint32_t inst, uint32_t lights, uint32_t instances,
                 uint32_t capId)
{
    struct Slots {
        ID3D11ShaderResourceView *srv[kMergeSlots];
        ID3D11SamplerState *samp[kMergeSlots];
    } s = {};
    nSlots = std::min(nSlots, kMergeSlots);
    for (uint32_t k = 1; k < nSlots; k++) {
        s.srv[k] = srv[k];
        s.samp[k] = samp[k];
    }
    /* (its pixel shader the merged variant of the one bound, for the draw;
     * PC, 2026-11-05: of the fold variant too, Ps::MainFold / MainFlatFold) */
    const bool flat = ps == Ps::MainFlat || ps == Ps::MainFlatFold || ps == Ps::MainFlatSs;
    const bool fold = ps == Ps::MainFold || ps == Ps::MainFlatFold;
    const bool ss = ps_per_sample(ps);      /* PC (2026-11-15): AGSS */
    ID3D11PixelShader *merged = fold ? (flat ? g_sh.psMainFlatFoldMerged.get() : g_sh.psMainFoldMerged.get())
                                : ss ? (flat ? g_sh.psMainFlatSsMerged.get() : g_sh.psMainSsMerged.get())
                                     : (flat ? g_sh.psMainFlatMerged.get() : g_sh.psMainMerged.get());
    ID3D11PixelShader *bound = fold ? (flat ? g_sh.psMainFlatFold.get() : g_sh.psMainFold.get())
                               : ss ? (flat ? g_sh.psMainFlatSs.get() : g_sh.psMainSs.get())
                                    : (flat ? g_sh.psMainFlat.get() : g_sh.psMain.get());
    Rings *const rg = &t_rings;             /* (this submitting thread's rings, for the D3D thread) */
    q::run([rg, s, nSlots, merged, bound, count, first, base, inst, lights, instances, capId] {
        /* the slots that hold another texture or sampler than the last
         * merged draw left there (g_mergedSlots, the D3D thread's) */
        MergedSlots &ms = g_mergedSlots;
        for (uint32_t k = 1; k < nSlots;) {
            if (ms.srv[k] == s.srv[k] && ms.samp[k] == s.samp[k]) {
                k++;
                continue;
            }
            uint32_t e = k + 1;
            while (e < nSlots && !(ms.srv[e] == s.srv[e] && ms.samp[e] == s.samp[e]))
                e++;
            qdc()->PSSetShaderResources(1 + k, e - k, &s.srv[k]);
            qdc()->PSSetSamplers(k, e - k, &s.samp[k]);
            for (uint32_t j = k; j < e; j++) {
                ms.srv[j] = s.srv[j];
                ms.samp[j] = s.samp[j];
            }
            k = e;
        }
        qdc()->PSSetShader(merged, nullptr, 0);
        vu1_inst_upload(rg, inst + instances - 1);  /* (PC: a C = Ad draw's second instance reads the next place) */
        light_upload(rg, lights);
        cap::gpu_begin(capId);              /* PC (2026-11-06): (the frame capture's queries, if capturing) */
        qdc()->DrawIndexedInstanced(count, instances, first, base, vu1_inst_slot(inst));
        cap::gpu_end(capId);
        qdc()->PSSetShader(bound, nullptr, 0);
    });
}

/* PC (2026-11-06): a pixel shader's name (the frame capture's notes) */
const char *ps_name(ID3D11PixelShader *ps)
{
    if (ps == nullptr) return "none";
    if (ps == g_sh.psMain.get()) return "main";
    if (ps == g_sh.psMainFlat.get()) return "main flat";
    if (ps == g_sh.psDepthCopy.get()) return "depth copy";
    if (ps == g_sh.psPresent.get()) return "present";
    if (ps == g_sh.psPfxColor.get()) return "pfx colour";
    if (ps == g_sh.psPlane.get()) return "plane";
    if (ps == g_sh.psFlare.get()) return "flare";
    if (ps == g_sh.psDepthResolve.get()) return "depth resolve";
    if (ps == g_sh.psFxaa.get()) return "fxaa";
    if (ps == g_sh.psColorResolve.get()) return "colour resolve";
    if (ps == g_sh.psMirrorSync.get()) return "mirror sync";
    if (ps == g_sh.psMainMerged.get() || ps == g_sh.psMainFlatMerged.get()) return "main merged";
    if (ps == g_sh.psMainMirror.get() || ps == g_sh.psMainFlatMirror.get()) return "mirror pass";
    if (ps == g_sh.psMainMirrorMerged.get() || ps == g_sh.psMainFlatMirrorMerged.get()) return "mirror pass merged";
    if (ps == g_sh.psMainFold.get() || ps == g_sh.psMainFlatFold.get()) return "main fold";
    /* PC (2026-11-15): AGSS's */
    if (ps == g_sh.psMainSs.get() || ps == g_sh.psMainFlatSs.get()) return "main per sample";
    if (ps == g_sh.psMainSsMerged.get() || ps == g_sh.psMainFlatSsMerged.get()) return "main merged per sample";
    if (ps == g_sh.psMainSsMirror.get() || ps == g_sh.psMainFlatSsMirror.get()) return "mirror pass per sample";
    if (ps == g_sh.psMainSsMirrorMerged.get() || ps == g_sh.psMainFlatSsMirrorMerged.get())
        return "mirror pass merged per sample";
    return "other";
}

void bind_vs_cb(const void *data, size_t bytes)
{
    if (q::t_forward) {                     /* PC: (the submit stage's: the data along) */
        sq::run_with(data, bytes, [bytes](const uint8_t *p) { bind_vs_cb(p, bytes); });
        return;
    }
    if (bytes > g_sh.cbVsSize)
        bytes = g_sh.cbVsSize;
    if (g_vsBound && bytes == g_bound.vsCbBytes && same_bytes(data, g_bound.vsCb, bytes))
        return;
    ProfScope prof(PROF_CB);                /* (the verbose monitor's profile) */
    g_profCount[PCOUNT_CBS]++;
    cap::bind(D3DB_VSCB);
    if (t_rings.cb.on) {
        if (ring_write(true, data, bytes)) {
            memcpy(g_bound.vsCb, data, bytes);
            g_bound.vsCbBytes = bytes;
        }
        return;
    }
    update_cb(g_sh.cbVs.get(), data, bytes);
    memcpy(g_bound.vsCb, data, bytes);
    g_bound.vsCbBytes = bytes;
    ID3D11Buffer *cb = g_sh.cbVs.get();
    q::run([cb] { qdc()->VSSetConstantBuffers(0, 1, &cb); });
}

void bind_ps_cb(const void *data, size_t bytes)
{
    if (q::t_forward) {                     /* PC: (the submit stage's: the data along) */
        sq::run_with(data, bytes, [bytes](const uint8_t *p) { bind_ps_cb(p, bytes); });
        return;
    }
    if (bytes > g_sh.cbPsSize)
        bytes = g_sh.cbPsSize;
    if (g_bound.ps != nullptr && bytes == g_bound.psCbBytes && same_bytes(data, g_bound.psCb, bytes))
        return;
    ProfScope prof(PROF_CB);
    g_profCount[PCOUNT_CBS]++;
    cap::bind(D3DB_PSCB);
    if (t_rings.cb.on) {
        if (ring_write(false, data, bytes)) {
            memcpy(g_bound.psCb, data, bytes);
            g_bound.psCbBytes = bytes;
        }
        return;
    }
    update_cb(g_sh.cbPs.get(), data, bytes);
    memcpy(g_bound.psCb, data, bytes);
    g_bound.psCbBytes = bytes;
    ID3D11Buffer *cb = g_sh.cbPs.get();
    q::run([cb] { qdc()->PSSetConstantBuffers(0, 1, &cb); });
}

static D3D11_BLEND d3d_factor(pgl::gs::BlendFactor f)
{
    using pgl::gs::BlendFactor;
    switch (f) {
    case BlendFactor::Zero: return D3D11_BLEND_ZERO;
    case BlendFactor::One: return D3D11_BLEND_ONE;
    case BlendFactor::SrcC: return D3D11_BLEND_SRC1_ALPHA;
    case BlendFactor::InvSrcC: return D3D11_BLEND_INV_SRC1_ALPHA;
    case BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
    case BlendFactor::DstColor: return D3D11_BLEND_DEST_COLOR;
    }
    return D3D11_BLEND_ONE;
}

ID3D11BlendState *blend_state(const pgl::gs::Blend &b, uint8_t writeMask, bool adInst)
{
    const uint64_t key = (uint64_t)pgl::gs::packBlendKey(b, writeMask) | (adInst ? 1ull << 32 : 0ull);
    if (key == t_blendLastKey)
        return t_blendLast;
    StateLock lock;
    auto it = g_blend.find(key);
    if (it != g_blend.end()) {
        t_blendLastKey = key;
        t_blendLast = it->second.get();
        return t_blendLast;
    }
    D3D11_BLEND_DESC d = {};
    D3D11_RENDER_TARGET_BLEND_DESC &r = d.RenderTarget[0];
    r.BlendEnable = b.enable ? TRUE : FALSE;
    r.SrcBlend = b.enable ? d3d_factor(b.src) : D3D11_BLEND_ONE;
    r.DestBlend = b.enable ? d3d_factor(b.dst) : D3D11_BLEND_ZERO;
    r.BlendOp = !b.enable ? D3D11_BLEND_OP_ADD
              : b.op == pgl::gs::BlendOp::Add ? D3D11_BLEND_OP_ADD
              : b.op == pgl::gs::BlendOp::Subtract ? D3D11_BLEND_OP_SUBTRACT : D3D11_BLEND_OP_REV_SUBTRACT;
    /* the GS writes As: the alpha channel never blends.  PC (2026-11-05): a
     * C = Ad draw of two instances (pgl_d3d.h PSF_AD_INST) keeps Ad for its
     * first (o0.a 0, o1.a 0) and writes As for its second (o1.a 1) */
    r.SrcBlendAlpha = D3D11_BLEND_ONE;
    r.DestBlendAlpha = adInst ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_ZERO;
    r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r.RenderTargetWriteMask = writeMask & 15;
    Com<ID3D11BlendState> s;
    dev()->CreateBlendState(&d, s.put());
    ID3D11BlendState *p = s.get();
    g_blend[key] = std::move(s);
    t_blendLastKey = key;
    t_blendLast = p;
    return p;
}

ID3D11BlendState *blend_plain(uint8_t writeMask)
{
    pgl::gs::Blend b = {};
    b.enable = false;
    return blend_state(b, writeMask);
}

static D3D11_COMPARISON_FUNC d3d_compare(pgl::gs::Compare c)
{
    using pgl::gs::Compare;
    switch (c) {
    case Compare::Never: return D3D11_COMPARISON_NEVER;
    case Compare::Always: return D3D11_COMPARISON_ALWAYS;
    case Compare::Less: return D3D11_COMPARISON_LESS;
    case Compare::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
    case Compare::Equal: return D3D11_COMPARISON_EQUAL;
    case Compare::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case Compare::Greater: return D3D11_COMPARISON_GREATER;
    case Compare::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
    }
    return D3D11_COMPARISON_ALWAYS;
}

ID3D11DepthStencilState *depth_state(bool test, pgl::gs::Compare func, bool write, StencilOp op, uint8_t ref)
{
    /* PC (2026-11-04): the stencil's part in the key (bits 5..7: the op, bit
     * 8: ref) - the alpha mirror's states are as few as the depth's */
    uint32_t key = (test ? 1u : 0u) | ((uint32_t)func << 1) | (write ? 0x10u : 0u) | ((uint32_t)op << 5) |
                   ((ref & 1u) << 8);
    if (key < 512 && g_depthFast[key] != nullptr)
        return g_depthFast[key];
    StateLock lock;
    auto it = g_depth.find(key);
    if (it != g_depth.end()) {
        if (key < 512)
            g_depthFast[key] = it->second.get();
        return it->second.get();
    }
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = (test || write) ? TRUE : FALSE;
    d.DepthFunc = test ? d3d_compare(func) : D3D11_COMPARISON_ALWAYS;
    d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilEnable = op != StencilOp::None ? TRUE : FALSE;
    /* PC (2026-11-06): the mirror is bit 1 (pgl_d3d.h kMirrorBit); Replace
     * and DatePair write bits 0..1 (the pair counts through them), the
     * inverts and the fold's ops bit 1 alone */
    d.StencilReadMask = kMirrorBit;
    d.StencilWriteMask = (op == StencilOp::Replace || op == StencilOp::DatePair) ? (uint8_t)(kMirrorBit | 0x01)
                                                                                 : kMirrorBit;
    d.FrontFace.StencilFunc = (op == StencilOp::DateKeep || op == StencilOp::DateInvert || op == StencilOp::FoldDate ||
                               op == StencilOp::DatePair)
                                  ? D3D11_COMPARISON_EQUAL : D3D11_COMPARISON_ALWAYS;
    d.FrontFace.StencilPassOp = op == StencilOp::Replace ? D3D11_STENCIL_OP_REPLACE
                              : op == StencilOp::DateInvert ? D3D11_STENCIL_OP_INVERT
                              : op == StencilOp::DatePair ? D3D11_STENCIL_OP_INCR
                              : op == StencilOp::FoldReplace ? D3D11_STENCIL_OP_ZERO : D3D11_STENCIL_OP_KEEP;
    /* PC (2026-11-05): the folded mirror pass marks on a depth failure (pgl_d3d.h StencilOp) */
    d.FrontFace.StencilDepthFailOp = op == StencilOp::FoldReplace ? D3D11_STENCIL_OP_REPLACE
                                   : op == StencilOp::FoldDate ? D3D11_STENCIL_OP_INVERT : D3D11_STENCIL_OP_KEEP;
    d.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    d.BackFace = d.FrontFace;
    Com<ID3D11DepthStencilState> s;
    dev()->CreateDepthStencilState(&d, s.put());
    ID3D11DepthStencilState *p = s.get();
    g_depth[key] = std::move(s);
    if (key < 512)
        g_depthFast[key] = p;
    return p;
}

ID3D11RasterizerState *raster_state(int cull, bool scissor)
{
    uint32_t key = (uint32_t)cull | (scissor ? 4u : 0u);
    if (key < 8 && g_rasterFast[key] != nullptr)
        return g_rasterFast[key];
    StateLock lock;
    auto it = g_raster.find(key);
    if (it != g_raster.end()) {
        if (key < 8)
            g_rasterFast[key] = it->second.get();
        return it->second.get();
    }
    D3D11_RASTERIZER_DESC d = {};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = cull == 1 ? D3D11_CULL_BACK : cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    /* the VU1 programs' front faces are counter-clockwise in GL's y-up NDC
     * (docs/models.md 4.6) */
    d.FrontCounterClockwise = TRUE;
    d.DepthClipEnable = TRUE;
    d.ScissorEnable = scissor ? TRUE : FALSE;
    Com<ID3D11RasterizerState> s;
    dev()->CreateRasterizerState(&d, s.put());
    ID3D11RasterizerState *p = s.get();
    g_raster[key] = std::move(s);
    if (key < 8)
        g_rasterFast[key] = p;
    return p;
}

ID3D11SamplerState *sampler_state(bool magLinear, bool minLinear, int wrapU, int wrapV)
{
    uint32_t key = (magLinear ? 1u : 0u) | (minLinear ? 2u : 0u) | ((uint32_t)wrapU << 2) | ((uint32_t)wrapV << 4);
    if (key < 64 && g_samplerFast[key] != nullptr)
        return g_samplerFast[key];
    StateLock lock;
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) {
        if (key < 64)
            g_samplerFast[key] = it->second.get();
        return it->second.get();
    }
    D3D11_SAMPLER_DESC d = {};
    if (magLinear && minLinear)
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    else if (magLinear)
        d.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    else if (minLinear)
        d.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    else
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = wrapU == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressV = wrapV == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    Com<ID3D11SamplerState> s;
    dev()->CreateSamplerState(&d, s.put());
    ID3D11SamplerState *p = s.get();
    g_samplers[key] = std::move(s);
    if (key < 64)
        g_samplerFast[key] = p;
    return p;
}

/* PC (2026-11-13): a replacement texture's sampler (pgl_texrepl.h): linear
 * between its texels and its mip levels, or the hardware's anisotropic
 * filter at [Graphics] AnisotropicFiltering's level (aniso > 1). */
ID3D11SamplerState *sampler_state_repl(int wrapU, int wrapV, int aniso)
{
    aniso = aniso < 1 ? 1 : aniso > 16 ? 16 : aniso;
    const uint32_t key = 0x10000u | (uint32_t)aniso << 8 | ((uint32_t)wrapU << 2) | ((uint32_t)wrapV << 4);
    static ID3D11SamplerState *fast[17][4];     /* (by the level and the two wraps) */
    ID3D11SamplerState *&slot = fast[aniso][(wrapU != 0 ? 1 : 0) | (wrapV != 0 ? 2 : 0)];
    if (slot != nullptr)
        return slot;
    StateLock lock;
    auto it = g_samplers.find(key);
    if (it != g_samplers.end())
        return slot = it->second.get();
    D3D11_SAMPLER_DESC d = {};
    d.Filter = aniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.MaxAnisotropy = (UINT)aniso;
    d.AddressU = wrapU == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressV = wrapV == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    Com<ID3D11SamplerState> s;
    dev()->CreateSamplerState(&d, s.put());
    ID3D11SamplerState *p = s.get();
    g_samplers[key] = std::move(s);
    return slot = p;
}

uint32_t stream_vertices(const void *data, uint32_t stride, uint32_t count, ID3D11Buffer **vb)
{
    ProfScope prof(PROF_STREAM);            /* (the verbose monitor's profile) */
    g_vstream.index = false;
    uint32_t at = stream_put(g_vstream, data, stride * count, stride);
    *vb = g_vstream.buf.get();
    return at / stride;
}

uint32_t stream_indices(const uint32_t *data, uint32_t count, ID3D11Buffer **ib)
{
    ProfScope prof(PROF_STREAM);
    g_istream.index = true;
    uint32_t at = stream_put(g_istream, data, 4 * count, 4);
    *ib = g_istream.buf.get();
    return at / 4;
}

bool make_texture(int w, int h, const uint32_t *rgba, Com<ID3D11Texture2D> &tex,
                  Com<ID3D11ShaderResourceView> &srv)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba;
    init.SysMemPitch = (UINT)w * 4;
    tex.reset();
    srv.reset();
    if (FAILED(dev()->CreateTexture2D(&d, &init, tex.put())))
        return false;
    return SUCCEEDED(dev()->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
}

}  /* namespace pglr */
