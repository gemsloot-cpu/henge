/* APK reader: ZIP container + binary AndroidManifest.xml (apk/). Depends only on zlib.
 * All input is treated as untrusted. */
#ifndef HENGE_APK_H
#define HENGE_APK_H

#include <stddef.h>
#include <stdint.h>

typedef struct henge_apk henge_apk;

/* Largest entry henge_apk_extract will inflate. */
#define HENGE_APK_MAX_ENTRY (1024u * 1024u * 1024u)

/* Opens (mmaps) an APK. Returns NULL on failure (see err). */
henge_apk *henge_apk_open(const char *path, char *err, size_t errlen);
/* Same, over a caller-owned buffer that must outlive the handle. */
henge_apk *henge_apk_open_mem(const uint8_t *data, size_t size, char *err, size_t errlen);
void henge_apk_close(henge_apk *apk);

/* Central-directory entries, in archive order. */
size_t henge_apk_count(const henge_apk *apk);
const char *henge_apk_name(const henge_apk *apk, size_t i); /* NULL if i is out of range */
long henge_apk_find(const henge_apk *apk, const char *name); /* index, or -1 */

/* Decompresses entry i (stored or deflate) and checks its CRC-32. *out is malloc'd
 * (caller frees). Returns 0 on success, -1 on failure (see err). */
int henge_apk_extract(const henge_apk *apk, size_t i, uint8_t **out, size_t *size, char *err,
                      size_t errlen);

/* Indices of the lib/<abi>/<name>.so entries (no subdirectories). *idx is malloc'd (caller frees;
 * NULL when *n is 0). Returns 0, or -1 when out of memory. */
int henge_apk_native_libs(const henge_apk *apk, const char *abi, size_t **idx, size_t *n);

struct henge_apk_manifest {
    char package[256];
    char version_name[128];
    int64_t version_code;
    int min_sdk, target_sdk; /* 0 if absent or a codename */
    int has_code;            /* <application android:hasCode>, default 1 */
    int native_activity;     /* 1 if an <activity> is android.app.NativeActivity */
    char lib_name[256];      /* its android.app.lib_name meta-data; "main" if absent */
    char label[256];         /* <application android:label> if a literal string, else empty */
};

/* Parses the binary AndroidManifest.xml. Returns 0 on success, -1 on failure (see err). */
int henge_apk_read_manifest(const henge_apk *apk, struct henge_apk_manifest *m, char *err,
                            size_t errlen);

#endif
