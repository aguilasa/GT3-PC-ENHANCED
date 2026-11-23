/* gpu.cpp - the Direct3D 11 device of GT3 PC (gpu.h). */
#include "platform/gpu.h"
#include "platform/frame_monitor.h"
#include "platform/frame_pacing.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/window.h"

#include <d3dcompiler.h>
#include <dxgi1_5.h>
#ifndef _WIN32
#include "engine/pgl/native/shader_blobs.h"   /* (no D3DCompile: the shaders compiled offline) */
#endif

#include <algorithm>
#include <cmath>
#include <vector>

namespace gpu {
namespace {

/* (No frame latency waitable object: D3D11 refuses exclusive fullscreen with
 * one.)  ALLOW_TEARING when the system has it (init): a windowed or
 * borderless flip chain can then present without vsync, on a variable
 * refresh display at once (the same flags for ResizeBuffers). */
UINT g_swapFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
bool g_tearing;                     /* the swap chain has ALLOW_TEARING */

ID3D11Device *g_device;
ID3D11DeviceContext *g_context;
IDXGISwapChain1 *g_swap;
ID3D11RenderTargetView *g_rtv;
int g_bbW, g_bbH;
HWND g_hwnd;
int g_presentInterval;              /* present(): 0 (no vsync, the default), or 1 */
int g_latency = 1;                  /* the frames queued at most (SetMaximumFrameLatency) */
bool g_flip;                        /* a flip model swap chain (its buffer count follows the latency) */
int g_vsyncApplied = 0;             /* settings::vsync() as applied (apply_vsync) */
bool g_lastExclusive;               /* the last present was in exclusive fullscreen */
double g_lastPresent;               /* sched::now_seconds() when the last Present returned */
double g_presentWait;               /* seconds spent in Present so far */
double g_fullscreenHz;              /* the exclusive fullscreen mode's rate */
int g_fullscreenW, g_fullscreenH;   /* its size: the back buffer's while in it */
/* The display mode applied (window::mode() once a request is taken). */
settings::DisplayMode g_mode = settings::DisplayMode::windowed;
IDXGIFactory2 *g_factory;           /* (kept for the extra screens' swap chains) */
IDXGIAdapter *g_adapter;

/* Multi-monitor (gpu.h "Screens"): the extra screens, each a borderless
 * window over its monitor with a swap chain of its own.  Borderless only:
 * exclusive fullscreen on several monitors at once lagged badly (user
 * test), so while extra screens are in use the game window leaves it too
 * (suspend_fullscreen) and takes it again after. */
struct Screen {
    HMONITOR monitor = nullptr;
    RECT rect = {};                 /* the monitor's desktop rectangle */
    HWND hwnd = nullptr;
    IDXGISwapChain1 *swap = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    int bbW = 0, bbH = 0;
};
std::vector<Screen> g_extra;
bool g_span;                        /* the extra screens are the span's */
bool g_fsSuspended;                 /* fullscreen mode, borderless while extra screens are in use */
/* The back buffers are made again at the next frame whatever their size: a
 * flip model swap chain presents nothing after a fullscreen transition
 * until ResizeBuffers (leaving exclusive fullscreen at the same size left
 * the game window's picture blank: user report, the split screen's player
 * 1 lost). */
bool g_forceResize;
ULONGLONG g_splitWanted;            /* GetTickCount64 of the last want_split_screen */
/* How long the split screen's second monitor stays after the last frame
 * that asked for it (a load between the race and its replay keeps it, and
 * the game window its borderless). */
constexpr ULONGLONG kSplitKeepMs = 4000;
ULONGLONG g_openFailed;             /* an extra screen could not be made (retried after 3 s) */
/* The canvas: the screens left to right (0 the game window's), each's x and
 * width in it at the game window's height. */
std::vector<int> g_order;
std::vector<int> g_canvasX, g_canvasW;
int g_canvasTotal;

void release_backbuffer()
{
    if (g_rtv) {
        g_context->OMSetRenderTargets(0, nullptr, nullptr);
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool make_backbuffer()
{
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)))
        return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    g_bbW = (int)d.Width;
    g_bbH = (int)d.Height;
    HRESULT hr = g_device->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

void resize(int w, int h)
{
    release_backbuffer();
    g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, g_swapFlags);
    make_backbuffer();
}

/* The vertical sync setting (settings::vsync, the settings menu; docs/
 * renderer.md "Frame rate"): off, Present(0) (tearing allowed), one frame
 * queued; N, Present(1) with at most N frames queued (SetMaximumFrameLatency)
 * and N + 1 buffers in a flip model swap chain.  Applied before a frame's
 * back buffer is taken. */
void apply_vsync()
{
    const int n = settings::vsync();
    if (n == g_vsyncApplied && g_swap != nullptr)
        return;
    g_vsyncApplied = n;
    g_presentInterval = n > 0 ? 1 : 0;
    const int latency = n > 0 ? n : 1;
    if (latency != g_latency) {
        IDXGIDevice1 *d1 = nullptr;
        if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice1), (void **)&d1))) {
            d1->SetMaximumFrameLatency((UINT)latency);
            d1->Release();
        }
        if (g_flip) {
            release_backbuffer();
            g_swap->ResizeBuffers((UINT)(latency + 1), 0, 0, DXGI_FORMAT_UNKNOWN, g_swapFlags);
            make_backbuffer();
        }
        g_latency = latency;
    }
    if (n > 0)
        pclog::write("gpu: vsync, %d frame(s) queued at most", n);
    else
        pclog::write("gpu: no vsync%s", g_tearing ? ", tearing allowed" : "");
}

