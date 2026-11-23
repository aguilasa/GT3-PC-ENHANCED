/* pgl_models.cpp - decoded VifModel meshes for the VU1 model draws
 * (docs/models.md 9 and 10, docs/renderer.md "Models").
 *
 * model_draw / vif_model_draw hand over a VifModel and a part index; the
 * whole model is decoded once with gt3vif::decodeModel into vertex / index
 * arrays (immutable native buffers) and cached by the model's address.  The
 * first use in each frame checks a hash of the model's header and part
 * table; the part data is hashed in turn (each model every kVerifyFrames
 * frames, the models spread over them) and, once found changed under the
 * same table (logged), at each first use in a frame.  The game says when it
 * writes a model (pglr_model_loaded: vif_model_relocate; pglr_model_written:
 * TStripBuilder::finish - the tyre and meter packets, rebuilt in place), which decodes it
 * again at its next use.  Parts drawn with positions sent by vu1_unpack_xyz
 * (preXyz) are decoded for the draw into the dynamic stream.
 *
 * PC: a model the game loads (pglr_model_loaded: vif_model_relocate) is
 * decoded ahead on the worker (pgl_prewarm.cpp) from a copy taken then; its
 * first draw takes that mesh when the model still hashes as the copy did.
 *
 * PC: a ModelSet's packets are compared as it loads (models_set_loaded):
 * one whose triangles lie on an earlier one's is an overlay, drawn a hair
 * nearer (model_overlay; docs/renderer.md "Overlays"). */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/pgl/native/pgl_win32.h"

#include "platform/log.h"

