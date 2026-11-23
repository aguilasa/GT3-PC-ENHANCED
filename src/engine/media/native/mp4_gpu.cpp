/* mp4_gpu.cpp - an MP4 movie decoded by the GPU (mp4_gpu.h).
 *
 * Off Windows there is no Media Foundation and no texture shared between
 * devices: open() always fails and an MP4 goes the CPU's way. */
#include "engine/media/native/mp4_gpu.h"

#ifdef _WIN32

#include "engine/media/native/mp4_frames.h"         /* mp4_frame_time */
#include "engine/media/native/mp4_movie.h"          /* MfScope */
#include "engine/pgl/native/pgl_bridge.h"           /* pglr_movie_* */
#include "platform/gpu.h"
#include "platform/log.h"
#include "platform/scheduler.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace gt3media {
namespace {

constexpr int kAhead = 2;                   /* frames ready beyond the player's queue */
constexpr DWORD kVideo = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
constexpr DWORD kCopyWaitMs = 100;          /* (the renderer holds a slot for a copy) */

std::atomic<uint32_t> g_nextSeq{1};
std::atomic<uint32_t> g_keepFrom{1};

template <class T> void release(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

std::wstring widen(const char *s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

/* The texture a sample's picture is in, and its index: null for a sample in
 * system memory (a software decoder). */
ID3D11Texture2D *sample_texture(IMFSample *s, UINT *sub)
{
    IMFMediaBuffer *buf = nullptr;
    IMFDXGIBuffer *dx = nullptr;
    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(s->GetBufferByIndex(0, &buf)) && SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&dx))) &&
        (FAILED(dx->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dx->GetSubresourceIndex(sub))))
        release(tex);
    release(dx);
    release(buf);
    return tex;
}

} // namespace

void mp4_gpu_keep_from(uint32_t seq)
{
    uint32_t cur = g_keepFrom.load(std::memory_order_relaxed);
    while (seq > cur && !g_keepFrom.compare_exchange_weak(cur, seq))
        ;
}

struct Mp4GpuFrames::Impl {
    std::string path;
    int frames = 0;
    bool loop = false;

    std::mutex m;
    std::condition_variable cv;
    struct Frame {
        int index;
        int32_t slot[2];
        uint32_t seq[2];
    };
    std::deque<Frame> ready;
    int produce = 0;                        /* the next frame the thread makes */
    int resetTo = -1;                       /* a jump asked for */
    bool quit = false;
    int opened = 0;                         /* 1 playing, -1 it did not open */
    std::thread thread;

    /* The thread's */
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IMFDXGIDeviceManager *manager = nullptr;
    IMFSourceReader *reader = nullptr;
    struct Slot {
        ID3D11Texture2D *tex = nullptr;
        IDXGIKeyedMutex *mutex = nullptr;
        uint32_t seq = 0;
    } slots[PGLR_MOVIE_SLOTS];
    int w = 0, h = 0, cropX = 0, cropY = 0;
    double aspect = 0.0;

    void run();
    bool make_device();
    bool make_reader();
    bool read_type();
    bool make_slots(ID3D11Texture2D *like);
    IMFSample *read(int64_t *time);
    bool seek(int64_t t);
    int free_slot();
    bool stopping();
    int to_slot(IMFSample *s, uint32_t *seq);
    void close();
};

bool Mp4GpuFrames::Impl::make_device()
{
    ID3D11Device *game = gpu::device();
    if (game == nullptr)
        return false;
    IDXGIDevice *gameDxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    HRESULT hr = game->QueryInterface(IID_PPV_ARGS(&gameDxgi));
    if (SUCCEEDED(hr))
        hr = gameDxgi->GetAdapter(&adapter);
    release(gameDxgi);
    if (FAILED(hr))
        return false;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &dev, nullptr,
                           &ctx);
    if (hr == E_INVALIDARG)                 /* (no 11.1 on this system) */
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx);
    release(adapter);
    if (FAILED(hr)) {
        pclog::write("movie: no video device (0x%08lX)", (unsigned long)hr);
        return false;
    }
    /* (Media Foundation's threads and this one share the context) */
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        release(mt);
    }
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(hr))
        hr = manager->ResetDevice(dev, token);
    return SUCCEEDED(hr);
}