/* Exclusive fullscreen on the window's monitor, in the display mode of the
 * settings (the OS's mode nearest to it; the desktop's size when the
 * settings give none, 60 Hz when they give no rate).  false: left as it was
 * (logged). */
bool enter_fullscreen()
{
    IDXGIOutput *out = nullptr;
    if (FAILED(g_swap->GetContainingOutput(&out))) {
        pclog::write("gpu: no monitor for fullscreen");
        return false;
    }
    DXGI_OUTPUT_DESC od;
    out->GetDesc(&od);
    const settings::Display &s = settings::display();
    DXGI_MODE_DESC want = {};
    want.Width = (UINT)(s.width > 0 ? s.width : od.DesktopCoordinates.right - od.DesktopCoordinates.left);
    want.Height = (UINT)(s.height > 0 ? s.height : od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    want.RefreshRate.Numerator = (UINT)(s.refresh > 0 ? s.refresh : 60);
    want.RefreshRate.Denominator = 1;
    want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_MODE_DESC got;
    if (FAILED(out->FindClosestMatchingMode(&want, &got, g_device)))
        got = want;
    /* the window and the back buffers at the mode's size first: DXGI takes
     * the full screen's display mode from them, and with the windowed
     * buffers still there (Alt+Enter back from a 1324x993 window) it took
     * the mode nearest to those, 1280x1024: a 5:4 picture stretched over a
     * 16:9 screen (user, 2026-10-20) */
    g_swap->ResizeTarget(&got);
    resize((int)got.Width, (int)got.Height);
    HRESULT hr = g_swap->SetFullscreenState(TRUE, out);
    if (FAILED(hr)) {
        out->Release();
        pclog::write("gpu: fullscreen %ux%u refused (0x%08lX)", want.Width, want.Height, (unsigned long)hr);
        return false;
    }
    /* the mode asked again, now in full screen (this sets the display
     * mode), and the buffers at its size */
    g_swap->ResizeTarget(&got);
    resize((int)got.Width, (int)got.Height);
    g_fullscreenW = (int)got.Width;
    g_fullscreenH = (int)got.Height;
    g_fullscreenHz =
        got.RefreshRate.Denominator ? (double)got.RefreshRate.Numerator / got.RefreshRate.Denominator : 0.0;
    /* what the display runs (logged: a different size is a stretched
     * picture) */
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    char dev[64];
    WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, dev, sizeof(dev), nullptr, nullptr);
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
        pclog::write("gpu: fullscreen on %s: %ux%u, %.2f Hz (the display runs %lux%lu, %lu Hz)%s", dev, got.Width,
                     got.Height, g_fullscreenHz, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                     dm.dmPelsWidth != got.Width || dm.dmPelsHeight != got.Height ? ": NOT THE MODE ASKED" : "");
    else
        pclog::write("gpu: fullscreen on %s: %ux%u, %.2f Hz", dev, got.Width, got.Height, g_fullscreenHz);
    out->Release();
    return true;
}

