/* pgl_textures.cpp - the VRAM address registry and the texture cache
 * (docs/graphics.md 5 and 6, docs/renderer.md "Textures").
 *
 * Nothing here stores GS memory.  Every object the game would have put into
 * VRAM is registered under the blocks it covers - a Tex1 set's uploads (an
 * upload group: the live Tex1Image), an image upload, a render target - in
 * a block owner map of two layers (the RGB / Z bytes and the top byte of
 * 32-bit words).  A texture (TEX0) is resolved through the owners of its
 * blocks: a render target is sampled as it is, uploads are decoded with
 * gstex (laid out the way the GS lays them out, read in the texture's
 * format, through the CLUT buffer state) and cached.  The GS CLUT buffer is
 * modelled by gstex::ClutBuffer, loaded at every TEX0 / TEX2 write with
 * CLD != 0 from the owner of CBP's blocks.
 *
 * Cost per frame (the game re-uploads everything it draws every frame):
 *   - a Tex1 set seen before is recognised by its header and upload table
 *     (no parse); only its small uploads (CLUTs, animated) are hashed; its
 *     layout is relative to the base it was first laid out at, so a set the
 *     VRAM allocator moves is not laid out again.  Each upload is placed at
 *     the base the set had when it was recorded (a set moves several times
 *     a frame: evicted and reloaded, a wheel face per wheel), and every block
 *     keeps the base of the copy that wrote it, which the draws recorded
 *     before a move sample;
 *   - PSMCT32 image uploads (movie frames and IPU pictures in 16 x 16
 *     macroblocks, icons, CLUT images) go into a CPU image of their buffer
 *     (a "surface", one per DBP / DBW) - a memcpy each - and the surface's
 *     GPU texture is updated once, when a draw uses it;
 *   - decoded textures are cached by content (set identity, TEX0 fields
 *     relative to the set, the palette, TEXA where it is used);
 *   - PC: a set's textures are decoded ahead on a worker as the game loads
 *     the set (pgl_prewarm.cpp; TexJob below), under the keys a draw works
 *     out for them. */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/pgl/native/pgl_win32.h"

#include "engine/pgl/native/gs_tex1.h"
#include "engine/pgl/native/gs_tex_decode.h"
#include "engine/pgl/native/gs_tex_layout.h"
#include "engine/pgl/native/gs_tex_space.h"
#include "engine/pgl/native/pgl_texrepl.h"      /* PC (2026-11-13): the texture replacements */
#include "platform/log.h"

namespace pglr {

namespace {

using gstex::BlockRange;

uint64_t hash_bytes(const void *p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull)
{
    const uint8_t *s = static_cast<const uint8_t *>(p);
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, s, 8);
        h = mul64(h ^ w, 0xFF51AFD7ED558CCDull);
        h ^= h >> 32;
        s += 8;
        n -= 8;
    }
    while (n-- > 0)
        h = mul64(h ^ *s++, 0x100000001B3ull);
    return h ^ (h >> 29);
}

inline uint64_t mix(uint64_t h, uint64_t v)
{
    return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

/* PC: a fingerprint of a big upload's bytes - 32 words of 8 bytes spread
 * over it, its first and last included - cheap enough for every upload of
 * every frame: a set's texels written again in place (no new load stamp)
 * change it (group_for; docs/renderer.md 6, "Changed in place"). */
uint64_t sample_hash(const void *p, size_t n)
{
    const uint8_t *s = static_cast<const uint8_t *>(p);
    if (n < 32 * 8)
        return hash_bytes(p, n, 0x5A3C96E1ull);
    uint64_t h = 0x5A3C96E1ull ^ n;
    const size_t step = (n - 8) / 31;
    for (size_t i = 0; i < 32; i++) {
        uint64_t w;
        memcpy(&w, s + (i == 31 ? n - 8 : i * step), 8);
        h = mul64(h ^ w, 0xFF51AFD7ED558CCDull);
        h ^= h >> 32;
    }
    return h;
}

/* PC: a stronger combine (one-to-one in v, multiply and shift) for the keys
 * that stand for content: the block tags, the CLUT loads, the CLUT rows and
 * the palettes (CPU time, docs/renderer.md 6). */
inline uint64_t comb(uint64_t h, uint64_t v)
{
    h = mul64(h ^ v, 0xFF51AFD7ED558CCDull);
    return h ^ (h >> 32);
}

const uint32_t BLOCK_MASK = gstex::GS_MEM_BLOCKS - 1;

/* Owner-map layers. */
enum : uint8_t { LAYER_RGB = 1, LAYER_TOP = 2 };

uint8_t layers_of(uint32_t psm)
{
    switch (psm) {
    case gstex::PSMCT24: case gstex::PSMZ24:
        return LAYER_RGB;
    case gstex::PSMT8H: case gstex::PSMT4HL: case gstex::PSMT4HH:
        return LAYER_TOP;
    default:
        return LAYER_RGB | LAYER_TOP;
    }
}

bool psm_top(uint32_t psm)
{
    return psm == gstex::PSMT8H || psm == gstex::PSMT4HL || psm == gstex::PSMT4HH;
}

/* A Tex1Image as the game holds it (G001_flare.h, 32-bit). */
struct RawTex1 {
    uint32_t magic, self, ptr08, size;
    uint16_t vramBase, vramSize, nTex, nXfer;
    uint32_t tex, xfer, ptrTable, clutAnim;
    uint8_t  unk28[8];
};
struct RawXfer {
    uint32_t data;
    uint16_t dbp;
    uint8_t  dbw, dpsm;
    uint16_t w, h;
};
static_assert(sizeof(RawTex1) == 0x30 && sizeof(RawXfer) == 0xC, "Tex1Image layout");

/* OBJ_PLANE: a byte plane of pfx_split_rgb_planes (the afterimage,
 * graphics.md 8.6): one colour channel of a captured frame held in the top
 * bytes of a PSMCT32 buffer's words (layer 1 only), read as PSMT8H / T4HL /
 * T4HH through the CLUT. */
enum ObjKind : uint8_t { OBJ_GROUP, OBJ_IMAGE, OBJ_SURFACE, OBJ_TARGET, OBJ_PLANE };

struct GroupUpload {
    BlockRange range;           /* at layoutBase */
    std::vector<std::pair<uint16_t, uint16_t>> runs;    /* PC: its very blocks, first / count (upload_under) */
    uint64_t hash = 0;          /* the data at the last upload: small ones (CLUTs) hashed whole, PC: big ones
                                   by sample_hash */
    bool isSmall = false;
    uint32_t ver = 0;           /* PC: its data changed (UploadRefs) */
    std::vector<uint8_t> last;  /* PC: a small one's bytes when last hashed (group_for) */
};

/* (UploadRefs: pgl_render.h - PC, 2026-11-05: the resolved runs' texture
 * identities carry one) */

struct Obj {
    uint32_t id = 0;
    ObjKind kind = OBJ_IMAGE;
    uint64_t serial = 0;        /* when it last wrote its blocks */
    uint64_t lastFrame = 0;
    double lastUse = 0.0;       /* PC: g_execSeconds when last uploaded (a group: kept that long unowned) */
    /* group: set, space, uploads and marks are at layoutBase; the set is
     * at curBase now (delta() moves blocks between the two) */
    const void *img = nullptr;
    uint64_t stamp = 0;
    uint32_t contentVer = 0;    /* PC: its data changed (parsed again, a small upload): g_blockGen */
    uint32_t parseVer = 0;      /* PC: parsed again (its uploads anew: UploadRefs) */
    bool live = false;          /* PC: (registry_end_frame) it owns a block */
    gstex::Tex1Set set;
    gstex::TexSpace space;
    bool laidOut = false;
    uint32_t layoutBase = 0, curBase = 0;
    RawTex1 rawHdr = {};
    std::vector<RawXfer> rawXfer;
    std::vector<GroupUpload> uploads;
    /* PC (CPU time, 2026-11-03): runs of consecutive blocks with the same
     * layers (group_mark) - a block, layers pair each was ~8 ns a block, the
     * cars' sets marked at a new place every frame */
    struct MarkRun {
        uint16_t first, count;
        uint8_t layers;
    };
    std::vector<MarkRun> marks;
    /* image */
    gstex::Transfer t;
    std::vector<uint8_t> data;
    uint64_t key = 0;
    /* surface: the CT32 pixels of buffer (sdbp, sdbw), sw x sh */
    uint32_t sdbp = 0, sdbw = 0;
    int sw = 0, sh = 0;
    std::vector<uint32_t> spx;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;     /* dirty since the GPU copy */
    Com<ID3D11Texture2D> stex;
    Com<ID3D11ShaderResourceView> ssrv;
    int texW = 0, texH = 0;
    /* target */
    Target *target = nullptr;
    bool needRemark[2] = {true, true};
    BlockRange targetRange;
    int markW = 0, markH = 0;                   /* the GS geometry targetRange was marked with */
    uint32_t markFbw = 0;
    /* surface: the region (hx, hy, hw, hh) holds the pixels of a high-res
     * image (g_hires, key hkey; 0: none) - registry_hires_readback */
    uint64_t hkey = 0;
    int hx = 0, hy = 0, hw = 0, hh = 0;
    /* plane: the top bytes of the pw x ph PSMCT32 buffer (pbp, pfbw) hold
     * channel pch of the capture pcap (its native pixels, capW x capH); the
     * texture last read through a palette (key: palette, PSM, serial) */
    uint32_t pbp = 0, pfbw = 0;
    int pw = 0, ph = 0, pch = 0;
    Com<ID3D11ShaderResourceView> pcap;
    int capW = 0, capH = 0;
    Com<ID3D11Texture2D> pdec;
    Com<ID3D11RenderTargetView> pdecRtv;
    Com<ID3D11ShaderResourceView> pdecSrv;
    uint64_t pdecKey = 0;

    uint32_t delta() const { return (curBase - layoutBase) & BLOCK_MASK; }
};

uint32_t g_own[2][gstex::GS_MEM_BLOCKS];       /* [0] RGB / Z layer, [1] top byte */
/* PC (CPU time): what each block holds, as a tag of its last write (for a
 * Tex1 set its id, base and data version; for an image upload its content
 * key; for a target its id - the same each time they mark the same blocks
 * again; a new g_markGen for a surface, a plane, a release): a CLUT load is
 * known by its blocks' tags (registry_clut_load's memo).  The front caches
 * (CLUT loads, texture binds) hold while the regions of VRAM they read keep
 * their generation (g_regionGen: 256 blocks each, moved when a tag - so an
 * owner - changes in it; the uploads of a frame touch a few of them) and
 * g_tagEpoch stays (it moves when a cache they point into drops entries). */
uint64_t g_blockGen[2][gstex::GS_MEM_BLOCKS];
uint64_t g_markGen;
uint64_t g_tagEpoch = 1;
constexpr uint32_t kRegionShift = 8, kRegions = gstex::GS_MEM_BLOCKS >> kRegionShift;
uint32_t g_regionGen[kRegions];

/* The regions (a bit each) whose tags changed: a generation further. */
void regions_changed(uint64_t mask)
{
    static_assert(kRegions <= 64, "a region mask of 64 bits");
    for (uint32_t r = 0; mask != 0; r++, mask >>= 1)
        if (mask & 1) {
            g_regionGen[r]++;
            g_profCount[PCOUNT_REGIONS]++;  /* (the verbose monitor's counts) */
        }
}

/* The generations of up to four regions, as a front cache entry saw them. */
struct RegionSnap {
    uint8_t n = 0;
    uint8_t r[4] = {};
    uint32_t gen[4] = {};
    bool add(uint32_t region)
    {
        for (uint8_t i = 0; i < n; i++)
            if (r[i] == region)
                return true;
        if (n == 4)
            return false;
        r[n] = (uint8_t)region;
        gen[n] = g_regionGen[region];
        n++;
        return true;
    }
    /* the regions of blocks first .. first + count - 1 (the VRAM wraps):
     * false when more than four */
    bool add_blocks(uint32_t first, uint32_t count)
    {
        if (count == 0)
            return true;
        const uint32_t mask = gstex::GS_MEM_BLOCKS - 1;
        const uint32_t r0 = (first & mask) >> kRegionShift;
        const uint32_t r1 = ((first + count - 1) & mask) >> kRegionShift;
        for (uint32_t r = r0, k = 0; k < kRegions; r = (r + 1) % kRegions, k++) {
            if (!add(r))
                return false;
            if (r == r1)
                return true;
        }
        return true;
    }
    bool same() const
    {
        for (uint8_t i = 0; i < n; i++)
            if (g_regionGen[r[i]] != gen[i])
                return false;
        return true;
    }
};
/* For blocks a Tex1 set owns: the VRAM base the set was uploaded at when it
 * wrote the block.  A set the allocator moves within a frame leaves its
 * earlier copies in VRAM (the GS keeps the bytes), and draws recorded while
 * it was there sample them: their delta is the one of the copy, not of the
 * set's latest upload. */
uint16_t g_ownBase[2][gstex::GS_MEM_BLOCKS];
uint64_t g_serial = 0;
uint32_t g_nextId = 1;
std::unordered_map<uint32_t, std::unique_ptr<Obj>> g_objs;
std::unordered_map<const void *, uint64_t> g_stamps;    /* Tex1Image -> load stamp */
uint64_t g_nextStamp = 1;
std::unordered_map<const void *, uint32_t> g_groupByImg;
std::unordered_map<uint64_t, uint32_t> g_imageByKey;
std::unordered_map<uint64_t, uint32_t> g_surfaceByKey;
std::unordered_map<uint32_t, uint32_t> g_planeByBlock;  /* plane objects by their first block */
std::vector<uint32_t> g_targetObjs;
/* PC (CPU time, 2026-11-03): the target objects by the regions (kRegions,
 * 256 blocks each) their targetRange touches, for touch_targets - every
 * upload of a frame looked at every target (a 32-car race: hundreds of each,
 * the most of a race frame's upload time); rebuilt when a target object
 * comes, goes or marks another range (g_targetIndexDirty). */
std::vector<Obj *> g_targetByRegion[gstex::GS_MEM_BLOCKS >> 8];  /* (a target object goes only with a
                                                                     release, which dirties the index) */
bool g_targetIndexDirty = true;

/* The frames pfx_split_rgb_planes captured, by the first plane's block (the
 * three planes of a split share one capture). */
struct Capture {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};
std::unordered_map<uint32_t, Capture> g_captures;

/* PC: the high-resolution images (registry_hires_readback): the native
 * pixels of a readback, by the content of its GS pixels (hires_key); at most
 * kHiresMax, the least recently used dropped. */
struct Hires {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;                           /* its GS size */
    uint64_t used = 0;
};
constexpr size_t kHiresMax = 64;
std::unordered_map<uint64_t, Hires> g_hires;
uint64_t g_hiresClock = 0;

uint64_t hires_key(const void *pixels, int w, int h)
{
    return mix(hash_bytes(pixels, (size_t)w * (size_t)h * 4), (uint32_t)w | (uint64_t)(uint32_t)h << 32) | 1;
}

/* Debug aid: GT3PC_AFTERIMAGE_LOG=1 logs (first 400 lines, with the frame
 * number) every plane split and every PSMT8H / T4HL / T4HH texture with the
 * owner of its block's top byte. */
bool afterimage_log()
{
    static int state = -1;
    static int lines = 0;
    if (state < 0) {
        const char *e = getenv("GT3PC_AFTERIMAGE_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging the afterimage planes and the top-byte textures (GT3PC_AFTERIMAGE_LOG,"
                         " 400 lines)");
    }
    return state == 1 && lines++ < 400;
}

const char *kind_name(ObjKind k)
{
    switch (k) {
    case OBJ_GROUP: return "a Tex1 set";
    case OBJ_IMAGE: return "an image upload";
    case OBJ_SURFACE: return "a PSMCT32 surface";
    case OBJ_TARGET: return "a render target";
    case OBJ_PLANE: return "a plane";
    }
    return "?";
}
std::vector<uint8_t> g_mask;                    /* mark_rect's scratch */

/* The GS CLUT buffer: one 1 KB store of 512 halfwords, as the GS has it - a
 * CT16 / CT16S entry e is halfword e, a CT32 entry e (0..255) has its low
 * half at halfword e and its high half at e + 256.  A load writes it in its
 * own CPSM (through a ClutBuffer per load; loads repeated within a frame -
 * every textured model part reloads its CLUT - come from a memo of what the
 * load wrote), a texture reads it in its TEX0's CPSM.  (Two arrays and the
 * last load's CPSM, as before, read a CT16 palette from the CT32 array once
 * a CT32 load had come between: the menu font's CT16 palette after a kanji
 * glyph's CT32 one - a car name went brown and blotchy after its "・",
 * user report.) */
struct Clut {
    uint16_t h[512] = {};
} g_clut;

struct ClutLoad {
    uint32_t cpsm = 0, base = 0, n = 0;
    uint64_t key = 0;                   /* its memo key (what it writes: g_clutRow) */
    uint32_t nRows = 0;                 /* the rows it writes and what it writes there (clut_rows_init) */
    uint8_t rowIdx[32];
    uint8_t rowSrc[32];                 /* where: entries 16 j .. 16 j + 15 (j << 1), their high halves (| 1) */
    uint64_t rowId[32];
    uint32_t raw[256];
};
/* PC (CPU time): the memo is keyed by what a load reads - CBP, CPSM, CSM,
 * CSA, T8 and the tags of its blocks (g_blockGen) - so it holds across
 * uploads elsewhere and across frames (at most 4096 loads, then emptied). */
std::unordered_map<uint64_t, ClutLoad, KeyHash> g_clutMemo;   /* PC: (KeyHash: the keys are hashes) */
/* PC (CPU time): the key of the last load (the same load again writes what
 * the buffer holds), and what each 16-halfword row of the buffer holds (the
 * key of the load that wrote it and its row there; 0 the zeros it starts
 * with): a load that would write what is there already is skipped (model
 * parts take turns with a few CLUTs), and the palettes texture_bind reads
 * are kept by their rows (g_palMemo). */
uint64_t g_clutLastKey;
bool g_clutLastValid;
uint64_t g_clutRow[32];
/* PC (CPU time): a load only notes, row by row, which load's entries the
 * row holds now (g_clutRowSrc, the row's bit in g_clutDirty); the halfwords
 * are copied into g_clut when they are read (clut_lookup: a palette worked
 * out, rare) - parts taking turns with a few CLUTs copied all 256 entries
 * at nearly every load (CLUT LOAD 0.56 ms with its key checks cut short). */
struct ClutRowSrc {
    const ClutLoad *load = nullptr;
    uint8_t src = 0;
};
ClutRowSrc g_clutRowSrc[32];
uint32_t g_clutDirty;
/* The palettes texture_bind read, by their rows' contents, entries, CSA,
 * CPSM and TEXA (direct-mapped) */
struct PalMemo {
    uint64_t key = 0;
    uint64_t hash = 0;
    bool valid = false;
    uint32_t pal[256];
};
/* (PC, 2026-11-03: 1024 - a 32-car race frame binds a thousand textures
 * past the front cache, most of them indexed, and with the sets' CLUT keys
 * kept from frame to frame (set_key) their palettes are the same ones) */
constexpr uint32_t kPalMemos = 1024;
PalMemo g_palMemo[kPalMemos];
/* PC (CPU time): the loads by their TEX0 fields (CBP, CPSM, CSM, CSA, T8),
 * direct-mapped, with the CLUT's blocks (first, count): checked by its key
 * worked out again from those blocks' tags and CBP's owner (clut_key: a few
 * multiplies; the VRAM's other regions change all the time - the wheel
 * faces, the course's sets streamed in - and do not matter), while
 * g_tagEpoch stays. */
struct ClutFront {
    uint32_t fields = ~0u;
    uint64_t epoch = 0;
    uint32_t first = 0, count = 0;
    uint64_t tagKey = 0;                /* PC: clut_key of those blocks (a set's load has its own key) */
    const ClutLoad *load = nullptr;
};
/* (2048: a race frame loads several hundred CLUTs, in the same order every
 * frame - 256 slots missed half of them, each evicting another.  PC,
 * 2026-11-03: 16384 - a 32-car race frame loads ~3,500, and 890 of them
 * missed 2048 direct-mapped slots) */
constexpr uint32_t kClutFronts = 16384;
ClutFront g_clutFront[kClutFronts];
/* PC (CPU time, 2026-11-03): a Tex1 set's loads by the set (its object and
 * parse), the fields and CBP's place in it (preKey), direct-mapped, while
 * g_tagEpoch stays and the uploads under the CLUT are unchanged (refs; over
 * 4 of them, the set's data version) - ClutFront is by CBP, and the cars'
 * sets move between VRAM slots from frame to frame (1,500 of a 32-car
 * frame's CLUT loads missed it) */
struct SetClutFront {
    uint64_t preKey = 0, epoch = 0;
    UploadRefs refs;
    uint32_t contentVer = 0;
    const ClutLoad *load = nullptr;
};
constexpr uint32_t kSetClutFronts = 16384;
SetClutFront g_setClutFront[kSetClutFronts];

/* (CLUT entries are CT32, CT16 or CT16S: the 16-bit ones by their CPSM.) */
inline bool clut_16bit(uint32_t cpsm)
{
    return cpsm == gstex::PSMCT16 || cpsm == gstex::PSMCT16S;
}

/* The rows of the buffer load l writes and what it writes there: CT32
 * entries are two rows (their low halves at row r, the high at r + 16). */
void clut_rows_init(ClutLoad &l)
{
    l.nRows = 0;
    const uint32_t rows = std::min<uint32_t>((l.n + 15) / 16, 16);
    for (uint32_t j = 0; j < rows; j++) {
        const uint64_t c = comb(l.key, j + 1) | 1;
        if (!clut_16bit(l.cpsm)) {
            const uint32_t r = (l.base / 16 + j) & 15;
            l.rowIdx[l.nRows] = (uint8_t)r;
            l.rowSrc[l.nRows] = (uint8_t)(j << 1);
            l.rowId[l.nRows++] = c;
            l.rowIdx[l.nRows] = (uint8_t)(r + 16);
            l.rowSrc[l.nRows] = (uint8_t)(j << 1 | 1);
            l.rowId[l.nRows++] = comb(c, 0x48) | 1;
        } else {
            l.rowIdx[l.nRows] = (uint8_t)((l.base / 16 + j) & 31);
            l.rowSrc[l.nRows] = (uint8_t)(j << 1);
            l.rowId[l.nRows++] = c;
        }
    }
}

/* PC (CPU time, 2026-11-05): the rows' version - moved whenever a load
 * writes them - and the palette keys texture_bind worked out of them while
 * it stays (palette_key: 32 products for a 256-entry palette, at most of a
 * race frame's 5,000 binds; a part's load most often leaves the rows as
 * they are, clut_apply) */
uint64_t g_clutRowsVer = 1;
struct PalKeyMemo {
    uint64_t rowsVer = 0, texaKey = 0, key = 0;
    uint32_t n = 0, csa = 0;
    bool ct16 = false;
};
PalKeyMemo g_palKeyMemo[128];

void clut_apply(const ClutLoad &l)
{
    bool same = true;
    for (uint32_t i = 0; i < l.nRows && same; i++)
        same = g_clutRow[l.rowIdx[i]] == l.rowId[i];
    if (same)
        return;                         /* PC: the buffer holds it already */
    for (uint32_t i = 0; i < l.nRows; i++) {
        const uint32_t r = l.rowIdx[i];
        g_clutRow[r] = l.rowId[i];
        g_clutRowSrc[r].load = &l;
        g_clutRowSrc[r].src = l.rowSrc[i];
        g_clutDirty |= 1u << r;
    }
    g_clutRowsVer++;
}

/* The rows noted since into g_clut: row r is halfwords 16 r .. 16 r + 15,
 * entries 16 j .. of its load (their low or high halves). */
void clut_materialize()
{
    for (uint32_t r = 0; g_clutDirty != 0; r++) {
        if (!(g_clutDirty & (1u << r)))
            continue;
        g_clutDirty &= ~(1u << r);
        const ClutRowSrc &s = g_clutRowSrc[r];
        const uint32_t *raw = s.load->raw + (s.src >> 1) * 16;
        uint16_t *h = &g_clut.h[r * 16];
        if (s.src & 1) {
            for (int k = 0; k < 16; k++)
                h[k] = (uint16_t)(raw[k] >> 16);
        } else {
            for (int k = 0; k < 16; k++)
                h[k] = (uint16_t)raw[k];
        }
    }
}

/* PC: what the n entries a texture reads at CSA in cpsm hold (the rows'
 * contents; TEXA expands CT16 entries). */
uint64_t palette_key(uint32_t n, bool ct16, uint32_t csa, uint64_t texaKey)
{
    /* PC (CPU time, 2026-11-05): the same rows as when it was last worked
     * out for these fields (g_clutRowsVer) */
    PalKeyMemo &m = g_palKeyMemo[((csa & 31) << 2 | (n == 256 ? 2u : 0u) | (ct16 ? 1u : 0u)) & 127];
    if (m.rowsVer == g_clutRowsVer && m.n == n && m.csa == csa && m.ct16 == ct16 && m.texaKey == texaKey)
        return m.key;
    uint64_t k = comb(comb(0x3C6EF372FE94F82Bull, n | (uint64_t)ct16 << 16 | (uint64_t)csa << 24), texaKey);
    const uint32_t rows = (n + 15) / 16;
    for (uint32_t j = 0; j < rows; j++) {
        if (!ct16) {
            const uint32_t r = (csa + j) & 15;
            k = comb(comb(k, g_clutRow[r]), g_clutRow[r + 16]);
        } else {
            k = comb(k, g_clutRow[(csa + j) & 31]);
        }
    }
    m.rowsVer = g_clutRowsVer;
    m.n = n;
    m.csa = csa;
    m.ct16 = ct16;
    m.texaKey = texaKey;
    m.key = k;
    return k;
}

/* Index i of a psm texture with CLUT offset csa read in cpsm (the texture's
 * TEX0.CPSM). */
uint32_t clut_lookup(uint32_t psm, uint32_t cpsm, uint32_t csa, uint32_t i, const gstex::Texa &texa)
{
    if (g_clutDirty != 0)
        clut_materialize();             /* PC: the rows the loads noted */
    const bool t8 = psm == gstex::PSMT8 || psm == gstex::PSMT8H;
    const uint32_t e = csa * 16 + (t8 ? (i & 255) : (i & 15));
    if (!clut_16bit(cpsm))
        return (uint32_t)g_clut.h[e & 255] | (uint32_t)g_clut.h[(e & 255) + 256] << 16;
    return gstex::expand_ct16(g_clut.h[e & 511], texa);
}

/* The indices decode_rect returns without a CLUT (index x 0x01010101)
 * through the palette. */
void apply_palette(std::vector<uint32_t> &px, const uint32_t *palette, uint32_t n)
{
    /* PC (CPU time): n is 16 or 256 - a mask, not a division per texel */
    const uint32_t mask = (n & (n - 1)) == 0 ? n - 1 : 0;
    if (mask != 0) {
        for (uint32_t &p : px)
            p = palette[p & mask];
        return;
    }
    for (uint32_t &p : px)
        p = palette[(p & 0xFF) % n];
}

struct CachedTex {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    gstex::Rect rect;
    uint64_t lastFrame = 0;
    double lastUse = 0.0;       /* PC: g_execSeconds when last bound */
    uint32_t mean = 0;          /* the texels' mean (TexBinding::mean: GT3PC_PGL_TRACE) */
    uint8_t aMin = 0, aMax = 255;   /* PC (2026-11-04): the texels' least and greatest alpha (TexBinding) */
    bool pre = false;           /* PC: decoded ahead (pgl_prewarm.cpp), not bound yet */
    /* (the F9 trace: registry_trace_texture) what it was decoded from - the
     * owner of TBP0's block (0: decoded ahead, before any object), its kind,
     * a Tex1 set's image and load stamp - and when (g_registryFrame) */
    uint32_t srcObj = 0;
    uint8_t srcKind = 0;
    const void *srcImg = nullptr;
    uint64_t srcStamp = 0;
    uint64_t madeFrame = 0;
    bool ahead = false;
    /* PC (2026-11-13, the texture replacements: pgl_texrepl.h) - a set's
     * texture: its content hash once worked out (hashed: decoded while the
     * hooks were on); gen, moved when what it is bound as changes without a
     * new decode (its replacement loaded): the front caches and the resolved
     * runs hold it by that number and ask again; its replacement once
     * loaded, with its mean, alpha range (the GS's units) and bytes; Select
     * mode's highlight (the picture in green) */
    uint64_t chash = 0;
    bool hashed = false;
    uint32_t gen = 0;
    Com<ID3D11Texture2D> replTex;
    Com<ID3D11ShaderResourceView> replSrv;
    uint32_t replMean = 0;
    uint8_t replAMin = 0, replAMax = 128;
    size_t replBytes = 0;
    bool replStale = false;     /* (its file changed - a refresh: asked for anew, shown until the new one is loaded) */
    Com<ID3D11Texture2D> hlTex;
    Com<ID3D11ShaderResourceView> hlSrv;
};
/* PC: the decoded textures are kept while they fit in kTexBudget bytes (the
 * least recently bound go first beyond it, never one bound in the last
 * kTexKeep seconds) or until unused for kTexExpire seconds: a course's
 * textures stay decoded from lap to lap (at 300 frames unused they went in
 * about a second at a high frame rate, decoded again every lap). */
constexpr size_t kTexBudget = 256u << 20;   /* PC (2026-11-07): 256, was 512 - the address space (platform/memstat.h);
                                               a 32-car race decodes ~80 MB */
constexpr double kTexKeep = 5.0, kTexExpire = 600.0;
/* (4096: several hundred textures a race frame, in the same order every
 * frame - 256 slots missed half the binds; g_texFront below.  PC,
 * 2026-11-03: 32768 - a 32-car race frame binds ~3,900, and 1,358 of them
 * missed 4096 direct-mapped slots) */
constexpr uint32_t kTexFronts = 32768;
/* PC (2026-11-05): 32768 - a 32-car race binds ~5,000 textures a frame,
 * most of them sets', and 8,192 direct-mapped slots had them evicting one
 * another (TEX SLOW 762 a frame in the user's capture, ~1.4 us each) */
constexpr uint32_t kGroupFronts = 32768;    /* PC: (g_groupFront) */

/* Debug aid: GT3PC_TEX_STRESS=<kinds> (comma separated, `all' for every
 * one; stress_on) - the registry's caches made tiny so that their rare
 * cases run all the time, to make a rare wrong texture common and find
 * which cache it comes from (user report 2026-10-25, renderer.md 11):
 * `fronts' the texture binds' and the CLUT loads' front caches 8 slots and
 * the palette memo 2 (their entries replaced at nearly every use); `clut'
 * the CLUT load memo emptied beyond 8 loads (the rows still noted by their
 * loads materialized each time); `evict' the decoded textures beyond 64 MB
 * evicted every frame, none kept for being recent, any unused for 2 s
 * expired, and sets that own no block dropped after 2 s.  (`thread': the
 * executor's start delayed, pgl_render.cpp.)  Logged once. */
struct TexStress {
    uint32_t texFronts = kTexFronts;
    uint32_t groupFronts = kGroupFronts;
    uint32_t clutFronts = kClutFronts;
    uint32_t setClutFronts = kSetClutFronts;
    uint32_t palMemos = kPalMemos;
    size_t clutMemoMax = 16384;             /* (PC: 4096 before 32-car races) */
    size_t budget = kTexBudget;
    double keep = kTexKeep, expire = kTexExpire;
    uint32_t housekeep = 64;                /* frames between the decoded textures' housekeeping */
};
TexStress g_stress;

void tex_stress_init()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    std::string on;
    if (stress_on("fronts")) {
        g_stress.texFronts = 8;
        g_stress.groupFronts = 8;
        g_stress.clutFronts = 8;
        g_stress.setClutFronts = 8;
        g_stress.palMemos = 2;
        on += "; fronts: the texture and CLUT front caches (by TEX0 and by set) 8 slots, the palette memo 2";
    }
    if (stress_on("clut")) {
        g_stress.clutMemoMax = 8;
        on += "; clut: the CLUT load memo emptied beyond 8 loads";
    }
    if (stress_on("evict")) {
        g_stress.budget = 64u << 20;
        g_stress.keep = 0.0;
        g_stress.expire = 2.0;
        g_stress.housekeep = 1;
        on += "; evict: decoded textures beyond 64 MB evicted every frame, unused ones and unowned sets after 2 s";
    }
    if (!on.empty())
        pclog::write("pgl: GT3PC_TEX_STRESS%s", on.c_str());
}
size_t g_texBytes;

