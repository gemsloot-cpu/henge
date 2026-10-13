/* ZIP container: central directory parsing and entry extraction (stored / deflate).
 * Supports ZIP64 end-of-central-directory records and extra fields. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

#include "internal.h"

#define SIG_LOCAL 0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD 0x06054b50u
#define SIG_EOCD64 0x06064b50u
#define SIG_EOCD64_LOC 0x07064b50u

#define EOCD_SIZE 22
#define CENTRAL_SIZE 46
#define LOCAL_SIZE 30

int ha_fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- end of central directory ---- */

static const uint8_t *find_eocd(const uint8_t *d, size_t size) {
    if (size < EOCD_SIZE) return NULL;
    size_t lo = size - EOCD_SIZE > 0xffff ? size - EOCD_SIZE - 0xffff : 0;
    const uint8_t *loose = NULL;
    for (size_t off = size - EOCD_SIZE + 1; off-- > lo;) {
        if (ha_u32(d + off) != SIG_EOCD) continue;
        size_t clen = ha_u16(d + off + 20);
        if (off + EOCD_SIZE + clen == size) return d + off; /* comment ends exactly at EOF */
        if (!loose && off + EOCD_SIZE + clen <= size) loose = d + off;
    }
    return loose;
}

static int read_cd_bounds(const henge_apk *a, uint64_t *count, uint64_t *cd_off, uint64_t *cd_size,
                          char *err, size_t errlen) {
    const uint8_t *eocd = find_eocd(a->data, a->size);
    if (!eocd) return ha_fail(err, errlen, "not a ZIP file (no end of central directory record)");
    if (ha_u16(eocd + 4) != 0 || ha_u16(eocd + 6) != 0)
        return ha_fail(err, errlen, "multi-disk ZIP archives are not supported");
    *count = ha_u16(eocd + 10);
    *cd_size = ha_u32(eocd + 12);
    *cd_off = ha_u32(eocd + 16);
    if (*count != 0xffff && *cd_size != 0xffffffffu && *cd_off != 0xffffffffu) return 0;

    /* ZIP64: the locator sits right before the EOCD record. */
    size_t eocd_off = (size_t)(eocd - a->data);
    if (eocd_off < 20 || ha_u32(eocd - 20) != SIG_EOCD64_LOC)
        return ha_fail(err, errlen, "ZIP64 end of central directory locator missing");
    uint64_t z = ha_u64(eocd - 20 + 8);
    if (z > a->size || a->size - z < 56 || ha_u32(a->data + z) != SIG_EOCD64)
        return ha_fail(err, errlen, "bad ZIP64 end of central directory record");
    const uint8_t *r = a->data + z;
    *count = ha_u64(r + 32);
    *cd_size = ha_u64(r + 40);
    *cd_off = ha_u64(r + 48);
    return 0;
}

/* Apply a ZIP64 extended-information extra field (id 0x0001) to the 0xffffffff fields. */
static int apply_zip64_extra(struct apk_entry *e, const uint8_t *x, size_t xlen, int need_u, int need_c,
                             int need_o) {
    while (xlen >= 4) {
        uint16_t id = ha_u16(x), n = ha_u16(x + 2);
        if ((size_t)n > xlen - 4) return -1;
        if (id == 0x0001) {
            const uint8_t *p = x + 4, *end = p + n;
            if (need_u) { if (end - p < 8) return -1; e->usize = ha_u64(p); p += 8; }
            if (need_c) { if (end - p < 8) return -1; e->csize = ha_u64(p); p += 8; }
            if (need_o) { if (end - p < 8) return -1; e->lho = ha_u64(p); }
            return 0;
        }
        x += 4 + n;
        xlen -= 4 + (size_t)n;
    }
    return (need_u || need_c || need_o) ? -1 : 0;
}