/* ======================================================================== */
/* Extra screens                                                            */
/* ======================================================================== */

struct MonitorEntry {
    HMONITOR monitor;
    RECT rect;
    wchar_t device[32];
};

BOOL CALLBACK enum_monitor(HMONITOR m, HDC, LPRECT, LPARAM lp)
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, &mi)) {
        MonitorEntry e = {m, mi.rcMonitor, {}};
        wcsncpy_s(e.device, mi.szDevice, _TRUNCATE);
        ((std::vector<MonitorEntry> *)lp)->push_back(e);
    }
    return TRUE;
}

/* The monitors (enumerated again each second: update_screens runs every
 * frame). */
std::vector<MonitorEntry> monitors()
{
    static std::vector<MonitorEntry> s_all;
    static ULONGLONG s_when;
    const ULONGLONG now = GetTickCount64();
    if (s_when == 0 || now - s_when >= 1000) {
        s_all.clear();
        EnumDisplayMonitors(nullptr, nullptr, enum_monitor, (LPARAM)&s_all);
        s_when = now;
    }
    return s_all;
}

/* The monitor of a device name (\\.\DISPLAYn, as gt3pc.ini gives it). */
const MonitorEntry *find_monitor(const std::vector<MonitorEntry> &all, const char *name)
{
    wchar_t w[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64);
    for (const MonitorEntry &e : all)
        if (_wcsicmp(e.device, w) == 0)
            return &e;
    return nullptr;
}

/* The span's monitors (gt3pc.ini SpanMonitors, those connected), left to
 * right; fewer than 2: no span. */
std::vector<MonitorEntry> span_monitors(const std::vector<MonitorEntry> &all)
{
    std::vector<MonitorEntry> v;
    const char *p = settings::display().spanMonitors;
    while (*p != 0) {
        char name[64];
        int n = 0;
        while (*p == ' ' || *p == ',')
            p++;
        while (*p != 0 && *p != ',' && n < 63)
            name[n++] = *p++;
        while (n > 0 && name[n - 1] == ' ')
            n--;
        name[n] = 0;
        const MonitorEntry *e = n > 0 ? find_monitor(all, name) : nullptr;
        if (e != nullptr && std::none_of(v.begin(), v.end(), [&](const MonitorEntry &x) { return x.monitor == e->monitor; }))
            v.push_back(*e);
    }
    std::sort(v.begin(), v.end(), [](const MonitorEntry &a, const MonitorEntry &b) { return a.rect.left < b.rect.left; });
    if (v.size() < 2)
        v.clear();
    return v;
}

/* The span's monitor for the game window: its own when it is one of them,
 * else the middle one (the left of the middle two). */
HMONITOR span_main(const std::vector<MonitorEntry> &span)
{
    const HMONITOR own = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    for (const MonitorEntry &e : span)
        if (e.monitor == own)
            return own;
    return span[(span.size() - 1) / 2].monitor;
}

/* Before the game window takes a full-screen mode: on the span's monitor
 * (window::set_full_monitor), or its own.  true: a span's. */
bool place_for_span(settings::DisplayMode m)
{
    const std::vector<MonitorEntry> span = span_monitors(monitors());
    const bool on = m != settings::DisplayMode::windowed && !span.empty();
    window::set_full_monitor(on ? (void *)span_main(span) : nullptr);
    return on;
}

void release_screen_rtv(Screen &s)
{
    if (s.rtv) {
        g_context->OMSetRenderTargets(0, nullptr, nullptr);
        s.rtv->Release();
        s.rtv = nullptr;
    }
}