/* The mean of decoded texels (R | G << 8 | B << 16 | A << 24). */
uint32_t texel_mean(const std::vector<uint32_t> &px)
{
    if (px.empty())
        return 0;
    /* PC (CPU time, 2026-11-03): 4 texels at a time, each channel's bytes
     * summed by SSE2's sum of absolute differences (5 % of the executor with
     * a lamp's textures decoded every frame) - the same sums */
    __m128i acc[4];
    __m128i mask[4];
    for (int c = 0; c < 4; c++) {
        acc[c] = _mm_setzero_si128();
        mask[c] = _mm_set1_epi32((int)(0xFFu << (8 * c)));
    }
    const size_t n4 = px.size() / 4;
    const __m128i *v = reinterpret_cast<const __m128i *>(px.data());
    for (size_t i = 0; i < n4; i++) {
        const __m128i x = _mm_loadu_si128(v + i);
        for (int c = 0; c < 4; c++)
            acc[c] = _mm_add_epi64(acc[c], _mm_sad_epu8(_mm_and_si128(x, mask[c]), _mm_setzero_si128()));
    }
    uint64_t s[4];
    for (int c = 0; c < 4; c++) {
        alignas(16) uint64_t lanes[2];
        _mm_store_si128(reinterpret_cast<__m128i *>(lanes), acc[c]);
        s[c] = lanes[0] + lanes[1];
    }
    for (size_t i = n4 * 4; i < px.size(); i++)
        for (int c = 0; c < 4; c++)
            s[c] += (px[i] >> (8 * c)) & 0xFF;
    uint32_t m = 0;
    for (int c = 0; c < 4; c++)
        m |= (uint32_t)((s[c] + px.size() / 2) / px.size()) << (8 * c);
    return m;
}

/* PC (2026-11-04): the least and greatest alpha byte of the texels (the
 * alpha mirror: whether a draw's alpha writes have one value of bit 7;
 * 4 texels at a time: the alpha lane alone, the others held at 0xFF for the
 * least and 0 for the greatest). */
void texel_alpha_range(const std::vector<uint32_t> &px, uint8_t &aMin, uint8_t &aMax)
{
    aMin = 0;
    aMax = 255;
    if (px.empty())
        return;
    __m128i lo = _mm_set1_epi8((char)0xFF), hi = _mm_setzero_si128();
    const __m128i low3 = _mm_set1_epi32(0x00FFFFFF), top = _mm_set1_epi32((int)0xFF000000u);
    const size_t n4 = px.size() / 4;
    const __m128i *v = reinterpret_cast<const __m128i *>(px.data());
    for (size_t i = 0; i < n4; i++) {
        const __m128i x = _mm_loadu_si128(v + i);
        lo = _mm_min_epu8(lo, _mm_or_si128(x, low3));
        hi = _mm_max_epu8(hi, _mm_and_si128(x, top));
    }
    alignas(16) uint8_t l[16], h[16];
    _mm_store_si128(reinterpret_cast<__m128i *>(l), lo);
    _mm_store_si128(reinterpret_cast<__m128i *>(h), hi);
    uint8_t mn = 255, mx = 0;
    for (int k = 3; k < 16; k += 4) {
        mn = std::min(mn, l[k]);
        mx = std::max(mx, h[k]);
    }
    for (size_t i = n4 * 4; i < px.size(); i++) {
        const uint8_t a = (uint8_t)(px[i] >> 24);
        mn = std::min(mn, a);
        mx = std::max(mx, a);
    }
    aMin = mn;
    aMax = mx;
}
std::unordered_map<uint64_t, CachedTex, KeyHash> g_cache;    /* PC: (KeyHash: the keys are hashes) */
/* PC (2026-11-13, the texture replacements): the hashed entries by their
 * content hash (a replacement loaded finds its textures: registry_end_frame),
 * the bytes of the replacements they hold, and texrepl::serial() as last
 * seen */
std::unordered_multimap<uint64_t, CachedTex *, KeyHash> g_byHash;
size_t g_replBytes;
uint64_t g_replSerial;

/* An entry about to leave the cache. */
void cache_forget(CachedTex &c)
{
    if (c.hashed) {
        auto r = g_byHash.equal_range(c.chash);
        for (auto i = r.first; i != r.second; ++i)
            if (i->second == &c) {
                g_byHash.erase(i);
                break;
            }
    }
    g_replBytes -= c.replBytes;
    c.replBytes = 0;
}

void cache_hashed(CachedTex &c, uint64_t chash)
{
    c.chash = chash;
    c.hashed = true;
    g_byHash.emplace(chash, &c);
}
uint64_t g_registryFrame = 0;

/* PC (CPU time, 2026-11-03): the indices of a set's indexed texture, before
 * its palette, kept once it was decoded twice (index_image) - by the same key
 * without the palette.  An animated CLUT (the cars' lamps) gives its
 * textures a new palette, so new decoded textures, frame after frame; their
 * indices read out of the GS layout were most of the work (`pgl decoded
 * again', TEX DECODE).  At most 16 MB, all let go beyond. */
std::unordered_map<uint64_t, std::vector<uint32_t>, KeyHash> g_indexImages;
std::unordered_map<uint64_t, uint8_t, KeyHash> g_indexSeen;
size_t g_indexBytes = 0;

/* PC (CPU time): obj's lookups, direct-mapped by id (a slot cleared as its
 * object goes: obj_forget) */
struct ObjSlot {
    uint32_t id = 0;
    Obj *o = nullptr;
};
ObjSlot g_objSlot[256];

Obj *obj(uint32_t id)
{
    if (id == 0)
        return nullptr;
    ObjSlot &s = g_objSlot[id & 255];
    if (s.id == id)
        return s.o;
    auto it = g_objs.find(id);
    Obj *o = it == g_objs.end() ? nullptr : it->second.get();
    if (o != nullptr) {
        s.id = id;
        s.o = o;
    }
    return o;
}

void obj_forget(uint32_t id)
{
    ObjSlot &s = g_objSlot[id & 255];
    if (s.id == id)
        s = ObjSlot();
}

Obj *new_obj(ObjKind kind)
{
    std::unique_ptr<Obj> o(new Obj());
    o->id = g_nextId++;
    if (g_nextId == 0)
        g_nextId = 1;
    o->kind = kind;
    Obj *p = o.get();
    g_objs[p->id] = std::move(o);
    return p;
}

BlockRange shifted(const BlockRange &r, uint32_t d)
{
    BlockRange s = r;
    if (!r.empty()) {
        s.first = r.first + d;
        s.last = r.last + d;
    }
    return s;
}

/* ---- decoded-texture keys (texture_bind; the textures decoded ahead) ---- */

/* The formats whose texels TEXA expands (CT24, CT16 and their Z forms):
 * the others decode the same under any TEXA (an indexed one's palette has
 * it in its content). */
bool texa_used(uint32_t psm)
{
    return psm == gstex::PSMCT24 || psm == gstex::PSMZ24 || psm == gstex::PSMCT16 || psm == gstex::PSMCT16S ||
           psm == gstex::PSMZ16 || psm == gstex::PSMZ16S;
}

/* What a decoded texture is of: PSM, TW, TH, TBW, the rectangle, the
 * palette's content, TEXA (PC: where it is used - a set's textures decoded
 * ahead are known without it), CSA. */
uint64_t tex_key(const gstex::Tex0 &t, const gstex::Rect &rect, uint64_t palHash, const gstex::Texa &ta)
{
    uint64_t key = mix(mix(0x7E, t.psm | (uint64_t)t.tw << 8 | (uint64_t)t.th << 12 | (uint64_t)t.tbw << 16),
                       rect.x | (uint64_t)rect.y << 16 | (uint64_t)rect.w << 32 | (uint64_t)rect.h << 48);
    const uint64_t texaKey = texa_used(t.psm) ? (ta.ta0 | ta.aem << 8 | (uint64_t)ta.ta1 << 16) : 0;
    return mix(mix(key, palHash), texaKey | (uint64_t)t.csa << 24);
}

/* Calls fn(block) for every block the rectangle touches: one block_number
 * per block cell of the format's block grid (a block holds one blockW x
 * blockH rectangle of a page), not per pixel. */
template <class Fn> void for_each_block(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w,
                                        uint32_t h, Fn fn)
{
    if (w == 0 || h == 0)
        return;
    const gstex::PsmGeom g = gstex::psm_geom(psm);
    const uint32_t bx0 = x / g.blockW, bx1 = (x + w - 1) / g.blockW;
    const uint32_t by0 = y / g.blockH, by1 = (y + h - 1) / g.blockH;
    for (uint32_t by = by0; by <= by1; by++)
        for (uint32_t bx = bx0; bx <= bx1; bx++)
            fn(gstex::block_number(psm, bp, bw, bx * g.blockW, by * g.blockH) & BLOCK_MASK);
}

/* PC (CPU time, 2026-11-03): the blocks a texture's rectangle reads, a bit
 * each - what its decoded texels are made of.  An upload is under the
 * texture only when it writes one of them (upload_meets): by the two block
 * ranges (block numbers are swizzled: a range holds blocks of neither) a
 * car's lamp CLUT, rewritten every frame, was under textures that never
 * read it, and decoded them again every frame (`pgl decoded again'). */
struct BlockSet {
    /* (PC, 2026-11-05: 32-bit words - a variable 64-bit shift is a runtime
     * call in the 32-bit build, one per block tested, upload_under) */
    uint32_t bits[gstex::GS_MEM_BLOCKS / 32] = {};
    void add(uint32_t b) { bits[(b & BLOCK_MASK) >> 5] |= 1u << (b & 31); }
    bool has(uint32_t b) const { return ((bits[(b & BLOCK_MASK) >> 5] >> (b & 31)) & 1u) != 0; }
};

