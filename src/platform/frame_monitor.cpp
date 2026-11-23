/* frame_monitor.cpp - the frame time monitor of GT3 PC (frame_monitor.h). */
#include "platform/frame_monitor.h"
#include "platform/gpu.h"
#include "platform/overlay.h"
#include "platform/settings.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace frame_monitor {
namespace {

using overlay::rect;
using overlay::rgba;
using overlay::text;
using overlay::text_width;

constexpr int kFrames = 300;                    /* the graph: the last 300 frames */
constexpr float kTickMs = 1000.0f / 59.94f;
/* The graph's heights in ms: the smallest that holds 1.5 x the 99th
 * percentile (it shrinks only once that is well below). */
constexpr float kScales[] = {5.0f, 10.0f, 20.0f, 2.0f * kTickMs, 4.0f * kTickMs, 8.0f * kTickMs};
float g_graphMs = 2.0f * kTickMs;

struct Frame {
    float ms;                                   /* since the frame before */
    uint8_t tick;                               /* it followed a game tick */
    float part[kParts];                         /* the breakdown (add) */
    float gpu;                                  /* the GPU's time (gpu_time; -1: not known) */
    float sec[kGpuSections];                    /* the GPU's sections (gpu_sections) */
    uint8_t hasSec;                             /* ... known */
    float exec[kExecKinds];                     /* EXECUTE's CPU profile (exec_profile) */
    uint32_t counts[kExecCounts];
    uint8_t hasExec;
    float d3d;                                  /* the D3D thread's busy ms (exec_d3d_thread; -1: none) */
    float d3dWait;                              /* the game thread's ms waiting for it */
    float execThread;                           /* the executor thread's busy ms (exec_thread; -1: none) */
    float sub;                                  /* PC: the submit thread's busy ms (exec_submit_thread; -1: none) */
    float subWait;                              /* the executor's ms waiting for it */
};

/* The renderer's executor thread reports too (EXECUTE's profile, the GPU
 * times): every entry point takes it. */
std::recursive_mutex g_lock;

/* The GPU sections' names (gpu_section_names) */
const char *g_secNames[kGpuSections];
int g_secCount;
/* The CPU profile's names (exec_profile_names) and the frame being timed's
 * (exec_profile) */
const char *g_execNames[kExecKinds];
int g_execKinds;
const char *g_countNames[kExecCounts];
int g_execCounts;
float g_curExec[kExecKinds];
uint32_t g_curCounts[kExecCounts];
bool g_curHasExec;
float g_curD3d = -1.0f;
float g_curD3dWait;
float g_curExecThread = -1.0f;
float g_curSub = -1.0f;                     /* PC (2026-11-05): the submit thread (exec_submit_thread) */
float g_curSubWait;

std::atomic<int> g_mode{MODE_OFF};
Frame g_frames[kFrames];
int g_count;                                    /* recorded so far (at most kFrames) */
int g_next;                                     /* the slot of the next one */
uint32_t g_seq;                                 /* frames recorded in all: the one being timed */
float g_cur[kParts];                            /* the breakdown of the frame being timed */
int64_t g_last;                                 /* QPC of the last present (0: none) */
int64_t g_outputStart;                          /* output_begin's QPC (0: none) */
double g_msPerCount;
int g_drawnTick = 1;                            /* the frame being drawn (note_frame) */
int g_shownTick = 1;                            /* the one drawn before: Present shows it */
int g_between;                                  /* frames are drawn between the ticks */

const uint32_t kWhite = rgba(235, 235, 235, 255);
const uint32_t kGrey = rgba(150, 150, 150, 255);
const uint32_t kTick = rgba(255, 165, 40, 255);         /* a frame after a tick */
const uint32_t kBetween = rgba(70, 215, 95, 255);       /* a frame between ticks */
const uint32_t kSpike = rgba(255, 60, 60, 255);
const uint32_t kRefreshLine = rgba(120, 190, 255, 200);
const uint32_t kTickLine = rgba(255, 220, 90, 160);
const uint32_t kGpuMark = rgba(245, 245, 245, 255);

/* The breakdown's rows, in the stack's order (bottom up), OTHER last. */
enum { ROW_UPDATE, ROW_RECORD, ROW_EXECUTE, ROW_WAIT, ROW_OUTPUT, ROW_PRESENT, ROW_PACE, ROW_OTHER, kRows };
const char *const kRowNames[kRows] = {"UPDATE", "RECORD", "EXECUTE", "GPU WAIT", "OUTPUT", "PRESENT", "PACE",
                                      "OTHER"};
const uint32_t kRowColors[kRows] = {
    rgba(255, 170, 60, 255),  rgba(240, 220, 90, 255), rgba(90, 160, 255, 255), rgba(255, 70, 70, 255),
    rgba(90, 220, 220, 255),  rgba(190, 120, 255, 255), rgba(125, 125, 125, 255), rgba(75, 75, 75, 255),
};

int64_t qpc()
{
#ifdef _WIN32
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    if (g_msPerCount == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_msPerCount = 1000.0 / (double)f.QuadPart;
    }
    return v.QuadPart;
#else
    /* (CLOCK_MONOTONIC in ns, as pgl's QueryPerformanceCounter off Windows) */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_msPerCount = 1e-6;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* A frame's rows: EXECUTE without its GPU wait, OTHER what the parts leave
 * of the frame's time. */
void frame_rows(const Frame &f, float *row)
{
    row[ROW_UPDATE] = f.part[PART_UPDATE];
    row[ROW_RECORD] = f.part[PART_RECORD];
    row[ROW_WAIT] = std::min(f.part[PART_WAIT], f.part[PART_EXECUTE]);
    row[ROW_EXECUTE] = f.part[PART_EXECUTE] - row[ROW_WAIT];
    row[ROW_OUTPUT] = f.part[PART_OUTPUT];
    row[ROW_PRESENT] = f.part[PART_PRESENT];
    row[ROW_PACE] = f.part[PART_PACE];
    float sum = 0.0f;
    for (int r = 0; r < ROW_OTHER; r++)
        sum += row[r];
    row[ROW_OTHER] = std::max(0.0f, f.ms - sum);
}

/* The graph's frame i (0 the oldest shown) */
const Frame &shown_frame(int i)
{
    const int first = g_count < kFrames ? 0 : g_next;
    return g_frames[(first + i) % kFrames];
}

/* Frame seq, if it is still in the graph and recorded (its queries are read
 * a few frames later); nullptr else */
Frame *recorded_frame(uint32_t seq)
{
    const uint32_t back = g_seq - seq;
    if (back == 0 || back > (uint32_t)g_count)
        return nullptr;
    return &g_frames[((g_next - (int)back) % kFrames + kFrames) % kFrames];
}

/* PC (2026-11-07): the longest of n names (characters, at least `least'):
 * the tables' name columns are as wide as their names.  The columns were a
 * fixed 11 / 9 characters: TARGET COPIES and FLARE QUERIES ran into the next
 * column, the counts' column into the next and past the panel. */
constexpr int kNameMax = 31;
int names_width(const char *const *names, int n, int least)
{
    int w = least;
    for (int k = 0; k < n; k++)
        if (names[k] != nullptr)
            w = std::max(w, (int)strnlen(names[k], kNameMax));
    return w;
}

/* A table of AVG / MAX, two to a line (the columns colW apart, the names
 * nameW wide), under its title from y (names in capitals); the y after it */
float draw_table(float x, float y, float colW, float lineH, int s, int nameW, const char *title,
                 const char *const *names, const float *avg, const float *mx, int n)
{
    text(x, y, s, title, kGrey);
    for (int k = 0; k < n; k++) {
        char name[kNameMax + 1], line[64];
        int c = 0;
        for (const char *p = names[k]; p != nullptr && *p && c < (int)sizeof(name) - 1; p++)
            name[c++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        name[c] = 0;
        snprintf(line, sizeof(line), "%-*s %5.2f / %5.2f", nameW, name, avg[k], mx[k]);
        text(x + (k & 1) * colW, y + lineH * (float)(1 + (k >> 1)), s, line, kWhite);
    }
    return y + lineH * (float)(1 + (n + 1) / 2);
}

/* The tables' panel's lines (its height): the GPU table's (a title and two
 * sections a line), a gap, the CPU table's (the kinds and the six thread
 * lines, two a line).  PC (2026-11-07): the CPU table counted one line
 * short (its last line sat on the panel's edge). */
int verbose_lines()
{
    return (1 + (g_secCount + 1) / 2) + 1 + (1 + (g_execKinds + 6 + 1) / 2);
}

/* The verbose panels, at least w wide: at (px, py) the GPU sections and
 * EXECUTE's CPU profile (and what the profile does not hold of it: the
 * threads apart); the counts in a panel of their own to the right of it, in
 * as many columns as the screen (bw x bh) has room for there (two at least),
 * else below it.  PC (2026-11-05): two panels - the one tall panel ran past
 * the bottom of a 1080 screen once the counts passed 60 (user report), and
 * the submit thread's lines sat far down it.  PC (2026-11-07): the columns
 * sized by their names (names_width), the panels by their columns; the
 * counts' columns by the room. */
void draw_verbose(float px, float py, float w, float pad, float lineH, int s, int bw, int bh)
{
    (void)bh;
    const float cw = 6.0f * s;                  /* a character cell (overlay.h) */
    /* the tables' names: the sections', the kinds' and the threads' */
    const char *names[kExecKinds + 6];
    for (int k = 0; k < g_execKinds; k++)
        names[k] = g_execNames[k];
    names[g_execKinds] = "OUTSIDE";
    names[g_execKinds + 1] = "D3D THREAD";
    names[g_execKinds + 2] = "D3D WAIT";
    names[g_execKinds + 3] = "EXEC THREAD";
    names[g_execKinds + 4] = "SUBMIT THR";
    names[g_execKinds + 5] = "SUBMIT WAIT";
    const int nameW = std::max(names_width(g_secNames, g_secCount, 11), names_width(names, g_execKinds + 6, 11));
    /* a column: the name, " 12.34 / 12.34", two blanks to the next */
    const float colW = (float)(nameW + 15 + 2) * cw;
    const float pw = std::max(w, 2.0f * colW - 2.0f * cw + 2.0f * pad);
    const float ph = pad + (float)verbose_lines() * lineH + pad;
    rect(px, py, pw, ph, rgba(0, 0, 0, 175));
    const float x = px + pad;
    float y = py + pad;

    /* the GPU sections */
    float avg[kExecKinds + 6] = {}, mx[kExecKinds + 6] = {};
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        const Frame &f = shown_frame(i);
        if (!f.hasSec)
            continue;
        for (int k = 0; k < g_secCount; k++) {
            avg[k] += f.sec[k];
            mx[k] = std::max(mx[k], f.sec[k]);
        }
        n++;
    }
    for (int k = 0; k < g_secCount; k++)
        avg[k] = n ? avg[k] / n : 0.0f;
    y = draw_table(x, y, colW, lineH, s, nameW, n ? "GPU SECTIONS, MS (AVG / MAX)" : "GPU SECTIONS: WAITING",
                   g_secNames, avg, mx, g_secCount) + lineH;

    /* EXECUTE's CPU profile, and the rest of EXECUTE without its wait (the
     * execution's set-up and end: targets, the flush, queries) */
    memset(avg, 0, sizeof(avg));
    memset(mx, 0, sizeof(mx));
    double counts[kExecCounts] = {};
    n = 0;
    int nD3d = 0, nExec = 0, nSub = 0;
    for (int i = 0; i < g_count; i++) {
        const Frame &f = shown_frame(i);
        if (!f.hasExec)
            continue;
        float sum = 0.0f;
        for (int k = 0; k < g_execKinds; k++) {
            avg[k] += f.exec[k];
            mx[k] = std::max(mx[k], f.exec[k]);
            sum += f.exec[k];
        }
        const float rest = std::max(0.0f, f.part[PART_EXECUTE] - std::min(f.part[PART_WAIT], f.part[PART_EXECUTE]) - sum);
        avg[g_execKinds] += rest;
        mx[g_execKinds] = std::max(mx[g_execKinds], rest);
        if (f.d3d >= 0.0f) {
            /* the D3D thread: apart (it runs alongside), and the waits for it */
            avg[g_execKinds + 1] += f.d3d;
            mx[g_execKinds + 1] = std::max(mx[g_execKinds + 1], f.d3d);
            avg[g_execKinds + 2] += f.d3dWait;
            mx[g_execKinds + 2] = std::max(mx[g_execKinds + 2], f.d3dWait);
            nD3d++;
        }
        if (f.execThread >= 0.0f) {
            /* the executor thread: apart too (alongside the game thread) */
            avg[g_execKinds + 3] += f.execThread;
            mx[g_execKinds + 3] = std::max(mx[g_execKinds + 3], f.execThread);
            nExec++;
        }
        if (f.sub >= 0.0f) {
            /* PC (2026-11-05): the submit thread (alongside the executor; the
             * kinds above are both stages' together) and the executor's waits */
            avg[g_execKinds + 4] += f.sub;
            mx[g_execKinds + 4] = std::max(mx[g_execKinds + 4], f.sub);
            avg[g_execKinds + 5] += f.subWait;
            mx[g_execKinds + 5] = std::max(mx[g_execKinds + 5], f.subWait);
            nSub++;
        }
        for (int k = 0; k < g_execCounts; k++)
            counts[k] += f.counts[k];
        n++;
    }
    for (int k = 0; k <= g_execKinds; k++)
        avg[k] = n ? avg[k] / n : 0.0f;
    avg[g_execKinds + 1] = nD3d ? avg[g_execKinds + 1] / nD3d : 0.0f;
    avg[g_execKinds + 2] = nD3d ? avg[g_execKinds + 2] / nD3d : 0.0f;
    avg[g_execKinds + 3] = nExec ? avg[g_execKinds + 3] / nExec : 0.0f;
    avg[g_execKinds + 4] = nSub ? avg[g_execKinds + 4] / nSub : 0.0f;
    avg[g_execKinds + 5] = nSub ? avg[g_execKinds + 5] / nSub : 0.0f;
    draw_table(x, y, colW, lineH, s, nameW, n ? "EXECUTE CPU, MS (AVG / MAX)" : "EXECUTE CPU: WAITING", names, avg,
               mx, g_execKinds + 6);

    /* what a frame executed: a panel of its own.  A column: the name, a
     * blank, six digits, two blanks to the next.  Beside the tables' panel
     * in the columns the screen has room for there, two at least; else
     * below it, up to four. */
    const int countNameW = names_width(g_countNames, g_execCounts, 9);
    const float countW = (float)(countNameW + 1 + 6 + 2) * cw;
    float cx = px + pw + 4.0f * s, cy = py;
    int columns = (int)(((float)bw - cx - 2.0f * pad) / countW);
    if (columns < 2) {
        cx = px;
        cy = py + ph + 4.0f * s;
        columns = std::min(4, (int)(((float)bw - cx - 2.0f * pad) / countW));
    }
    columns = std::max(1, std::min(columns, std::max(1, g_execCounts)));
    const int rows = (g_execCounts + columns - 1) / columns;
    const float chw = (float)columns * countW - 2.0f * cw + 2.0f * pad;
    const float chh = pad + (float)(1 + rows) * lineH + pad;
    rect(cx, cy, chw, chh, rgba(0, 0, 0, 175));
    const float tx = cx + pad, ty = cy + pad;
    text(tx, ty, s, "PER FRAME (AVG)", kGrey);
    for (int k = 0; k < g_execCounts; k++) {
        char line[64];
        snprintf(line, sizeof(line), "%-*s %6.0f", countNameW, g_countNames[k] ? g_countNames[k] : "",
                 n ? counts[k] / n : 0.0);
        text(tx + (float)(k % columns) * countW, ty + lineH * (float)(1 + k / columns), s, line, kWhite);
    }
}

/* The graph's height: the smallest scale that holds 1.5 x p99 (shrunk only
 * once it is well below). */
float graph_top(float p99)
{
    float want = kScales[sizeof(kScales) / sizeof(kScales[0]) - 1];
    for (float sc : kScales) {
        if (sc >= 1.5f * p99) {
            want = sc;
            break;
        }
    }
    if (want > g_graphMs || 2.5f * p99 < g_graphMs)
        g_graphMs = want;
    return g_graphMs;
}

/* A horizontal line at ms (within the height) with its label (above the
 * line, below it near the top), at the right or the left. */
void graph_line(float gx, float gy, float gw, float gh, float topMs, float ms, const char *label, bool right,
                uint32_t color, int s)
{
    if (ms >= topMs)
        return;
    const float bottom = gy + gh;
    const float ly = bottom - ms / topMs * gh;
    rect(gx, ly, gw, (float)std::max(1, s / 2), color);
    const float ty = ly - 8.0f * s < gy + s ? ly + 2.0f * s : ly - 8.0f * s;
    text(right ? gx + gw - text_width(label, s) - s : gx + s, ty, s, label, color);
}

void build(int bw, int bh)
{
    const int mode = g_mode.load();
    const bool detailed = mode >= MODE_DETAILED;
    const bool verbose = mode == MODE_VERBOSE;
    const int s = std::max(1, bh / 540);        /* 1080p: 2 screen pixels a font pixel */

    /* the statistics of the frames in the graph */
    float sorted[kFrames];
    double sum = 0.0;
    float lo = 0.0f, hi = 0.0f;
    for (int i = 0; i < g_count; i++) {
        const float ms = g_frames[i].ms;
        sorted[i] = ms;
        sum += ms;
        lo = i == 0 ? ms : std::min(lo, ms);
        hi = std::max(hi, ms);
    }
    const float avg = g_count > 0 ? (float)(sum / g_count) : 0.0f;
    float p99 = 0.0f;
    if (g_count > 0) {
        std::sort(sorted, sorted + g_count);
        p99 = sorted[(int)((g_count - 1) * 0.99f)];
    }
    const double fps = sum > 0.0 ? g_count * 1000.0 / sum : 0.0;
    const double hz = gpu::refresh_hz();
    const float topMs = graph_top(p99);

    const float pad = 4.0f * s, lineH = 9.0f * s;
    const float gw = (float)(kFrames * s), gh = 60.0f * s;
    const float px = 8.0f * s, py = 8.0f * s;
    /* simple: a line and the graph; detailed: 4 lines, the graph, the
     * breakdown's title and 5 lines, its graph */
    const int lines = detailed ? 4 : 1;
    const float graphs = detailed ? 2.0f * gh + 6.0f * lineH + 4.0f * s : gh;
    const float pw = gw + 2.0f * pad;
    const float ph = pad + lines * lineH + 2.0f * s + graphs + pad;
    rect(px, py, pw, ph, rgba(0, 0, 0, 175));

    char line[128];
    float x = px + pad, y = py + pad;
    char label[32];
    if (!detailed) {
        snprintf(line, sizeof(line), "FPS %.1f   %.2f MS", fps, avg);
        text(x, y, s, line, kWhite);
        snprintf(label, sizeof(label), "TOP %.1f MS", topMs);
        text(px + pad + gw - text_width(label, s) + s, y, s, label, kGrey);
        y += lineH + 2.0f * s;
    } else {
        snprintf(line, sizeof(line), "FPS %.1f   FRAME %.2f MS", fps, avg);
        text(x, y, s, line, kWhite);
        y += lineH;
        snprintf(line, sizeof(line), "MIN %.2f  MAX %.2f  99%% %.2f MS", lo, hi, p99);
        text(x, y, s, line, kWhite);
        y += lineH;
        int interval = 0;
        bool tearing = false, exclusive = false;
        gpu::present_info(&interval, &tearing, &exclusive);
        char vs[16], cap[24];
        if (interval)
            snprintf(vs, sizeof(vs), "%d", settings::vsync() > 0 ? settings::vsync() : 1);
        else
            snprintf(vs, sizeof(vs), "OFF");
        if (settings::frame_cap_ms() > 0.0)
            snprintf(cap, sizeof(cap), "%.3f MS", settings::frame_cap_ms());
        else
            snprintf(cap, sizeof(cap), "OFF");
        snprintf(line, sizeof(line), "%.0f HZ  VSYNC %s  CAP %s  TEAR %s%s", hz, vs, cap, tearing ? "ON" : "OFF",
                 exclusive ? "  FS" : "");
        text(x, y, s, line, kGrey);
        y += lineH;
        if (g_between) {
            rect(x, y + s, 5.0f * s, 5.0f * s, kTick);
            x = text(x + 7.0f * s, y, s, "AFTER A TICK", kGrey);
            rect(x + 2.0f * s, y + s, 5.0f * s, 5.0f * s, kBetween);
            x = text(x + 9.0f * s, y, s, "BETWEEN TICKS", kGrey);
            rect(x + 2.0f * s, y + s, 5.0f * s, 5.0f * s, kSpike);
            text(x + 9.0f * s, y, s, "SPIKE", kGrey);
        } else {
            text(x, y, s, "ONE FRAME PER TICK", kGrey);
        }
        snprintf(label, sizeof(label), "TOP %.1f MS", topMs);
        text(px + pad + gw - text_width(label, s) + s, y, s, label, kGrey);
        y += lineH + 2.0f * s;
    }

    /* the graph: oldest left, newest right, bars up from the bottom */
    const float gx = px + pad;
    float gy = y, bottom = gy + gh;
    rect(gx, gy, gw, gh, rgba(0, 0, 0, 120));
    const int offset = kFrames - g_count;      /* fewer frames: right-aligned */
    for (int i = 0; i < g_count; i++) {
        const Frame &f = shown_frame(i);
        float h = std::min(f.ms / topMs, 1.0f) * gh;
        if (h < 1.0f)
            h = 1.0f;
        uint32_t c = detailed && g_between && f.tick ? kTick : kBetween;
        if (f.ms > 2.0f * avg && f.ms - avg > 1.0f)
            c = kSpike;
        rect(gx + (float)((offset + i) * s), bottom - h, (float)s, h, c);
    }
    /* the lines: the display's refresh period; detailed, the tick too */
    if (hz > 1.0) {
        snprintf(label, sizeof(label), "%.0f HZ %.1f", hz, 1000.0 / hz);
        graph_line(gx, gy, gw, gh, topMs, (float)(1000.0 / hz), label, true, kRefreshLine, s);
    }
    if (detailed) {
        snprintf(label, sizeof(label), "TICK %.1f", kTickMs);
        graph_line(gx, gy, gw, gh, topMs, kTickMs, label, false, kTickLine, s);
    }
    if (!detailed)
        return;

    /* the breakdown: each row's mean and maximum, two to a line */
    y = bottom + 3.0f * s;
    float rowSum[kRows] = {}, rowMax[kRows] = {};
    double gpuSum = 0.0;
    float gpuMax = 0.0f;
    int gpuN = 0;
    for (int i = 0; i < g_count; i++) {
        const Frame &f = shown_frame(i);
        float row[kRows];
        frame_rows(f, row);
        for (int r = 0; r < kRows; r++) {
            rowSum[r] += row[r];
            rowMax[r] = std::max(rowMax[r], row[r]);
        }
        if (f.gpu >= 0.0f) {
            gpuSum += f.gpu;
            gpuMax = std::max(gpuMax, f.gpu);
            gpuN++;
        }
    }
    text(gx, y, s, "BREAKDOWN, MS (AVG / MAX)", kGrey);
    y += lineH;
    const float colW = gw * 0.5f;
    for (int r = 0; r < kRows; r++) {
        const float cx = gx + (r & 1) * colW, cy = y + (float)(r >> 1) * lineH;
        rect(cx, cy + s, 5.0f * s, 5.0f * s, kRowColors[r]);
        snprintf(line, sizeof(line), "%-8s %5.2f / %5.2f", kRowNames[r], g_count ? rowSum[r] / g_count : 0.0f,
                 rowMax[r]);
        text(cx + 7.0f * s, cy, s, line, kWhite);
    }
    y += 4.0f * lineH;
    rect(gx, y + 3.0f * s, 5.0f * s, (float)std::max(1, s), kGpuMark);
    if (gpuN > 0)
        snprintf(line, sizeof(line), "%-8s %5.2f / %5.2f", "GPU", (float)(gpuSum / gpuN), gpuMax);
    else
        snprintf(line, sizeof(line), "%-8s -", "GPU");
    text(gx + 7.0f * s, y, s, line, kWhite);
    snprintf(line, sizeof(line), "%-8s %5.2f / %5.2f", "FRAME", avg, hi);
    text(gx + colW + 7.0f * s, y, s, line, kGrey);
    y += lineH + s;

    /* its graph: each frame's rows stacked, the GPU's time a mark */
    gy = y;
    bottom = gy + gh;
    rect(gx, gy, gw, gh, rgba(0, 0, 0, 120));
    for (int i = 0; i < g_count; i++) {
        const Frame &f = shown_frame(i);
        float row[kRows];
        frame_rows(f, row);
        const float bx = gx + (float)((offset + i) * s);
        float at = 0.0f;                        /* ms stacked so far */
        for (int r = 0; r < kRows && at < topMs; r++) {
            if (row[r] <= 0.0f)
                continue;
            const float top = std::min(at + row[r], topMs);
            const float y0 = bottom - top / topMs * gh, y1 = bottom - at / topMs * gh;
            if (y1 - y0 > 0.0f)
                rect(bx, y0, (float)s, std::max(y1 - y0, 1.0f), kRowColors[r]);
            at = top;
        }
        if (f.gpu >= 0.0f) {
            const float gyMark = bottom - std::min(f.gpu / topMs, 1.0f) * gh;
            rect(bx, std::max(gy, gyMark - (float)s * 0.5f), (float)s, (float)std::max(1, s), kGpuMark);
        }
    }
    if (hz > 1.0) {
        snprintf(label, sizeof(label), "%.0f HZ %.1f", hz, 1000.0 / hz);
        graph_line(gx, gy, gw, gh, topMs, (float)(1000.0 / hz), label, true, kRefreshLine, s);
    }
    if (verbose)
        draw_verbose(px + pw + 4.0f * s, py, pw, pad, lineH, s, bw, bh);
}

} // namespace

void toggle()
{
    g_mode = (g_mode.load() + 1) % kModes;
}

Mode mode()
{
    return (Mode)g_mode.load(std::memory_order_relaxed);
}

void set_mode(Mode m)
{
    if (m >= MODE_OFF && m < kModes)
        g_mode = m;
}

bool shown()
{
    return g_mode.load(std::memory_order_relaxed) != MODE_OFF;
}

void note_frame(int tick, int between)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    /* the frame Present shows next is the one drawn before this one (the
     * game flips to it after drawing this one: T002) */
    g_shownTick = g_drawnTick;
    g_drawnTick = tick;
    g_between = between;
}