static int read_central_directory(henge_apk *a, char *err, size_t errlen) {
    uint64_t count, cd_off, cd_size;
    if (read_cd_bounds(a, &count, &cd_off, &cd_size, err, errlen) != 0) return -1;
    if (cd_off > a->size || cd_size > a->size - cd_off)
        return ha_fail(err, errlen, "central directory extends past end of file");
    if (count > cd_size / CENTRAL_SIZE) return ha_fail(err, errlen, "central directory entry count is too large");

    a->entries = calloc(count ? (size_t)count : 1, sizeof *a->entries);
    if (!a->entries) return ha_fail(err, errlen, "out of memory");

    const uint8_t *p = a->data + cd_off, *end = p + cd_size;
    for (uint64_t i = 0; i < count; i++) {
        if (end - p < CENTRAL_SIZE || ha_u32(p) != SIG_CENTRAL)
            return ha_fail(err, errlen, "bad central directory entry #%llu", (unsigned long long)i);
        size_t nlen = ha_u16(p + 28), xlen = ha_u16(p + 30), clen = ha_u16(p + 32);
        if ((size_t)(end - p) - CENTRAL_SIZE < nlen + xlen + clen)
            return ha_fail(err, errlen, "central directory entry #%llu is truncated", (unsigned long long)i);

        struct apk_entry *e = &a->entries[a->count];
        e->flags = ha_u16(p + 8);
        e->method = ha_u16(p + 10);
        e->crc = ha_u32(p + 16);
        e->csize = ha_u32(p + 20);
        e->usize = ha_u32(p + 24);
        e->lho = ha_u32(p + 42);
        if (apply_zip64_extra(e, p + CENTRAL_SIZE + nlen, xlen, e->usize == 0xffffffffu,
                              e->csize == 0xffffffffu, e->lho == 0xffffffffu) != 0)
            return ha_fail(err, errlen, "entry #%llu: bad ZIP64 extra field", (unsigned long long)i);
        e->name = malloc(nlen + 1);
        if (!e->name) return ha_fail(err, errlen, "out of memory");
        memcpy(e->name, p + CENTRAL_SIZE, nlen);
        e->name[nlen] = '\0';
        a->count++;
        p += CENTRAL_SIZE + nlen + xlen + clen;
    }
    return 0;
}

/* ---- open / close ---- */

static henge_apk *open_common(henge_apk *a, char *err, size_t errlen) {
    if (read_central_directory(a, err, errlen) != 0) {
        henge_apk_close(a);
        return NULL;
    }
    return a;
}

henge_apk *henge_apk_open_mem(const uint8_t *data, size_t size, char *err, size_t errlen) {
    henge_apk *a = calloc(1, sizeof *a);
    if (!a) {
        ha_fail(err, errlen, "out of memory");
        return NULL;
    }
    a->data = data;
    a->size = size;
    return open_common(a, err, errlen);
}

henge_apk *henge_apk_open(const char *path, char *err, size_t errlen) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ha_fail(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        ha_fail(err, errlen, "%s: not a non-empty regular file", path);
        return NULL;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        ha_fail(err, errlen, "%s: mmap failed: %s", path, strerror(errno));
        return NULL;
    }
    henge_apk *a = calloc(1, sizeof *a);
    if (!a) {
        munmap(m, (size_t)st.st_size);
        ha_fail(err, errlen, "out of memory");
        return NULL;
    }
    a->data = m;
    a->size = (size_t)st.st_size;
    a->mapped = 1;
    return open_common(a, err, errlen);
}

void henge_apk_close(henge_apk *a) {
    if (!a) return;
    for (size_t i = 0; i < a->count; i++) free(a->entries[i].name);
    free(a->entries);
    if (a->mapped) munmap((void *)a->data, a->size);
    free(a);
}

/* ---- lookup ---- */

size_t henge_apk_count(const henge_apk *a) { return a ? a->count : 0; }

const char *henge_apk_name(const henge_apk *a, size_t i) { return a && i < a->count ? a->entries[i].name : NULL; }

long henge_apk_find(const henge_apk *a, const char *name) {
    if (!a || !name) return -1;
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->entries[i].name, name) == 0) return (long)i;
    return -1;
}