/* The blocks of texture t's rectangle with TBP0 at tbp. */
void texture_blocks(const gstex::Tex0 &t, uint32_t tbp, const gstex::Rect &rect, BlockSet &out)
{
    for_each_block(t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h, [&](uint32_t b) { out.add(b); });
}

/* PC (2026-11-13, the texture replacements): the content hash of a set's
 * texture decoded into px (pgl_texrepl.h) - the texels of its rectangle but
 * those in the blocks of the set's CLUTs (every register block's and every
 * variation's: the 16 x 16 entries at its CBP), read as zeros.  The game
 * keeps a CLUT in an unused corner of a texture's space and rewrites it as a
 * lamp glows: bytes that are not the picture's, and would give one picture
 * many names.  set: at its vramBase, as parsed; tbp: TBP0 there. */
uint64_t set_content_hash_of(const gstex::Tex1Set &set, const gstex::Tex0 &t, uint32_t tbp, const gstex::Rect &rect,
                             const std::vector<uint32_t> &px, bool *maskedOut);

/* (GT3PC_TEXREPL_LOG=1: a line for every texture hashed - its name, size,
 * format, the range of its alpha, whether CLUT blocks were left out) */
uint64_t set_content_hash(const gstex::Tex1Set &set, const gstex::Tex0 &t, uint32_t tbp, const gstex::Rect &rect,
                          const std::vector<uint32_t> &px)
{
    static int log = -1;
    if (log < 0) {
        const char *e = getenv("GT3PC_TEXREPL_LOG");
        log = (e != nullptr && e[0] == '1') ? 1 : 0;
    }
    bool masked = false;
    const uint64_t h = set_content_hash_of(set, t, tbp, rect, px, &masked);
    if (log != 0) {
        uint8_t lo = 255, hi = 0;
        uint32_t orRgb = 0;
        for (uint32_t p : px) {
            lo = std::min<uint8_t>(lo, (uint8_t)(p >> 24));
            hi = std::max<uint8_t>(hi, (uint8_t)(p >> 24));
            orRgb |= p & 0xFFFFFF;
        }
        char name[17];
        texrepl::hash_name(h, name);
        pclog::write("textures: hashed %s %ux%u at (%u,%u) %s TBW %u, CPSM %s, alpha %u..%u%s%s", name, rect.w, rect.h,
                     rect.x, rect.y, gstex::psm_name(t.psm), t.tbw, gstex::psm_name(t.cpsm), lo, hi,
                     masked ? ", CLUT blocks left out" : "", orRgb == 0 ? ", all black" : "");
    }
    return h;
}

uint64_t set_content_hash_of(const gstex::Tex1Set &set, const gstex::Tex0 &t, uint32_t tbp, const gstex::Rect &rect,
                             const std::vector<uint32_t> &px, bool *maskedOut)
{
    BlockSet cl;
    bool any = false;
    /* (a T8's CLUT: the 16 x 16 entries at CBP; a T4's: the 8 x 2 of its
     * CSA there, gs_tex_decode.h ClutBuffer) */
    auto add = [&](uint32_t psm, uint32_t cpsm, uint32_t cbp, uint32_t csa) {
        if (!gstex::psm_valid(psm) || !gstex::psm_indexed(psm))
            return;
        if (cpsm != gstex::PSMCT32 && cpsm != gstex::PSMCT16 && cpsm != gstex::PSMCT16S)
            return;
        const bool t8 = psm == gstex::PSMT8 || psm == gstex::PSMT8H;
        const uint32_t x = t8 ? 0 : (csa & 1) * 8, y = t8 ? 0 : ((csa >> 1) & 7) * 2;
        for_each_block(cpsm, cbp & BLOCK_MASK, 1, x, y, t8 ? 16 : 8, t8 ? 16 : 2, [&](uint32_t b) { cl.add(b); });
        any = true;
    };
    for (const gstex::TexRegs &r : set.tex) {
        const gstex::Tex0 tt = gstex::Tex0::decode(r.tex0);
        add(tt.psm, tt.cpsm, tt.cbp, tt.csa);
    }
    for (const auto &v : set.variations)
        for (uint32_t e : v) {
            const uint32_t ti = e >> 23;
            if (ti < set.tex.size())
                add(gstex::Tex0::decode(set.tex[ti].tex0).psm, (e >> 19) & 0xF, ((e >> 5) & 0x3FFF) + set.vramBase,
                    e & 0x1F);
        }
    if (any) {
        const gstex::PsmGeom g = gstex::psm_geom(t.psm);
        static thread_local std::vector<uint32_t> masked;
        bool copied = false;
        for (uint32_t cy = rect.y / g.blockH * g.blockH; cy < rect.y + rect.h; cy += g.blockH)
            for (uint32_t cx = rect.x / g.blockW * g.blockW; cx < rect.x + rect.w; cx += g.blockW) {
                if (!cl.has(gstex::block_number(t.psm, tbp, t.tbw, cx, cy)))
                    continue;
                if (!copied) {
                    masked = px;
                    copied = true;
                }
                const uint32_t x0 = std::max(cx, rect.x), y0 = std::max(cy, rect.y);
                const uint32_t x1 = std::min(cx + g.blockW, rect.x + rect.w);
                const uint32_t y1 = std::min(cy + g.blockH, rect.y + rect.h);
                for (uint32_t y = y0; y < y1; y++)
                    memset(masked.data() + (size_t)(y - rect.y) * rect.w + (x0 - rect.x), 0, (size_t)(x1 - x0) * 4);
            }
        if (copied) {
            *maskedOut = true;
            return texrepl::content_hash(masked.data(), rect.w, rect.h);
        }
    }
    return texrepl::content_hash(px.data(), rect.w, rect.h);
}

/* An upload under the blocks of `blocks' (null: under its range r) - its
 * range first, then its own blocks. */
bool upload_under(const GroupUpload &gu, const BlockRange &r, const BlockSet *blocks)
{
    if (!(gu.range.overlaps(r) || gu.range.overlaps(shifted(r, gstex::GS_MEM_BLOCKS)) ||
          shifted(gu.range, gstex::GS_MEM_BLOCKS).overlaps(r)))
        return false;
    if (blocks == nullptr)
        return true;
    for (const auto &run : gu.runs)
        for (uint32_t i = 0; i < run.second; i++)
            if (blocks->has((uint32_t)run.first + i))
                return true;
    return false;
}

/* ... read from a Tex1 set: the set by its image and load stamp (PC: not
 * the object's id - a set dropped and met again finds its decoded
 * textures), TBP0's place in it (blocks from its base) and the uploads under
 * the footprint brL (laid out at the set's base; PC: under its very blocks,
 * `blocks', BlockSet) by their data's hashes: the
 * small ones (animated CLUT data, small textures), PC: and the big ones'
 * fingerprints (texels written again in place, docs/renderer.md 6). */
uint64_t group_key(uint64_t key, const void *img, uint64_t stamp, uint32_t offset,
                   const std::vector<GroupUpload> &uploads, const BlockRange &brL, const BlockSet *blocks)
{
    key = mix(mix(key, 0x6), mix((uint64_t)(uintptr_t)img, stamp));
    key = mix(key, offset & BLOCK_MASK);
    for (const GroupUpload &gu : uploads)
        if (upload_under(gu, brL, blocks))
            key = mix(key, gu.hash);
    return key;
}

/* PC (UploadRefs): the uploads of set g under brL (laid out at the set's
 * base; under its blocks `blocks' if not null): their data hashes combined
 * into key (returned), their indices and versions into refs (if not null). */
uint64_t upload_refs(const std::vector<GroupUpload> &uploads, const BlockRange &brL, UploadRefs *refs, uint64_t key,
                     const BlockSet *blocks = nullptr)
{
    if (refs != nullptr)
        refs->n = 0;
    for (size_t i = 0; i < uploads.size(); i++) {
        const GroupUpload &gu = uploads[i];
        if (!upload_under(gu, brL, blocks))
            continue;
        key = comb(key, gu.hash);
        if (refs != nullptr && refs->n != 0xFF) {
            if (refs->n < kUploadRefs && i < 256) {
                refs->idx[refs->n] = (uint8_t)i;
                refs->ver[refs->n] = gu.ver;
                refs->n++;
            } else {
                refs->n = 0xFF;
            }
        }
    }
    return key;
}

/* Those uploads unchanged since (false when none were kept). */
bool upload_refs_same(const std::vector<GroupUpload> &uploads, const UploadRefs &r)
{
    if (r.n == 0xFF)
        return false;
    for (uint8_t k = 0; k < r.n; k++)
        if (r.idx[k] >= uploads.size() || uploads[r.idx[k]].ver != r.ver[k])
            return false;
    return true;
}

/* An upload's data hash: a small one's whole, a big one's fingerprint. */
uint64_t upload_hash(const gstex::Tex1Upload &u)
{
    if (u.data == nullptr)
        return 0;
    return u.bytes <= 4096 ? hash_bytes(u.data, u.bytes) : sample_hash(u.data, u.bytes);
}

/* A set's uploads as GroupUploads (their blocks; their data hashes). */
void uploads_of(const gstex::Tex1Set &set, std::vector<GroupUpload> &out)
{
    out.clear();
    std::vector<uint16_t> blocks;
    for (const gstex::Tex1Upload &u : set.uploads) {
        GroupUpload gu;
        gu.range = gstex::image_blocks(u.t.dpsm, u.t.dbp, u.t.dbw, 0, 0, u.t.w, u.t.h);
        blocks.clear();
        for_each_block(u.t.dpsm, u.t.dbp, u.t.dbw, 0, 0, u.t.w, u.t.h,
                       [&](uint32_t b) { blocks.push_back((uint16_t)b); });
        std::sort(blocks.begin(), blocks.end());
        blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
        for (uint16_t b : blocks) {
            if (!gu.runs.empty() && gu.runs.back().first + gu.runs.back().second == b)
                gu.runs.back().second++;
            else
                gu.runs.push_back({b, 1});
        }
        gu.isSmall = u.bytes <= 4096;
        gu.hash = upload_hash(u);
        out.push_back(gu);
    }
}

/* PC: fn(region) for each region (kRegionShift) of block range r, the VRAM
 * wrapping at GS_MEM_BLOCKS; each once. */
template <class Fn> void for_each_region(const BlockRange &r, Fn fn)
{
    if (r.empty())
        return;
    /* (the regions counted unwrapped: a range of nearly all of the VRAM
     * may end in the region it starts in) */
    const uint32_t first = r.first & BLOCK_MASK;
    const uint64_t r0 = first >> kRegionShift, r1 = ((uint64_t)first + r.count() - 1) >> kRegionShift;
    if (r1 - r0 + 1 >= kRegions) {
        for (uint32_t g = 0; g < kRegions; g++)
            fn(g);
        return;
    }
    for (uint64_t g = r0; g <= r1; g++)
        fn((uint32_t)(g % kRegions));
}

/* PC: g_targetByRegion again from the target objects' ranges. */
void target_index_build()
{
    for (std::vector<Obj *> &v : g_targetByRegion)
        v.clear();
    for (uint32_t id : g_targetObjs) {
        Obj *o = obj(id);
        if (o != nullptr)
            for_each_region(o->targetRange, [&](uint32_t g) { g_targetByRegion[g].push_back(o); });
    }
    g_targetIndexDirty = false;
}

/* Targets whose blocks another object wrote must re-mark at their next draw.
 * PC: only the targets of r's regions looked at (g_targetByRegion: a target
 * whose range overlaps r shares a region with it), each by the same test. */
void touch_targets(const BlockRange &r, uint32_t writer)
{
    if (g_targetIndexDirty)
        target_index_build();
    for_each_region(r, [&](uint32_t region) {
    for (Obj *o : g_targetByRegion[region]) {
        if (o->id == writer)
            continue;
        /* ranges may pass 0x3FFF: compare both wrapped forms */
        BlockRange a = o->targetRange, b = r;
        a.first &= BLOCK_MASK;
        a.last = a.first + (o->targetRange.last - o->targetRange.first);
        b.first &= BLOCK_MASK;
        b.last = b.first + (r.last - r.first);
        if (a.overlaps(b) || shifted(a, gstex::GS_MEM_BLOCKS).overlaps(b) ||
            a.overlaps(shifted(b, gstex::GS_MEM_BLOCKS))) {
            o->needRemark[0] = true;
            o->needRemark[1] = true;
        }
    }
    });
}

/* Marks the exact block set of a rectangle for o. */
void mark_rect(Obj *o, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               uint8_t layers, std::vector<std::pair<uint32_t, uint8_t>> *record)
{
    BlockRange r = gstex::image_blocks(psm, bp, bw, x, y, w, h);
    if (r.empty())
        return;
    const uint32_t id = o->id;
    /* (the tag: an image's bytes never change, a target's blocks read as
     * zeros wherever a texture or CLUT is composed: their tags stay the
     * same each time they mark the same blocks; surfaces, planes: new) */
    uint64_t gen;
    if (o->kind == OBJ_IMAGE)
        gen = (comb(comb(0x510E527FADE682D1ull, o->key), id) & ~(3ull << 62)) | (1ull << 62);
    else if (o->kind == OBJ_TARGET)
        gen = (comb(0x9B05688C2B3E6C1Full, id) & ~(7ull << 61)) | (1ull << 61);
    else
        gen = ++g_markGen;
    uint64_t changed = 0;                   /* (the regions whose tags changed) */
    bool planeLost = false;
    uint32_t lostBlock = 0;
    for_each_block(psm, bp, bw, x, y, w, h, [&](uint32_t b) {
        if (layers & LAYER_RGB) {
            g_own[0][b] = id;
            if (g_blockGen[0][b] != gen) {
                g_blockGen[0][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
        }
        if (layers & LAYER_TOP) {
            if (g_blockGen[1][b] != gen) {
                g_blockGen[1][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
            /* (GT3PC_AFTERIMAGE_LOG: a target taking an afterimage plane's
             * bytes before drawAfterimage read them) */
            if (!planeLost && o->kind == OBJ_TARGET && g_own[1][b] != id) {
                const Obj *prev = obj(g_own[1][b]);
                if (prev != nullptr && prev->kind == OBJ_PLANE) {
                    planeLost = true;
                    lostBlock = prev->pbp;
                }
            }
            g_own[1][b] = id;
        }
        if (record != nullptr)
            record->push_back({b, layers});
    });
    regions_changed(changed);
    if (planeLost && o->target != nullptr && afterimage_log())
        pclog::write("pgl afterimage: frame %llu: the plane at block 0x%X loses its bytes to a write of the target at"
                     " block 0x%X (created as psm 0x%02X, GS %dx%d, FBW %u)",
                     (unsigned long long)frame_number(), lostBlock, o->target->base, o->target->psm,
                     o->target->gsW, o->target->gsH, o->target->fbw);
    touch_targets(r, o->id);
}

/* ---- groups ------------------------------------------------------------ */

void group_prepare_uploads(Obj *g)
{
    g->marks.clear();
    uploads_of(g->set, g->uploads);
}

void group_layout(Obj *g)
{
    if (g->laidOut)
        return;
    gstex::tex1_layout(g->set, g->space);
    g->laidOut = true;
}

void group_mark(Obj *g)
{
    if (g->marks.empty()) {
        /* the marks at layoutBase, recorded (by block, an upload's layers
         * added to another's on the same block - the same blocks marked),
         * then applied at curBase */
        std::vector<uint8_t> layersAt(gstex::GS_MEM_BLOCKS, 0);
        for (const gstex::Tex1Upload &u : g->set.uploads) {
            const uint8_t layers = layers_of(u.t.dpsm);
            for_each_block(u.t.dpsm, u.t.dbp, u.t.dbw, 0, 0, u.t.w, u.t.h,
                           [&](uint32_t b) { layersAt[b & BLOCK_MASK] |= layers; });
        }
        for (uint32_t b = 0; b < gstex::GS_MEM_BLOCKS; b++) {
            const uint8_t l = layersAt[b];
            if (l == 0)
                continue;
            if (!g->marks.empty() && g->marks.back().layers == l &&
                g->marks.back().first + g->marks.back().count == b)
                g->marks.back().count++;
            else
                g->marks.push_back({(uint16_t)b, 1, l});
        }
    }
    const uint32_t d = g->delta();
    const uint16_t base = (uint16_t)(g->curBase & BLOCK_MASK);
    /* (the tag: bit 63 a set's, 62 an image's, 61 a target's, else a
     * g_markGen) */
    const uint64_t gen = comb(comb(comb(0x6A09E667F3BCC908ull, g->id), base), g->contentVer) | (1ull << 63);
    uint64_t changed = 0;                   /* (the regions whose tags changed) */
    for (const Obj::MarkRun &m : g->marks) {
        /* (a run moved past the last block goes on at block 0) */
        uint32_t b0 = (m.first + d) & BLOCK_MASK, left = m.count;
        while (left != 0) {
            const uint32_t n = std::min<uint32_t>(left, gstex::GS_MEM_BLOCKS - b0);
            for (int l = 0; l < 2; l++) {
                if ((m.layers & (l == 0 ? LAYER_RGB : LAYER_TOP)) == 0)
                    continue;
                std::fill(&g_own[l][b0], &g_own[l][b0] + n, g->id);
                std::fill(&g_ownBase[l][b0], &g_ownBase[l][b0] + n, base);
                /* (a region at a time: the 32-bit build shifted a 64-bit 1
                 * by a variable count with a call, per block) */
                uint64_t *tags = &g_blockGen[l][b0];
                for (uint32_t i = 0; i < n;) {
                    const uint32_t region = (b0 + i) >> kRegionShift;
                    const uint32_t end = std::min(n, ((region + 1) << kRegionShift) - b0);
                    bool diff = false;
                    for (; i < end; i++) {
                        diff |= tags[i] != gen;
                        tags[i] = gen;
                    }
                    if (diff)
                        changed |= 1ull << region;
                }
            }
            b0 = (b0 + n) & BLOCK_MASK;
            left -= n;
        }
    }
    regions_changed(changed);
    for (const GroupUpload &gu : g->uploads)
        touch_targets(shifted(gu.range, d), g->id);
    g->serial = ++g_serial;
    g->lastFrame = g_registryFrame;
    g->lastUse = g_execSeconds;
}

/* The delta (base - layoutBase) of the copy of set g that block `block' of
 * layer `layer' holds (g owns the block). */
uint32_t group_delta_at(const Obj *g, int layer, uint32_t block)
{
    return ((uint32_t)g_ownBase[layer][block & BLOCK_MASK] - g->layoutBase) & BLOCK_MASK;
}

/* The set at img is the one g holds (moved at most): no parse needed. */
bool group_same(const Obj *g, const RawTex1 &h, const RawXfer *x)
{
    const RawTex1 &o = g->rawHdr;
    if (h.self != o.self || h.ptr08 != o.ptr08 || h.size != o.size || h.vramSize != o.vramSize ||
        h.nTex != o.nTex || h.nXfer != o.nXfer || h.tex != o.tex || h.xfer != o.xfer ||
        h.ptrTable != o.ptrTable || h.clutAnim != o.clutAnim || g->rawXfer.size() != h.nXfer)
        return false;
    for (uint32_t i = 0; i < h.nXfer; i++) {
        const RawXfer &a = x[i], &b = g->rawXfer[i];
        if (a.data != b.data || a.dbw != b.dbw || a.dpsm != b.dpsm || a.w != b.w || a.h != b.h ||
            (uint16_t)(a.dbp - h.vramBase) != (uint16_t)(b.dbp - o.vramBase))
            return false;
    }
    return true;
}

void group_remember(Obj *g, const RawTex1 &h, const RawXfer *x)
{
    g->rawHdr = h;
    g->rawXfer.assign(x, x + h.nXfer);
}

/* ---- surfaces ---------------------------------------------------------- */

void surface_upload(Obj *s, int dx, int dy, int w, int h, const uint8_t *src, size_t bytes)
{
    const int needW = std::max({s->sw, (int)s->sdbw * 64, dx + w});
    const int needH = std::max(s->sh, dy + h);
    if (needW != s->sw || needH != s->sh) {
        std::vector<uint32_t> px((size_t)needW * needH, 0);
        for (int y = 0; y < s->sh; y++)
            memcpy(&px[(size_t)y * needW], &s->spx[(size_t)y * s->sw], (size_t)s->sw * 4);
        s->spx.swap(px);
        s->sw = needW;
        s->sh = needH;
        s->dx0 = 0;
        s->dy0 = 0;
        s->dx1 = needW;
        s->dy1 = needH;
    }
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        if ((size_t)(y + 1) * rowBytes > bytes)
            break;
        memcpy(&s->spx[(size_t)(dy + y) * s->sw + dx], src + (size_t)y * rowBytes, rowBytes);
    }
    if (s->dx1 <= s->dx0 || s->dy1 <= s->dy0) {
        s->dx0 = dx;
        s->dy0 = dy;
        s->dx1 = dx + w;
        s->dy1 = dy + h;
    } else {
        s->dx0 = std::min(s->dx0, dx);
        s->dy0 = std::min(s->dy0, dy);
        s->dx1 = std::max(s->dx1, dx + w);
        s->dy1 = std::max(s->dy1, dy + h);
    }
}

/* The surface's GPU texture, brought up to date. */
ID3D11ShaderResourceView *surface_srv(Obj *s)
{
    if (!s->stex || s->texW != s->sw || s->texH != s->sh) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)s->sw;
        d.Height = (UINT)s->sh;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = s->spx.data();
        init.SysMemPitch = (UINT)s->sw * 4;
        s->stex.reset();
        s->ssrv.reset();
        if (FAILED(dev()->CreateTexture2D(&d, &init, s->stex.put())) ||
            FAILED(dev()->CreateShaderResourceView(s->stex.get(), nullptr, s->ssrv.put())))
            return nullptr;
        s->texW = s->sw;
        s->texH = s->sh;
        s->dx1 = s->dx0;
        return s->ssrv.get();
    }
    if (s->dx1 > s->dx0 && s->dy1 > s->dy0) {
        /* (queued: the D3D thread, the rows written copied along) */
        const D3D11_BOX box = {(UINT)s->dx0, (UINT)s->dy0, 0, (UINT)s->dx1, (UINT)s->dy1, 1};
        const UINT rowBytes = (box.right - box.left) * 4, rows = box.bottom - box.top;
        static std::vector<uint8_t> packed;
        packed.resize((size_t)rowBytes * rows);
        for (UINT y = 0; y < rows; y++)
            memcpy(&packed[(size_t)y * rowBytes], &s->spx[(size_t)(s->dy0 + (int)y) * s->sw + s->dx0], rowBytes);
        ID3D11Texture2D *tex = s->stex.get();
        q::run_with(packed.data(), packed.size(), [tex, box, rowBytes](const uint8_t *p) {
            qdc()->UpdateSubresource(tex, 0, &box, p, rowBytes, 0);
        });
        s->dx1 = s->dx0;
    }
    return s->ssrv.get();
}

/* A space of the block range holding what the owners wrote there, in the
 * order they wrote it (the composition path). */
void compose_space(const BlockRange &r, gstex::TexSpace &space, uint64_t *key)
{
    g_profCount[PCOUNT_COMPOSES]++;         /* (the verbose monitor's profile) */
    const uint32_t count = std::min<uint32_t>(r.count(), gstex::GS_MEM_BLOCKS);
    space.reset(r.first & BLOCK_MASK, count);
    std::vector<Obj *> owners;
    std::set<uint32_t> seen;
    std::unordered_map<uint32_t, uint32_t> groupDelta;  /* a set's copy here (the first block it owns) */
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t b = (r.first + i) & BLOCK_MASK;
        for (int l = 0; l < 2; l++) {
            const uint32_t id = g_own[l][b];
            if (id != 0 && seen.insert(id).second) {
                Obj *o = obj(id);
                if (o != nullptr) {
                    owners.push_back(o);
                    if (o->kind == OBJ_GROUP)
                        groupDelta[id] = group_delta_at(o, l, b);
                }
            }
        }
    }
    std::sort(owners.begin(), owners.end(), [](const Obj *a, const Obj *b) { return a->serial < b->serial; });
    uint64_t k = 0xC0;
    for (Obj *o : owners) {
        k = mix(mix(k, o->id), o->serial);
        if (o->kind == OBJ_GROUP) {
            const uint32_t d = groupDelta[o->id];
            k = mix(k, d);
            for (size_t i = 0; i < o->set.uploads.size(); i++) {
                const gstex::Tex1Upload &u = o->set.uploads[i];
                if (u.data == nullptr)
                    continue;
                if (i < o->uploads.size())
                    k = mix(k, o->uploads[i].hash);  /* PC: the big ones' fingerprints too */
                gstex::Transfer t = u.t;
                t.dbp = (t.dbp + d) & BLOCK_MASK;
                space.upload(t, u.data, u.bytes);
            }
        } else if (o->kind == OBJ_IMAGE) {
            space.upload(o->t, o->data.data(), o->data.size());
        } else if (o->kind == OBJ_SURFACE) {
            gstex::Transfer t;
            t.dbp = o->sdbp;
            t.dbw = o->sdbw;
            t.dpsm = gstex::PSMCT32;
            t.w = (uint32_t)o->sw;
            t.h = (uint32_t)o->sh;
            space.upload(t, o->spx.data(), o->spx.size() * 4);
            k = mix(k, hash_bytes(o->spx.data(), o->spx.size() * 4));
        } else {
            log_once("pgl-compose-target", "pgl: a texture mixes uploads and render-target (or afterimage plane)"
                     " blocks: the target part reads as zeros");
        }
    }
    if (key != nullptr)
        *key = k;
}