bool screen_rtv(Screen &s)
{
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(s.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)))
        return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    s.bbW = (int)d.Width;
    s.bbH = (int)d.Height;
    HRESULT hr = g_device->CreateRenderTargetView(bb, nullptr, &s.rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

bool open_screen(const MonitorEntry &m)
{
    Screen s;
    s.monitor = m.monitor;
    s.rect = m.rect;
    const int w = (int)(m.rect.right - m.rect.left), h = (int)(m.rect.bottom - m.rect.top);
    s.hwnd = window::create_screen((int)m.rect.left, (int)m.rect.top, w, h);
    if (s.hwnd == nullptr) {
        pclog::write("gpu: no window for the screen at %ld,%ld", m.rect.left, m.rect.top);
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = (UINT)w;
    sd.Height = (UINT)h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = g_flip ? (UINT)(g_latency + 1) : 1;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = g_flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_swapFlags;
    HRESULT hr = g_factory->CreateSwapChainForHwnd(g_device, s.hwnd, &sd, nullptr, nullptr, &s.swap);
    if (FAILED(hr) || !screen_rtv(s)) {
        pclog::write("gpu: no swap chain for the screen at %ld,%ld (0x%08lX)", m.rect.left, m.rect.top,
                     (unsigned long)hr);
        if (s.swap)
            s.swap->Release();
        window::destroy_screen(s.hwnd);
        return false;
    }
    g_factory->MakeWindowAssociation(s.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    char dev[32];
    WideCharToMultiByte(CP_UTF8, 0, m.device, -1, dev, sizeof(dev), nullptr, nullptr);
    pclog::write("gpu: screen on %s (%dx%d at %ld,%ld), borderless", dev, w, h, m.rect.left, m.rect.top);
    g_extra.push_back(s);
    return true;
}

void close_screen(size_t i)
{
    Screen &s = g_extra[i];
    release_screen_rtv(s);
    if (s.swap)
        s.swap->Release();
    window::destroy_screen(s.hwnd);
    pclog::write("gpu: screen at %ld,%ld closed", s.rect.left, s.rect.top);
    g_extra.erase(g_extra.begin() + (ptrdiff_t)i);
}

/* The canvas (gpu.h): the screens left to right - the span's by their
 * desktop places, the split screen's the game window's first - each as wide
 * as its picture at the game window's height. */
void canvas_layout()
{
    const int n = 1 + (int)g_extra.size();
    g_order.resize((size_t)n);
    for (int i = 0; i < n; i++)
        g_order[(size_t)i] = i;
    if (g_span) {
        RECT own = {};
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            own = mi.rcMonitor;
        std::sort(g_order.begin(), g_order.end(), [&](int a, int b) {
            const long la = a == 0 ? own.left : g_extra[(size_t)a - 1].rect.left;
            const long lb = b == 0 ? own.left : g_extra[(size_t)b - 1].rect.left;
            return la < lb;
        });
    }
    g_canvasX.assign((size_t)n, 0);
    g_canvasW.assign((size_t)n, 0);
    const int h0 = std::max(1, g_bbH);
    int x = 0;
    for (int k = 0; k < n; k++) {
        const int i = g_order[(size_t)k];
        int w = g_bbW, h = g_bbH;
        if (i > 0) {
            w = g_extra[(size_t)i - 1].bbW;
            h = g_extra[(size_t)i - 1].bbH;
        }
        const int cw = std::max(1, (int)std::lround((double)w * h0 / std::max(1, h)));
        g_canvasX[(size_t)i] = x;
        g_canvasW[(size_t)i] = cw;
        x += cw;
    }
    g_canvasTotal = x;
}

/* The extra screens the frame wants (before its back buffers are taken):
 * the span's in a full-screen mode, else the split screen's second monitor
 * while the game asks for it (want_split_screen; kept kSplitKeepMs after). */
void wanted_screens(std::vector<MonitorEntry> &want, bool &span)
{
    const std::vector<MonitorEntry> all = monitors();
    want.clear();
    span = false;
    const HMONITOR own = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    if (g_mode != settings::DisplayMode::windowed) {
        const std::vector<MonitorEntry> s = span_monitors(all);
        if (std::any_of(s.begin(), s.end(), [&](const MonitorEntry &e) { return e.monitor == own; })) {
            span = true;
            for (const MonitorEntry &e : s)
                if (e.monitor != own)
                    want.push_back(e);
        }
    }
    if (!span && g_splitWanted != 0 && GetTickCount64() - g_splitWanted < kSplitKeepMs && all.size() >= 2) {
        const MonitorEntry *m = nullptr;
        if (settings::display().splitMonitor[0] != 0)
            m = find_monitor(all, settings::display().splitMonitor);
        if (m == nullptr || m->monitor == own) {
            /* auto: the nearest other monitor (centre to centre) */
            RECT o = {};
            for (const MonitorEntry &e : all)
                if (e.monitor == own)
                    o = e.rect;
            double best = 1e30;
            m = nullptr;
            for (const MonitorEntry &e : all) {
                if (e.monitor == own)
                    continue;
                const double dx = (e.rect.left + e.rect.right - o.left - o.right) * 0.5;
                const double dy = (e.rect.top + e.rect.bottom - o.top - o.bottom) * 0.5;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    m = &e;
                }
            }
        }
        if (m != nullptr)
            want.push_back(*m);
    }
}

/* The extra screens made what the frame wants (want): those no longer
 * wanted closed, the new ones opened (a failure retried after 3 s); the
 * canvas laid out. */
void update_screens(const std::vector<MonitorEntry> &want, bool span)
{
    for (size_t i = g_extra.size(); i-- > 0;) {
        const Screen &s = g_extra[i];
        if (std::none_of(want.begin(), want.end(), [&](const MonitorEntry &e) { return e.monitor == s.monitor; }))
            close_screen(i);
    }
    if (GetTickCount64() - g_openFailed >= 3000) {
        for (const MonitorEntry &e : want) {
            if (std::any_of(g_extra.begin(), g_extra.end(), [&](const Screen &s) { return s.monitor == e.monitor; }))
                continue;
            if (!open_screen(e))
                g_openFailed = GetTickCount64();
        }
    }
    g_span = span && !g_extra.empty();
    canvas_layout();
}

/* The fullscreen mode while extra screens are in use: the game window leaves
 * exclusive fullscreen for borderless (its monitor's rectangle), and takes
 * it again once they are gone. */
void suspend_fullscreen(bool on)
{
    if (on == g_fsSuspended)
        return;
    g_fsSuspended = on;
    if (on) {
        g_swap->SetFullscreenState(FALSE, nullptr);
        window::apply_mode(settings::DisplayMode::borderless);
        g_forceResize = true;
        pclog::write("gpu: exclusive fullscreen suspended: borderless for now");
    } else {
        window::apply_mode(settings::DisplayMode::fullscreen);
        if (enter_fullscreen())
            pclog::write("gpu: exclusive fullscreen again");
    }
}

/* A display mode change (Alt+Enter), on the game thread: the swap chain
 * leaves exclusive fullscreen first, the window takes its style and place,
 * then fullscreen is entered (borderless if that fails). */
void set_mode(settings::DisplayMode m)
{
    if (m == g_mode)
        return;
    g_fsSuspended = false;                  /* (taken again by the next frame if still wanted) */
    if (g_mode == settings::DisplayMode::fullscreen) {
        g_swap->SetFullscreenState(FALSE, nullptr);
        g_forceResize = true;
    }
    place_for_span(m);
    window::apply_mode(m);
    g_mode = m;
    if (m == settings::DisplayMode::fullscreen && !enter_fullscreen()) {
        window::apply_mode(settings::DisplayMode::borderless);
        g_mode = settings::DisplayMode::borderless;
    }
}

} // namespace

bool init(HWND window)
{
    g_hwnd = window;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef GT3PC_D3D_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4,
                                   D3D11_SDK_VERSION, &g_device, &got, &g_context);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 3,
                               D3D11_SDK_VERSION, &g_device, &got, &g_context);
    if (FAILED(hr)) {
        pclog::write("gpu: D3D11CreateDevice failed (0x%08lX)", (unsigned long)hr);
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    g_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    pclog::write("gpu: %s, feature level %X", name, (unsigned)got);

    /* presenting without vsync in a window: the tearing support (Windows 10
     * and later, with the driver's; a variable refresh display then follows
     * the frames) */
    IDXGIFactory5 *factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void **)&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
            allow) {
            g_tearing = true;
            g_swapFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
        factory5->Release();
    }

    int w, h;
    window::client_size(&w, &h);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w > 0 ? w : 1;
    sd.Height = h > 0 ? h : 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_swapFlags;
    hr = factory->CreateSwapChainForHwnd(g_device, window, &sd, nullptr, nullptr, &g_swap);
    if (FAILED(hr)) {
        g_tearing = false;                      /* (a flip model flag) */
        g_swapFlags &= ~(UINT)DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        sd.Flags = g_swapFlags;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = factory->CreateSwapChainForHwnd(g_device, window, &sd, nullptr, nullptr, &g_swap);
    }
    if (FAILED(hr)) {
        pclog::write("gpu: CreateSwapChainForHwnd failed (0x%08lX)", (unsigned long)hr);
        return false;
    }
    g_flip = sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    pclog::write(g_tearing ? "gpu: tearing allowed without vsync (a variable refresh display follows the frames)"
                           : "gpu: tearing not supported: windowed frames without vsync wait for the compositor");
    /* Alt+Enter is the window's (window.h): DXGI's own would skip the
     * borderless mode */
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    /* one frame queued at most: Present waits for the GPU (or the display,
     * with vsync) instead of letting the frames run ahead */
    IDXGIDevice1 *dxgiDevice1 = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dxgiDevice1))) {
        dxgiDevice1->SetMaximumFrameLatency(1);
        dxgiDevice1->Release();
    }
    g_factory = factory;                    /* (kept: the extra screens) */
    g_adapter = adapter;
    dxgiDevice->Release();
    if (!make_backbuffer())
        return false;
    g_vsyncApplied = -1;
    apply_vsync();                          /* the setting's interval, latency and buffers */
    g_mode = window::mode();
    /* the span: the game window on its monitor of it */
    if (place_for_span(g_mode)) {
        /* (the span's fullscreen mode is borderless: suspend_fullscreen) */
        g_fsSuspended = g_mode == settings::DisplayMode::fullscreen;
        window::apply_mode(g_fsSuspended ? settings::DisplayMode::borderless : g_mode);
    }
    if (g_mode == settings::DisplayMode::fullscreen && !g_fsSuspended && !enter_fullscreen()) {
        window::apply_mode(settings::DisplayMode::borderless);
        g_mode = settings::DisplayMode::borderless;
    }
    return true;
}