int henge_apk_native_libs(const henge_apk *a, const char *abi, size_t **idx, size_t *n) {
    *idx = NULL;
    *n = 0;
    char prefix[128];
    if (!a || !abi || snprintf(prefix, sizeof prefix, "lib/%s/", abi) >= (int)sizeof prefix) return 0;
    size_t plen = strlen(prefix), cap = 0;
    for (size_t i = 0; i < a->count; i++) {
        const char *name = a->entries[i].name;
        size_t len = strlen(name);
        if (strncmp(name, prefix, plen) != 0 || strchr(name + plen, '/') || len < plen + 4 ||
            strcmp(name + len - 3, ".so") != 0)
            continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 8;
            size_t *ni = realloc(*idx, cap * sizeof *ni);
            if (!ni) {
                free(*idx);
                *idx = NULL;
                *n = 0;
                return -1;
            }
            *idx = ni;
        }
        (*idx)[(*n)++] = i;
    }
    return 0;
}

/* ---- extraction ---- */

static int inflate_raw(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen, char *err, size_t errlen,
                       const char *name) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return ha_fail(err, errlen, "%s: inflateInit failed", name);
    int rc = Z_OK;
    size_t in_left = slen, out_left = dlen;
    zs.next_in = (Bytef *)(uintptr_t)src;
    zs.next_out = dst;
    /* uInt is 32 bits; feed in chunks so >4 GiB inputs can't wrap. */
    while (rc == Z_OK) {
        if (zs.avail_in == 0 && in_left) {
            zs.avail_in = in_left > 0x40000000u ? 0x40000000u : (uInt)in_left;
            in_left -= zs.avail_in;
        }
        if (zs.avail_out == 0 && out_left) {
            zs.avail_out = out_left > 0x40000000u ? 0x40000000u : (uInt)out_left;
            out_left -= zs.avail_out;
        }
        uInt before_in = zs.avail_in, before_out = zs.avail_out;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_BUF_ERROR && zs.avail_in == before_in && zs.avail_out == before_out) break;
        if (rc == Z_BUF_ERROR) rc = Z_OK;
    }
    uLong total = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return ha_fail(err, errlen, "%s: corrupt deflate data", name);
    if (total != dlen) return ha_fail(err, errlen, "%s: inflated size does not match the header", name);
    return 0;
}

int henge_apk_extract(const henge_apk *a, size_t i, uint8_t **out, size_t *size, char *err, size_t errlen) {
    *out = NULL;
    *size = 0;
    if (!a || i >= a->count) return ha_fail(err, errlen, "entry index %zu out of range", i);
    const struct apk_entry *e = &a->entries[i];
    if (e->flags & 1) return ha_fail(err, errlen, "%s: encrypted entries are not supported", e->name);
    if (e->method != 0 && e->method != 8)
        return ha_fail(err, errlen, "%s: unsupported compression method %u", e->name, e->method);
    if (e->usize > HENGE_APK_MAX_ENTRY)
        return ha_fail(err, errlen, "%s: entry too large (%llu bytes)", e->name, (unsigned long long)e->usize);
    if (e->method == 0 && e->csize != e->usize)
        return ha_fail(err, errlen, "%s: stored entry has mismatched sizes", e->name);

    if (e->lho > a->size || a->size - e->lho < LOCAL_SIZE || ha_u32(a->data + e->lho) != SIG_LOCAL)
        return ha_fail(err, errlen, "%s: bad local file header", e->name);
    const uint8_t *lh = a->data + e->lho;
    uint64_t data_off = e->lho + LOCAL_SIZE + ha_u16(lh + 26) + ha_u16(lh + 28);
    if (data_off > a->size || e->csize > a->size - data_off)
        return ha_fail(err, errlen, "%s: entry data extends past end of file", e->name);
    const uint8_t *src = a->data + data_off;

    uint8_t *buf = malloc(e->usize ? (size_t)e->usize : 1);
    if (!buf) return ha_fail(err, errlen, "out of memory");
    if (e->method == 0) {
        memcpy(buf, src, (size_t)e->usize);
    } else if (inflate_raw(src, (size_t)e->csize, buf, (size_t)e->usize, err, errlen, e->name) != 0) {
        free(buf);
        return -1;
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    for (uint64_t off = 0; off < e->usize;) {
        uInt n = e->usize - off > 0x40000000u ? 0x40000000u : (uInt)(e->usize - off);
        crc = crc32(crc, buf + off, n);
        off += n;
    }
    if ((uint32_t)crc != e->crc) {
        free(buf);
        return ha_fail(err, errlen, "%s: CRC-32 mismatch", e->name);
    }
    *out = buf;
    *size = (size_t)e->usize;
    return 0;
}