/* Where texel (0, 0) of a texture at tbp lies in a target (pixels). */
bool target_offset(const Target &tg, uint32_t tbp, int &ox, int &oy)
{
    if (tbp == tg.base) {
        ox = oy = 0;
        return true;
    }
    static std::unordered_map<uint64_t, std::pair<int, int>> cache;
    /* (gsH too: a depth target's GS geometry follows the frame that wrote it) */
    const uint64_t key = ((uint64_t)tg.base << 40) ^ ((uint64_t)tg.fbw << 32) ^ ((uint64_t)tg.psm << 24) ^
                         ((uint64_t)tg.gsW << 14) ^ ((uint64_t)tg.gsH << 54) ^ tbp;
    auto it = cache.find(key);
    if (it != cache.end()) {
        ox = it->second.first;
        oy = it->second.second;
        return ox >= 0;
    }
    const uint32_t psm = tg.depth ? (tg.psm & 0x3F) : (tg.psm & 0x0F);
    gstex::PsmGeom g = gstex::psm_geom(psm);
    ox = oy = -1;
    const uint32_t fbw = tg.fbw ? tg.fbw : (uint32_t)((tg.gsW + 63) / 64);
    for (int y = 0; y < tg.gsH && ox < 0; y += (int)g.blockH) {
        for (int x = 0; x < tg.gsW; x += (int)g.blockW) {
            if ((gstex::block_number(psm, tg.base, fbw, (uint32_t)x, (uint32_t)y) & BLOCK_MASK) == tbp) {
                ox = x;
                oy = y;
                break;
            }
        }
    }
    cache[key] = {ox, oy};
    return ox >= 0;
}

/* ---- planes ------------------------------------------------------------ */

/* A plane read as a texture of psm (PSMT8H / T4HL / T4HH) through palette
 * (n entries, CT32 words as clut_lookup gives them): the plane's bytes are
 * the indices, the texture holds the palette's colours at the capture's
 * native size.  A full-screen pass (ps_plane) from the capture into the
 * plane's texture, done again when the palette, the PSM or the capture
 * changed.  The renderer's bound state is forgotten (d3d_invalidate): the
 * caller's draw binds its own. */
ID3D11ShaderResourceView *plane_texture(Obj *o, uint32_t psm, const uint32_t *palette, uint32_t n)
{
    const uint64_t key = mix(mix(hash_bytes(palette, (size_t)n * 4), psm), o->serial);
    if (o->pdecSrv && o->pdecKey == key)
        return o->pdecSrv.get();
    D3D11_TEXTURE2D_DESC have = {};
    if (o->pdec)
        o->pdec->GetDesc(&have);
    if (!o->pdec || have.Width != (UINT)o->capW || have.Height != (UINT)o->capH) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)o->capW;
        d.Height = (UINT)o->capH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        o->pdecSrv.reset();
        o->pdecRtv.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, o->pdec.put())) ||
            FAILED(dev()->CreateRenderTargetView(o->pdec.get(), nullptr, o->pdecRtv.put())) ||
            FAILED(dev()->CreateShaderResourceView(o->pdec.get(), nullptr, o->pdecSrv.put()))) {
            o->pdec.reset();
            o->pdecRtv.reset();
            o->pdecSrv.reset();
            log_once("pgl-plane-create", "pgl: could not create a %dx%d plane texture", o->capW, o->capH);
            return nullptr;
        }
    }
    Com<ID3D11Texture2D> ptex;
    Com<ID3D11ShaderResourceView> psrv;
    if (!make_texture((int)n, 1, palette, ptex, psrv))
        return nullptr;
    /* (queued: the D3D thread, pgl_queue.h; the state objects found here) */
    d3d_invalidate();
    ID3D11RenderTargetView *rtv = o->pdecRtv.get();
    const D3D11_VIEWPORT vp = {0, 0, (float)o->capW, (float)o->capH, 0, 1};
    ID3D11RasterizerState *rs = raster_state(0, false);
    ID3D11BlendState *bs = blend_plain(pgl::gs::WRITE_RGBA);
    ID3D11DepthStencilState *ds = depth_state(false, pgl::gs::Compare::Always, false);
    q::run([rtv, vp, rs, bs, ds] {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(1, &rtv, nullptr);
        qdc()->RSSetViewports(1, &vp);
        qdc()->RSSetState(rs);
        qdc()->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(ds, 0);
    });
    bind_vs(Vs::Full);
    bind_ps(Ps::Plane);
    CbPlane cb = {};
    cb.channel = o->pch;
    cb.shift = psm == gstex::PSMT4HH ? 4 : 0;
    cb.mask = psm == gstex::PSMT8H ? 0xFF : 0xF;
    bind_ps_cb(&cb, sizeof(cb));
    ID3D11ShaderResourceView *srv0 = o->pcap.get(), *srv1 = psrv.get();
    q::run([srv0, srv1] {
        ID3D11ShaderResourceView *srvs[2] = {srv0, srv1};
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, srvs);
        qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        qdc()->Draw(3, 0);
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(0, nullptr, nullptr);
    });
    d3d_invalidate();
    o->pdecKey = key;
    return o->pdecSrv.get();
}

}  /* namespace */

namespace {

/* ---- decoded ahead (pgl_prewarm.cpp) ------------------------------------- */

/* The textures the worker decoded, under their keys, until the executor
 * puts them in the cache (tex_pre_drain: at a texture's miss, at a frame's
 * end).  (Never freed, the lock has no destructor: the worker may still run
 * while the process exits.) */
struct PreTex {
    uint64_t key = 0;
    CachedTex ct;
};
SRWLOCK g_preTexLock = SRWLOCK_INIT;
std::vector<PreTex> *g_preTex = new std::vector<PreTex>();
std::atomic<uint32_t> g_preTexCount{0};
/* TEXA of the last texture bound from a set (the CT16 palettes and texels
 * of the sets loaded next are expanded with it; the PS2's reset value until
 * then) */
std::atomic<uint64_t> g_preTexa{0x80ull << 32};

/* A Tex1 set as the game loaded it - parsed, its uploads' bytes copied (on
 * the game thread: registry_tex1_loaded, before any link) - and its load
 * stamp.  Each texture is decoded as texture_bind decodes it from the set
 * at a draw (its CLUT loaded the way pgl loads it, pgl_clut_load: the
 * palette that draw reads), under the key it works out there. */
struct TexJob : pre::Job {
    const void *img = nullptr;
    uint64_t stamp = 0;
    gstex::Tex1Set set;
    std::vector<uint8_t> bytes;
    gstex::Texa texa;
    void run() override;
};

/* (the worker) */
void TexJob::run()
{
    gstex::TexSpace space;
    gstex::tex1_layout(set, space);
    std::vector<GroupUpload> uploads;
    uploads_of(set, uploads);
    const uint32_t base = set.vramBase & BLOCK_MASK;
    std::vector<PreTex> out;
    size_t made = 0;
    for (const gstex::TexRegs &regs : set.tex) {
        const gstex::Tex0 t = gstex::Tex0::decode(regs.tex0);
        const gstex::Clamp c = gstex::Clamp::decode(regs.clamp);
        if (!gstex::psm_valid(t.psm))
            continue;
        const uint32_t tbp = t.tbp0 & BLOCK_MASK;
        const bool indexed = gstex::psm_indexed(t.psm);
        const uint32_t palN = (t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H) ? 256 : 16;
        uint32_t pal[256];
        uint64_t palHash = 0;
        if (indexed) {
            /* (what clut_lookup gives after registry_clut_load of that
             * TEX2: the CT32 entries as they are, CT16 ones by TEXA) */
            gstex::ClutBuffer cb;
            cb.load(space, gstex::pgl_clut_load(t));
            const bool ct16 = clut_16bit(t.cpsm);
            for (uint32_t i = 0; i < palN; i++) {
                const uint32_t raw = cb.lookup_raw(t.psm, t.csa, i);
                pal[i] = ct16 ? gstex::expand_ct16(raw & 0xFFFF, texa) : raw;
            }
            palHash = hash_bytes(pal, palN * 4);
        }
        const gstex::Rect rect = gstex::decode_region(t, c);
        const BlockRange br = gstex::image_blocks(t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h);
        BlockSet blocks;
        texture_blocks(t, tbp, rect, blocks);
        const uint64_t key = group_key(tex_key(t, rect, palHash, texa), img, stamp, tbp - base, uploads, br, &blocks);
        bool dup = false;
        for (const PreTex &p : out)
            dup = dup || p.key == key;
        if (dup)
            continue;
        std::vector<uint32_t> px((size_t)rect.w * rect.h, 0);
        gstex::decode_rect(space, t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h, nullptr, t.csa, texa,
                           px.data(), rect.w);
        if (indexed)
            apply_palette(px, pal, palN);
        PreTex p;
        p.key = key;
        p.ct.rect = rect;
        p.ct.mean = texel_mean(px);
        /* PC (2026-11-13): its name, its dump, its replacement asked for
         * (loaded by the time it is drawn, most often) */
        if (texrepl::hooks_on()) {
            p.ct.chash = set_content_hash(set, t, tbp, rect, px);
            p.ct.hashed = true;
            texrepl::decoded(p.ct.chash, px.data(), rect.w, rect.h, 0);
            texrepl::ask(p.ct.chash);
        }
        p.ct.pre = true;
        p.ct.ahead = true;                  /* (the F9 trace) */
        p.ct.srcKind = OBJ_GROUP;
        p.ct.srcImg = img;
        p.ct.srcStamp = stamp;
        if (!make_texture((int)rect.w, (int)rect.h, px.data(), p.ct.tex, p.ct.srv))
            continue;
        made += px.size() * 4;
        out.push_back(std::move(p));
    }
    pre::made(pre::TEXTURE, (uint32_t)out.size(), made);
    if (out.empty())
        return;
    AcquireSRWLockExclusive(&g_preTexLock);
    for (PreTex &p : out)
        g_preTex->push_back(std::move(p));
    g_preTexCount.store((uint32_t)g_preTex->size(), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_preTexLock);
}

/* The textures decoded ahead into the cache (the executor's thread; one
 * decoded at a draw meanwhile keeps its own); false: none were waiting. */
bool tex_pre_drain()
{
    if (g_preTexCount.load(std::memory_order_acquire) == 0)
        return false;
    std::vector<PreTex> ready;
    AcquireSRWLockExclusive(&g_preTexLock);
    ready.swap(*g_preTex);
    g_preTexCount.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_preTexLock);
    for (PreTex &p : ready) {
        if (g_cache.count(p.key) != 0)
            continue;
        p.ct.lastFrame = g_registryFrame;
        p.ct.lastUse = g_execSeconds;
        p.ct.madeFrame = g_registryFrame;   /* (the F9 trace: in the cache from now) */
        g_texBytes += (size_t)p.ct.rect.w * p.ct.rect.h * 4;
        const bool hashed = p.ct.hashed;
        const uint64_t chash = p.ct.chash;
        CachedTex &in = g_cache.emplace(p.key, std::move(p.ct)).first->second;
        if (hashed)
            g_byHash.emplace(chash, &in);   /* PC (2026-11-13) */
    }
    return true;
}

/* A set just loaded at img, its stamp given: its textures to the worker. */
void tex_prewarm(const void *img, uint64_t stamp)
{
    std::unique_ptr<TexJob> j(new TexJob());
    /* (a set without uploads of its own reads another's blocks: its
     * textures are that set's at a draw) */
    if (!gstex::tex1_from_memory(img, j->set) || j->set.tex.empty() || j->set.uploads.empty())
        return;
    size_t total = 0;
    for (const gstex::Tex1Upload &u : j->set.uploads)
        if (u.data != nullptr)
            total += (u.bytes + 15) & ~(size_t)15;
    j->bytes.resize(total);
    size_t at = 0;
    for (gstex::Tex1Upload &u : j->set.uploads) {
        if (u.data == nullptr)
            continue;
        memcpy(j->bytes.data() + at, u.data, u.bytes);
        u.data = j->bytes.data() + at;
        at += (u.bytes + 15) & ~(size_t)15;
    }
    j->set.anims.clear();                   /* (they point into the game's memory) */
    j->img = img;
    j->stamp = stamp;
    j->texa = gstex::Texa::decode(g_preTexa.load(std::memory_order_relaxed));
    pre::post(j.release());
}

}  /* namespace */

/* ======================================================================== */
/* Registration                                                             */
/* ======================================================================== */

void registry_init()
{
    tex_stress_init();                      /* (GT3PC_TEX_STRESS) */
    memset(g_own, 0, sizeof(g_own));
    memset(g_ownBase, 0, sizeof(g_ownBase));
    g_tagEpoch++;
    texrepl::init();                        /* PC (2026-11-13): the texture replacements */
}

void registry_tex1_loaded(void *img)
{
    const uint64_t stamp = ++g_nextStamp;
    g_stamps[img] = stamp;
    if (pre::on())
        tex_prewarm(img, stamp);            /* PC: decoded ahead (pgl_prewarm.cpp) */
}

uint32_t registry_tex1_base(const void *img)
{
    return img != nullptr ? (uint32_t)static_cast<const RawTex1 *>(img)->vramBase & BLOCK_MASK : 0;
}

/* PC: a big upload of a known set whose bytes changed without a new load
 * (group_for: decoded again).  Always logged, the first 40 times: the
 * evidence of a menu car's garbled reflection (docs/renderer.md 11). */
static void tex1_log_changed(const void *img, size_t upload, size_t bytes)
{
    static int lines = 0;
    if (lines >= 40)
        return;
    lines++;
    pclog::write("pgl tex1: frame %llu: set %p's upload %u (%u bytes) changed in place (no new load): decoded "
                 "again%s", (unsigned long long)frame_number(), img, (unsigned)upload, (unsigned)bytes,
                 lines == 40 ? " (no more of these lines)" : "");
}

/* Debug aid: GT3PC_TEX1_LOG=1 logs (first 4000 lines, with the frame number)
 * every Tex1 set uploaded at a second VRAM base within one frame (the
 * allocator evicted and reloaded it, or TexSet::load moved it): the draws
 * recorded before the move sample the earlier copy. */
static void tex1_log_move(const void *img, uint32_t id, uint32_t base, uint32_t blocks)
{
    static int state = -1;
    static int lines = 0;
    static std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> last;    /* group: frame, base */
    if (state < 0) {
        const char *e = getenv("GT3PC_TEX1_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging Tex1 sets uploaded at two VRAM bases in one frame (GT3PC_TEX1_LOG, 4000"
                         " lines)");
    }
    if (state != 1)
        return;
    auto it = last.find(id);
    if (it != last.end() && it->second.first == g_registryFrame && it->second.second != base && lines < 4000) {
        lines++;
        pclog::write("pgl tex1: frame %llu: set %p (%u blocks) uploaded at block 0x%X, earlier this frame at"
                     " 0x%X", (unsigned long long)frame_number(), img, blocks, base, it->second.second);
    }
    last[id] = {g_registryFrame, base};
}