ID3D11Device *device() { return g_device; }
IDXGIAdapter *adapter() { return g_adapter; }   /* PC (2026-11-07) */
/* (set_context_guard) */
static void (*g_contextGuard)();

void set_context_guard(void (*guard)())
{
    g_contextGuard = guard;
}

static void guard_context()
{
    if (g_contextGuard != nullptr)
        g_contextGuard();
}

ID3D11DeviceContext *context()
{
    guard_context();
    return g_context;
}

ID3D11RenderTargetView *backbuffer()
{
    guard_context();
    apply_vsync();                          /* (a change of the setting) */
    settings::DisplayMode req;
    if (window::take_mode_request(&req))
        set_mode(req);
    /* the extra screens this frame wants (the span's, the split screen's):
     * while there are any, the fullscreen mode borderless */
    std::vector<MonitorEntry> want;
    bool span = false;
    wanted_screens(want, span);
    suspend_fullscreen(g_mode == settings::DisplayMode::fullscreen && !want.empty());
    /* exclusive fullscreen ends when the window loses the focus (Alt+Tab):
     * back once it is active again */
    BOOL fs = FALSE;
    if (g_mode == settings::DisplayMode::fullscreen && !g_fsSuspended) {
        g_swap->GetFullscreenState(&fs, nullptr);
        static ULONGLONG lastTry = 0;
        if (!fs && window::active() && GetTickCount64() - lastTry >= 1000) {
            lastTry = GetTickCount64();
            fs = enter_fullscreen();
        }
    }
    /* the back buffer: the display mode's size in exclusive fullscreen (the
     * window's size can lag behind a mode change: the window thread's
     * WM_SIZE), else the window's */
    int w, h;
    if (fs && g_fullscreenW > 0 && g_fullscreenH > 0) {
        w = g_fullscreenW;
        h = g_fullscreenH;
    } else {
        window::client_size(&w, &h);
    }
    if (w > 0 && h > 0 && (g_forceResize || w != g_bbW || h != g_bbH)) {
        resize(w, h);
        g_forceResize = false;
    }
    update_screens(want, span);             /* (and the canvas) */
    return g_rtv;
}