bool Mp4GpuFrames::Impl::make_reader()
{
    IMFAttributes *attr = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 3)))
        return false;
    attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    /* (RGB out of the GPU's video processor) */
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    const HRESULT hr = MFCreateSourceReaderFromURL(widen(path.c_str()).c_str(), attr, &reader);
    release(attr);
    if (FAILED(hr))
        return false;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(reader->SetStreamSelection(kVideo, TRUE)))
        return false;
    IMFMediaType *t = nullptr;
    if (FAILED(MFCreateMediaType(&t)))
        return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    const HRESULT set = reader->SetCurrentMediaType(kVideo, nullptr, t);
    release(t);
    return SUCCEEDED(set) && read_type();
}

/* The picture's size (the display area: an H.264 frame is coded in 16-line
 * rows, 1088 for 1080) and shape. */
bool Mp4GpuFrames::Impl::read_type()
{
    IMFMediaType *cur = nullptr;
    if (FAILED(reader->GetCurrentMediaType(kVideo, &cur)))
        return false;
    UINT32 fw = 0, fh = 0, n = 1, d = 1;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &fw, &fh);
    const double par = SUCCEEDED(MFGetAttributeRatio(cur, MF_MT_PIXEL_ASPECT_RATIO, &n, &d)) && n > 0 && d > 0
                           ? (double)n / d
                           : 1.0;
    w = (int)fw;
    h = (int)fh;
    cropX = cropY = 0;
    MFVideoArea area;
    UINT32 got = 0;
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), &got)) &&
        got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
        cropX = std::max(0, (int)area.OffsetX.value);
        cropY = std::max(0, (int)area.OffsetY.value);
        w = std::min((int)area.Area.cx, (int)fw - cropX);
        h = std::min((int)area.Area.cy, (int)fh - cropY);
    }
    release(cur);
    if (w <= 0 || h <= 0)
        return false;
    aspect = (double)w * par / h;
    return true;
}

/* The shared textures, of the first picture's format, given to the renderer. */
bool Mp4GpuFrames::Impl::make_slots(ID3D11Texture2D *like)
{
    D3D11_TEXTURE2D_DESC d;
    like->GetDesc(&d);
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.CPUAccessFlags = 0;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    void *handles[PGLR_MOVIE_SLOTS];
    for (int i = 0; i < PGLR_MOVIE_SLOTS; i++) {
        Slot &s = slots[i];
        IDXGIResource *res = nullptr;
        HANDLE shared = nullptr;
        HRESULT hr = dev->CreateTexture2D(&d, nullptr, &s.tex);
        if (SUCCEEDED(hr))
            hr = s.tex->QueryInterface(IID_PPV_ARGS(&s.mutex));
        if (SUCCEEDED(hr))
            hr = s.tex->QueryInterface(IID_PPV_ARGS(&res));
        if (SUCCEEDED(hr))
            hr = res->GetSharedHandle(&shared);
        release(res);
        if (FAILED(hr)) {
            pclog::write("movie: no shared %d x %d texture (0x%08lX)", w, h, (unsigned long)hr);
            return false;
        }
        handles[i] = shared;
    }
    pglr_movie_attach(handles, PGLR_MOVIE_SLOTS, w, h);
    return true;
}

/* The next picture and its time (100 ns); null at the end. */
IMFSample *Mp4GpuFrames::Impl::read(int64_t *time)
{
    for (;;) {
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = nullptr;
        const HRESULT hr = reader->ReadSample(kVideo, 0, &index, &flags, &ts, &s);
        if (FAILED(hr)) {
            pclog::write("movie: a video frame does not decode (0x%08lX)", (unsigned long)hr);
            return nullptr;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            read_type();
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release(s);
            return nullptr;
        }
        if (s != nullptr) {
            *time = ts;
            return s;
        }
    }
}

bool Mp4GpuFrames::Impl::seek(int64_t t)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = t;
    const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, v);
    PropVariantClear(&v);
    return SUCCEEDED(hr);
}

bool Mp4GpuFrames::Impl::stopping()
{
    std::lock_guard<std::mutex> l(m);
    return quit || resetTo >= 0;
}