namespace {

/* The group of the set at img placed at vramBase (masked): recognised by
 * its header and upload table, else parsed (laid out as linked now);
 * nullptr if it does not parse.  Nothing marked.  copy: the set's header,
 * register blocks and upload records as they were when the upload was
 * recorded (registry_tex1_copy); nullptr: read at img (the game waiting). */
Obj *group_for(const void *img, uint32_t vramBase, const uint8_t *copy)
{
    auto st = g_stamps.find(img);
    if (st == g_stamps.end())
        st = g_stamps.emplace(img, ++g_nextStamp).first;
    const uint64_t stamp = st->second;
    /* PC: the header and the upload table from the copy.  Read at img on the
     * executor thread, they were whatever the game had relinked them to
     * meanwhile - tex1_set_vram_base writes the base, then every TBP0 / CBP,
     * then every DBP, for each VRAM block the allocator hands the set, on
     * the game thread while the executor runs the frame before - and a read
     * in the middle of that took the new base with old DBPs: a layout off
     * by the move, kept by every later upload (the first parse's), so the
     * set's textures decoded from the wrong place of its own space - empty,
     * or another of its textures (user reports 2026-10-24 / 25: a car's
     * reflection map black under a pink body with rainbow noise, a bridge
     * with glyphs, HUD panels with a road texture; GT3PC_TEX_STRESS thread
     * made it common, renderer.md 11) */
    RawTex1 hdr;
    const RawXfer *xfer;
    const uint8_t *texCopy = nullptr, *xferCopy = nullptr;
    if (copy != nullptr) {
        memcpy(&hdr, copy, sizeof(hdr));
        texCopy = copy + sizeof(RawTex1);
        xferCopy = texCopy + (hdr.tex != 0 ? (size_t)hdr.nTex * 0x28 : 0);
        xfer = hdr.xfer != 0 ? reinterpret_cast<const RawXfer *>(xferCopy) : nullptr;
    } else {
        memcpy(&hdr, img, sizeof(hdr));
        xfer = reinterpret_cast<const RawXfer *>((uintptr_t)hdr.xfer);
    }
    if (hdr.nXfer != 0 && xfer == nullptr)
        return nullptr;

    Obj *g = nullptr;
    auto gi = g_groupByImg.find(img);
    if (gi != g_groupByImg.end()) {
        g = obj(gi->second);
        if (g != nullptr && g->stamp != stamp)
            g = nullptr;
    }
    if (g != nullptr && group_same(g, hdr, xfer)) {
        /* the same set again (relinked elsewhere at most): its small
         * uploads may have changed (CLUT animations).  PC: and its big ones,
         * by their fingerprints: texels written again in place keep the
         * load stamp, and the textures decoded from the old bytes stayed
         * until the set was loaded again (user report: a menu car's
         * reflection garbled until the menu was entered again); the keys
         * follow the hashes (group_key) */
        g->curBase = vramBase;
        for (size_t i = 0; i < g->uploads.size() && i < g->set.uploads.size(); i++) {
            GroupUpload &gu = g->uploads[i];
            const gstex::Tex1Upload &u = g->set.uploads[i];
            if (u.data == nullptr)
                continue;
            /* PC (CPU time, 2026-11-03): a small one compared with its bytes
             * of the last time - hashed again only when they differ (the
             * hash's chain of 64-bit products over every small upload of
             * every set, every upload: 3 % of the executor) */
            const uint8_t *bytes = static_cast<const uint8_t *>(u.data);
            if (gu.isSmall && gu.last.size() == u.bytes && memcmp(gu.last.data(), bytes, u.bytes) == 0)
                continue;
            const uint64_t h = upload_hash(u);
            if (gu.isSmall)
                gu.last.assign(bytes, bytes + u.bytes);
            if (h != gu.hash) {
                if (!gu.isSmall)
                    tex1_log_changed(img, i, u.bytes);
                gu.hash = h;
                gu.ver++;                   /* PC: (UploadRefs) */
                g->contentVer++;
                if (g->laidOut)
                    g->space.upload(u.t, u.data, u.bytes);
            }
        }
        group_remember(g, hdr, xfer);
        return g;
    }

    gstex::Tex1Set set;
    if (copy != nullptr ? !gstex::tex1_from_copy(copy, texCopy, xferCopy, set) : !gstex::tex1_from_memory(img, set)) {
        log_once("pgl-tex1-parse", "pgl: a Tex1 set at %p does not parse: not uploaded", img);
        return nullptr;
    }
    if (g == nullptr) {
        g = new_obj(OBJ_GROUP);
        g->img = img;
        g->stamp = stamp;
        g_groupByImg[img] = g->id;
    }
    g->set = std::move(set);
    g->laidOut = false;
    g->contentVer++;
    g->parseVer++;                          /* PC: (UploadRefs: the uploads made anew) */
    /* laid out as linked now, placed where the upload was recorded */
    g->layoutBase = g->set.vramBase & BLOCK_MASK;
    g->curBase = vramBase;
    group_prepare_uploads(g);
    group_remember(g, hdr, xfer);
    return g;
}

/* PC (ROADMAP 18, no memory limits; registry_texture_source): the texture
 * last named as a VRAM-less set's - the set's group (never marked: it owns
 * no block), the base it is linked for, its TEX0 (CLD aside) and CLUT
 * fields. */
struct TexSource {
    Obj *obj = nullptr;
    uint32_t base = 0;
    uint64_t tex0 = 0;
    uint32_t cbp = 0, cpsm = 0, csm = 0;
};
TexSource g_texSrc;

/* PC (2026-11-05, the resolved runs' textures: pgl_render.h TexIdent): what
 * the last bind and the last CLUT load read, in the terms of the objects
 * (texture_last_ident, clut_last_ident), and the indexed loads counted
 * (clut_load_serial) */
TexIdent g_lastTexIdent;
ClutIdent g_lastClutIdent;
uint32_t g_clutLoadSerial;
bool g_texIdentOn = false;                  /* taken only while a run is recorded or replayed (texture_ident_capture) */

/* A CLUT load's fields without CBP (ClutIdent::fields): CPSM, CSM, CSA, T8 */
inline uint32_t clut_fields(const gstex::Tex0 &t)
{
    const bool t8 = t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H;
    return (t.cpsm & 15) | (t.csm & 1) << 4 | (t.csa & 31) << 5 | (uint32_t)t8 << 10;
}
/* ... from the register itself (TEX0: PSM bits 20..25, CPSM 51..54, CSM 55,
 * CSA 56..60), without the whole decode */
inline uint32_t clut_fields_of(uint64_t tex0v)
{
    const uint32_t psm = (uint32_t)(tex0v >> 20) & 0x3F;
    const bool t8 = psm == gstex::PSMT8 || psm == gstex::PSMT8H;
    return ((uint32_t)(tex0v >> 51) & 15) | ((uint32_t)(tex0v >> 55) & 1) << 4 | ((uint32_t)(tex0v >> 56) & 31) << 5 |
           (uint32_t)t8 << 10;
}

/* The rows of the CLUT buffer load l writes, a bit each */
inline uint32_t clut_rows_mask(const ClutLoad &l)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < l.nRows; i++)
        m |= 1u << l.rowIdx[i];
    return m;
}

/* The last load's rows alone (not a set's: kind 0) */
inline void clut_ident_rows(const ClutLoad &l)
{
    if (g_texIdentOn)
        g_lastClutIdent.rows = clut_rows_mask(l);
}

/* The last load's identity: a set's (o) at CBP's place ofs in it, under
 * the uploads refs */
void clut_ident_set(const Obj *o, uint32_t ofs, const gstex::Tex0 &t, const UploadRefs &refs, const ClutLoad &l)
{
    if (!g_texIdentOn)
        return;
    ClutIdent &id = g_lastClutIdent;
    id.kind = 1;
    id.obj = o->id;
    id.parseVer = o->parseVer;
    id.contentVer = o->contentVer;
    id.ofs = ofs;
    id.fields = clut_fields(t);
    id.rows = clut_rows_mask(l);
    id.refs = refs;
}

}  /* namespace */

size_t registry_tex1_copy_size(const void *img)
{
    if (img == nullptr)
        return 0;
    RawTex1 h;
    memcpy(&h, img, sizeof(h));
    return sizeof(RawTex1) + (h.tex != 0 ? (size_t)h.nTex * 0x28 : 0) + (h.xfer != 0 ? (size_t)h.nXfer * 0xC : 0);
}

void registry_tex1_copy(const void *img, void *out)
{
    if (img == nullptr)
        return;
    RawTex1 h;
    memcpy(&h, img, sizeof(h));
    uint8_t *o = static_cast<uint8_t *>(out);
    memcpy(o, &h, sizeof(h));
    o += sizeof(h);
    if (h.tex != 0) {
        memcpy(o, reinterpret_cast<const void *>((uintptr_t)h.tex), (size_t)h.nTex * 0x28);
        o += (size_t)h.nTex * 0x28;
    }
    if (h.xfer != 0)
        memcpy(o, reinterpret_cast<const void *>((uintptr_t)h.xfer), (size_t)h.nXfer * 0xC);
}

/* vramBase: where the set was linked when its upload was recorded
 * (pglr_tex1_upload), copy what its header and tables held then (the game
 * relinks them in place while this runs a frame later).  The layout stays
 * the one of the first parse, curBase - layoutBase moves it. */
void registry_tex1_upload(const void *img, uint32_t vramBase, const void *copy)
{
    if (img == nullptr)
        return;
    vramBase &= BLOCK_MASK;
    Obj *g = group_for(img, vramBase, static_cast<const uint8_t *>(copy));
    if (g == nullptr)
        return;
    tex1_log_move(img, g->id, vramBase, g->set.vramSize);
    group_mark(g);
}

void registry_texture_source(const void *img, uint32_t vramBase, uint64_t tex0, const void *copy)
{
    g_texSrc = TexSource();
    if (img == nullptr)
        return;
    Obj *g = group_for(img, vramBase & BLOCK_MASK, static_cast<const uint8_t *>(copy));
    if (g == nullptr)
        return;
    g->lastFrame = g_registryFrame;
    g->lastUse = g_execSeconds;             /* (kept while drawn: it owns no block) */
    const gstex::Tex0 t = gstex::Tex0::decode(tex0);
    g_texSrc.obj = g;
    g_texSrc.base = vramBase & BLOCK_MASK;
    g_texSrc.tex0 = tex0 & ~(7ull << 61);
    g_texSrc.cbp = t.cbp & BLOCK_MASK;
    g_texSrc.cpsm = t.cpsm;
    g_texSrc.csm = t.csm;
}

/* The native pixels of src's GS rectangle (x, y, w, h), when it has more
 * than one per GS pixel, copied into an image of their own under the key of
 * the GS pixels read back (the least recently used dropped beyond
 * kHiresMax). */
void registry_hires_readback(Target &src, int x, int y, int w, int h, const void *pixels)
{
    if (src.depth || pixels == nullptr || w <= 0 || h <= 0 || w > 256 || h > 256)
        return;
    if (src.sx <= 1.01f && src.sy <= 1.01f)
        return;                             /* (GS resolution: nothing more to keep) */
    const int nx0 = std::max(0, (int)std::lround((float)x * src.sx));
    const int ny0 = std::max(0, (int)std::lround((float)y * src.sy));
    const int nx1 = std::min(src.nw, (int)std::lround((float)(x + w) * src.sx));
    const int ny1 = std::min(src.nh, (int)std::lround((float)(y + h) * src.sy));
    if (nx1 <= nx0 || ny1 <= ny0)
        return;
    ID3D11Texture2D *from = target_tex1(src);
    if (from == nullptr)
        return;
    const uint64_t key = hires_key(pixels, w, h);
    Hires &hr = g_hires[key];
    D3D11_TEXTURE2D_DESC have = {};
    if (hr.tex)
        hr.tex->GetDesc(&have);
    if (!hr.tex || hr.w != w || hr.h != h || have.Width != (UINT)(nx1 - nx0) || have.Height != (UINT)(ny1 - ny0)) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)(nx1 - nx0);
        d.Height = (UINT)(ny1 - ny0);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr.srv.reset();
        hr.tex.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, hr.tex.put())) ||
            FAILED(dev()->CreateShaderResourceView(hr.tex.get(), nullptr, hr.srv.put()))) {
            g_hires.erase(key);
            return;
        }
        hr.w = w;
        hr.h = h;
    }
    const D3D11_BOX box = {(UINT)nx0, (UINT)ny0, 0, (UINT)nx1, (UINT)ny1, 1};
    ID3D11Texture2D *to = hr.tex.get();
    q::run([to, from, box] { qdc()->CopySubresourceRegion(to, 0, 0, 0, 0, from, 0, &box); });
    hr.used = ++g_hiresClock;
    while (g_hires.size() > kHiresMax) {
        auto oldest = g_hires.begin();
        for (auto it = g_hires.begin(); it != g_hires.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        g_hires.erase(oldest);
    }
    char key64[48];
    snprintf(key64, sizeof(key64), "pgl-hires-%dx%d", w, h);
    log_once(key64, "pgl: a %dx%d readback kept at %dx%d native pixels (high-res car icons: docs/renderer.md)", w, h,
             nx1 - nx0, ny1 - ny0);
}

void registry_upload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, int dx, int dy, int w, int h,
                     const void *data, size_t bytes)
{
    if (data == nullptr || w <= 0 || h <= 0 || bytes == 0)
        return;
    dbp &= BLOCK_MASK;
    if (dpsm == gstex::PSMCT32 && dx >= 0 && dy >= 0 && dx + w <= 2048 && dy + h <= 2048) {
        /* into the surface of buffer (dbp, dbw) */
        const uint64_t skey = (uint64_t)dbp | ((uint64_t)dbw << 16);
        Obj *s = nullptr;
        auto it = g_surfaceByKey.find(skey);
        if (it != g_surfaceByKey.end())
            s = obj(it->second);
        if (s == nullptr) {
            s = new_obj(OBJ_SURFACE);
            s->sdbp = dbp;
            s->sdbw = dbw;
            g_surfaceByKey[skey] = s->id;
        }
        surface_upload(s, dx, dy, w, h, static_cast<const uint8_t *>(data), bytes);
        mark_rect(s, dpsm, dbp, dbw, (uint32_t)dx, (uint32_t)dy, (uint32_t)w, (uint32_t)h, LAYER_RGB | LAYER_TOP,
                  nullptr);
        /* PC: the pixels of a high-res image (registry_hires_readback): that
         * region is sampled from it; another upload over it ends that */
        uint64_t hk = 0;
        if (!g_hires.empty() && w <= 256 && h <= 256 && bytes >= (size_t)w * (size_t)h * 4) {
            hk = hires_key(data, w, h);
            if (g_hires.find(hk) == g_hires.end())
                hk = 0;
        }
        if (hk != 0) {
            s->hkey = hk;
            s->hx = dx;
            s->hy = dy;
            s->hw = w;
            s->hh = h;
        } else if (s->hkey != 0 && dx < s->hx + s->hw && s->hx < dx + w && dy < s->hy + s->hh && s->hy < dy + h) {
            s->hkey = 0;
        }
        s->serial = ++g_serial;
        s->lastFrame = g_registryFrame;
        return;
    }
    uint64_t key = hash_bytes(data, bytes);
    key = mix(mix(mix(key, dbp | (uint64_t)dbw << 16 | (uint64_t)dpsm << 24), (uint32_t)dx | (uint64_t)(uint32_t)dy << 32),
              (uint32_t)w | (uint64_t)(uint32_t)h << 32);
    Obj *o = nullptr;
    auto it = g_imageByKey.find(key);
    if (it != g_imageByKey.end())
        o = obj(it->second);
    if (o == nullptr) {
        o = new_obj(OBJ_IMAGE);
        o->t.dbp = dbp;
        o->t.dbw = dbw;
        o->t.dpsm = dpsm;
        o->t.dsax = (uint32_t)dx;
        o->t.dsay = (uint32_t)dy;
        o->t.w = (uint32_t)w;
        o->t.h = (uint32_t)h;
        o->data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + bytes);
        o->key = key;
        g_imageByKey[key] = o->id;
    }
    mark_rect(o, dpsm, dbp, dbw, (uint32_t)dx, (uint32_t)dy, (uint32_t)w, (uint32_t)h, layers_of(dpsm), nullptr);
    o->serial = ++g_serial;
    o->lastFrame = g_registryFrame;
}

void registry_target_written(Target &t, bool rgb, bool top)
{
    Obj *o = obj(t.objId);
    if (o == nullptr) {
        o = new_obj(OBJ_TARGET);
        o->target = &t;
        t.objId = o->id;
        g_targetObjs.push_back(o->id);
        g_targetIndexDirty = true;          /* PC (touch_targets) */
    }
    const uint32_t psm = t.depth ? (0x30 | (t.psm & 0x0F)) : (t.psm & 0x0F);
    const uint32_t fbw = t.fbw ? t.fbw : (uint32_t)((t.gsW + 63) / 64);
    const uint8_t own = layers_of(psm);
    if (t.gsW != o->markW || t.gsH != o->markH || fbw != o->markFbw) {
        /* a depth target that took another frame's GS geometry
         * (depth_target_written) covers other blocks now */
        o->needRemark[0] = true;
        o->needRemark[1] = true;
    }
    uint8_t layers = 0;
    if (rgb && (own & LAYER_RGB) && o->needRemark[0])
        layers |= LAYER_RGB;
    if (top && (own & LAYER_TOP) && o->needRemark[1])
        layers |= LAYER_TOP;
    if (layers == 0)
        return;
    const BlockRange range = gstex::image_blocks(psm, t.base, fbw, 0, 0, (uint32_t)t.gsW, (uint32_t)t.gsH);
    if (range.first != o->targetRange.first || range.last != o->targetRange.last)
        g_targetIndexDirty = true;          /* PC (touch_targets) */
    o->targetRange = range;
    o->markW = t.gsW;
    o->markH = t.gsH;
    o->markFbw = fbw;
    mark_rect(o, psm, t.base, fbw, 0, 0, (uint32_t)t.gsW, (uint32_t)t.gsH, layers, nullptr);
    if (layers & LAYER_RGB)
        o->needRemark[0] = false;
    if (layers & LAYER_TOP)
        o->needRemark[1] = false;
    o->serial = ++g_serial;
}

/* pfx_split_rgb_planes (VU1 micro program 0x78D, graphics.md 8.6) read the
 * frame's bytes as PSMT8 through an identity CLUT and wrote its R, G and B
 * into the alpha bytes of three planes (RacePS2Base::captureAfterimage: the
 * unused top bytes of the CT24 frame buffers and of a Z24 buffer).  Native:
 * the frame's native pixels are copied once into a capture; each plane is a
 * registry object owning the top-byte layer of its blocks that stands for
 * one channel of it, so the PSMT8H draws of drawAfterimage read it (through
 * their CLUT, plane_texture) until something else writes those bytes. */
void registry_split_planes(Target &src, const uint32_t blocks[3], int w, int h)
{
    if (src.depth || !src.tex || w <= 0 || h <= 0)
        return;
    /* the native pixels of src's GS pixels 0..w-1 x 0..h-1 */
    const int cw = std::min(src.nw, std::max(1, (int)std::lround((float)w * src.sx)));
    const int ch = std::min(src.nh, std::max(1, (int)std::lround((float)h * src.sy)));
    Capture &cap = g_captures[blocks[0] & BLOCK_MASK];
    if (!cap.tex || cap.w != cw || cap.h != ch) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)cw;
        d.Height = (UINT)ch;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cap.srv.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, cap.tex.put())) ||
            FAILED(dev()->CreateShaderResourceView(cap.tex.get(), nullptr, cap.srv.put()))) {
            cap.tex.reset();
            cap.srv.reset();
            log_once("pgl-plane-capture", "pgl: could not create a %dx%d frame capture (afterimage)", cw, ch);
            return;
        }
        cap.w = cw;
        cap.h = ch;
    }
    const D3D11_BOX box = {0, 0, 0, (UINT)cw, (UINT)ch, 1};
    ID3D11Texture2D *to = cap.tex.get(), *from = target_tex1(src);
    q::run([to, from, box] { qdc()->CopySubresourceRegion(to, 0, 0, 0, 0, from, 0, &box); });
    const uint32_t fbw = (uint32_t)((w + 63) / 64);
    for (int c = 0; c < 3; c++) {
        const uint32_t bp = blocks[c] & BLOCK_MASK;
        Obj *o = nullptr;
        auto it = g_planeByBlock.find(bp);
        if (it != g_planeByBlock.end())
            o = obj(it->second);
        if (o == nullptr) {
            o = new_obj(OBJ_PLANE);
            g_planeByBlock[bp] = o->id;
        }
        o->pbp = bp;
        o->pfbw = fbw;
        o->pw = w;
        o->ph = h;
        o->pch = c;
        o->pcap = cap.srv;
        o->capW = cw;
        o->capH = ch;
        mark_rect(o, gstex::PSMCT32, bp, fbw, 0, 0, (uint32_t)w, (uint32_t)h, LAYER_TOP, nullptr);
        o->serial = ++g_serial;
        o->lastFrame = g_registryFrame;
    }
    if (afterimage_log())
        pclog::write("pgl afterimage: frame %llu: the frame at block 0x%X (GS %dx%d, native %dx%d) split into the"
                     " planes at blocks 0x%X / 0x%X / 0x%X (%dx%d, FBW %u, native %dx%d)",
                     (unsigned long long)frame_number(), src.base, src.gsW, src.gsH, src.nw, src.nh,
                     blocks[0] & BLOCK_MASK, blocks[1] & BLOCK_MASK, blocks[2] & BLOCK_MASK, w, h, fbw, cw, ch);
}

