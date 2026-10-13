/* Private interfaces shared by the APK reader's translation units (not installed).
 *
 *   zip.c   ZIP central directory, entry extraction (stored / deflate)
 *   axml.c  binary XML (AndroidManifest.xml) parsing
 */
#ifndef HENGE_APK_INTERNAL_H
#define HENGE_APK_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "henge/apk.h"

struct apk_entry {
    char *name;
    uint16_t flags, method;
    uint32_t crc;
    uint64_t csize, usize, lho; /* compressed size, uncompressed size, local header offset */
};

struct henge_apk {
    const uint8_t *data;
    size_t size;
    int mapped; /* data is our mmap */
    struct apk_entry *entries;
    size_t count;
};

/* Formats a message into err (if non-NULL) and returns -1. */
int ha_fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static inline uint16_t ha_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t ha_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t ha_u64(const uint8_t *p) { return ha_u32(p) | (uint64_t)ha_u32(p + 4) << 32; }

#endif
