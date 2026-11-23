/* disc.cpp - the game's "disc" in GT3 PC (disc.h). */
#include "platform/disc.h"
#include "platform/log.h"

#include "platform/os/fs.h"

namespace disc {
namespace {

fs::File g_file;
uint64_t g_size = 0;

} // namespace

bool open(const char *volumePath)
{
    if (!g_file.open(fs::native_to_utf8(volumePath), fs::kRead, fs::Create::open_existing, fs::kShareRead,
                     fs::kRandomAccess)) {
        pclog::write("disc: cannot open %s (error %lu)", volumePath, fs::last_error());
        return false;
    }
    const int64_t sz = g_file.size();
    g_size = sz > 0 ? (uint64_t)sz : 0;
    pclog::write("disc: %s, %llu bytes", volumePath, (unsigned long long)g_size);
    return true;
}

uint64_t size()
{
    return g_size;
}

bool read_bytes(uint64_t offset, void *buf, uint32_t bytes)
{
    if (!g_file.is_open())
        return false;
    uint8_t *dst = (uint8_t *)buf;
    while (bytes > 0) {
        const int got = g_file.read_at(offset, dst, bytes);
        if (got <= 0) {
            pclog::write("disc: read of %u bytes at 0x%llX failed", bytes, (unsigned long long)offset);
            return false;
        }
        dst += got;
        offset += (uint32_t)got;
        bytes -= (uint32_t)got;
    }
    return true;
}

} // namespace disc