void registry_target_released(Target &t)
{
    if (t.objId == 0)
        return;
    const uint64_t gen = ++g_markGen;
    uint64_t changed = 0;
    for (int l = 0; l < 2; l++)
        for (uint32_t b = 0; b < gstex::GS_MEM_BLOCKS; b++)
            if (g_own[l][b] == t.objId) {
                g_own[l][b] = 0;
                g_blockGen[l][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
    regions_changed(changed);
    g_targetObjs.erase(std::remove(g_targetObjs.begin(), g_targetObjs.end(), t.objId), g_targetObjs.end());
    g_targetIndexDirty = true;              /* PC (touch_targets) */
    obj_forget(t.objId);
    g_objs.erase(t.objId);
    t.objId = 0;
}

Target *registry_colour_target_at(uint32_t block, int &ox, int &oy)
{
    Obj *o = obj(g_own[0][block & BLOCK_MASK]);
    if (o == nullptr || o->kind != OBJ_TARGET || o->target == nullptr || o->target->depth)
        return nullptr;
    if (!target_offset(*o->target, block & BLOCK_MASK, ox, oy))
        return nullptr;
    return o->target;
}

void registry_clut_load(uint64_t tex0v)
{
    ProfScope prof(PROF_CLUT);              /* (the verbose monitor's profile) */
    g_profCount[PCOUNT_CLUTS]++;
    gstex::Tex0 t = gstex::Tex0::decode(tex0v);
    if (!gstex::psm_indexed(t.psm))
        return;
    g_clutLoadSerial++;                     /* PC: (the resolved runs count the loads: clut_load_serial) */
    g_lastClutIdent.kind = 0;
    g_lastClutIdent.rows = 0;
    const uint32_t cbp = t.cbp & BLOCK_MASK;
    const bool t8 = t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H;
    /* PC (CPU time): these fields loaded since the last owner change */
    const uint32_t fields = cbp | (t.cpsm & 15) << 14 | (t.csm & 1) << 18 | (t.csa & 31) << 19 | (uint32_t)t8 << 24;
    /* the memo key: the fields, CBP's owner, the tags of the CLUT's blocks */
    const uint64_t fieldKey = cbp | (uint64_t)t.cpsm << 16 | (uint64_t)t.csm << 20 | (uint64_t)t.csa << 24 |
                              (uint64_t)t8 << 32;
    auto clut_key = [&](uint32_t first, uint32_t count) {
        uint64_t k = comb(comb(0xBB67AE8584CAA73Bull, fieldKey), g_own[0][cbp]);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t b = (first + i) & BLOCK_MASK;
            k = comb(comb(k, g_blockGen[0][b]), g_blockGen[1][b]);
        }
        return k;
    };
    /* PC (CPU time, 2026-11-03): the CLUT of a Tex1 set is read from the
     * set's own space (the load below), so its memo key is what it reads
     * there - the set (its image and load stamp), CBP's place in it (blocks
     * from the copy's base, copyBase) and the data of the set's uploads under
     * the CLUT's blocks (their hashes; refs: which they are) - wherever the
     * set lies now.  By its blocks' tags (which hold the set's base and data
     * version) the cars' sets, moved between VRAM slots every frame of a
     * 32-car race, made 250-535 new keys a frame: those CLUTs decoded again
     * and the memo emptied every few seconds (CLUT LOAD 1.3-2.2 ms a frame);
     * by the set's data version, the ~15 sets whose lamp CLUT changes every
     * frame still decoded all their CLUTs again (~220 a frame). */
    auto set_key = [&](const Obj *g, uint32_t copyBase, UploadRefs *refs) {
        const uint32_t ofs = (cbp - copyBase) & BLOCK_MASK;
        BlockRange r = gstex::clut_blocks(t, false);
        if (r.empty())
            r.first = r.last = cbp;
        BlockRange rL;
        rL.first = (r.first + g->layoutBase - copyBase) & BLOCK_MASK;
        rL.last = rL.first + (r.last - r.first);
        const uint64_t k = comb(comb(0x9E3779B97F4A7C15ull, (fieldKey & ~0xFFFFull) | ofs),
                                comb((uint64_t)(uintptr_t)g->img, g->stamp));
        return upload_refs(g->uploads, rL, refs, k);
    };
    /* the load of the CLUT cb holds, memoized under key */
    auto add_load = [&](uint64_t key, const gstex::ClutBuffer &cb) -> const ClutLoad & {
        ClutLoad l;
        l.key = key;
        l.cpsm = cb.cpsm();
        l.base = t.csa * 16;
        l.n = t8 ? 256 : 16;
        for (uint32_t i = 0; i < l.n; i++)
            l.raw[i] = cb.lookup_raw(t.psm, t.csa, i);
        clut_rows_init(l);
        if (g_clutMemo.size() > g_stress.clutMemoMax) {
            clut_materialize();             /* (the rows still noted by their loads) */
            g_clutMemo.clear();
            g_tagEpoch++;                   /* (the front's loads went with it) */
        }
        return g_clutMemo.emplace(key, l).first->second;
    };
    /* PC (ROADMAP 18, no memory limits): the CLUT of a VRAM-less set's
     * texture (registry_texture_source) is the set's: by the set, its data
     * version and CBP's place in it - no front entry (it owns no block) */
    if (g_texSrc.obj != nullptr && cbp == g_texSrc.cbp && t.cpsm == g_texSrc.cpsm && t.csm == g_texSrc.csm) {
        Obj *g = g_texSrc.obj;
        const uint32_t ofs = (cbp - g_texSrc.base) & BLOCK_MASK;
        const uint64_t key = set_key(g, g_texSrc.base, nullptr);  /* PC: (the same load as the set's in VRAM) */
        auto it = g_clutMemo.find(key);
        const ClutLoad *load = it != g_clutMemo.end() ? &it->second : nullptr;
        if (load == nullptr) {
            group_layout(g);
            gstex::Tex0 tl = t;
            tl.cbp = (g->layoutBase + ofs) & BLOCK_MASK;
            gstex::ClutBuffer cb;
            cb.load(g->space, tl);
            load = &add_load(key, cb);
        }
        clut_ident_rows(*load);             /* PC: (not identified: kind 0; its rows) */
        if (!g_clutLastValid || key != g_clutLastKey) {
            g_clutLastKey = key;
            g_clutLastValid = true;
            clut_apply(*load);
        }
        return;
    }
    /* PC: a set's load again by the set, CBP's place in it and the uploads
     * under it unchanged, wherever the set lies (SetClutFront) - first: the
     * cars' sets move between VRAM slots every frame of a race, and the probe
     * of ClutFront (by CBP) missed for nearly all of their loads, a cache
     * miss each (CPU time, 2026-11-03) */
    Obj *o = obj(g_own[0][cbp]);
    SetClutFront *sf = nullptr;
    uint64_t setKey = 0, preKey = 0;
    UploadRefs refs;
    if (o != nullptr && o->kind == OBJ_GROUP) {
        const uint32_t ofs = (cbp - g_ownBase[0][cbp]) & BLOCK_MASK;
        preKey = comb(comb(0x5BE0CD19137E2179ull, (uint64_t)o->id << 32 | o->parseVer), (fieldKey & ~0xFFFFull) | ofs);
        sf = &g_setClutFront[(uint32_t)(preKey >> 40) % g_stress.setClutFronts];
        if (sf->load != nullptr && sf->preKey == preKey && sf->epoch == g_tagEpoch &&
            (sf->refs.n != 0xFF ? upload_refs_same(o->uploads, sf->refs) : sf->contentVer == o->contentVer)) {
            clut_ident_set(o, ofs, t, sf->refs, *sf->load);     /* PC: (the resolved runs) */
            if (!g_clutLastValid || sf->load->key != g_clutLastKey) {
                g_clutLastKey = sf->load->key;
                g_clutLastValid = true;
                clut_apply(*sf->load);
            }
            return;
        }
    }
    ClutFront &front = g_clutFront[(uint32_t)(comb(0x243F6A8885A308D3ull, fields) >> 40) % g_stress.clutFronts];
    if (front.load != nullptr && front.fields == fields && front.epoch == g_tagEpoch &&
        clut_key(front.first, front.count) == front.tagKey) {
        clut_ident_rows(*front.load);
        if (!g_clutLastValid || front.load->key != g_clutLastKey) {
            g_clutLastKey = front.load->key;
            g_clutLastValid = true;
            clut_apply(*front.load);
        }
        return;
    }
    if (o == nullptr) {
        log_once("pgl-clut-none", "pgl: a CLUT load from block 0x%X that nothing wrote (TEX0 psm %s cpsm 0x%X"
                 " csa %u; kept the old CLUT)", cbp, gstex::psm_name(t.psm), t.cpsm, t.csa);
        return;
    }
    if (o->kind == OBJ_TARGET) {
        log_once("pgl-clut-target", "pgl: a CLUT load from a render target (block 0x%X, TEX0 psm %s tbp 0x%X"
                 " cpsm 0x%X csa %u): not native, the old CLUT kept", cbp, gstex::psm_name(t.psm), t.tbp0, t.cpsm,
                 t.csa);
        return;
    }
    const uint32_t setOfs = (cbp - g_ownBase[0][cbp]) & BLOCK_MASK;    /* PC: (clut_ident_set) */
    if (sf != nullptr)
        setKey = set_key(o, g_ownBase[0][cbp], &refs);
    BlockRange r = gstex::clut_blocks(t, false);
    if (r.empty())
        r.first = r.last = cbp;
    /* PC (CPU time): the key by what the load reads, its blocks' tags (the
     * front's entry checks it with its blocks; a long run of them is not
     * kept there) */
    const uint32_t count = std::min<uint32_t>(r.count(), gstex::GS_MEM_BLOCKS);
    const uint64_t tagKey = clut_key(r.first, count);
    /* (a set's: by the set and CBP's place in the copy there, set_key) */
    const uint64_t key = o->kind == OBJ_GROUP ? setKey : tagKey;
    const bool frontable = count <= 16;
    g_profCount[PCOUNT_CLUTSLOW]++;
    auto it = g_clutMemo.find(key);
    if (it != g_clutMemo.end()) {
        front.fields = fields;
        front.epoch = g_tagEpoch;
        front.first = r.first;
        front.count = count;
        front.tagKey = tagKey;
        front.load = frontable ? &it->second : nullptr;
        if (sf != nullptr) {
            *sf = SetClutFront{preKey, g_tagEpoch, refs, o->contentVer, &it->second};
            clut_ident_set(o, setOfs, t, refs, it->second);
        } else {
            clut_ident_rows(it->second);
        }
        if (g_clutLastValid && key == g_clutLastKey)
            return;                         /* PC: the buffer holds this load already */
        g_clutLastKey = key;
        g_clutLastValid = true;
        clut_apply(it->second);
        return;
    }
    g_clutLastKey = key;
    g_clutLastValid = true;
    g_profCount[PCOUNT_CLUTMISS]++;
    gstex::ClutBuffer cb;
    if (o->kind == OBJ_GROUP) {
        group_layout(o);
        gstex::Tex0 tl = t;
        tl.cbp = (cbp - group_delta_at(o, 0, cbp)) & BLOCK_MASK;     /* the copy at CBP */
        cb.load(o->space, tl);
    } else {
        gstex::TexSpace space;
        compose_space(r, space, nullptr);
        cb.load(space, t);
    }
    const ClutLoad &added = add_load(key, cb);
    front.fields = fields;
    front.epoch = g_tagEpoch;
    front.first = r.first;
    front.count = count;
    front.tagKey = tagKey;
    front.load = frontable ? &added : nullptr;
    if (sf != nullptr) {
        *sf = SetClutFront{preKey, g_tagEpoch, refs, o->contentVer, &added};
        clut_ident_set(o, setOfs, t, refs, added);
    } else {
        clut_ident_rows(added);
    }
    clut_apply(added);
}

/* PC (2026-11-05, the resolved runs' textures: pgl_render.h ClutIdent) */
uint32_t clut_load_serial()
{
    return g_clutLoadSerial;
}

bool clut_load_indexed(uint64_t tex0v)
{
    return gstex::psm_indexed((uint32_t)(tex0v >> 20) & 0x3F);
}

void clut_last_ident(ClutIdent &out)
{
    out = g_lastClutIdent;
}

/* The recorded load reads the same CLUT now: the same set owns the live
 * CBP at the same place in it, with the same fields, and the set's uploads
 * under the CLUT's blocks are unchanged - so it writes the same entries. */
bool clut_ident_check(const ClutIdent &id, uint64_t tex0v)
{
    if (id.kind == 0)
        return false;
    if (clut_fields_of(tex0v) != id.fields)
        return false;
    const uint32_t cbp = (uint32_t)(tex0v >> 37) & BLOCK_MASK;
    const Obj *o = obj(g_own[0][cbp]);
    if (o == nullptr || o->kind != OBJ_GROUP || o->id != id.obj || o->parseVer != id.parseVer ||
        ((cbp - g_ownBase[0][cbp]) & BLOCK_MASK) != id.ofs)
        return false;
    /* (the set's data unchanged since: every upload is; else those under the
     * CLUT checked) */
    return id.contentVer == o->contentVer || (id.refs.n != 0xFF && upload_refs_same(o->uploads, id.refs));
}

/* ======================================================================== */
/* Resolving TEX0                                                           */
/* ======================================================================== */

namespace {

/* PC (CPU time): the bindings of the decoded-texture path (texture_bind's
 * last one) by TEX0 (CLD aside), CLAMP, TEXA, an alpha-only draw and the
 * palette's content, direct-mapped, while g_tagEpoch stays (no decoded
 * texture dropped) and what was read is as it was: a Tex1 set's texture
 * (most of a race's) is the set's data at the copy TBP0's block holds - its
 * two tags (the set, its base, its data version) say it all (exact); an
 * image's or a composed one, by the regions of TBP0's block and of the
 * texture's blocks (no tag changed there). */
struct TexFront {
    uint64_t tex0 = 0, clamp = 0, texa = 0, pal = 0, epoch = 0;
    bool alphaOnly = false;
    bool exact = false;
    uint64_t tag0 = 0, tag1 = 0;
    RegionSnap regions;
    CachedTex *ct = nullptr;
    uint32_t ctGen = 0;                     /* PC (2026-11-13): ct->gen then (CachedTex) */
    TexBinding out;
};
TexFront g_texFront[kTexFronts];           /* (kTexFronts: with the budget above) */

/* PC (CPU time, 2026-11-03): a Tex1 set's bindings again by the set (its
 * object and data version) and TBP0's place in the copy there, wherever the
 * set lies now - TexFront is by TEX0, and the cars' sets, moved between VRAM
 * slots every frame of a 32-car race, missed it ~1,300 times a frame (TEX
 * SLOW), each a palette worked out (PAL MISS) and the decoded texture looked
 * up by its content.  Its binding is the set's data alone (the slow path:
 * the copy at TBP0, the uploads under the footprint by their hashes), so the
 * set, the place, TEX0 without TBP0 / CBP, CLAMP, TEXA, the palette's
 * content (palKey) and an alpha-only draw say it all, while g_tagEpoch
 * stays and the uploads under the footprint are unchanged (refs; over 4 of
 * them, the set's data version: a lamp's CLUT changed every frame in ~15
 * sets, which moved it and threw away all their binds). */
struct GroupFront {
    uint32_t obj = 0, parseVer = 0, contentVer = 0, ofs = 0;
    UploadRefs refs;
    uint8_t layer = 0;
    bool alphaOnly = false;
    uint64_t tex0 = 0, clamp = 0, texa = 0, pal = 0, epoch = 0;
    CachedTex *ct = nullptr;
    uint32_t ctGen = 0;                     /* PC (2026-11-13): ct->gen then (CachedTex) */
    TexBinding out;
};
GroupFront g_groupFront[kGroupFronts];

/* PC (2026-11-05): the last bind's identity from its by-set entry (the
 * resolved runs: pgl_render.h TexIdent) */
void tex_ident_set(const GroupFront &gf, const Obj *o)
{
    if (!g_texIdentOn)
        return;
    TexIdent &id = g_lastTexIdent;
    id.kind = 1;
    id.layer = gf.layer;
    id.obj = gf.obj;
    id.parseVer = o->parseVer;
    id.contentVer = o->contentVer;
    id.ofs = gf.ofs;
    id.tex0 = gf.tex0;
    id.clamp = gf.clamp;
    id.texa = gf.texa;
    id.refs = gf.refs;
    id.target = nullptr;
    id.epoch = g_tagEpoch;
    id.ct = gf.ct;
    id.ctGen = gf.ctGen;                    /* PC (2026-11-13) */
}

/* PC (2026-11-13, the texture replacements: pgl_texrepl.h): a set's texture
 * bound as its replacement once that is loaded (asked for here until then;
 * alphaMode 3: the usual alpha, sampled through its mip levels), or as Select
 * mode's highlight; with the hooks off what it held is let go. */
void texture_dress(CachedTex &c, bool hooks, uint64_t selHash, TexBinding &out)
{
    if (hooks && c.hashed && selHash != 0 && c.chash == selHash && c.hlSrv) {
        out.srv = c.hlSrv.get();
        return;
    }
    if (c.hlSrv) {
        c.hlTex.reset();
        c.hlSrv.reset();
    }
    if (hooks && c.hashed && texrepl::replacing()) {
        if (!c.replSrv || c.replStale) {
            /* (replStale: its file changed - what it has is shown until the
             * new one is loaded, and let go when the file is gone) */
            texrepl::Found f;
            const int found = texrepl::lookup(c.chash, &f);
            if (found != texrepl::kPending && c.replSrv) {
                g_replBytes -= c.replBytes;
                c.replBytes = 0;
                c.replTex.reset();
                c.replSrv.reset();
            }
            if (found != texrepl::kPending)
                c.replStale = false;
            if (found == texrepl::kReady) {
                *c.replTex.put() = f.tex;
                *c.replSrv.put() = f.srv;
                c.replMean = f.mean;
                c.replAMin = f.aMin;
                c.replAMax = f.aMax;
                c.replBytes = f.bytes;
                g_replBytes += f.bytes;
            }
        }
        if (c.replSrv) {
            out.srv = c.replSrv.get();
            out.alphaMode = 3;
            out.mean = c.replMean;
            out.aMin = c.replAMin;
            out.aMax = c.replAMax;
        }
    }
    /* (replacing off: what it holds is kept - the on / off key compares back
     * and forth; registry_end_frame lets it go when it stays off) */
}

/* PC (GT3PC_PGL_PROFILE, 2026-11-03): the textures decoded in the profile's
 * frames, by what they are (a set's by the set, wherever it lies) - a 32-car
 * race decoded 4 a frame (TEX DECODE ~0.8 ms) with nothing new on the
 * screen; registry_decode_report logs the ones decoded again and again,
 * with what owns their blocks and the set's versions (a small upload under
 * a texture's footprint changed: contentVer climbs). */
struct DecodeNote {
    uint32_t count = 0;
    std::string what;
};
std::unordered_map<uint64_t, DecodeNote> g_decodeNotes;

void decode_note(const gstex::Tex0 &t, const gstex::Rect &rect, const Obj *o, bool group, bool linear,
                 const BlockRange &br)
{
    if (g_decodeNotes.size() >= 4096)
        return;
    const uint32_t tbp = t.tbp0 & BLOCK_MASK;
    const uint64_t k = comb(comb(comb((uint64_t)t.psm | (uint64_t)t.tbw << 8 | (uint64_t)t.tw << 16 |
                                          (uint64_t)t.th << 20 | (uint64_t)t.csa << 24 | (uint64_t)(group ? 0 : tbp) << 32,
                                      (uint64_t)rect.x | (uint64_t)rect.y << 16 | (uint64_t)rect.w << 32 |
                                          (uint64_t)rect.h << 48),
                                 group ? (uint64_t)(uintptr_t)o->img : (uint64_t)o->id),
                            (uint64_t)(group ? 1 : linear ? 2 : 3));
    DecodeNote &n = g_decodeNotes[k];
    char s[400];
    int at = snprintf(s, sizeof(s), "%s %s %ux%u (rect %u,%u %ux%u) TBP0 0x%X TBW %u CBP 0x%X CSA %u, TBP0's owner: %s %u",
                      group ? "a set's" : linear ? "an image's" : "composed", gstex::psm_name(t.psm), 1u << t.tw,
                      1u << t.th, rect.x, rect.y, rect.w, rect.h, tbp, t.tbw, t.cbp & BLOCK_MASK, t.csa,
                      kind_name(o->kind), o->id);
    if (group)
        at += snprintf(s + at, sizeof(s) - at, " (%p stamp %llu, data version %u, parsed %u)", o->img,
                       (unsigned long long)o->stamp, o->contentVer, o->parseVer);
    if (!group && !linear) {
        /* (the owners of its blocks: what the composed space is made of) */
        uint32_t ids[5];
        int nIds = 0;
        const int layer = psm_top(t.psm) ? 1 : 0;
        const uint32_t count = std::min<uint32_t>(br.count(), gstex::GS_MEM_BLOCKS);
        for (uint32_t i = 0; i < count && nIds < 5; i++) {
            const uint32_t id = g_own[layer][(br.first + i) & BLOCK_MASK];
            if (std::find(ids, ids + nIds, id) == ids + nIds)
                ids[nIds++] = id;
        }
        at += snprintf(s + at, sizeof(s) - at, "; its blocks owned by");
        for (int i = 0; i < nIds && at < (int)sizeof(s) - 40; i++) {
            const Obj *b = obj(ids[i]);
            at += snprintf(s + at, sizeof(s) - at, " %s %u%s", b ? kind_name(b->kind) : "nothing", ids[i],
                           i + 1 < nIds ? "," : "");
        }
    }
    n.count++;
    n.what = s;
}

}  /* namespace */

void registry_decode_report(uint32_t frames)
{
    std::vector<const DecodeNote *> again;
    for (const auto &e : g_decodeNotes)
        if (e.second.count >= 2)
            again.push_back(&e.second);
    std::sort(again.begin(), again.end(), [](const DecodeNote *a, const DecodeNote *b) { return a->count > b->count; });
    for (size_t i = 0; i < again.size() && i < 8; i++)
        pclog::write("pgl decoded again: %u times in %u frames: %s (the last)", again[i]->count, frames,
                     again[i]->what.c_str());
    g_decodeNotes.clear();
}

bool texture_bind(uint64_t tex0v, uint64_t tex1v, uint64_t clampv, uint64_t texav, uint8_t writeMask,
                  Target *drawing, TexBinding &out)
{
    g_lastTexIdent.kind = 0;                /* PC: (the resolved runs: kind 0 unless identified below) */
    /* PC (vector fonts): a host image's TEX0 - TBW 0, PSMCT32 (pgl_host_image.cpp) */
    if ((tex0v & (0xFFFull << 14)) == 0 && host_image_bind(tex0v & ~(7ull << 61), out))
        return true;
    const gstex::Tex0 t = gstex::Tex0::decode(tex0v);
    const gstex::Clamp c = gstex::Clamp::decode(clampv);
    const gstex::Texa ta = gstex::Texa::decode(texav);
    const gstex::Tex1 t1 = gstex::Tex1::decode(tex1v);
    const uint32_t tbp = t.tbp0 & BLOCK_MASK;
    const bool top = psm_top(t.psm);
    out.hasMean = false;
    out.linear = false;
    out.src = nullptr;
    out.fromFront = false;
    out.fromSet = false;

    /* the palette's content (PC, CPU time: what the CLUT buffer's rows it
     * reads hold) and this binding again (TexFront) */
    const bool indexed = gstex::psm_indexed(t.psm);
    const uint32_t palN = (t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H) ? 256 : 16;
    uint64_t palKey = 0;
    if (indexed) {
        const bool ct16 = clut_16bit(t.cpsm);
        palKey = palette_key(palN, ct16, t.csa, ct16 ? (ta.ta0 | ta.aem << 8 | (uint64_t)ta.ta1 << 16) : 0);
    }
    const uint64_t tex0Key = tex0v & ~(7ull << 61);
    const bool alphaOnly = (writeMask & pgl::gs::WRITE_RGB) == 0;
    /* PC (ROADMAP 18, no memory limits): the texture last named as a
     * VRAM-less set's is that set's, whatever owns its blocks (no front
     * entry: its blocks say nothing of it) */
    const bool virt = g_texSrc.obj != nullptr && tex0Key == g_texSrc.tex0;
    /* PC: a set's texture by the set and TBP0's place in it (GroupFront; an
     * alpha-only CT32 draw may take its alpha from a target: not here) -
     * first: the cars' sets move between VRAM slots every frame of a race,
     * and the probe of TexFront (by TEX0, TBP0 with it) missed for nearly
     * all of their binds, a cache miss each (CPU time, 2026-11-03) */
    GroupFront *gf = nullptr;
    uint32_t gfObj = 0, gfOfs = 0;
    const uint64_t tex0Rel = tex0Key & ~(0x3FFFull | 0x3FFFull << 37);
    if (!virt && !(alphaOnly && !top && t.psm == gstex::PSMCT32)) {
        const int layer = top ? 1 : 0;
        const Obj *go = obj(g_own[layer][tbp]);
        if (go != nullptr && go->kind == OBJ_GROUP) {
            gfObj = go->id;
            gfOfs = (tbp - g_ownBase[layer][tbp]) & BLOCK_MASK;
            const uint64_t k = comb(comb(comb(tex0Rel, clampv) ^ palKey, texav),
                                    (uint64_t)gfObj << 20 | (uint64_t)gfOfs << 2 | (uint64_t)layer << 1 |
                                        (uint64_t)alphaOnly);
            gf = &g_groupFront[(uint32_t)(k >> 40) % g_stress.groupFronts];
            if (gf->ct != nullptr && gf->epoch == g_tagEpoch && gf->obj == gfObj && gf->parseVer == go->parseVer &&
                gf->ofs == gfOfs && gf->layer == layer && gf->alphaOnly == alphaOnly && gf->tex0 == tex0Rel &&
                gf->clamp == clampv && gf->texa == texav && gf->pal == palKey && gf->ctGen == gf->ct->gen &&
                (gf->refs.n != 0xFF ? upload_refs_same(go->uploads, gf->refs) : gf->contentVer == go->contentVer)) {
                gf->ct->lastFrame = g_registryFrame;
                gf->ct->lastUse = g_execSeconds;
                out = gf->out;
                out.fromFront = true;
                tex_ident_set(*gf, go);     /* PC: (the resolved runs) */
                return true;
            }
        }
    }
    TexFront &front = g_texFront[(uint32_t)(comb(comb(tex0Key, clampv), palKey) >> 40) % g_stress.texFronts];
    if (!virt && front.ct != nullptr && front.epoch == g_tagEpoch && front.tex0 == tex0Key && front.clamp == clampv &&
        front.texa == texav && front.pal == palKey && front.alphaOnly == alphaOnly && front.ctGen == front.ct->gen &&
        (front.exact ? front.tag0 == g_blockGen[0][tbp] && front.tag1 == g_blockGen[1][tbp]
                     : front.regions.same())) {
        front.ct->lastFrame = g_registryFrame;
        front.ct->lastUse = g_execSeconds;
        out = front.out;
        out.fromFront = true;               /* (the F9 trace) */
        return true;
    }
    g_profCount[PCOUNT_TEXSLOW]++;
    g_profCount[gf != nullptr ? PCOUNT_TEXSLOW_SET : PCOUNT_TEXSLOW_OTHER]++;   /* PC: (why: the counts) */

    if (!gstex::psm_valid(t.psm)) {
        log_once("pgl-tex-psm", "pgl: texture with an invalid PSM 0x%X: not drawn", t.psm);
        return false;
    }
    if (t1.mxl != 0)
        log_once("pgl-tex-mip", "pgl: mipmapped textures (MXL %u): level 0 only", t1.mxl);

    Obj *o = virt ? g_texSrc.obj : obj(g_own[top ? 1 : 0][tbp]);
    int ownLayer = top ? 1 : 0;             /* PC: (the layer the owner was read from: TexIdent) */
    /* A PSMCT32 texel is both layers: its RGB is what the owner of the RGB
     * layer wrote, its alpha what the owner of the top byte wrote.  A draw
     * that writes no colour channel uses nothing of it but the alpha (the GS
     * never mixes texel RGB into the alpha it writes or tests), so the top
     * byte's owner decides when that is a colour target: a target drawn
     * alpha-only owns only that layer.  CarIconMaker::drawIcon (T184) clears
     * and draws its mask under FBMSK 0x00FFFFFF and halves it alpha-only into
     * the icon; read through the RGB layer, the mask was whatever had last
     * written those blocks' RGB (an old frame, an earlier car's textures)
     * and the icons' alpha came from there. */
    if (!virt && !top && t.psm == gstex::PSMCT32 && (writeMask & pgl::gs::WRITE_RGB) == 0) {
        Obj *a = obj(g_own[1][tbp]);
        if (a != nullptr && a != o && a->kind == OBJ_TARGET && a->target != nullptr && !a->target->depth) {
            o = a;
            ownLayer = 1;
        }
    }
    if (top && afterimage_log())
        pclog::write("pgl afterimage: frame %llu: %s texture at block 0x%X (TBW %u, CSA %u): the top byte is %s%s",
                     (unsigned long long)frame_number(), gstex::psm_name(t.psm), tbp, t.tbw, t.csa,
                     o == nullptr ? "unwritten" : kind_name(o->kind),
                     o != nullptr && o->kind == OBJ_PLANE ? (o->pch == 0 ? " (R)" : o->pch == 1 ? " (G)" : " (B)")
                                                          : "");
    if (o == nullptr) {
        char key[48];
        snprintf(key, sizeof(key), "pgl-tex-none-%X-%X", tbp, t.psm);
        log_once(key, "pgl: texture at block 0x%X (%s %ux%u) has no source: not drawn", tbp,
                 gstex::psm_name(t.psm), t.width(), t.height());
        return false;
    }

    /* ---- a plane of pfx_split_rgb_planes (the afterimage): its bytes as
     * the indices of a PSMT8H / T4HL / T4HH texture at its first block ---- */
    if (o->kind == OBJ_PLANE) {
        if (!top || tbp != o->pbp || t.tbw != o->pfbw) {
            log_once("pgl-tex-plane-ofs", "pgl: a %s texture at block 0x%X (TBW %u) inside a plane at 0x%X (FBW %u):"
                     " not native", gstex::psm_name(t.psm), tbp, t.tbw, o->pbp, o->pfbw);
            return false;
        }
        const uint32_t n = t.psm == gstex::PSMT8H ? 256 : 16;
        uint32_t pal[256];
        for (uint32_t i = 0; i < n; i++)
            pal[i] = clut_lookup(t.psm, t.cpsm, t.csa, i, ta);
        ID3D11ShaderResourceView *srv = plane_texture(o, t.psm, pal, n);
        if (srv == nullptr)
            return false;
        out.srv = srv;
        out.x0 = 0;
        out.y0 = 0;
        out.w = (float)o->pw;
        out.h = (float)o->ph;
        out.alphaMode = 0;
        out.target = nullptr;
        out.depthTex = false;
        return true;
    }

    /* ---- a render target ---- */
    if (o->kind == OBJ_TARGET) {
        Target *tg = o->target;
        if (gstex::psm_depth(t.psm) != tg->depth) {
            log_once("pgl-tex-target-kind", "pgl: a %s texture of a %s buffer (block 0x%X): not native",
                     gstex::psm_name(t.psm), tg->depth ? "depth" : "colour", tbp);
            return false;
        }
        if (gstex::psm_indexed(t.psm)) {
            log_once("pgl-tex-target-index", "pgl: an indexed (%s) texture of a render target (block 0x%X):"
                     " not native", gstex::psm_name(t.psm), tbp);
            return false;
        }
        int ox = 0, oy = 0;
        if (!target_offset(*tg, tbp, ox, oy)) {
            log_once("pgl-tex-target-ofs", "pgl: texture at block 0x%X inside a render target at 0x%X:"
                     " offset not found", tbp, tg->base);
            return false;
        }
        if (tg->fbw != 0 && t.tbw != tg->fbw)
            log_once("pgl-tex-target-tbw", "pgl: render target at 0x%X sampled with TBW %u (FBW %u)", tg->base,
                     t.tbw, tg->fbw);
        out.srv = (tg == drawing) ? target_copy_srv(*tg, false, COPY_TEXTURE) : target_srv(*tg);
        out.x0 = -(float)ox;
        out.y0 = -(float)oy;
        out.w = (float)tg->gsW;
        out.h = (float)tg->gsH;
        out.alphaMode = 0;
        if (t.psm == gstex::PSMCT24)
            out.alphaMode = 1;
        else if (t.psm == gstex::PSMCT16 || t.psm == gstex::PSMCT16S)
            out.alphaMode = 2;
        out.target = tg;
        out.depthTex = tg->depth;
        /* PC (2026-11-05): its identity for the resolved runs - the target
         * and TBP0's place in it (not the target being drawn: its copy is
         * the draw's own) */
        if (tg != drawing && !virt && g_texIdentOn) {
            TexIdent &id = g_lastTexIdent;
            id.kind = 2;
            id.layer = (uint8_t)ownLayer;
            id.obj = o->id;
            id.ofs = (tbp - tg->base) & BLOCK_MASK;
            id.tex0 = tex0Key & ~(0x3FFFull | 0x3FFFull << 37);
            id.clamp = clampv;
            id.texa = texav;
            id.target = tg;
            id.epoch = g_tagEpoch;
        }
        return true;
    }

    /* ---- uploads ---- */
    const gstex::Rect rect = gstex::decode_region(t, c);
    const BlockRange br = gstex::image_blocks(t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h);
    /* The owner of TBP0's block decides when it is a Tex1 set or a surface:
     * the 2^TW x 2^TH space usually reaches past the set's (or the movie
     * buffer's) blocks into other objects' VRAM, which the game never
     * samples - composing it would decode the texture anew every frame.
     * A lone image upload must own the whole footprint for the linear
     * path; anything else is composed. */
    bool single = o->kind == OBJ_GROUP || o->kind == OBJ_SURFACE;
    if (!single) {
        single = true;
        const uint32_t count = std::min<uint32_t>(br.count(), gstex::GS_MEM_BLOCKS);
        const int layer = top ? 1 : 0;
        for (uint32_t i = 0; i < count && single; i++) {
            const uint32_t id = g_own[layer][(br.first + i) & BLOCK_MASK];
            if (id != o->id && id != 0)
                single = false;
        }
    }

    /* a CT32 surface read as itself: its own GPU texture */
    if (single && o->kind == OBJ_SURFACE && t.psm == gstex::PSMCT32 && o->sdbp == tbp && o->sdbw == t.tbw) {
        /* PC: the region of a high-res image (registry_hires_readback) read
         * as exactly itself: the image, bilinear (a car icon) */
        if (o->hkey != 0 && (int)rect.x == o->hx && (int)rect.y == o->hy && (int)rect.w == o->hw &&
            (int)rect.h == o->hh) {
            auto hi = g_hires.find(o->hkey);
            if (hi != g_hires.end() && hi->second.srv) {
                hi->second.used = ++g_hiresClock;
                out.srv = hi->second.srv.get();
                out.x0 = (float)o->hx;
                out.y0 = (float)o->hy;
                out.w = (float)o->hw;
                out.h = (float)o->hh;
                out.alphaMode = 0;
                out.target = nullptr;
                out.depthTex = false;
                out.linear = true;
                return true;
            }
        }
        ID3D11ShaderResourceView *srv = surface_srv(o);
        if (srv == nullptr)
            return false;
        out.srv = srv;
        out.x0 = 0;
        out.y0 = 0;
        out.w = (float)o->sw;
        out.h = (float)o->sh;
        out.alphaMode = 0;
        out.target = nullptr;
        out.depthTex = false;
        return true;
    }

    /* the palette the texture reads (PC, CPU time: from the memo, by its
     * content, palKey) */
    const uint32_t *palette = nullptr;
    uint64_t palHash = 0;
    if (indexed) {
        PalMemo &m = g_palMemo[(palKey ^ (palKey >> 17)) % g_stress.palMemos];
        if (!m.valid || m.key != palKey) {
            g_profCount[PCOUNT_PALMISS]++;
            for (uint32_t i = 0; i < palN; i++)
                m.pal[i] = clut_lookup(t.psm, t.cpsm, t.csa, i, ta);
            m.hash = hash_bytes(m.pal, palN * 4);
            m.key = palKey;
            m.valid = true;
        }
        palette = m.pal;
        palHash = m.hash;
    }
    const bool linear = single && o->kind == OBJ_IMAGE && o->t.dpsm == t.psm && o->t.dbp == tbp &&
                        o->t.dbw == t.tbw && o->t.dsax == 0 && o->t.dsay == 0;

    uint64_t key = tex_key(t, rect, palHash, ta);
    uint64_t composeKey = 0;
    gstex::TexSpace composed;
    const bool group = single && o->kind == OBJ_GROUP;
    uint32_t tbpLayout = tbp;
    BlockRange brL = br;                    /* PC: (the footprint at the set's base: GroupFront's refs) */
    static BlockSet texBlocks;              /* PC: (its very blocks there: group_key, GroupFront's refs) */
    if (group) {
        /* the copy of the set at TBP0 (an earlier one of the frame, if the
         * set has moved since); the small uploads under the footprint there */
        const uint32_t d = virt ? ((g_texSrc.base - o->layoutBase) & BLOCK_MASK)   /* PC: (linked there) */
                                : group_delta_at(o, top ? 1 : 0, tbp);
        tbpLayout = (tbp - d) & BLOCK_MASK;
        brL = shifted(br, (0u - d) & BLOCK_MASK);
        texBlocks = BlockSet();
        texture_blocks(t, tbpLayout, rect, texBlocks);
        key = group_key(key, o->img, o->stamp, tbpLayout - o->layoutBase, o->uploads, brL, &texBlocks);
        g_preTexa.store(texav, std::memory_order_relaxed);  /* PC: (the sets decoded ahead next) */
    } else if (linear) {
        key = mix(mix(key, 0x1), o->key);
    } else {
        compose_space(br, composed, &composeKey);
        key = mix(mix(mix(key, 0xC), composeKey), tbp);
    }

    auto it = g_cache.find(key);
    if (it == g_cache.end() && tex_pre_drain())
        it = g_cache.find(key);             /* PC: decoded ahead since (pgl_prewarm.cpp) */
    if (it != g_cache.end() && it->second.pre) {
        it->second.pre = false;
        pre::taken(pre::TEXTURE);
    }
    /* PC (2026-11-13, the texture replacements: pgl_texrepl.h): a set's
     * texture is hashed as it is decoded; one decoded before the hooks were
     * on, or Select mode's choice without its highlight, is decoded once
     * more for that (again) */
    const bool hooks = group && texrepl::hooks_on();
    const uint64_t selHash = hooks ? texrepl::selected() : 0;
    const bool again = hooks && it != g_cache.end() &&
                       (!it->second.hashed || (selHash != 0 && it->second.chash == selHash && !it->second.hlSrv));
    uint64_t family = 0;                    /* (what its indices are, whatever the palette: texrepl::decoded) */
    if (it == g_cache.end() || again) {
        ProfScope prof(PROF_DECODE);        /* (the verbose monitor's profile) */
        g_profCount[PCOUNT_DECODES]++;
        if (group && pre::on())
            pre::late(pre::TEXTURE);
        std::vector<uint32_t> px((size_t)rect.w * rect.h, 0);
        if (group) {
            group_layout(o);
            /* PC: an indexed one's indices kept (g_indexImages), by the big
             * uploads under its blocks; the blocks of the small ones (a CLUT
             * the game keeps in a corner of the texture's space - the lamps'
             * change every frame) decoded anew each time */
            uint64_t idxKey = 0;
            bool smallUnder = false;
            static BlockSet smallBlocks;
            if (indexed) {
                idxKey = mix(mix(tex_key(t, rect, 0, ta), mix((uint64_t)(uintptr_t)o->img, o->stamp)),
                             (tbpLayout - o->layoutBase) & BLOCK_MASK);
                family = idxKey | 1;        /* PC (2026-11-13) */
                smallBlocks = BlockSet();
                for (const GroupUpload &gu : o->uploads) {
                    if (!upload_under(gu, brL, &texBlocks))
                        continue;
                    if (!gu.isSmall) {
                        idxKey = mix(idxKey, gu.hash);
                        continue;
                    }
                    smallUnder = true;
                    for (const auto &run : gu.runs)
                        for (uint32_t i = 0; i < run.second; i++)
                            smallBlocks.add((uint32_t)run.first + i);
                }
            }
            auto ii = indexed ? g_indexImages.find(idxKey) : g_indexImages.end();
            if (ii != g_indexImages.end() && ii->second.size() == px.size()) {
                px = ii->second;
                if (smallUnder) {
                    /* (the rectangle's block cells in a small upload's blocks) */
                    const gstex::PsmGeom g = gstex::psm_geom(t.psm);
                    for (uint32_t cy = rect.y / g.blockH * g.blockH; cy < rect.y + rect.h; cy += g.blockH)
                        for (uint32_t cx = rect.x / g.blockW * g.blockW; cx < rect.x + rect.w; cx += g.blockW) {
                            if (!smallBlocks.has(gstex::block_number(t.psm, tbpLayout, t.tbw, cx, cy)))
                                continue;
                            const uint32_t x0 = std::max(cx, rect.x), y0 = std::max(cy, rect.y);
                            const uint32_t x1 = std::min(cx + g.blockW, rect.x + rect.w);
                            const uint32_t y1 = std::min(cy + g.blockH, rect.y + rect.h);
                            gstex::decode_rect(o->space, t.psm, tbpLayout, t.tbw, x0, y0, x1 - x0, y1 - y0, nullptr,
                                               t.csa, ta, px.data() + (size_t)(y0 - rect.y) * rect.w + (x0 - rect.x),
                                               rect.w);
                        }
                }
            } else {
                gstex::decode_rect(o->space, t.psm, tbpLayout, t.tbw, rect.x, rect.y, rect.w, rect.h, nullptr,
                                   t.csa, ta, px.data(), rect.w);
                if (indexed && g_indexSeen[idxKey]++ != 0) {
                    if (g_indexBytes + px.size() * 4 > (16u << 20)) {
                        g_indexImages.clear();
                        g_indexBytes = 0;
                    }
                    g_indexImages[idxKey] = px;
                    g_indexBytes += px.size() * 4;
                }
                if (g_indexSeen.size() > 65536)
                    g_indexSeen.clear();
            }
            if (indexed)
                apply_palette(px, palette, palN);
        } else if (linear) {
            const uint32_t iw = o->t.w, ih = o->t.h;
            std::vector<uint32_t> img((size_t)iw * ih, 0);
            gstex::decode_linear(t.psm, o->data.data(), o->data.size(), iw, ih, indexed ? palette : nullptr, ta,
                                 img.data(), iw);
            for (uint32_t y = 0; y < rect.h; y++) {
                const uint32_t sy = rect.y + y;
                if (sy >= ih)
                    break;
                for (uint32_t x = 0; x < rect.w; x++) {
                    const uint32_t sx = rect.x + x;
                    if (sx < iw)
                        px[(size_t)y * rect.w + x] = img[(size_t)sy * iw + sx];
                }
            }
        } else {
            gstex::decode_rect(composed, t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h, nullptr, t.csa, ta,
                               px.data(), rect.w);
            if (indexed)
                apply_palette(px, palette, palN);
        }
        uint64_t chash = 0;
        if (hooks) {
            chash = set_content_hash(o->set, t, tbpLayout, rect, px);
            texrepl::decoded(chash, px.data(), rect.w, rect.h, family);
        }
        if (again) {
            if (!it->second.hashed)
                cache_hashed(it->second, chash);
        } else {
        CachedTex ct;
        ct.rect = rect;
        ct.mean = texel_mean(px);
        texel_alpha_range(px, ct.aMin, ct.aMax);    /* PC: (the alpha mirror) */
        ct.srcObj = o->id;                  /* (the F9 trace) */
        ct.srcKind = o->kind;
        ct.srcImg = group ? o->img : nullptr;
        ct.srcStamp = group ? o->stamp : 0;
        ct.madeFrame = g_registryFrame;
        if (flicker_on())
            flicker_event("texture decoded: 0x%X %s TBW %u (%u,%u) %ux%u from %s, mean %08X (%u cached)", tbp,
                          gstex::psm_name(t.psm), t.tbw, rect.x, rect.y, rect.w, rect.h,
                          group ? "a Tex1 set" : linear ? "an image upload" : "composed VRAM", ct.mean,
                          (unsigned)g_cache.size() + 1);
        if (!make_texture((int)rect.w, (int)rect.h, px.data(), ct.tex, ct.srv)) {
            log_once("pgl-tex-create", "pgl: could not create a %ux%u texture", rect.w, rect.h);
            return false;
        }
        it = g_cache.emplace(key, std::move(ct)).first;
        g_texBytes += (size_t)rect.w * rect.h * 4;
        if (hooks)
            cache_hashed(it->second, chash);
        if (g_profOn)
            decode_note(t, rect, o, group, linear, br);
        }
        if (selHash != 0 && chash == selHash && !it->second.hlSrv) {
            /* Select mode's highlight: the picture in green, its alpha kept;
             * its pixels to the interface (its dump) */
            std::vector<uint32_t> green(px.size());
            for (size_t i = 0; i < px.size(); i++)
                green[i] = (px[i] & 0xFF000000u) | 0x0000FF00u;
            make_texture((int)rect.w, (int)rect.h, green.data(), it->second.hlTex, it->second.hlSrv);
            texrepl::selected_pixels(chash, px.data(), rect.w, rect.h);
        }
    }
    it->second.lastFrame = g_registryFrame;
    it->second.lastUse = g_execSeconds;
    out.srv = it->second.srv.get();
    out.x0 = (float)rect.x;
    out.y0 = (float)rect.y;
    out.w = (float)rect.w;
    out.h = (float)rect.h;
    out.alphaMode = 0;
    out.target = nullptr;
    out.depthTex = false;
    out.mean = it->second.mean;
    out.hasMean = true;
    out.aMin = it->second.aMin;             /* PC: (the alpha mirror) */
    out.aMax = it->second.aMax;
    out.hasAlpha = true;
    out.src = &it->second;                  /* (the F9 trace) */
    out.fromSet = group;                    /* PC: (the GPU sections) */
    if (group && (hooks || it->second.replSrv || it->second.hlSrv))
        texture_dress(it->second, hooks, selHash, out);     /* PC (2026-11-13): its replacement, the highlight */
    /* the front's entry: what was read - a set's: TBP0's tags; else TBP0's
     * block and the texture's blocks by the regions they lie in (none if
     * over four) */
    RegionSnap regions;
    const bool snapped = group || (regions.add_blocks(tbp, 1) &&
                                   regions.add_blocks(br.first, std::min<uint32_t>(br.count(), gstex::GS_MEM_BLOCKS)));
    front.tex0 = tex0Key;
    front.clamp = clampv;
    front.texa = texav;
    front.pal = palKey;
    front.alphaOnly = alphaOnly;
    front.epoch = g_tagEpoch;
    front.exact = group;
    front.tag0 = g_blockGen[0][tbp];
    front.tag1 = g_blockGen[1][tbp];
    front.regions = regions;
    front.ct = snapped && !virt ? &it->second : nullptr;
    front.ctGen = it->second.gen;           /* PC (2026-11-13) */
    front.out = out;
    if (gf != nullptr && group && o->id == gfObj) {
        gf->obj = gfObj;
        gf->parseVer = o->parseVer;
        gf->contentVer = o->contentVer;
        upload_refs(o->uploads, brL, &gf->refs, 0, &texBlocks);
        gf->ofs = gfOfs;
        gf->layer = top ? 1 : 0;
        gf->alphaOnly = alphaOnly;
        gf->tex0 = tex0Rel;
        gf->clamp = clampv;
        gf->texa = texav;
        gf->pal = palKey;
        gf->epoch = g_tagEpoch;
        gf->ct = &it->second;
        gf->ctGen = it->second.gen;         /* PC (2026-11-13) */
        gf->out = out;
        tex_ident_set(*gf, o);              /* PC: (the resolved runs) */
    }
    return true;
}

/* PC (2026-11-05, the resolved runs' textures: pgl_render.h TexIdent) */
void texture_ident_capture(bool on)
{
    g_texIdentOn = on;
}

void texture_last_ident(TexIdent &out)
{
    out = g_lastTexIdent;
}

/* The recorded bind stands now: no decoded texture dropped since (the
 * epoch), the same fields but the addresses, and at the live TBP0 the same
 * set (its parse) at the same place with the same uploads under the
 * texture, or the same target at the same place.  A set's texture is marked
 * used (its eviction is by age). */
bool texture_ident_check(const TexIdent &id, uint64_t tex0v, uint64_t clampv, uint64_t texav)
{
    /* (GT3PC_PGL_RUN_LOG=1: the first failures, field by field) */
    static int logged = -1;
    if (logged < 0) {
        const char *e = getenv("GT3PC_PGL_RUN_LOG");
        logged = (e != nullptr && strcmp(e, "0") != 0) ? 0 : 40;
    }
    auto fail = [&](const char *what, const Obj *o) {
        if (logged >= 40)
            return false;
        logged++;
        pclog::write("pgl runs: a texture's identity failed: %s (kind %u, TEX0 %016llX; recorded obj %u parse %u"
                     " content %u ofs 0x%X epoch %llu refs %u; now owner %s %u parse %u content %u ofs 0x%X epoch %llu)",
                     what, id.kind, (unsigned long long)tex0v, id.obj, id.parseVer, id.contentVer, id.ofs,
                     (unsigned long long)id.epoch, id.refs.n, o != nullptr ? kind_name(o->kind) : "none",
                     o != nullptr ? o->id : 0u, o != nullptr ? o->parseVer : 0u, o != nullptr ? o->contentVer : 0u,
                     o != nullptr ? (((uint32_t)tex0v & BLOCK_MASK) - g_ownBase[id.layer][(uint32_t)tex0v & BLOCK_MASK]) &
                                        BLOCK_MASK : 0u,
                     (unsigned long long)g_tagEpoch);
        return false;
    };
    if (id.kind == 0)
        return false;
    if (id.epoch != g_tagEpoch)
        return fail("the epoch", nullptr);
    const uint64_t tex0Rel = tex0v & ~(7ull << 61) & ~(0x3FFFull | 0x3FFFull << 37);
    if (tex0Rel != id.tex0)
        return fail("TEX0's fields", nullptr);
    if (clampv != id.clamp)
        return fail("CLAMP", nullptr);
    if (texav != id.texa)
        return fail("TEXA", nullptr);
    const uint32_t tbp = (uint32_t)tex0v & BLOCK_MASK;
    const Obj *o = obj(g_own[id.layer][tbp]);
    if (o == nullptr || o->id != id.obj)
        return fail("the owner", o);
    if (id.kind == 1) {
        if (o->kind != OBJ_GROUP || o->parseVer != id.parseVer)
            return fail("the parse", o);
        if (((tbp - g_ownBase[id.layer][tbp]) & BLOCK_MASK) != id.ofs)
            return fail("the place in the set", o);
        /* (the set's data unchanged since: every upload is; else those under
         * the texture checked) */
        if (id.contentVer != o->contentVer &&
            !(id.refs.n != 0xFF && upload_refs_same(o->uploads, id.refs)))
            return fail("the uploads under it", o);
        CachedTex *ct = static_cast<CachedTex *>(const_cast<void *>(id.ct));
        if (ct->gen != id.ctGen)
            return fail("its replacement loaded since", o);    /* PC (2026-11-13) */
        ct->lastFrame = g_registryFrame;
        ct->lastUse = g_execSeconds;
        return true;
    }
    if (!(o->kind == OBJ_TARGET && o->target == id.target && id.target != nullptr &&
          ((tbp - id.target->base) & BLOCK_MASK) == id.ofs))
        return fail("the target", o);
    return true;
}

/* ---- the F9 trace (docs/renderer.md 10, GT3PC_PGL_TRACE) ---------------- */

namespace {

const char *kind_short(uint8_t k)
{
    switch (k) {
    case OBJ_GROUP: return "set";
    case OBJ_IMAGE: return "image";
    case OBJ_SURFACE: return "surface";
    case OBJ_TARGET: return "target";
    case OBJ_PLANE: return "plane";
    }
    return "?";
}

/* An owner as text: its kind and id and what identifies it (block: a block
 * it owns in layer l, for a set the copy there). */
std::string trace_obj(const Obj *o, uint32_t block, int l)
{
    char s[224];
    if (o == nullptr)
        return "nothing";
    switch (o->kind) {
    case OBJ_GROUP:
        snprintf(s, sizeof(s), "set %u (%p stamp %llu, at 0x%X, this block's copy at 0x%X, version %u, uploaded %llu"
                 " frames ago)", o->id, o->img, (unsigned long long)o->stamp, o->curBase,
                 (unsigned)g_ownBase[l][block], o->contentVer, (unsigned long long)(g_registryFrame - o->lastFrame));
        break;
    case OBJ_IMAGE:
        snprintf(s, sizeof(s), "image %u (key %016llX: %s at 0x%X TBW %u, %ux%u)", o->id, (unsigned long long)o->key,
                 gstex::psm_name(o->t.dpsm), (unsigned)o->t.dbp, (unsigned)o->t.dbw, (unsigned)o->t.w,
                 (unsigned)o->t.h);
        break;
    case OBJ_SURFACE:
        snprintf(s, sizeof(s), "surface %u (0x%X FBW %u)", o->id, o->sdbp, o->sdbw);
        break;
    case OBJ_TARGET:
        snprintf(s, sizeof(s), "target %u (0x%X %dx%d)", o->id, o->target != nullptr ? o->target->base : 0u,
                 o->target != nullptr ? o->target->gsW : 0, o->target != nullptr ? o->target->gsH : 0);
        break;
    case OBJ_PLANE:
        snprintf(s, sizeof(s), "plane %u (0x%X)", o->id, o->pbp);
        break;
    }
    return s;
}

}  /* namespace */

void registry_trace_texture(uint64_t tex0v, const TexBinding &b, char *out, size_t n)
{
    const gstex::Tex0 t = gstex::Tex0::decode(tex0v);
    const uint32_t tbp = t.tbp0 & BLOCK_MASK;
    const int layer = psm_top(t.psm) ? 1 : 0;
    const Obj *o = obj(g_own[layer][tbp]);
    const CachedTex *ct = static_cast<const CachedTex *>(b.src);
    std::string s;
    char part[160];
    if (ct != nullptr) {
        snprintf(part, sizeof(part), "decoded %s %llu frames ago%s from ", ct->ahead ? "ahead" : "at a draw",
                 (unsigned long long)(g_registryFrame - ct->madeFrame),
                 b.fromFront ? " (bound by the front cache)" : "");
        s += part;
        if (ct->srcKind == OBJ_GROUP)
            snprintf(part, sizeof(part), "set %u (%p stamp %llu)", ct->srcObj, ct->srcImg,
                     (unsigned long long)ct->srcStamp);
        else
            snprintf(part, sizeof(part), "%s %u", kind_short(ct->srcKind), ct->srcObj);
        s += part;
        s += "; ";
    }
    snprintf(part, sizeof(part), "block 0x%X: ", tbp);
    s += part;
    s += trace_obj(o, tbp, layer);
    /* (a set's texture is its set's by image and stamp - decoded ahead it has
     * no object yet; anything else by the object) */
    bool differs = false;
    if (ct != nullptr && o == nullptr)
        differs = true;
    else if (ct != nullptr && ct->srcKind == OBJ_GROUP)
        differs = o->kind != OBJ_GROUP || o->img != ct->srcImg || o->stamp != ct->srcStamp;
    else if (ct != nullptr)
        differs = ct->srcObj != o->id;
    if (differs)
        s += " - NOT THE TEXTURE'S SOURCE";
    snprintf(out, n, "%s", s.c_str());
}

bool stress_on(const char *kind)
{
    static std::string kinds;
    static bool read = false;
    if (!read) {
        read = true;
        const char *e = getenv("GT3PC_TEX_STRESS");
        if (e != nullptr && e[0] != 0)
            kinds = std::string(",") + e + ",";
    }
    return !kinds.empty() &&
           (kinds.find(std::string(",") + kind + ",") != std::string::npos || kinds.find(",all,") != std::string::npos);
}

void registry_trace_owners()
{
    pclog::write("pgl owners: VRAM at the end of the traced frame (registry frame %llu), a line per run of blocks",
                 (unsigned long long)g_registryFrame);
    for (int l = 0; l < 2; l++) {
        /* a run: the same owner, and for a set the same copy */
        auto same = [&](uint32_t a, uint32_t c) {
            if (g_own[l][a] != g_own[l][c])
                return false;
            const Obj *o = obj(g_own[l][a]);
            return o == nullptr || o->kind != OBJ_GROUP || g_ownBase[l][a] == g_ownBase[l][c];
        };
        uint32_t start = 0;
        for (uint32_t b = 1; b <= gstex::GS_MEM_BLOCKS; b++) {
            if (b < gstex::GS_MEM_BLOCKS && same(b, start))
                continue;
            if (g_own[l][start] != 0)
                pclog::write("pgl owners %s 0x%04X-0x%04X: %s", l == 0 ? "RGB" : "top", start, b - 1,
                             trace_obj(obj(g_own[l][start]), start, l).c_str());
            start = b;
        }
    }
}

/* ======================================================================== */
/* Housekeeping                                                             */
/* ======================================================================== */

size_t registry_cache_bytes()               /* PC (2026-11-07) */
{
    return g_texBytes;
}

void registry_end_frame()
{
    g_registryFrame++;
    tex_pre_drain();                        /* PC: the textures decoded ahead (pgl_prewarm.cpp) */
    /* PC (2026-11-13, the texture replacements: pgl_texrepl.h): the textures
     * whose replacements were loaded since are bound anew (their gen); a
     * setting, the folders' list or Select mode's choice changed: every
     * bind anew; Select mode's list, the textures bound in the last frames */
    {
        static std::vector<uint64_t> loaded;
        texrepl::take_loaded(loaded);
        for (uint64_t h : loaded) {
            auto r = g_byHash.equal_range(h);
            for (auto i = r.first; i != r.second; ++i)
                i->second->gen++;
        }
        /* (a refresh: the textures whose files changed, came or went ask
         * anew) */
        texrepl::take_changed(loaded);
        for (uint64_t h : loaded) {
            auto r = g_byHash.equal_range(h);
            for (auto i = r.first; i != r.second; ++i) {
                i->second->replStale = true;
                i->second->gen++;
            }
        }
        const uint64_t serial = texrepl::serial();
        if (serial != g_replSerial) {
            g_replSerial = serial;
            g_tagEpoch++;
        }
        if (texrepl::list_wanted() && (g_registryFrame % 10) == 0) {
            std::vector<texrepl::Seen> list;
            for (const auto &e : g_cache) {
                const CachedTex &c = e.second;
                if (c.hashed && c.lastFrame + 2 >= g_registryFrame)
                    list.push_back({c.chash, (uint16_t)c.rect.w, (uint16_t)c.rect.h, (bool)c.replSrv});
            }
            std::sort(list.begin(), list.end(),
                      [](const texrepl::Seen &a, const texrepl::Seen &b) { return a.hash < b.hash; });
            list.erase(std::unique(list.begin(), list.end(),
                                   [](const texrepl::Seen &a, const texrepl::Seen &b) { return a.hash == b.hash; }),
                       list.end());
            texrepl::publish(list);
        }
    }
    if ((g_registryFrame % g_stress.housekeep) != 0)
        return;
    const size_t kTexBudget = g_stress.budget;  /* (GT3PC_TEX_STRESS evict) */
    const size_t replBudget = g_replBytes != 0 ? texrepl::budget_bytes() : 0;   /* PC (2026-11-13) */
    texrepl::housekeep(g_execSeconds);
    /* PC (2026-11-13): replacing left off (texrepl::keep_loaded: past the
     * time the on / off key keeps them for): the replacements let go.  None
     * is bound - turning it off moved g_tagEpoch, every bind since took the
     * game's own */
    if (g_replBytes != 0 && !texrepl::keep_loaded()) {
        for (auto &e : g_cache) {
            CachedTex &c = e.second;
            if (c.replSrv) {
                c.replTex.reset();
                c.replSrv.reset();
                c.replBytes = 0;
                c.replStale = false;
            }
        }
        g_replBytes = 0;
    }
    const double kTexKeep = g_stress.keep, kTexExpire = g_stress.expire;
    /* decoded textures unused for kTexExpire seconds; then, over the budget,
     * the least recently bound (not within kTexKeep seconds: composed ones,
     * whose keys follow the uploads, are among the first) */
    bool dropped = false;
    const double now = g_execSeconds;
    std::vector<std::pair<double, uint64_t>> old;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->second.lastUse + kTexExpire < now) {
            g_texBytes -= (size_t)it->second.rect.w * it->second.rect.h * 4;
            cache_forget(it->second);       /* PC (2026-11-13) */
            it = g_cache.erase(it);
            dropped = true;
            continue;
        }
        if (it->second.lastUse + kTexKeep < now)
            old.push_back({it->second.lastUse, it->first});
        ++it;
    }
    if (g_texBytes > kTexBudget || g_replBytes > replBudget) {  /* PC (2026-11-13): or the replacements' budget */
        std::sort(old.begin(), old.end());
        for (const auto &o : old) {
            if (g_texBytes <= kTexBudget && g_replBytes <= replBudget)
                break;
            auto it = g_cache.find(o.second);
            if (it == g_cache.end())
                continue;
            g_texBytes -= (size_t)it->second.rect.w * it->second.rect.h * 4;
            cache_forget(it->second);       /* PC (2026-11-13) */
            g_cache.erase(it);
            dropped = true;
        }
    }
    if (dropped)
        g_tagEpoch++;                       /* PC: (the texture binds' front pointed into it) */
    /* objects that own no block any more (PC, CPU time: flagged through the
     * owner map, an object looked up where the owner changes - a std::set
     * of the 32768 entries was a spike every 64 frames) */
    for (auto &e : g_objs)
        e.second->live = false;
    for (int l = 0; l < 2; l++) {
        uint32_t last = 0;
        for (uint32_t b = 0; b < gstex::GS_MEM_BLOCKS; b++) {
            const uint32_t id = g_own[l][b];
            if (id != 0 && id != last) {
                if (Obj *o = obj(id))
                    o->live = true;
                last = id;
            }
        }
    }
    for (auto it = g_objs.begin(); it != g_objs.end();) {
        Obj *o = it->second.get();
        bool drop = false;
        if (o->kind == OBJ_IMAGE || o->kind == OBJ_SURFACE || o->kind == OBJ_PLANE)
            drop = !o->live;
        else if (o->kind == OBJ_GROUP)
            drop = !o->live && o->lastUse + kTexExpire < now;  /* PC: by time (it was 600 frames) */
        if (drop) {
            obj_forget(o->id);
            if (g_texSrc.obj == o)
                g_texSrc = TexSource();     /* PC: (registry_texture_source's) */
            if (o->kind == OBJ_IMAGE) {
                auto k = g_imageByKey.find(o->key);
                if (k != g_imageByKey.end() && k->second == o->id)
                    g_imageByKey.erase(k);
            } else if (o->kind == OBJ_PLANE) {
                auto k = g_planeByBlock.find(o->pbp);
                if (k != g_planeByBlock.end() && k->second == o->id)
                    g_planeByBlock.erase(k);
            } else if (o->kind == OBJ_SURFACE) {
                auto k = g_surfaceByKey.find((uint64_t)o->sdbp | ((uint64_t)o->sdbw << 16));
                if (k != g_surfaceByKey.end() && k->second == o->id)
                    g_surfaceByKey.erase(k);
            } else {
                auto k = g_groupByImg.find(o->img);
                if (k != g_groupByImg.end() && k->second == o->id)
                    g_groupByImg.erase(k);
            }
            it = g_objs.erase(it);
        } else {
            ++it;
        }
    }
    /* captures whose first plane is gone */
    for (auto it = g_captures.begin(); it != g_captures.end();) {
        if (g_planeByBlock.count(it->first) == 0)
            it = g_captures.erase(it);
        else
            ++it;
    }
}

}  /* namespace pglr */