namespace pglr {

namespace {

/* A model's part data is hashed every this many frames (its slot among
 * them).  PC (CPU time, 2026-11-05): 128 - at 32, a 32-car race hashed ~17
 * models' data a frame (~20 us each: a third of a millisecond); the game
 * says when it writes a model (models_written), the hash is the net under
 * it, and a model found changed under the same table is hashed every frame
 * from then on. */
constexpr uint32_t kVerifyFrames = 128;

struct MeshEntry {
    gt3vif::Mesh mesh;
    std::vector<uint32_t> partFirst;    /* part p's batches: [partFirst[p], partFirst[p + 1]) (empty: unsorted) */
    Com<ID3D11Buffer> vb, ib;
    Com<ID3D11Buffer> bvb;          /* PC: each vertex's batch (make_batch_buffer) */
    /* PC (2026-11-06): the doubled index stream (make_mesh_buffers): each
     * batch's indices, then the same again into the vertices' second copy
     * (vb and bvb hold every vertex twice) - a C = Ad draw's two passes as
     * one draw's first and second half, pgl_exec.cpp sub_vu1; nVerts: the
     * vertices of one copy (null / 0: not doubled) */
    Com<ID3D11Buffer> ib2;
    uint32_t nVerts = 0;
    uint64_t tableHash = 0;         /* the header and the part table */
    uint64_t dataHash = 0;          /* the parts' data */
    uint64_t checkedFrame = 0;
    uint64_t lastFrame = 0;
    double lastUse = 0.0;           /* g_execSeconds when last drawn */
    size_t bytes = 0;               /* its decoded vertices and indices */
    uint32_t verifySlot = 0;        /* the frame (mod kVerifyFrames) its data is hashed */
    uint64_t serial = 0;            /* PC (2026-11-05): the decode the mesh is of (model_mesh's serial) */
    uint64_t writtenFrame = 0;      /* PC (2026-11-05): the frame it was last written in, and how often (model_volatile) */
    uint32_t writes = 0;
    bool ok = false;
    bool written = false;           /* the game wrote it (pglr_model_written / _loaded) */
    bool changing = false;          /* its data changed under the same table: hashed each frame */
};

std::unordered_map<const void *, std::unique_ptr<MeshEntry>> g_meshes;
/* PC: the decoded meshes are kept while they fit in kMeshBudget bytes (the
 * least recently drawn go first beyond it, never one drawn in the last
 * kMeshKeep seconds) or until unused for kMeshExpire seconds: a course's
 * models stay decoded from lap to lap (at 600 frames unused they went in
 * under three seconds at a high frame rate, decoded again every lap - the
 * stutters where a stretch of the course came back into view). */
constexpr size_t kMeshBudget = 128u << 20;  /* PC (2026-11-07): 128, was 256 - the address space (platform/memstat.h);
                                               a 32-car race decodes ~60 MB */
constexpr double kMeshKeep = 5.0, kMeshExpire = 600.0;
size_t g_meshBytes;
gt3vif::Mesh g_tmp;
uint64_t g_frame = 1;
uint64_t g_meshSerial = 0;              /* PC (2026-11-05): the last decode's number (MeshEntry::serial) */
uint32_t g_nextSlot;
/* The last model looked up (a model's parts are drawn one after another);
 * forgotten as a frame ends (the entries erased then). */
const void *g_lastModel;
MeshEntry *g_lastEntry;
/* The models the game wrote (pglr_model_written, any thread), applied at
 * the next lookup. */
std::mutex g_writtenLock;
std::vector<const void *> g_written;
std::atomic<bool> g_writtenAny{false};

/* PC (docs/renderer.md "Overlays"): the ModelSet packets drawn on an
 * earlier packet's surface - a lit lamp's glow on its lens, the same
 * triangles with the vertices up to 6e-8 apart - and their layers (drawn
 * nearer by them: vu1_depth).  The GS's integer Z tied such a pair and
 * GEQUAL kept the later; a float depth split them by its last bits, a
 * triangle at a time (the glow flickered, more so far away).  Written by
 * the game thread, read by the executor (g_overlayLock). */
std::unordered_map<const void *, int> g_overlay;
SRWLOCK g_overlayLock = SRWLOCK_INIT;
/* PC (CPU time, 2026-11-03): model_overlay's answers by model, direct-mapped,
 * while g_overlayGen stays (moved under the lock at every change of
 * g_overlay) - the lock and the map's lookup were taken for every part */
std::atomic<uint32_t> g_overlayGen{1};
struct OverlaySlot {
    const void *model = nullptr;
    uint32_t gen = 0;
    int layer = 0;
};
constexpr uint32_t kOverlaySlots = 4096;
OverlaySlot g_overlaySlot[kOverlaySlots];
/* A vertex lies on another within kOverlayNear (model units, per axis), a
 * triangle on another when its three corners do; a packet is an overlay
 * when kOverlayShare of its triangles do.  (Counted by vertices, a level
 * of detail lay on the one above it - most of its vertices are that one's -
 * and the levels of a car stacked up to 14 layers.) */
constexpr float kOverlayNear = 1e-5f;
constexpr float kOverlayShare = 0.5f;

/* A fast 64-bit hash (four independent lanes over 32-byte steps: memory
 * bound, several GB/s). */
uint64_t hash_words(const void *p, size_t bytes, uint64_t h)
{
    const uint8_t *s = static_cast<const uint8_t *>(p);
    if (bytes >= 32) {
        uint64_t a = h ^ 0x9E3779B97F4A7C15ull, b = h + 0xC2B2AE3D27D4EB4Full, c = h ^ 0x165667B19E3779F9ull,
                 d = h - 0x27D4EB2F165667C5ull;
        while (bytes >= 32) {
            uint64_t w[4];
            memcpy(w, s, 32);
            a = mul64(a ^ w[0], 0xFF51AFD7ED558CCDull);     /* PC: (mul64: no _allmul call) */
            b = mul64(b ^ w[1], 0xC4CEB9FE1A85EC53ull);
            c = mul64(c ^ w[2], 0xFF51AFD7ED558CCDull);
            d = mul64(d ^ w[3], 0xC4CEB9FE1A85EC53ull);
            a ^= a >> 29;
            b ^= b >> 31;
            c ^= c >> 29;
            d ^= d >> 31;
            s += 32;
            bytes -= 32;
        }
        h = a ^ (b * 3) ^ (c * 5) ^ (d * 7);
    }
    while (bytes >= 8) {
        uint64_t w;
        memcpy(&w, s, 8);
        h = mul64(h ^ w, 0xFF51AFD7ED558CCDull);
        h ^= h >> 31;
        s += 8;
        bytes -= 8;
    }
    while (bytes-- > 0)
        h = mul64(h ^ *s++, 0x100000001B3ull);
    return h;
}

/* The header and the part table (the data's addresses and sizes). */
uint64_t table_hash(const void *model)
{
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(model);
    return hash_words(model, sizeof(*hdr) + (size_t)hdr->nParts * sizeof(gt3vif::VifPartRecord), 0x51ED270B27u);
}

/* The parts' data (the VIF packets). */
uint64_t data_hash(const void *model)
{
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(model);
    const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
    uint64_t h = 0x2545F4914F6CDD1Dull;
    for (uint32_t i = 0; i < hdr->nParts; i++) {
        if (parts[i].data != 0)
            h = hash_words(reinterpret_cast<const void *>((uintptr_t)parts[i].data), (size_t)parts[i].qwc * 16, h);
    }
    return h;
}

/* PC (2026-11-05): model_volatile's answers by model, direct-mapped, while
 * g_volatileGen stays (moved by take_written: a write noted) */
uint32_t g_volatileGen = 1;
struct VolatileSlot {
    const void *model = nullptr;
    uint32_t gen = 0;
    bool yes = false;
};
constexpr uint32_t kVolatileSlots = 4096;
VolatileSlot g_volatileSlot[kVolatileSlots];

/* The models written since the last lookup: decoded again at their next
 * use. */
void take_written()
{
    std::vector<const void *> w;
    {
        std::lock_guard<std::mutex> lock(g_writtenLock);
        w.swap(g_written);
        g_writtenAny.store(false, std::memory_order_relaxed);
    }
    for (const void *model : w) {
        auto it = g_meshes.find(model);
        if (it != g_meshes.end()) {
            MeshEntry &e = *it->second;
            e.written = true;
            e.checkedFrame = 0;
            /* PC (2026-11-05): written frame after frame (model_volatile) */
            if (e.writtenFrame != g_frame) {
                e.writes = g_frame - e.writtenFrame <= 4 ? e.writes + 1 : 1;
                e.writtenFrame = g_frame;
            }
        }
    }
    g_volatileGen++;
}

bool make_buffer(const void *data, size_t bytes, UINT bind, Com<ID3D11Buffer> &out)
{
    out.reset();
    if (bytes == 0)
        return false;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (UINT)bytes;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = data;
    return SUCCEEDED(dev()->CreateBuffer(&d, &init, out.put()));
}

/* PC (GPU time, 2026-11-03): each vertex's batch (its index in the mesh's
 * batches, 16 bits) - a merged draw's vertex finds its part's record by it
 * (vs_vu1; a search by the parts' first vertices, a chain of reads per
 * vertex, cost the GPU ~10 % of a 32-car frame) */
bool make_batch_buffer(const gt3vif::Mesh &m, Com<ID3D11Buffer> &out, bool twice)
{
    const size_t n = m.vertices.size();
    std::vector<uint16_t> batch(twice ? 2 * n : n, 0);
    for (size_t b = 0; b < m.batches.size(); b++) {
        const gt3vif::Batch &bt = m.batches[b];
        for (uint32_t v = 0; v < bt.vertexCount && (size_t)bt.firstVertex + v < n; v++) {
            batch[bt.firstVertex + v] = (uint16_t)b;
            if (twice)
                batch[n + bt.firstVertex + v] = (uint16_t)b;
        }
    }
    return make_buffer(batch.data(), batch.size() * sizeof(uint16_t), D3D11_BIND_VERTEX_BUFFER, out);
}

/* PC (2026-11-06): the doubled index stream (docs/renderer.md 3, "C = Ad").
 * A C = Ad draw (Cs Ad + Cd: the cars' env passes) is two passes over the
 * same triangles, the first adding without the alpha, the second with it,
 * and the GS draws each part whole before the next: a car's env passes went
 * as one draw of two instances each, never joined (the next part's first
 * pass would come before this part's second).  With every vertex held twice
 * (vb, bvb) and, per batch, its indices followed by the same into the second
 * copy (ib2), one draw of the doubled range is the two passes in order -
 * vs_vu1 tells the pass by the vertex number (SV_VertexID >= nVerts) - and
 * consecutive parts' doubled ranges are consecutive, so they join as other
 * parts do (pgl_exec.cpp merge_join): a car's ~15 env pairs one draw.
 * Memory for speed: the vertices and their batches twice, the indices
 * three times (the plain stream stays for every other draw).  Needs the
 * batches to tile the indices in order (they do: decodePart appends them);
 * else no ib2 and the plain buffers.  GT3PC_PGL_AD_DOUBLE=0: the plain
 * buffers only (the two-instance draws as before). */
bool models_double_on()
{
    static const bool on = !(getenv("GT3PC_PGL_AD_DOUBLE") != nullptr && strcmp(getenv("GT3PC_PGL_AD_DOUBLE"), "0") == 0);
    return on;
}

bool make_mesh_buffers(const gt3vif::Mesh &m, Com<ID3D11Buffer> &vb, Com<ID3D11Buffer> &ib, Com<ID3D11Buffer> &bvb,
                       Com<ID3D11Buffer> &ib2, uint32_t &nVerts, size_t &bytes)
{
    const size_t n = m.vertices.size(), ni = m.indices.size();
    bytes = n * sizeof(gt3vif::Vertex) + ni * 4;
    ib2.reset();
    nVerts = 0;
    bool tiled = models_double_on() && n > 0 && n < 0x7FFFFFFFu / 8 && ni > 0;
    if (tiled) {
        size_t pos = 0;
        for (const gt3vif::Batch &bt : m.batches) {
            if (bt.firstIndex != pos) {
                tiled = false;
                break;
            }
            pos += bt.indexCount;
        }
        tiled = tiled && pos == ni;
    }
    if (!tiled) {
        return make_buffer(m.vertices.data(), n * sizeof(gt3vif::Vertex), D3D11_BIND_VERTEX_BUFFER, vb) &&
               make_buffer(m.indices.data(), ni * 4, D3D11_BIND_INDEX_BUFFER, ib) && make_batch_buffer(m, bvb, false);
    }
    std::vector<gt3vif::Vertex> v2(2 * n);
    memcpy(v2.data(), m.vertices.data(), n * sizeof(gt3vif::Vertex));
    memcpy(v2.data() + n, m.vertices.data(), n * sizeof(gt3vif::Vertex));
    std::vector<uint32_t> i2(2 * ni);
    size_t at = 0;
    for (const gt3vif::Batch &bt : m.batches) {
        for (uint32_t k = 0; k < bt.indexCount; k++)
            i2[at + k] = m.indices[bt.firstIndex + k];
        for (uint32_t k = 0; k < bt.indexCount; k++)
            i2[at + bt.indexCount + k] = m.indices[bt.firstIndex + k] + (uint32_t)n;
        at += 2 * (size_t)bt.indexCount;
    }
    if (!(make_buffer(v2.data(), v2.size() * sizeof(gt3vif::Vertex), D3D11_BIND_VERTEX_BUFFER, vb) &&
          make_buffer(m.indices.data(), ni * 4, D3D11_BIND_INDEX_BUFFER, ib) &&
          make_buffer(i2.data(), i2.size() * 4, D3D11_BIND_INDEX_BUFFER, ib2) && make_batch_buffer(m, bvb, true)))
        return false;
    nVerts = (uint32_t)n;
    bytes = 2 * n * sizeof(gt3vif::Vertex) + 3 * ni * 4 + 2 * n * sizeof(uint16_t);
    return true;
}

void log_warnings(const gt3vif::Mesh &m, const void *model)
{
    for (const std::string &w : m.warnings) {
        char key[96];
        snprintf(key, sizeof(key), "pgl-model-warn-%p-%s", model, w.c_str());
        log_once(key, "pgl: model %p: %s", model, w.c_str());
    }
}

/* PC: each batch's vertex box, morph targets included (Batch::bmin / bmax:
 * the screen area a draw writes, vu1_draw). */
void batch_bounds(gt3vif::Mesh &m)
{
    for (gt3vif::Batch &b : m.batches) {
        b.hasBounds = false;
        b.aMin = 0;
        b.aMax = 255;
        b.nMax2 = 1e30f;
        if (b.vertexCount == 0 || (size_t)b.firstVertex + b.vertexCount > m.vertices.size())
            continue;
        for (int k = 0; k < 3; k++) {
            b.bmin[k] = 1e30f;
            b.bmax[k] = -1e30f;
        }
        /* PC (2026-11-04): and the vertex alphas' range and the normals'
         * greatest length (the alpha mirror: light_alpha_range) */
        uint8_t aMin = 255, aMax = 0;
        float n2 = 0.0f;
        for (uint32_t i = 0; i < b.vertexCount; i++) {
            const gt3vif::Vertex &v = m.vertices[b.firstVertex + i];
            for (int k = 0; k < 3; k++) {
                b.bmin[k] = std::min(b.bmin[k], std::min(v.pos[k], v.pos2[k]));
                b.bmax[k] = std::max(b.bmax[k], std::max(v.pos[k], v.pos2[k]));
            }
            aMin = std::min(aMin, std::min(v.rgba[3], v.rgba2[3]));
            aMax = std::max(aMax, std::max(v.rgba[3], v.rgba2[3]));
            const float a = v.nrm[0] * v.nrm[0] + v.nrm[1] * v.nrm[1] + v.nrm[2] * v.nrm[2];
            const float c = v.nrm2[0] * v.nrm2[0] + v.nrm2[1] * v.nrm2[1] + v.nrm2[2] * v.nrm2[2];
            n2 = std::max(n2, std::max(a, c));
        }
        b.hasBounds = true;
        b.aMin = aMin;
        b.aMax = aMax;
        b.nMax2 = n2;
    }
}

/* PC (CPU time): where each part's batches lie (decodeModel decodes the
 * parts in order), so a part's draw does not scan the whole model's. */
void part_index(const gt3vif::Mesh &m, uint32_t nParts, std::vector<uint32_t> &first)
{
    first.assign((size_t)nParts + 1, 0);
    uint32_t b = 0;
    for (uint32_t p = 0; p <= nParts; p++) {
        while (b < m.batches.size() && m.batches[b].part < p)
            b++;
        first[p] = b;
    }
    for (size_t i = 1; i < m.batches.size(); i++)
        if (m.batches[i].part < m.batches[i - 1].part) {
            first.clear();          /* (not in part order: the whole list) */
            return;
        }
}

/* ---- decoded ahead (pgl_prewarm.cpp) ------------------------------------- */

/* The meshes the worker decoded of the models the game loaded, by address,
 * until a draw takes them (take_pre); a model loaded there again replaces
 * its own, those no draw took in kMeshExpire seconds go (models_end_frame).
 * g_preSkip: the models a draw decoded itself before the worker was done
 * (their table hash): the worker drops those.  (Never freed, the lock has no
 * destructor: the worker may still run while the process exits.) */
struct PreMesh {
    gt3vif::Mesh mesh;
    std::vector<uint32_t> partFirst;
    Com<ID3D11Buffer> vb, ib, bvb;
    Com<ID3D11Buffer> ib2;          /* PC (2026-11-06): (the doubled index stream, make_mesh_buffers) */
    uint32_t nVerts = 0;
    uint64_t tableHash = 0, dataHash = 0;
    size_t bytes = 0;
    double made = 0.0;              /* pre::seconds() */
    bool ok = false;
};
SRWLOCK g_preLock = SRWLOCK_INIT;
std::unordered_map<const void *, std::unique_ptr<PreMesh>> *g_pre =
    new std::unordered_map<const void *, std::unique_ptr<PreMesh>>();
std::unordered_map<const void *, uint64_t> *g_preSkip = new std::unordered_map<const void *, uint64_t>();
std::atomic<uint32_t> g_preCount{0};

/* A model as the game loaded it: its header and part table (part i's data
 * word now i + 1, 0 without data) and its parts' data, copied on the game
 * thread (models_loaded); the table hashed there. */
struct MeshJob : pre::Job {
    const void *model = nullptr;
    uint64_t tableHash = 0;
    std::vector<uint8_t> table;
    std::vector<uint8_t> data;
    std::vector<size_t> at;         /* part i's data in `data' */
    void run() override;
};

const void *resolve_copy(uint32_t word, void *user)
{
    const MeshJob *j = static_cast<const MeshJob *>(user);
    if (word == 0 || word > j->at.size())
        return nullptr;
    return j->data.data() + j->at[word - 1];
}

/* (the worker) */
void MeshJob::run()
{
    std::unique_ptr<PreMesh> p(new PreMesh());
    const gt3vif::VifModelHeader *hdr = reinterpret_cast<const gt3vif::VifModelHeader *>(table.data());
    const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
    /* data_hash as the game's memory gives it (the same bytes, part by
     * part) */
    uint64_t h = 0x2545F4914F6CDD1Dull;
    for (uint32_t i = 0; i < hdr->nParts; i++)
        if (parts[i].data != 0)
            h = hash_words(data.data() + at[i], (size_t)parts[i].qwc * 16, h);
    p->tableHash = tableHash;
    p->dataHash = h;
    gt3vif::DecodeOptions opt;
    p->ok = gt3vif::decodeModel(table.data(), resolve_copy, this, opt, p->mesh) && !p->mesh.vertices.empty() &&
            !p->mesh.indices.empty();
    log_warnings(p->mesh, model);
    p->bytes = p->mesh.vertices.size() * sizeof(gt3vif::Vertex) + p->mesh.indices.size() * 4;
    if (p->ok) {
        batch_bounds(p->mesh);
        part_index(p->mesh, hdr->nParts, p->partFirst);
        p->ok = make_mesh_buffers(p->mesh, p->vb, p->ib, p->bvb, p->ib2, p->nVerts, p->bytes);
    }
    p->made = pre::seconds();
    pre::made(pre::MODEL, 1, p->bytes);
    std::unique_ptr<PreMesh> old;           /* (released after the lock) */
    AcquireSRWLockExclusive(&g_preLock);
    auto s = g_preSkip->find(model);
    if (s != g_preSkip->end() && s->second == tableHash) {
        g_preSkip->erase(s);
        old = std::move(p);
    } else {
        std::unique_ptr<PreMesh> &slot = (*g_pre)[model];
        old = std::move(slot);
        slot = std::move(p);
        g_preCount.store((uint32_t)g_pre->size(), std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_preLock);
}

/* The mesh decoded ahead for model into e, if the model is still what was
 * copied (its table hash th; its data's dh, hashed here unless `hashed').
 * Else, with the worker on, the model is noted as decoded at the draw. */
bool take_pre(const void *model, uint64_t th, bool hashed, uint64_t dh, MeshEntry *e)
{
    if (!pre::on())
        return false;
    std::unique_ptr<PreMesh> p;
    AcquireSRWLockExclusive(&g_preLock);
    if (g_preCount.load(std::memory_order_relaxed) != 0) {
        auto it = g_pre->find(model);
        if (it != g_pre->end()) {
            p = std::move(it->second);
            g_pre->erase(it);
            g_preCount.store((uint32_t)g_pre->size(), std::memory_order_relaxed);
        }
    }
    if (!p) {
        if (g_preSkip->size() > 4096)
            g_preSkip->clear();
        (*g_preSkip)[model] = th;
    }
    ReleaseSRWLockExclusive(&g_preLock);
    if (!p || p->tableHash != th)
        return false;
    if (!hashed)
        dh = data_hash(model);
    if (p->dataHash != dh)
        return false;
    e->mesh = std::move(p->mesh);
    e->partFirst = std::move(p->partFirst);
    e->vb = std::move(p->vb);
    e->ib = std::move(p->ib);
    e->bvb = std::move(p->bvb);
    e->ib2 = std::move(p->ib2);
    e->nVerts = p->nVerts;
    e->ok = p->ok;
    e->tableHash = th;
    e->dataHash = dh;
    e->written = false;
    e->serial = ++g_meshSerial;             /* PC: (another decode) */
    g_meshBytes -= e->bytes;
    e->bytes = p->bytes;
    g_meshBytes += e->bytes;
    pre::taken(pre::MODEL);
    return true;
}

}  /* namespace */

bool model_mesh(const PglrModelPart &part, const gt3vif::Mesh **mesh, ID3D11Buffer **vb, ID3D11Buffer **ib,
                uint32_t *baseVertex, uint32_t *baseIndex, uint32_t *firstBatch, uint32_t *endBatch,
                ID3D11Buffer **batchVb, uint64_t *serial, ID3D11Buffer **ib2, uint32_t *nVerts)
{
    *batchVb = nullptr;
    if (ib2 != nullptr)
        *ib2 = nullptr;                     /* PC (2026-11-06): (the doubled index stream: a decoded model's) */
    if (nVerts != nullptr)
        *nVerts = 0;
    if (serial != nullptr)
        *serial = 0;                        /* PC: (a streamed part's, or none: no decode to name) */
    if (part.model == nullptr)
        return false;
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(part.model);
    if (part.part < 0 || part.part >= (int)hdr->nParts)
        return false;

    if (part.preXyz != nullptr) {
        /* positions sent in front of the part: decode it for this draw */
        const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
        const gt3vif::VifPartRecord &pr = parts[part.part];
        if (pr.data == 0)
            return false;
        ProfScope prof(PROF_MESH_DECODE);   /* (the verbose monitor's profile) */
        gt3vif::DecodeOptions opt;
        opt.preXyz = part.preXyz;
        opt.preXyzCount = part.preXyzCount;
        g_tmp.clear();
        g_tmp.mode = hdr->mode;
        g_tmp.scaleFlags = hdr->scaleFlags;
        if (!gt3vif::decodePart(reinterpret_cast<const void *>((uintptr_t)pr.data), pr.qwc, pr.tex,
                                (uint16_t)part.part, hdr->mode, opt, g_tmp) ||
            g_tmp.vertices.empty() || g_tmp.indices.empty())
            return false;
        batch_bounds(g_tmp);
        *baseVertex = stream_vertices(g_tmp.vertices.data(), sizeof(gt3vif::Vertex),
                                      (uint32_t)g_tmp.vertices.size(), vb);
        *baseIndex = stream_indices(g_tmp.indices.data(), (uint32_t)g_tmp.indices.size(), ib);
        *mesh = &g_tmp;
        *firstBatch = 0;
        *endBatch = (uint32_t)g_tmp.batches.size();
        return true;
    }

    if (g_writtenAny.load(std::memory_order_relaxed))
        take_written();
    MeshEntry *e = g_lastEntry;
    if (part.model != g_lastModel) {
        std::unique_ptr<MeshEntry> &slot = g_meshes[part.model];
        if (!slot) {
            slot.reset(new MeshEntry());
            slot->verifySlot = g_nextSlot++ % kVerifyFrames;
        }
        e = slot.get();
        g_lastModel = part.model;
        g_lastEntry = e;
    }
    if (e->checkedFrame != g_frame) {
        const uint64_t th = table_hash(part.model);
        bool stale = !e->ok || e->written || th != e->tableHash;
        uint64_t dh = 0;
        bool hashed = false;
        if (!stale && (e->changing || (g_frame + e->verifySlot) % kVerifyFrames == 0)) {
            dh = data_hash(part.model);
            hashed = true;
            if (dh != e->dataHash) {
                stale = true;
                if (!e->changing) {
                    e->changing = true;
                    char key[48];
                    snprintf(key, sizeof(key), "pgl-model-changed-%p", part.model);
                    log_once(key, "pgl: model %p changed in place (its data, not its part table): checked every frame",
                             part.model);
                }
            }
        }
        const bool fresh = e->tableHash == 0;   /* (never decoded: the log's counts) */
        if (stale && take_pre(part.model, th, hashed, dh, e))
            stale = false;                  /* PC: decoded ahead (pgl_prewarm.cpp) */
        else if (stale && fresh && pre::on())
            pre::late(pre::MODEL);
        if (stale) {
            ProfScope prof(PROF_MESH_DECODE);
            gt3vif::DecodeOptions opt;
            e->mesh.clear();
            e->ok = gt3vif::decodeModel(part.model, e->mesh, opt) && !e->mesh.vertices.empty() &&
                    !e->mesh.indices.empty();
            log_warnings(e->mesh, part.model);
            if (e->ok) {
                batch_bounds(e->mesh);
                part_index(e->mesh, hdr->nParts, e->partFirst);
            }
            size_t bytes = e->mesh.vertices.size() * sizeof(gt3vif::Vertex) + e->mesh.indices.size() * 4;
            if (e->ok)
                e->ok = make_mesh_buffers(e->mesh, e->vb, e->ib, e->bvb, e->ib2, e->nVerts, bytes);
            e->tableHash = th;
            e->dataHash = hashed ? dh : data_hash(part.model);
            e->written = false;
            e->serial = ++g_meshSerial;     /* PC: (another decode) */
            g_meshBytes -= e->bytes;
            e->bytes = bytes;
            g_meshBytes += e->bytes;
        }
        e->checkedFrame = g_frame;
        e->lastUse = g_execSeconds;
    }
    e->lastFrame = g_frame;
    if (!e->ok)
        return false;
    if (serial != nullptr)
        *serial = e->serial;
    *mesh = &e->mesh;
    *vb = e->vb.get();
    *ib = e->ib.get();
    *batchVb = e->bvb.get();
    if (ib2 != nullptr)
        *ib2 = e->ib2.get();
    if (nVerts != nullptr)
        *nVerts = e->nVerts;
    *baseVertex = 0;
    *baseIndex = 0;
    if ((size_t)part.part + 1 < e->partFirst.size()) {
        *firstBatch = e->partFirst[part.part];
        *endBatch = e->partFirst[part.part + 1];
    } else {
        *firstBatch = 0;
        *endBatch = (uint32_t)e->mesh.batches.size();
    }
    return true;
}

void models_written(const void *model)
{
    if (model == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_writtenLock);
    g_written.push_back(model);
    g_writtenAny.store(true, std::memory_order_relaxed);
}

void models_loaded(const void *model)
{
    models_written(model);
    if (model == nullptr)
        return;
    /* PC: an overlay's address taken by another model (modelset_relocate
     * marks its own again after this) */
    AcquireSRWLockExclusive(&g_overlayLock);
    g_overlay.erase(model);
    g_overlayGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_overlayLock);
    if (!pre::on())
        return;
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(model);
    const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
    MeshJob *j = new MeshJob();
    j->model = model;
    j->tableHash = table_hash(model);
    const uint8_t *tbl = static_cast<const uint8_t *>(model);
    j->table.assign(tbl, tbl + sizeof(*hdr) + (size_t)hdr->nParts * sizeof(gt3vif::VifPartRecord));
    j->at.resize(hdr->nParts);
    size_t total = 0;
    for (uint32_t i = 0; i < hdr->nParts; i++) {
        j->at[i] = total;
        if (parts[i].data != 0)
            total += (size_t)parts[i].qwc * 16;
    }
    j->data.resize(total);
    gt3vif::VifPartRecord *copy = reinterpret_cast<gt3vif::VifPartRecord *>(j->table.data() + sizeof(*hdr));
    for (uint32_t i = 0; i < hdr->nParts; i++) {
        if (parts[i].data != 0 && parts[i].qwc != 0)
            memcpy(j->data.data() + j->at[i], reinterpret_cast<const void *>((uintptr_t)parts[i].data),
                   (size_t)parts[i].qwc * 16);
        copy[i].data = parts[i].data != 0 ? i + 1 : 0;
    }
    /* (a draw of an earlier model here decoded itself: not this one) */
    AcquireSRWLockExclusive(&g_preLock);
    g_preSkip->erase(model);
    ReleaseSRWLockExclusive(&g_preLock);
    pre::post(j);
}

void models_set_loaded(const void *const *packets, int n)
{
    if (packets == nullptr || n <= 0)
        return;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    /* the earlier packets' corners as points (vertices within kOverlayNear
     * one point) in cells of 4 kOverlayNear - a point within kOverlayNear of
     * a vertex is in its cell or, on an axis where it is that near the cell's
     * edge, the next one over - and their triangles by their points */
    struct Point {
        float p[3];
        uint32_t next;                  /* the cell's next + 1, 0 none */
    };
    constexpr float kCell = 4.0f * kOverlayNear;
    std::vector<Point> points;
    std::unordered_map<uint64_t, uint32_t> cells;   /* cell -> its first point + 1 */
    std::unordered_map<uint64_t, uint32_t> tris;    /* three points -> the highest layer drawn there */
    auto mix = [](uint64_t a, uint64_t b, uint64_t c) {
        uint64_t h = a * 0x9E3779B97F4A7C15ull;
        h = (h ^ b) * 0xC2B2AE3D27D4EB4Full;
        h = (h ^ c) * 0x165667B19E3779F9ull;
        return h ^ (h >> 29);
    };
    auto cell_of = [&](const float *p, int64_t c[3]) {
        for (int k = 0; k < 3; k++)
            c[k] = (int64_t)std::floor(p[k] / kCell);
    };
    /* the point within kOverlayNear of p (its number + 1), 0 none */
    auto find_point = [&](const float *p) -> uint32_t {
        int64_t c[3];
        int side[3];
        cell_of(p, c);
        for (int k = 0; k < 3; k++) {
            const float in = p[k] - (float)c[k] * kCell;
            side[k] = in < kOverlayNear ? -1 : (in > kCell - kOverlayNear ? 1 : 0);
        }
        for (int corner = 0; corner < 8; corner++) {
            int64_t cc[3];
            bool skip = false;
            for (int k = 0; k < 3; k++) {
                cc[k] = c[k];
                if (corner & (1 << k)) {
                    if (side[k] == 0) {
                        skip = true;
                        break;
                    }
                    cc[k] += side[k];
                }
            }
            if (skip)
                continue;
            auto it = cells.find(mix((uint64_t)cc[0], (uint64_t)cc[1], (uint64_t)cc[2]));
            if (it == cells.end())
                continue;
            for (uint32_t s = it->second; s != 0; s = points[s - 1].next) {
                const Point &e = points[s - 1];
                if (std::fabs(e.p[0] - p[0]) <= kOverlayNear && std::fabs(e.p[1] - p[1]) <= kOverlayNear &&
                    std::fabs(e.p[2] - p[2]) <= kOverlayNear)
                    return s;
            }
        }
        return 0;
    };
    auto add_point = [&](const float *p) -> uint32_t {
        uint32_t s = find_point(p);
        if (s != 0)
            return s;
        int64_t c[3];
        cell_of(p, c);
        uint32_t &head = cells[mix((uint64_t)c[0], (uint64_t)c[1], (uint64_t)c[2])];
        points.push_back({{p[0], p[1], p[2]}, head});
        head = (uint32_t)points.size();
        return head;
    };
    /* a triangle's key (its points in order; 0: a corner without a point or
     * a degenerate triangle) */
    auto tri_key = [&](uint32_t a, uint32_t b, uint32_t c) -> uint64_t {
        if (a == 0 || b == 0 || c == 0 || a == b || b == c || a == c)
            return 0;
        if (a > b) std::swap(a, b);
        if (b > c) std::swap(b, c);
        if (a > b) std::swap(a, b);
        return mix(a, b, c) | 1;
    };
    auto finite = [](const float *p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); };
    std::vector<std::pair<const void *, int>> found;
    int overlays = 0, topLayer = 0;
    char list[256];
    int listLen = 0;
    list[0] = 0;
    std::unordered_set<const void *> done;  /* (a packet listed twice is not its own overlay) */
    std::vector<uint32_t> ids;
    gt3vif::Mesh m;
    for (int i = 0; i < n; i++) {
        const void *model = packets[i];
        if (model == nullptr || !done.insert(model).second)
            continue;
        m.clear();
        uint32_t layer = 0;
        if (gt3vif::decodeModel(model, m, gt3vif::DecodeOptions{}) && m.indices.size() >= 3) {
            ids.assign(m.vertices.size(), 0);
            for (size_t k = 0; k < m.vertices.size(); k++)
                if (finite(m.vertices[k].pos))
                    ids[k] = find_point(m.vertices[k].pos);
            uint32_t matched = 0, total = 0, below = 0;
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
                if (a >= ids.size() || b >= ids.size() || c >= ids.size())
                    continue;
                total++;
                auto it = tris.find(tri_key(ids[a], ids[b], ids[c]));
                if (it != tris.end()) {
                    matched++;
                    below = std::max(below, it->second);
                }
            }
            if (matched != 0 && (float)matched >= kOverlayShare * (float)total) {
                layer = below + 1;
                overlays++;
                topLayer = std::max(topLayer, (int)layer);
                if (listLen < (int)sizeof(list) - 40)
                    listLen += snprintf(list + listLen, sizeof(list) - listLen, " %d (layer %u, %u/%u)", i, layer,
                                        matched, total);
            }
            for (size_t k = 0; k < m.vertices.size(); k++)
                if (ids[k] == 0 && finite(m.vertices[k].pos))
                    ids[k] = add_point(m.vertices[k].pos);
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
                if (a >= ids.size() || b >= ids.size() || c >= ids.size())
                    continue;
                const uint64_t key = tri_key(ids[a], ids[b], ids[c]);
                if (key == 0)
                    continue;
                auto r = tris.emplace(key, layer);
                if (!r.second)
                    r.first->second = std::max(r.first->second, layer);
            }
        }
        found.push_back({model, (int)layer});
    }
    AcquireSRWLockExclusive(&g_overlayLock);
    for (const auto &f : found) {
        if (f.second != 0)
            g_overlay[f.first] = f.second;
        else
            g_overlay.erase(f.first);
    }
    g_overlayGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_overlayLock);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    if (overlays != 0 || ms >= 20.0)
        pclog::write("pgl: model set of %d packets: %d overlays, layers up to %d (%.1f ms):%s%s", n, overlays,
                     topLayer, ms, list, listLen >= (int)sizeof(list) - 40 ? " ..." : "");
}