void backbuffer_size(int *w, int *h)
{
    *w = g_bbW;
    *h = g_bbH;
}

void leave_fullscreen()
{
    guard_context();
    if (g_mode == settings::DisplayMode::fullscreen)
        suspend_fullscreen(true);
}

int screen_count()
{
    return 1 + (int)g_extra.size();
}

ID3D11RenderTargetView *screen_backbuffer(int i)
{
    guard_context();
    if (i <= 0)
        return g_rtv;
    return i - 1 < (int)g_extra.size() ? g_extra[(size_t)i - 1].rtv : nullptr;
}

void screen_size(int i, int *w, int *h)
{
    *w = g_bbW;
    *h = g_bbH;
    if (i > 0 && i - 1 < (int)g_extra.size()) {
        *w = g_extra[(size_t)i - 1].bbW;
        *h = g_extra[(size_t)i - 1].bbH;
    }
}

void canvas_size(int *w, int *h)
{
    *w = g_canvasTotal > 0 ? g_canvasTotal : g_bbW;
    *h = g_bbH;
}

void canvas_rect(int i, int *x, int *w)
{
    *x = 0;
    *w = g_bbW;
    if (i >= 0 && i < (int)g_canvasX.size()) {
        *x = g_canvasX[(size_t)i];
        *w = g_canvasW[(size_t)i];
    }
}

