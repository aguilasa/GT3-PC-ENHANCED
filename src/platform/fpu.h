/* fpu.h - the FPU mode of native code called from game threads.
 *
 * Game threads run in the EE's FPU mode (scheduler.cpp set_game_fpu_mode:
 * round toward zero, denormals flushed), which keeps the game's float
 * results closest to the PS2's.  Driver code (Direct3D 11, DXGI, the shader
 * compiler) expects the default mode: round to nearest, denormals kept.
 * Every block of such calls made on a game thread is wrapped in a
 * DefaultFpuMode guard, which switches to the default mode and restores the
 * caller's mode when it goes out of scope.  The guards nest. */
#pragma once

#ifdef _WIN32
#include <float.h>
#else
#include <fpu_control.h>
#endif

namespace fpu {

#ifdef _WIN32
class DefaultFpuMode {
public:
    DefaultFpuMode()
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, 0, 0);
        saved_ = cw & (_MCW_RC | _MCW_DN);
        _controlfp_s(&cw, _RC_NEAR | _DN_SAVE, _MCW_RC | _MCW_DN);
    }
    ~DefaultFpuMode()
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, saved_, _MCW_RC | _MCW_DN);
    }
    DefaultFpuMode(const DefaultFpuMode &) = delete;
    DefaultFpuMode &operator=(const DefaultFpuMode &) = delete;

    /* The caller's mode (rounding and denormal bits). */
    unsigned int saved() const { return saved_; }

private:
    unsigned int saved_ = 0;
};

/* Inside a DefaultFpuMode block: a call back into game code (a readback
 * callback) runs in the game's mode again, given as DefaultFpuMode::saved()
 * returned it. */
class ScopedFpuMode {
public:
    explicit ScopedFpuMode(unsigned int mode)
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, 0, 0);
        saved_ = cw & (_MCW_RC | _MCW_DN);
        _controlfp_s(&cw, mode & (_MCW_RC | _MCW_DN), _MCW_RC | _MCW_DN);
    }
    ~ScopedFpuMode()
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, saved_, _MCW_RC | _MCW_DN);
    }
    ScopedFpuMode(const ScopedFpuMode &) = delete;
    ScopedFpuMode &operator=(const ScopedFpuMode &) = delete;

private:
    unsigned int saved_ = 0;
};

#else
/* The mode is the rounding and denormal bits of both units in one word:
 * MXCSR's rounding control (bits 13-14), flush-to-zero (15) and
 * denormals-are-zero (6) as they are, and the x87 rounding control (control
 * word bits 10-11) moved to bits 18-19.  Read and set with stmxcsr /
 * ldmxcsr (the i686 build does not enable SSE for the compiler, so the
 * _mm_getcsr / _mm_setcsr intrinsics are not available everywhere) and
 * _FPU_GETCW / _FPU_SETCW.  The x87 precision control is left alone. */
constexpr unsigned int kSseRound  = 0x6000u;     /* MXCSR RC */
constexpr unsigned int kSseFtz    = 0x8000u;     /* MXCSR FTZ */
constexpr unsigned int kSseDaz    = 0x0040u;     /* MXCSR DAZ */
constexpr unsigned int kSseBits   = kSseRound | kSseFtz | kSseDaz;
constexpr unsigned int kX87Shift  = 8;
constexpr unsigned int kX87Bits   = (unsigned int)_FPU_RC_ZERO << kX87Shift;   /* x87 RC */

/* The EE's mode (round toward zero, denormals flushed) and the default
 * one (round to nearest, denormals kept). */
constexpr unsigned int kGameMode    = kSseRound | kSseFtz | kSseDaz | kX87Bits;
constexpr unsigned int kDefaultMode = 0;

inline unsigned int read_mxcsr()
{
    unsigned int csr;
    __asm__ __volatile__("stmxcsr %0" : "=m"(csr));
    return csr;
}

inline void write_mxcsr(unsigned int csr)
{
    __asm__ __volatile__("ldmxcsr %0" : : "m"(csr));
}

inline unsigned int mode()
{
    fpu_control_t cw;
    _FPU_GETCW(cw);
    return (read_mxcsr() & kSseBits) | (((unsigned int)cw << kX87Shift) & kX87Bits);
}

inline void set_mode(unsigned int m)
{
    write_mxcsr((read_mxcsr() & ~kSseBits) | (m & kSseBits));
    fpu_control_t cw;
    _FPU_GETCW(cw);
    cw = (fpu_control_t)((cw & ~(fpu_control_t)_FPU_RC_ZERO) |
                         ((m & kX87Bits) >> kX87Shift));
    _FPU_SETCW(cw);
}

class DefaultFpuMode {
public:
    DefaultFpuMode() : saved_(mode()) { set_mode(kDefaultMode); }
    ~DefaultFpuMode() { set_mode(saved_); }
    DefaultFpuMode(const DefaultFpuMode &) = delete;
    DefaultFpuMode &operator=(const DefaultFpuMode &) = delete;

    /* The caller's mode (rounding and denormal bits). */
    unsigned int saved() const { return saved_; }

private:
    unsigned int saved_ = 0;
};

/* Inside a DefaultFpuMode block: a call back into game code (a readback
 * callback) runs in the game's mode again, given as DefaultFpuMode::saved()
 * returned it. */
class ScopedFpuMode {
public:
    explicit ScopedFpuMode(unsigned int m) : saved_(mode()) { set_mode(m); }
    ~ScopedFpuMode() { set_mode(saved_); }
    ScopedFpuMode(const ScopedFpuMode &) = delete;
    ScopedFpuMode &operator=(const ScopedFpuMode &) = delete;

private:
    unsigned int saved_ = 0;
};
#endif

} // namespace fpu