int model_overlay(const void *model)
{
    /* (the executor's alone: the slots are not shared) */
    const uintptr_t p = (uintptr_t)model;
    OverlaySlot &s = g_overlaySlot[((p >> 4) ^ (p >> 16)) % kOverlaySlots];
    const uint32_t gen = g_overlayGen.load(std::memory_order_acquire);
    if (s.model == model && s.gen == gen)
        return s.layer;
    AcquireSRWLockShared(&g_overlayLock);
    auto it = g_overlay.find(model);
    const int layer = it != g_overlay.end() ? it->second : 0;
    const uint32_t genNow = g_overlayGen.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_overlayLock);
    s.model = model;
    s.gen = genNow;
    s.layer = layer;
    return layer;
}

uint32_t models_overlay_gen()
{
    return g_overlayGen.load(std::memory_order_acquire);
}

bool model_volatile(const void *model)
{
    /* (the executor's alone, after take_written: the writes of this frame
     * are in) */
    if (g_writtenAny.load(std::memory_order_relaxed))
        take_written();
    const uintptr_t p = (uintptr_t)model;
    VolatileSlot &s = g_volatileSlot[((p >> 4) ^ (p >> 16)) % kVolatileSlots];
    if (s.model == model && s.gen == g_volatileGen)
        return s.yes;
    bool yes = false;
    auto it = g_meshes.find(model);
    if (it != g_meshes.end()) {
        /* written in two frames close together, the last of them recent */
        const MeshEntry &e = *it->second;
        yes = e.writes >= 2 && g_frame - e.writtenFrame <= 64;
    }
    s.model = model;
    s.gen = g_volatileGen;
    s.yes = yes;
    return yes;
}

