/* Shared contract between henge-convert and the runtime. See docs/m1-elf-to-macho.md. */
#ifndef HENGE_H
#define HENGE_H

#include <stddef.h>
#include <stdint.h>

/* Every ELF vaddr V lives at Mach-O vmaddr V + H, where H is the address of the __TEXT,__elf
 * section. H is HENGE_SHIFT unless the image has a patch island, which grows the header area
 * to a larger multiple of HENGE_PAGE. */
#define HENGE_SHIFT 0x4000u
#define HENGE_PAGE 0x4000u

/* ---- patch island (converter -> runtime contract, docs/m2-patching.md) ----
 * Images whose code was patched carry a __TEXT,__henge section that starts with this header,
 * followed by the trampolines. The slots live in __DATA after the ELF image and are filled
 * by henge_link before any patched code runs. */
#define HENGE_ISLAND_MAGIC 0x45474e48u /* "HNGE" */
#define HENGE_ISLAND_VERSION 1u

enum henge_slot {
    HENGE_SLOT_SVC = 0,    /* address of the runtime's Linux syscall entry */
    HENGE_SLOT_TP_KEY = 1, /* byte offset of the thread-pointer key in Darwin's TSD array */
    HENGE_SLOT_TP_SLOW = 2, /* address of the runtime's thread-pointer allocator */
    HENGE_NSLOTS = 4,
};

struct henge_island_hdr {
    uint32_t magic;
    uint32_t version;
    uint64_t slots_vmaddr; /* Mach-O vmaddr (relative to the header) of HENGE_NSLOTS words */
    uint32_t svc_sites;
    uint32_t tpidr_sites;
    uint32_t reserved[2];
};

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

struct henge_import {
    char *name;
    int weak;
};

struct henge_convert_result {
    uint8_t *macho; /* free with henge_convert_result_free */
    size_t macho_size;
    char *manifest_json;
    unsigned svc_count;   /* svc #0, each rewritten to call the runtime */
    unsigned tpidr_count; /* mrs xN, tpidr_el0, each rewritten to load henge's thread pointer */
    unsigned x18_count;   /* instructions using x18 (Darwin's platform register); warning only */
    int shadow_call_stack; /* the x18 uses look like Shadow Call Stack prologues */
    int has_tls;          /* PT_TLS present */
    struct henge_import *imports; /* undefined dynamic symbols */
    size_t nimports;
};

/* Returns 0 on success. On failure, writes a message to err and returns -1. */
int henge_convert(const uint8_t *elf, size_t elf_size, const struct henge_convert_opts *opts,
                  struct henge_convert_result *out, char *err, size_t errlen);
void henge_convert_result_free(struct henge_convert_result *res);

/* ---- runtime (runtime/) ---- */

typedef struct henge_image henge_image;

/* Call after dlopen(path). Finds the image in dyld's list, resolves NEEDED, applies
 * relocations, and runs DT_INIT / DT_INIT_ARRAY. Returns NULL on failure (see err). */
henge_image *henge_link(const char *path, char *err, size_t errlen);

/* Looks up an exported symbol via DT_GNU_HASH (or DT_HASH). */
void *henge_dlsym(henge_image *image, const char *name);

/* Nonzero if the bionic shim implements `name` (for compatibility reports). */
int henge_shim_provides(const char *name);

#endif
