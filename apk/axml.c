/* Binary XML (AXML) parsing for AndroidManifest.xml. Only the fields in
 * struct henge_apk_manifest are extracted. Every offset is bounds-checked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ResChunk_header types */
#define RES_STRING_POOL 0x0001
#define RES_XML 0x0003
#define RES_XML_START_ELEMENT 0x0102
#define RES_XML_END_ELEMENT 0x0103
#define RES_XML_RESOURCE_MAP 0x0180

#define POOL_UTF8 0x100

/* Res_value data types */
#define TYPE_STRING 0x03
#define TYPE_FIRST_INT 0x10
#define TYPE_INT_BOOLEAN 0x12
#define TYPE_LAST_INT 0x1f

/* android:* attribute resource IDs */
#define ATTR_LABEL 0x01010001u
#define ATTR_NAME 0x01010003u
#define ATTR_HAS_CODE 0x0101000cu
#define ATTR_MIN_SDK 0x0101020cu
#define ATTR_VERSION_CODE 0x0101021bu
#define ATTR_VERSION_NAME 0x0101021cu
#define ATTR_VALUE 0x01010024u
#define ATTR_TARGET_SDK 0x01010270u

#define NO_INDEX 0xffffffffu

struct pool {
    const uint8_t *chunk;
    size_t size;           /* chunk size */
    const uint8_t *offsets; /* u32[count] */
    uint32_t count;
    uint32_t strings_start;
    int utf8;
};

struct axml {
    struct pool pool;
    const uint8_t *resmap; /* u32[resmap_count] */
    uint32_t resmap_count;
};

/* Appends code point c as UTF-8 to out (capacity cap incl. NUL). */
static void put_utf8(char *out, size_t cap, size_t *n, uint32_t c) {
    uint8_t b[4];
    size_t k;
    if (c < 0x80) { b[0] = (uint8_t)c; k = 1; }
    else if (c < 0x800) { b[0] = (uint8_t)(0xc0 | c >> 6); b[1] = (uint8_t)(0x80 | (c & 0x3f)); k = 2; }
    else if (c < 0x10000) {
        b[0] = (uint8_t)(0xe0 | c >> 12); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
        b[2] = (uint8_t)(0x80 | (c & 0x3f)); k = 3;
    } else {
        b[0] = (uint8_t)(0xf0 | c >> 18); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
        b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); b[3] = (uint8_t)(0x80 | (c & 0x3f)); k = 4;
    }
    if (*n + k >= cap) return; /* truncate at a code point boundary */
    memcpy(out + *n, b, k);
    *n += k;
}

/* Copies string idx into out as NUL-terminated UTF-8 (truncated to cap). Returns 0 or -1. */
static int pool_get(const struct pool *p, uint32_t idx, char *out, size_t cap) {
    if (cap == 0) return -1;
    out[0] = '\0';
    if (!p->chunk || idx >= p->count) return -1;
    uint64_t off = (uint64_t)p->strings_start + ha_u32(p->offsets + 4 * (size_t)idx);
    if (off >= p->size) return -1;
    const uint8_t *s = p->chunk + off, *end = p->chunk + p->size;
    size_t n = 0;
    if (p->utf8) {
        /* utf16 length (1 or 2 bytes), then utf8 length (1 or 2 bytes), then bytes */
        for (int field = 0; field < 2; field++) {
            if (s >= end) return -1;
            uint32_t len = *s++;
            if (len & 0x80) {
                if (s >= end) return -1;
                len = (len & 0x7f) << 8 | *s++;
            }
            if (field == 1) {
                if ((size_t)(end - s) < len) return -1;
                n = len < cap - 1 ? len : cap - 1;
                memcpy(out, s, n);
            }
        }
    } else {
        if (end - s < 2) return -1;
        uint32_t len = ha_u16(s);
        s += 2;
        if (len & 0x8000) {
            if (end - s < 2) return -1;
            len = (len & 0x7fff) << 16 | ha_u16(s);
            s += 2;
        }
        if ((size_t)(end - s) / 2 < len) return -1;
        for (uint32_t i = 0; i < len; i++) {
            uint32_t c = ha_u16(s + 2 * (size_t)i);
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < len) {
                uint32_t d = ha_u16(s + 2 * (size_t)(i + 1));
                if (d >= 0xdc00 && d < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
                    i++;
                }
            }
            put_utf8(out, cap, &n, c);
        }
    }
    out[n] = '\0';
    return 0;
}

