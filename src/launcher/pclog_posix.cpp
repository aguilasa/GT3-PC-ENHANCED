/* pclog_posix.cpp - the game's log (platform/log.h) for the game code the
 * launcher shares, off Windows: the launcher has no log file, and its lines
 * (pclog.cpp: the debugger's output on Windows) are dropped - what fails is
 * said where it is used, on the standard output. */
#include "platform/log.h"

#include <cstdarg>

namespace pclog {

void vwrite(const char *, va_list) {}

void write(const char *, ...) {}

} // namespace pclog