/* A slot no frame of the game needs, waited for (-1: a stop or a jump). */
int Mp4GpuFrames::Impl::free_slot()
{
    for (;;) {
        const uint32_t keep = g_keepFrom.load(std::memory_order_relaxed);
        for (int i = 0; i < PGLR_MOVIE_SLOTS; i++)
            if (slots[i].seq < keep && !pglr_movie_busy(i))
                return i;
        std::unique_lock<std::mutex> l(m);
        if (quit || resetTo >= 0)
            return -1;
        cv.wait_for(l, std::chrono::milliseconds(1));
    }
}

/* The picture into a free slot: the slot, *seq its number; -1 without one. */
int Mp4GpuFrames::Impl::to_slot(IMFSample *s, uint32_t *seq)
{
    const int i = free_slot();
    if (i < 0)
        return -1;
    UINT sub = 0;
    ID3D11Texture2D *src = sample_texture(s, &sub);
    if (src == nullptr)
        return -1;
    Slot &slot = slots[i];
    pglr_movie_stamp(i, 0);                 /* (what it held is gone) */
    /* (the area shown: a WAIT_TIMEOUT is no failure to the HRESULT tests) */
    const HRESULT hr = slot.mutex->AcquireSync(0, kCopyWaitMs);
    if (hr == S_OK) {
        const D3D11_BOX box = {(UINT)cropX, (UINT)cropY, 0, (UINT)(cropX + w), (UINT)(cropY + h), 1};
        ctx->CopySubresourceRegion(slot.tex, 0, 0, 0, 0, src, sub, &box);
        slot.mutex->ReleaseSync(0);
    }
    release(src);
    if (hr != S_OK)
        return -1;
    slot.seq = *seq = g_nextSeq.fetch_add(1, std::memory_order_relaxed);
    pglr_movie_stamp(i, slot.seq);
    return i;
}

void Mp4GpuFrames::Impl::close()
{
    release(reader);
    pglr_movie_attach(nullptr, 0, 0, 0);
    for (Slot &s : slots) {
        release(s.mutex);
        release(s.tex);
        s.seq = 0;
    }
    if (ctx != nullptr)
        ctx->ClearState();
    release(manager);
    release(ctx);
    release(dev);
}

void Mp4GpuFrames::Impl::run()
{
    MfScope mf;
    IMFSample *cur = nullptr, *nxt = nullptr;
    int64_t curT = 0, nxtT = 0;
    int curSlot = -1;
    uint32_t curSeq = 0;
    bool ended = false;
    /* the movie opened, and its first picture really a texture */
    bool ok = mf.ok() && make_device() && make_reader();
    if (ok) {
        nxt = read(&nxtT);
        UINT sub = 0;
        ID3D11Texture2D *first = nxt != nullptr ? sample_texture(nxt, &sub) : nullptr;
        ok = first != nullptr && make_slots(first);
        release(first);
    }
    {
        std::lock_guard<std::mutex> l(m);
        opened = ok ? 1 : -1;
    }
    cv.notify_all();
    int n = 0;
    while (ok) {
        {
            std::unique_lock<std::mutex> l(m);
            cv.wait(l, [&] { return quit || resetTo >= 0 || ((int)ready.size() < kAhead && (n < frames || loop)); });
            if (quit)
                break;
            bool restart = false;
            if (resetTo >= 0) {
                n = resetTo;
                resetTo = -1;
                ready.clear();
                restart = true;
                /* (the frames dropped never show: their slots are free) */
                mp4_gpu_keep_from(g_nextSeq.load(std::memory_order_relaxed));
            }
            if (n >= frames) {              /* (loop) the start again */
                n = 0;
                restart = true;
            }
            if (restart) {
                seek(mp4_frame_time(n));
                release(cur);
                release(nxt);
                curSlot = -1;
                ended = false;
            }
            produce = n;
        }
        Frame f;
        f.index = n;
        bool stopped = false;
        for (int half = 0; half < 2; half++) {
            /* the picture shown at this half's time (frame n's, then half a
             * frame later): the last at or before it (0.5 ms of rounding) */
            const int64_t t = mp4_frame_time(2 * (int64_t)n + half) / 2 + 5000;
            while (!ended) {
                if (nxt == nullptr && (nxt = read(&nxtT)) == nullptr) {
                    ended = true;
                    break;
                }
                if (cur == nullptr || nxtT <= t) {  /* (the first picture shows from frame 0) */
                    release(cur);
                    cur = nxt;
                    curT = nxtT;
                    nxt = nullptr;
                    curSlot = -1;
                    if (curT <= t)
                        continue;
                }
                break;
            }
            /* the picture copied once (a slower movie shows it again) */
            if (cur != nullptr && curSlot < 0 && (curSlot = to_slot(cur, &curSeq)) < 0 && stopping()) {
                stopped = true;
                break;
            }
            f.slot[half] = curSlot;
            f.seq[half] = curSeq;
        }
        if (stopped)
            continue;
        {
            std::lock_guard<std::mutex> l(m);
            if (resetTo < 0)
                ready.push_back(f);
            n++;
            produce = n;
        }
        cv.notify_all();
    }
    release(cur);
    release(nxt);
    close();
}