static int parse_pool(struct pool *p, const uint8_t *c, size_t size, size_t hsize) {
    if (hsize < 28 || hsize > size) return -1;
    uint32_t count = ha_u32(c + 8);
    uint32_t flags = ha_u32(c + 16);
    uint32_t strings_start = ha_u32(c + 20);
    if (count > (size - hsize) / 4) return -1;
    p->chunk = c;
    p->size = size;
    p->offsets = c + hsize;
    p->count = count;
    p->strings_start = strings_start;
    p->utf8 = (flags & POOL_UTF8) != 0;
    return 0;
}

/* Attribute name match: by resource ID through the resource map, else by string. */
static int attr_is(const struct axml *x, uint32_t name_idx, uint32_t resid, const char *name) {
    if (name_idx < x->resmap_count) {
        uint32_t id = ha_u32(x->resmap + 4 * (size_t)name_idx);
        if (id != 0) return id == resid;
    }
    char buf[64];
    return pool_get(&x->pool, name_idx, buf, sizeof buf) == 0 && strcmp(buf, name) == 0;
}

struct attr {
    uint32_t name, raw;
    uint8_t type;
    uint32_t data;
};

/* String value of an attribute: the raw string, or a TYPE_STRING value. */
static int attr_str(const struct axml *x, const struct attr *a, char *out, size_t cap) {
    uint32_t idx = a->raw != NO_INDEX ? a->raw : (a->type == TYPE_STRING ? a->data : NO_INDEX);
    if (idx == NO_INDEX) {
        if (cap) out[0] = '\0';
        return -1;
    }
    return pool_get(&x->pool, idx, out, cap);
}

static int attr_int(const struct attr *a, int64_t *v) {
    if (a->type < TYPE_FIRST_INT || a->type > TYPE_LAST_INT) return -1;
    *v = a->type == TYPE_INT_BOOLEAN ? (a->data != 0) : (int64_t)(int32_t)a->data;
    return 0;
}