bool span_active()
{
    return g_span;
}

void want_split_screen()
{
    g_splitWanted = GetTickCount64();
}

/* A failed Present logged (once per code and swap chain kind; the back
 * buffers made again at the next frame). */
static void present_result(HRESULT hr, const char *what)
{
    if (SUCCEEDED(hr))
        return;
    g_forceResize = true;
    static char s_seen[8][48];
    char key[48];
    snprintf(key, sizeof(key), "%s %08lX", what, (unsigned long)hr);
    for (auto &k : s_seen) {
        if (strcmp(k, key) == 0)
            return;
        if (k[0] == 0) {
            strcpy(k, key);
            break;
        }
    }
    pclog::write("gpu: present to %s failed (0x%08lX)", what, (unsigned long)hr);
}

static void present_call(void *)
{
    /* Without vsync: ALLOW_TEARING in a window (not valid in exclusive
     * fullscreen, which presents at once by itself) */
    BOOL fs = FALSE;
    if (g_mode == settings::DisplayMode::fullscreen)
        g_swap->GetFullscreenState(&fs, nullptr);
    g_lastExclusive = fs != FALSE;
    /* the extra screens first (the game window's present is the one that
     * waits: vsync, the frame queue) */
    for (Screen &s : g_extra)
        present_result(s.swap->Present((UINT)g_presentInterval,
                                       g_presentInterval == 0 && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0),
                       "a screen");
    UINT flags = 0;
    if (g_presentInterval == 0 && g_tearing && !fs)
        flags = DXGI_PRESENT_ALLOW_TEARING;
    present_result(g_swap->Present((UINT)g_presentInterval, flags), "the game window");
}