void add(Part p, double ms)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    if (p >= 0 && p < kParts && ms > 0.0)
        g_cur[p] += (float)ms;
}

uint32_t frame_seq()
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    return g_seq;
}

void gpu_time(uint32_t seq, double ms)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    if (Frame *f = recorded_frame(seq))
        f->gpu = (float)ms;
}

void exec_profile_names(const char *const *kinds, int nKinds, const char *const *counts, int nCounts)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    g_execKinds = std::min(std::max(nKinds, 0), kExecKinds);
    for (int i = 0; i < g_execKinds; i++)
        g_execNames[i] = kinds[i];
    g_execCounts = std::min(std::max(nCounts, 0), kExecCounts);
    for (int i = 0; i < g_execCounts; i++)
        g_countNames[i] = counts[i];
}

void exec_profile(const float *ms, int nKinds, const uint32_t *counts, int nCounts)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    for (int i = 0; i < std::min(nKinds, kExecKinds); i++)
        g_curExec[i] += ms[i];
    for (int i = 0; i < std::min(nCounts, kExecCounts); i++)
        g_curCounts[i] += counts[i];
    g_curHasExec = true;
}

void exec_d3d_thread(float busyMs, float waitMs)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    g_curD3d = busyMs < 0.0f ? -1.0f : std::max(g_curD3d, 0.0f) + busyMs;
    g_curD3dWait += waitMs;
}