int henge_apk_read_manifest(const henge_apk *apk, struct henge_apk_manifest *m, char *err, size_t errlen) {
    memset(m, 0, sizeof *m);
    m->has_code = 1;
    long mi = henge_apk_find(apk, "AndroidManifest.xml");
    if (mi < 0) return ha_fail(err, errlen, "AndroidManifest.xml not found");
    uint8_t *buf;
    size_t len;
    if (henge_apk_extract(apk, (size_t)mi, &buf, &len, err, errlen) != 0) return -1;

    int rc = -1;
    struct axml x;
    memset(&x, 0, sizeof x);
    if (len < 8 || ha_u16(buf) != RES_XML || ha_u16(buf + 2) < 8 || ha_u16(buf + 2) > len) {
        ha_fail(err, errlen, "AndroidManifest.xml is not binary XML");
        goto out;
    }
    size_t total = ha_u32(buf + 4) < len ? ha_u32(buf + 4) : len;

    int depth = 0, native_depth = 0, have_lib_name = 0, seen_manifest = 0;
    for (size_t off = ha_u16(buf + 2); off + 8 <= total;) {
        const uint8_t *c = buf + off;
        uint16_t type = ha_u16(c), hsize = ha_u16(c + 2);
        uint32_t csize = ha_u32(c + 4);
        if (csize < 8 || hsize < 8 || hsize > csize || csize > total - off) {
            ha_fail(err, errlen, "AndroidManifest.xml: bad chunk at offset %zu", off);
            goto out;
        }
        off += csize;

        if (type == RES_STRING_POOL) {
            if (!x.pool.chunk && parse_pool(&x.pool, c, csize, hsize) != 0) {
                ha_fail(err, errlen, "AndroidManifest.xml: bad string pool");
                goto out;
            }
            continue;
        }
        if (type == RES_XML_RESOURCE_MAP) {
            x.resmap = c + hsize;
            x.resmap_count = (csize - hsize) / 4;
            continue;
        }
        if (type == RES_XML_END_ELEMENT) {
            if (depth == native_depth) native_depth = 0;
            if (depth > 0) depth--;
            continue;
        }
        if (type != RES_XML_START_ELEMENT) continue;

        /* ResXMLTree_node (16) + ResXMLTree_attrExt (20) */
        if (hsize < 16 || csize - hsize < 20) {
            ha_fail(err, errlen, "AndroidManifest.xml: truncated start element");
            goto out;
        }
        depth++;
        const uint8_t *ext = c + hsize;
        char elem[64];
        pool_get(&x.pool, ha_u32(ext + 4), elem, sizeof elem);
        uint16_t astart = ha_u16(ext + 8), asize = ha_u16(ext + 10), acount = ha_u16(ext + 12);
        if (asize < 20 || (uint64_t)hsize + astart + (uint64_t)asize * acount > csize) {
            ha_fail(err, errlen, "AndroidManifest.xml: <%s> attributes out of bounds", elem);
            goto out;
        }

        int is_manifest = depth == 1 && strcmp(elem, "manifest") == 0;
        int is_activity = strcmp(elem, "activity") == 0;
        int is_meta = native_depth && strcmp(elem, "meta-data") == 0;
        char meta_name[64] = "", meta_value[sizeof m->lib_name] = "";
        int meta_has_value = 0;
        if (is_manifest) seen_manifest = 1;

        for (uint16_t ai = 0; ai < acount; ai++) {
            const uint8_t *ap = ext + astart + (size_t)ai * asize;
            struct attr a = {ha_u32(ap + 4), ha_u32(ap + 8), ap[15], ha_u32(ap + 16)};
            int64_t v;
            if (is_manifest) {
                if (attr_is(&x, a.name, 0, "package")) attr_str(&x, &a, m->package, sizeof m->package);
                else if (attr_is(&x, a.name, ATTR_VERSION_CODE, "versionCode") && attr_int(&a, &v) == 0)
                    m->version_code = v;
                else if (attr_is(&x, a.name, ATTR_VERSION_NAME, "versionName"))
                    attr_str(&x, &a, m->version_name, sizeof m->version_name);
            } else if (strcmp(elem, "uses-sdk") == 0) {
                if (attr_is(&x, a.name, ATTR_MIN_SDK, "minSdkVersion") && attr_int(&a, &v) == 0)
                    m->min_sdk = (int)v;
                else if (attr_is(&x, a.name, ATTR_TARGET_SDK, "targetSdkVersion") && attr_int(&a, &v) == 0)
                    m->target_sdk = (int)v;
            } else if (strcmp(elem, "application") == 0) {
                if (attr_is(&x, a.name, ATTR_HAS_CODE, "hasCode") && attr_int(&a, &v) == 0)
                    m->has_code = v != 0;
                else if (attr_is(&x, a.name, ATTR_LABEL, "label"))
                    attr_str(&x, &a, m->label, sizeof m->label);
            } else if (is_activity) {
                char name[64];
                if (attr_is(&x, a.name, ATTR_NAME, "name") && attr_str(&x, &a, name, sizeof name) == 0 &&
                    strcmp(name, "android.app.NativeActivity") == 0 && !native_depth) {
                    native_depth = depth;
                    m->native_activity = 1;
                }
            } else if (is_meta) {
                if (attr_is(&x, a.name, ATTR_NAME, "name")) attr_str(&x, &a, meta_name, sizeof meta_name);
                else if (attr_is(&x, a.name, ATTR_VALUE, "value"))
                    meta_has_value = attr_str(&x, &a, meta_value, sizeof meta_value) == 0;
            }
        }
        if (is_meta && meta_has_value && !have_lib_name && strcmp(meta_name, "android.app.lib_name") == 0) {
            memcpy(m->lib_name, meta_value, sizeof m->lib_name);
            have_lib_name = 1;
        }
    }
    if (!seen_manifest || !m->package[0]) {
        ha_fail(err, errlen, "AndroidManifest.xml: no <manifest package>");
        goto out;
    }
    if (m->native_activity && !have_lib_name) snprintf(m->lib_name, sizeof m->lib_name, "main");
    rc = 0;
out:
    free(buf);
    return rc;
}
