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
    struct henge_image *next;
};

/* Linked images in load order (head = first loaded). Guarded by linker.c's lock. */
extern henge_image *hr_images;

/* Formats a message into err (if non-NULL) and returns -1. */
int hr_fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* symbols.c */
const Elf64_Sym *hr_lookup_sym(const henge_image *im, const char *name);
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
int hr_apply_relr(henge_image *im, const uint64_t *relr, size_t count, char *err, size_t errlen);

#endif