void present()
{
    /* Present may wait for the display (vsync), the GPU (one frame queued)
     * or a frame limiter hooked into it (RTSS): outside the game CPU, so the
     * other game threads run meanwhile (platform/scheduler.h).  The other
     * game threads make no D3D calls. */
    guard_context();
    frame_monitor::present_begin();
    const double t0 = sched::now_seconds();
    sched::run_outside(present_call, nullptr);
    g_lastPresent = sched::now_seconds();
    g_presentWait += g_lastPresent - t0;
    frame_monitor::add(frame_monitor::PART_PRESENT, (g_lastPresent - t0) * 1000.0);
    frame_monitor::presented();
}

void set_present_interval(int syncInterval)
{
    g_presentInterval = syncInterval;
}

void present_info(int *interval, bool *tearing, bool *exclusive)
{
    *interval = g_presentInterval;
    *exclusive = g_lastExclusive;
    *tearing = g_presentInterval == 0 && (g_tearing || g_lastExclusive);
}

double refresh_hz()
{
    static ULONGLONG last = 0;
    static double hz = 60.0;
    static settings::DisplayMode lastMode = settings::DisplayMode::windowed;
    const ULONGLONG now = GetTickCount64();
    if (last != 0 && now - last < 2000 && lastMode == g_mode)
        return hz;
    last = now;
    lastMode = g_mode;
    BOOL fs = FALSE;
    if (g_mode == settings::DisplayMode::fullscreen && g_fullscreenHz > 1.0 &&
        SUCCEEDED(g_swap->GetFullscreenState(&fs, nullptr)) && fs) {
        hz = g_fullscreenHz;
        return hz;
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = (double)dm.dmDisplayFrequency;
    else
        hz = 60.0;
    return hz;
}

} // namespace gpu

/* frame_pacing.h, over the swap chain above */
namespace pacing {

bool high_refresh() { return gpu::refresh_hz() > 61.0; }
double refresh_hz() { return gpu::refresh_hz(); }
void set_vsync(bool on) { gpu::set_present_interval(on ? 1 : 0); }
double last_present_time() { return gpu::g_lastPresent; }
double present_wait_total() { return gpu::g_presentWait; }
void output_size(int *w, int *h) { gpu::backbuffer_size(w, h); }
int screen_count() { return gpu::screen_count(); }
void canvas_size(int *w, int *h) { gpu::canvas_size(w, h); }
void canvas_rect(int i, int *x, int *w) { gpu::canvas_rect(i, x, w); }
bool span_active() { return gpu::span_active(); }
void want_split_screen() { gpu::want_split_screen(); }

} // namespace pacing

namespace gpu {

static ID3DBlob *compile(const char *name, const char *source, const char *entry, const char *target)
{
#ifdef _WIN32
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), name, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        pclog::fatal("shader %s (%s) does not compile:\n%s", name, entry,
                     errors ? (const char *)errors->GetBufferPointer() : "?");
    }
    if (errors)
        errors->Release();
    return code;
#else
    /* (the blob compiled offline from the same source, name, entry and
     * target: shader_blobs.h, tools/shader_blobs) */
    const shader_blobs::Blob *b = shader_blobs::find(name, entry, target, "");
    if (b == nullptr)
        pclog::fatal("shader %s (%s %s): not in shader_blobs.h", name, entry, target);
    if (b->source_hash != shader_blobs::source_hash(source, strlen(source)))
        pclog::fatal("shader %s (%s %s): shader_blobs.h was made from another source (regenerate it)", name, entry,
                     target);
    return new shader_blobs::BlobRef(*b);
#endif
}

ID3D11VertexShader *compile_vs(const char *name, const char *source, const char *entry, ID3DBlob **bytecode)
{
    ID3DBlob *code = compile(name, source, entry, "vs_4_0");
    ID3D11VertexShader *vs = nullptr;
    g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
    if (bytecode)
        *bytecode = code;
    else
        code->Release();
    return vs;
}

ID3D11PixelShader *compile_ps(const char *name, const char *source, const char *entry)
{
    ID3DBlob *code = compile(name, source, entry, "ps_4_0");
    ID3D11PixelShader *ps = nullptr;
    g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    code->Release();
    return ps;
}

} // namespace gpu
