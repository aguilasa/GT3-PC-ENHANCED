/* crash.h - the crash report of GT3 PC: on an unhandled exception in any
 * thread, the exception, the registers and the code addresses found on the
 * stack (with their functions and lines from gt3pc.pdb) go to the log, and
 * a minidump to gt3pc_crash.dmp next to the program.
 *
 * Off Windows (platform/posix/crash_posix.cpp) a fatal signal (SIGSEGV,
 * SIGBUS, SIGILL, SIGFPE, SIGABRT) is the exception: the signal, the
 * registers, the unwound stack and the scanned one go to the log, the
 * functions named from the program's own symbol table (no line numbers:
 * the log names the image's load address for addr2line), and the system
 * writes its core dump as it would without the handler. */
#pragma once

namespace crash {

/* Installs the handler; call once, early. */
void install();

/* The code addresses found on the stack of another thread of the program
 * (`thread': on Windows its HANDLE, with suspend and context access; held
 * still while its stack is copied; elsewhere its pthread_t, cast by
 * (void *)(uintptr_t), which copies its own stack in a signal handler),
 * nearest first, described as the crash report does, each a log line
 * after `prefix'.  A scan: stale return addresses show up too.  Not for
 * the calling thread. */
void log_stack(void *thread, const char *prefix);

/* "function+0xNN (file:line)" for a code address of the program. */
void describe_address(const void *addr, char *out, unsigned outSize);

#ifndef _WIN32
/* An alternate signal stack for the calling thread, so that the report of
 * its stack overflowing can still run (each thread needs its own; install()
 * gives the calling thread one).  At a thread's start; once per thread. */
void prepare_thread();
#endif

} // namespace crash
