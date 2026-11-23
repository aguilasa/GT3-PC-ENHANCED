/* gpu.h - the Direct3D 11 device of GT3 PC.
 *
 * One device and immediate context, a flip-model swap chain on the game
 * window.  Only game threads draw, and only one of them runs at a time
 * (platform/scheduler.h), so the immediate context needs no lock of its
 * own; the window thread never touches it.  The renderer's D3D thread uses
 * it while it has calls queued: every use through here waits for them first
 * (set_context_guard).  Shaders are compiled at start
 * from HLSL text (d3dcompiler_47, part of Windows).
 *
 * Off Windows: DXVK-native's Direct3D 11 on the SDL3 window, the shaders
 * compiled offline (engine/pgl/native/shader_blobs.h), the monitors SDL's
 * displays and exclusive fullscreen the window module's SDL display mode
 * (gpu.cpp). */
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdint.h>

namespace gpu {

bool init(HWND window);
ID3D11Device *device();
ID3D11DeviceContext *context();
/* PC (2026-11-07): the device's adapter (nullptr before init): the memory
 * line's DXGI query (platform/memstat.h). */
IDXGIAdapter *adapter();
/* The GPU memory DXGI counts for this process, bytes: in use on the card
 * and its budget, in use in system memory (false before init or when the
 * adapter has no IDXGIAdapter3). */
bool video_memory(uint64_t *localUsed, uint64_t *localBudget, uint64_t *nonLocalUsed);
/* A function run first whenever the context or the swap chain is used
 * through here (context, backbuffer, screen_backbuffer, present): the
 * renderer's D3D thread runs what it has queued (engine/pgl/native/
 * pgl_queue.h; on that thread itself nothing). */
void set_context_guard(void (*guard)());

/* The back buffer for this frame (recreated when the window size changed).
 * Also where the display mode changes (window.h: Alt+Enter) and exclusive
 * fullscreen is entered again after Alt+Tab: on the game thread, which owns
 * the swap chain. */
ID3D11RenderTargetView *backbuffer();
void backbuffer_size(int *w, int *h);
/* The fullscreen mode out of exclusive fullscreen, borderless, for a window
 * that must show over the game (a Windows box hides behind exclusive
 * fullscreen; platform/file_alert.h): before the first frame, on the game
 * thread.  The next frame's backbuffer() takes it again. */
void leave_fullscreen();

/* Screens (multi-monitor; docs/renderer.md "Screens").  Screen 0 is the game
 * window's; the extra screens are borderless windows over other monitors
 * with swap chains of their own (exclusive fullscreen when asked): the
 * span's (gt3pc.ini SpanMonitors, in a full-screen mode) or the split
 * screen's second monitor (while the game asks for it: want_split_screen,
 * kept 1.5 s).  backbuffer() opens and closes them.  The canvas: the
 * screens side by side at screen 0's height - the span's left to right as
 * on the desktop, the split screen's screen 0 first - a race picture drawn
 * over all of them, each showing its part (canvas_rect). */
int screen_count();
ID3D11RenderTargetView *screen_backbuffer(int i);
void screen_size(int i, int *w, int *h);
void canvas_size(int *w, int *h);
void canvas_rect(int i, int *x, int *w);
bool span_active();
void want_split_screen();

/* Shows the back buffer, with the present interval set (0: at once, the
 * default: no vsync, tearing allowed where the system has it; 1: at the
 * monitor's next vblank). */
void present();
void set_present_interval(int syncInterval);
/* For the frame monitor: the interval, whether a frame presented at once
 * may tear (a variable refresh display then follows the frames), whether
 * the last present was in exclusive fullscreen. */
void present_info(int *interval, bool *tearing, bool *exclusive);

/* The dynamic frame rate (docs/ROADMAP.md item 9): frames are drawn between
 * the game's ticks on a display faster than 61 Hz (one frame queued;
 * present() waits outside the game CPU).  The refresh rate of the window's
 * monitor (or of the fullscreen mode), checked every two seconds. */
double refresh_hz();

/* HLSL to shader objects; a failed compile ends the program with the
 * compiler's message (the shaders are part of the program). */
ID3D11VertexShader *compile_vs(const char *name, const char *source, const char *entry,
                               ID3DBlob **bytecode);
ID3D11PixelShader *compile_ps(const char *name, const char *source, const char *entry);

} // namespace gpu