size_t models_cache_bytes()                 /* PC (2026-11-07) */
{
    return g_meshBytes;
}

void models_end_frame()
{
    g_lastModel = nullptr;
    g_lastEntry = nullptr;
    g_frame++;
    if ((g_frame % 64) != 0)
        return;
    /* those unused for kMeshExpire seconds; then, over the budget, the least
     * recently drawn (not within kMeshKeep seconds) */
    const double now = g_execSeconds;
    /* PC: the meshes decoded ahead that no draw took in as long (released
     * after the lock) */
    if (g_preCount.load(std::memory_order_relaxed) != 0) {
        std::vector<std::unique_ptr<PreMesh>> gone;
        AcquireSRWLockExclusive(&g_preLock);
        for (auto it = g_pre->begin(); it != g_pre->end();) {
            if (it->second->made + kMeshExpire < now) {
                gone.push_back(std::move(it->second));
                it = g_pre->erase(it);
            } else {
                ++it;
            }
        }
        g_preCount.store((uint32_t)g_pre->size(), std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_preLock);
    }
    std::vector<std::pair<double, const void *>> old;
    for (auto it = g_meshes.begin(); it != g_meshes.end();) {
        if (it->second->lastUse + kMeshExpire < now) {
            g_meshBytes -= it->second->bytes;
            it = g_meshes.erase(it);
            continue;
        }
        if (it->second->lastUse + kMeshKeep < now)
            old.push_back({it->second->lastUse, it->first});
        ++it;
    }
    if (g_meshBytes <= kMeshBudget)
        return;
    std::sort(old.begin(), old.end());
    for (const auto &o : old) {
        if (g_meshBytes <= kMeshBudget)
            break;
        auto it = g_meshes.find(o.second);
        if (it == g_meshes.end())
            continue;
        g_meshBytes -= it->second->bytes;
        g_meshes.erase(it);
    }
}

}  /* namespace pglr */
