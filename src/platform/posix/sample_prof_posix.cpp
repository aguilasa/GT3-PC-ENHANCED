/* sample_prof_posix.cpp - the sampling profiler off Windows (sample_prof.h):
 * not there yet.  GT3PC_SAMPLE says so once in the log; nothing runs. */
#include "platform/sample_prof.h"
#include "platform/log.h"

#include <stdlib.h>

#include <atomic>

namespace sample_prof {

void register_thread(const char *name)
{
    static std::atomic<bool> told{false};
    if (getenv("GT3PC_SAMPLE") != nullptr && !told.exchange(true))
        pclog::write("sample prof: not available on this system (GT3PC_SAMPLE ignored; %s not sampled)", name);
}

} // namespace sample_prof
