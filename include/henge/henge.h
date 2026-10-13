/* Shared contract between henge-convert and the runtime. See docs/m1-elf-to-macho.md. */
#ifndef HENGE_H
#define HENGE_H

#include <stddef.h>
#include <stdint.h>

/* Every ELF vaddr V lives at Mach-O vmaddr V + HENGE_SHIFT (the Mach-O header area). */
#define HENGE_SHIFT 0x4000u
#define HENGE_PAGE 0x4000u

/* ---- converter (convert/) ---- */

enum henge_platform {
    HENGE_PLATFORM_IOS = 2,   /* PLATFORM_IOS */
    HENGE_PLATFORM_MACOS = 1, /* PLATFORM_MACOS; used by tests on Apple Silicon Macs */
    HENGE_PLATFORM_IOSSIM = 7, /* PLATFORM_IOSSIMULATOR; arm64 iOS Simulator on Apple Silicon */
};

struct henge_convert_opts {
    enum henge_platform platform;
    const char *install_name; /* LC_ID_DYLIB, e.g. "@rpath/<package>/libadd.dylib" */
    int link_libsystem;       /* nonzero: add LC_LOAD_DYLIB /usr/lib/libSystem.B.dylib (V5 fallback) */
};

struct henge_convert_result {
    uint8_t *macho; /* malloc'd; caller frees */
    size_t macho_size;
    char *manifest_json; /* malloc'd; caller frees */
    unsigned svc_count;
    unsigned tpidr_count;
};

/* Returns 0 on success. On failure, writes a message to err and returns -1. */
int henge_convert(const uint8_t *elf, size_t elf_size, const struct henge_convert_opts *opts,
                  struct henge_convert_result *out, char *err, size_t errlen);

/* ---- runtime (runtime/) ---- */

typedef struct henge_image henge_image;

/* Call after dlopen(path). Finds the image in dyld's list, resolves NEEDED, applies
 * relocations, and runs DT_INIT / DT_INIT_ARRAY. Returns NULL on failure (see err). */
henge_image *henge_link(const char *path, char *err, size_t errlen);

/* Looks up an exported symbol via DT_GNU_HASH (or DT_HASH). */
void *henge_dlsym(henge_image *image, const char *name);

#endif
