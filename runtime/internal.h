/* Private interfaces shared by the runtime's translation units (not installed).
 *
 *   linker.c   image discovery, dynamic-section parsing, DT_NEEDED, initializers, henge_link
 *   reloc.c    RELA / JMPREL / RELR application
 *   symbols.c  hash-table symbol lookup, henge_dlsym, global resolution
 *   shim.c     bionic compatibility shim
 */
#ifndef HENGE_RUNTIME_INTERNAL_H
#define HENGE_RUNTIME_INTERNAL_H

#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE /* realpath, PATH_MAX under -std=c11 */
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "henge/elf64.h"
#include "henge/henge.h"

struct henge_image {
    char path[PATH_MAX];
    const char *soname; /* DT_SONAME, may be NULL */
    uintptr_t base;     /* runtime address of ELF vaddr 0 */
    const Elf64_Dyn *dynamic;
    const Elf64_Sym *symtab;
    const char *strtab;
    uint64_t strsz;
    const uint32_t *gnu_hash;
    const uint32_t *hash;
    uintptr_t w_lo, w_hi; /* writable range [w_lo, w_hi) */
    struct henge_image *next;
};

/* Linked images in load order (head = first loaded). Guarded by linker.c's lock. */
extern henge_image *hr_images;

/* Formats a message into err (if non-NULL) and returns -1. */
int hr_fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* symbols.c */
const Elf64_Sym *hr_lookup_sym(const henge_image *im, const char *name);
/* Global lookup for an undefined symbol: shim first, then linked images. Returns 1 if found. */
int hr_resolve_global(const char *name, uintptr_t *out);

/* reloc.c */
int hr_apply_rela(henge_image *im, const Elf64_Rela *rel, size_t count, uint64_t nsyms, char *err,
                  size_t errlen);
int hr_apply_relr(henge_image *im, const uint64_t *relr, size_t count, char *err, size_t errlen);

#endif
