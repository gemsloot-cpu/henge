/* Private interfaces shared by the runtime's translation units (not installed).
 *
 *   linker.c   image discovery, dynamic-section parsing, DT_NEEDED, initializers, henge_link
 *   reloc.c    RELA / JMPREL / RELR application
 *   symbols.c  hash-table symbol lookup, henge_dlsym, global resolution
 *   thread.c   thread pointer for patched tpidr_el0 reads, ELF TLS (TLSDESC)
 *   syscall.c  Linux syscalls for patched svc #0
 *   entry.S    register-preserving entry points called from the patch island
 *   linux.c    Linux <-> Darwin constants
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
    uint64_t nsyms;       /* .dynsym entries if known (DT_HASH), else 0 */
    struct hr_tls_module *tls; /* PT_TLS, or NULL */
    const Elf64_Phdr *phdr;
    uint16_t phnum;
    uintptr_t end;        /* base + end of the last PT_LOAD */
    struct henge_image **deps; /* linked DT_NEEDED images (shim-backed ones are not listed) */
    size_t ndeps;
    struct henge_image *next;
};

/* Linked images in load order (head = first loaded). Guarded by hr_lock. */
extern henge_image *hr_images;
void hr_lock(void);
void hr_unlock(void);

/* linker.c: dlopen + link by path or soname (caller holds hr_lock). */
henge_image *hr_load_locked(const char *name, char *err, size_t errlen);
/* Nonzero if `name` (e.g. "libm.so") is a system library backed by the shim. */
int hr_is_system_lib(const char *name);
/* The linked image containing addr, or NULL. */
henge_image *hr_image_for_addr(uintptr_t addr);

/* Formats a message into err (if non-NULL) and returns -1. */
int hr_fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* symbols.c */
const Elf64_Sym *hr_lookup_sym(const henge_image *im, const char *name);
/* .dynsym entry count from DT_HASH or by walking DT_GNU_HASH. */
uint64_t hr_symbol_count(const henge_image *im);
/* Runtime address of a defined symbol; calls the resolver for STT_GNU_IFUNC. */
uintptr_t hr_sym_addr(const henge_image *im, const Elf64_Sym *s);
/* Global lookup for an undefined symbol: shim first, then linked images. Returns 1 if found. */
int hr_resolve_global(const char *name, uintptr_t *out);

/* thread.c */
struct hr_tls_module;
int hr_thread_init(char *err, size_t errlen);
uintptr_t hr_tp_key_offset(void);
void *hr_tp_get(void);
extern uintptr_t hr_stack_guard;
int hr_tls_register(henge_image *im, const Elf64_Phdr *tls, char *err, size_t errlen);
void *hr_tlsdesc_arg(struct hr_tls_module *mod, uint64_t offset);
intptr_t hr_tlsdesc_resolve(const uintptr_t *desc);

/* cpu.c */
void hr_hwcap(uint64_t *hwcap, uint64_t *hwcap2);

/* entry.S */
void hr_svc_entry(void);
void hr_tp_slow_entry(void);
void hr_tlsdesc_entry(void);

/* syscall.c */
long hr_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr);

/* reloc.c */
uintptr_t hr_call_ifunc(uintptr_t resolver);
/* irelative_pass 0: everything but IRELATIVE; 1: IRELATIVE only. */
int hr_apply_rela(henge_image *im, const Elf64_Rela *rel, size_t count, uint64_t nsyms,
                  int irelative_pass, char *err, size_t errlen);
/* Decodes DT_ANDROID_RELA (APS2) into a malloc'd RELA array. */
int hr_decode_aps2(const uint8_t *data, size_t size, Elf64_Rela **out, size_t *count, char *err,
                   size_t errlen);
int hr_apply_relr(henge_image *im, const uint64_t *relr, size_t count, char *err, size_t errlen);

#endif