Mp4GpuFrames::Mp4GpuFrames() : impl_(new Impl)
{
}

std::unique_ptr<Mp4GpuFrames> Mp4GpuFrames::open(const std::string &path, int frames, bool loop)
{
    std::unique_ptr<Mp4GpuFrames> self(new Mp4GpuFrames);
    self->path_ = path;
    Impl &i = *self->impl_;
    i.path = path;
    i.frames = std::max(1, frames);
    i.loop = loop;
    i.thread = std::thread([&i] { i.run(); });
    /* (only this thread waits: the game's others run) */
    sched::run_outside(
        [](void *arg) {
            Impl &i = *static_cast<Impl *>(arg);
            std::unique_lock<std::mutex> l(i.m);
            i.cv.wait(l, [&i] { return i.opened != 0; });
        },
        &i);
    if (i.opened < 0) {
        i.thread.join();
        pclog::write("movie: %s: the GPU does not decode it: on the CPU, at the slot's size", path.c_str());
        return nullptr;
    }
    pclog::write("movie: %s: %d x %d pictures (shape %.3f), decoded by the GPU, drawn at their own size", path.c_str(),
                 i.w, i.h, i.aspect);
    return self;
}

Mp4GpuFrames::~Mp4GpuFrames()
{
    {
        std::lock_guard<std::mutex> l(impl_->m);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    /* (some 70 ms: the device goes.  Only this thread waits) */
    sched::run_outside(
        [](void *arg) {
            Impl &i = *static_cast<Impl *>(arg);
            if (i.thread.joinable())
                i.thread.join();
        },
        impl_.get());
}

int Mp4GpuFrames::width() const
{
    return impl_->w;
}

int Mp4GpuFrames::height() const
{
    return impl_->h;
}

double Mp4GpuFrames::aspect() const
{
    return impl_->aspect;
}

bool Mp4GpuFrames::take(int index, Mp4GpuFrame *out)
{
    Impl &i = *impl_;
    std::unique_lock<std::mutex> l(i.m);
    auto it = std::find_if(i.ready.begin(), i.ready.end(), [index](const Impl::Frame &f) { return f.index == index; });
    if (it != i.ready.end()) {
        std::memcpy(out->magic, kMp4GpuMagic, sizeof(out->magic));
        for (int half = 0; half < 2; half++) {
            out->slot[half] = it->slot[half];
            out->seq[half] = it->seq[half];
        }
        i.ready.erase(i.ready.begin(), it + 1);
        l.unlock();
        i.cv.notify_all();
        return true;
    }
    /* not there and not next: decode from there */
    if (i.resetTo != index && !(i.ready.empty() && i.produce == index)) {
        i.resetTo = index;
        l.unlock();
        i.cv.notify_all();
    }
    return false;
}

} // namespace gt3media

#else /* !_WIN32 */

namespace gt3media {

struct Mp4GpuFrames::Impl {
};

void mp4_gpu_keep_from(uint32_t)
{
}

Mp4GpuFrames::Mp4GpuFrames() : impl_(new Impl)
{
}

std::unique_ptr<Mp4GpuFrames> Mp4GpuFrames::open(const std::string &, int, bool)
{
    return nullptr;
}

Mp4GpuFrames::~Mp4GpuFrames()
{
}

int Mp4GpuFrames::width() const
{
    return 0;
}

int Mp4GpuFrames::height() const
{
    return 0;
}

double Mp4GpuFrames::aspect() const
{
    return 0.0;
}

bool Mp4GpuFrames::take(int, Mp4GpuFrame *)
{
    return false;
}

} // namespace gt3media

#endif /* _WIN32 */