void exec_thread(float ms)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    g_curExecThread = std::max(g_curExecThread, 0.0f) + ms;
}

void exec_submit_thread(float busyMs, float waitMs)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    g_curSub = busyMs < 0.0f ? -1.0f : std::max(g_curSub, 0.0f) + busyMs;
    g_curSubWait += waitMs;
}

void gpu_section_names(const char *const *names, int n)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    g_secCount = std::min(std::max(n, 0), kGpuSections);
    for (int i = 0; i < g_secCount; i++)
        g_secNames[i] = names[i];
}

void gpu_sections(uint32_t seq, const float *ms, int n)
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    Frame *f = recorded_frame(seq);
    if (f == nullptr)
        return;
    n = std::min(n, kGpuSections);
    for (int i = 0; i < kGpuSections; i++)
        f->sec[i] = i < n ? ms[i] : 0.0f;
    f->hasSec = 1;
}

void output_begin()
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    g_outputStart = qpc();
}

void present_begin()
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    const int64_t now = qpc();
    if (g_outputStart != 0)
        add(PART_OUTPUT, (double)(now - g_outputStart) * g_msPerCount);
    g_outputStart = 0;
}

void presented()
{
    std::lock_guard<std::recursive_mutex> hold(g_lock);
    const int64_t now = qpc();
    if (g_last != 0) {
        Frame &f = g_frames[g_next];
        f.ms = (float)((double)(now - g_last) * g_msPerCount);
        f.tick = (uint8_t)(g_shownTick != 0);
        memcpy(f.part, g_cur, sizeof(f.part));
        f.gpu = -1.0f;
        f.hasSec = 0;
        memcpy(f.exec, g_curExec, sizeof(f.exec));
        memcpy(f.counts, g_curCounts, sizeof(f.counts));
        f.hasExec = g_curHasExec ? 1 : 0;
        f.d3d = g_curD3d;
        f.d3dWait = g_curD3dWait;
        f.execThread = g_curExecThread;
        f.sub = g_curSub;
        f.subWait = g_curSubWait;
        g_next = (g_next + 1) % kFrames;
        if (g_count < kFrames)
            g_count++;
        g_seq++;
    }
    memset(g_cur, 0, sizeof(g_cur));
    memset(g_curExec, 0, sizeof(g_curExec));
    memset(g_curCounts, 0, sizeof(g_curCounts));
    g_curHasExec = false;
    g_curD3d = -1.0f;
    g_curD3dWait = 0.0f;
    g_curExecThread = -1.0f;
    g_curSub = -1.0f;
    g_curSubWait = 0.0f;
    g_last = now;
}

void present(ID3D11RenderTargetView *bb)
{
    if (!shown() || bb == nullptr)
        return;
    if (!overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    {
        /* (only the frames read under the lock: the overlay's context use
         * may wait for the renderer's threads) */
        std::lock_guard<std::recursive_mutex> hold(g_lock);
        build(bw, bh);
    }
    overlay::end(bb);
}

} // namespace frame_monitor
