/* henge mini linker (M1). See docs/m1-elf-to-macho.md §5 and docs/architecture.md §5.
 *
 * dyld maps the converted dylib but performs no fixups. This file finds the image,
 * locates the original ELF headers inside it, resolves DT_NEEDED, applies the ELF
 * dynamic relocations and runs the initializers.
 */
#define _DARWIN_C_SOURCE
#include "henge/elf64.h"
#include "henge/henge.h"
#include "shim.h"

#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER; /* initializers may call henge_link */
static henge_image *g_images; /* load order (head = first loaded) */

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- symbol lookup ---- */

static uint32_t gnu_hash_name(const char *s)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = h * 33 + *p;
    return h;
}

static uint32_t elf_hash_name(const char *s)
{
    uint32_t h = 0, g;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h = (h << 4) + *p;
        g = h & 0xf0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static int sym_exported(const henge_image *im, const Elf64_Sym *s, const char *name)
{
    unsigned bind = ELF64_ST_BIND(s->st_info);
    if (s->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK))
        return 0;
    if (s->st_name >= im->strsz)
        return 0;
    return strcmp(im->strtab + s->st_name, name) == 0;
}

static const Elf64_Sym *lookup_sym(const henge_image *im, const char *name)
{
    if (im->gnu_hash) {
        const uint32_t *h = im->gnu_hash;
        uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2], bloom_shift = h[3];
        const uint64_t *bloom = (const uint64_t *)(h + 4);
        const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
        const uint32_t *chain = buckets + nbuckets;
        uint32_t hv = gnu_hash_name(name);

        if (nbuckets == 0 || bloom_size == 0)
            return NULL;
        uint64_t word = bloom[(hv / 64) % bloom_size];
        uint64_t mask = (1ull << (hv % 64)) | (1ull << ((hv >> bloom_shift) % 64));
        if ((word & mask) != mask)
            return NULL;
        uint32_t i = buckets[hv % nbuckets];
        if (i < symoffset)
            return NULL;
        for (;; i++) {
            uint32_t ch = chain[i - symoffset];
            if ((ch | 1) == (hv | 1) && sym_exported(im, &im->symtab[i], name))
                return &im->symtab[i];
            if (ch & 1)
                break;
        }
        return NULL;
    }
    if (im->hash) {
        uint32_t nbucket = im->hash[0];
        const uint32_t *bucket = im->hash + 2;
        const uint32_t *chain = bucket + nbucket;
        if (nbucket == 0)
            return NULL;
        for (uint32_t i = bucket[elf_hash_name(name) % nbucket]; i != 0; i = chain[i]) {
            if (sym_exported(im, &im->symtab[i], name))
                return &im->symtab[i];
        }
    }
    return NULL;
}

void *henge_dlsym(henge_image *image, const char *name)
{
    if (!image || !name)
        return NULL;
    const Elf64_Sym *s = lookup_sym(image, name);
    return s ? (void *)(image->base + s->st_value) : NULL;
}

/* Global lookup for an undefined symbol: shim first, then linked images in load order. */
static int resolve_global(const char *name, uintptr_t *out)
{
    void *p = henge_shim_lookup(name);
    if (p) {
        *out = (uintptr_t)p;
        return 1;
    }
    for (henge_image *im = g_images; im; im = im->next) {
        const Elf64_Sym *s = lookup_sym(im, name);
        if (s) {
            *out = im->base + s->st_value;
            return 1;
        }
    }
    return 0;
}

/* ---- DT_NEEDED ---- */

static const char *const k_system_libs[] = {
    "libc.so",     "libm.so",       "libdl.so",      "liblog.so",       "libandroid.so",
    "libEGL.so",   "libGLESv2.so",  "libGLESv3.so",  "libOpenSLES.so",  "libaaudio.so",
    "libz.so",     "libstdc++.so",
};

static int needed_satisfied(const char *needed)
{
    for (size_t i = 0; i < sizeof k_system_libs / sizeof k_system_libs[0]; i++)
        if (strcmp(needed, k_system_libs[i]) == 0)
            return 1; /* backed by the shim */
    for (henge_image *im = g_images; im; im = im->next) {
        if (im->soname && strcmp(im->soname, needed) == 0)
            return 1;
        /* Fall back to comparing basenames, ignoring the extension (.so vs .dylib). */
        const char *b = strrchr(im->path, '/');
        b = b ? b + 1 : im->path;
        size_t n = strcspn(needed, ".");
        if (n > 0 && strncmp(b, needed, n) == 0 && b[n] == '.')
            return 1;
    }
    return 0;
}

/* ---- relocation ---- */

static int check_target(const henge_image *im, uintptr_t p, char *err, size_t errlen)
{
    if (p < im->w_lo || p + 8 > im->w_hi || (p & 7) != 0)
        return fail(err, errlen, "relocation target 0x%llx (vaddr 0x%llx) outside writable segment",
                    (unsigned long long)p, (unsigned long long)(p - im->base));
    return 0;
}

static int symbol_value(henge_image *im, uint32_t symidx, uint64_t nsyms, uintptr_t *out,
                        char *err, size_t errlen)
{
    if (symidx == 0) {
        *out = 0;
        return 0;
    }
    if (nsyms && symidx >= nsyms)
        return fail(err, errlen, "symbol index %u out of range", symidx);
    const Elf64_Sym *s = &im->symtab[symidx];
    if (s->st_name >= im->strsz)
        return fail(err, errlen, "bad symbol name offset");
    const char *name = im->strtab + s->st_name;
    if (ELF64_ST_TYPE(s->st_info) == 6 /* STT_TLS */)
        return fail(err, errlen, "TLS symbol %s: not supported until M2", name);
    if (s->st_shndx != SHN_UNDEF) {
        *out = im->base + s->st_value;
        return 0;
    }
    if (resolve_global(name, out))
        return 0;
    if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        *out = 0;
        return 0;
    }
    return fail(err, errlen, "undefined symbol: %s", name);
}

static int apply_rela(henge_image *im, const Elf64_Rela *rel, size_t count, uint64_t nsyms,
                      char *err, size_t errlen)
{
    for (size_t i = 0; i < count; i++) {
        const Elf64_Rela *r = &rel[i];
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uintptr_t p = im->base + r->r_offset;
        uintptr_t s;

        switch (type) {
        case R_AARCH64_NONE:
            continue;
        case R_AARCH64_RELATIVE:
            if (check_target(im, p, err, errlen))
                return -1;
            *(uint64_t *)p = im->base + (uint64_t)r->r_addend;
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_ABS64:
        case R_AARCH64_JUMP_SLOT:
            if (check_target(im, p, err, errlen))
                return -1;
            if (symbol_value(im, ELF64_R_SYM(r->r_info), nsyms, &s, err, errlen))
                return -1;
            /* JUMP_SLOT: *P = S (eager). Addends are 0 in practice; ignore as bionic does. */
            *(uint64_t *)p = type == R_AARCH64_JUMP_SLOT ? s : s + (uint64_t)r->r_addend;
            break;
        case R_AARCH64_TLS_TPREL64:
        case R_AARCH64_TLSDESC:
        case R_AARCH64_IRELATIVE:
            return fail(err, errlen, "relocation type %u at 0x%llx: not supported until M2", type,
                        (unsigned long long)r->r_offset);
        default:
            return fail(err, errlen, "unknown relocation type %u at 0x%llx", type,
                        (unsigned long long)r->r_offset);
        }
    }
    return 0;
}

/* Standard RELR: an even word is an address (relocate it, then continue after it); an odd
 * word is a bitmap of the next 63 words. Each relocated word gets base added. */
static int apply_relr(henge_image *im, const uint64_t *relr, size_t count, char *err,
                      size_t errlen)
{
    uintptr_t where = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t e = relr[i];
        if ((e & 1) == 0) {
            where = im->base + e;
            if (check_target(im, where, err, errlen))
                return -1;
            *(uint64_t *)where += im->base;
            where += 8;
        } else {
            if (where == 0)
                return fail(err, errlen, "RELR bitmap without preceding address");
            uintptr_t p = where;
            for (uint64_t bits = e >> 1; bits; bits >>= 1, p += 8) {
                if (bits & 1) {
                    if (check_target(im, p, err, errlen))
                        return -1;
                    *(uint64_t *)p += im->base;
                }
            }
            where += 63 * 8;
        }
    }
    return 0;
}

/* ---- image discovery ---- */

static const struct mach_header *find_header(const char *path, char *resolved_out)
{
    char want[PATH_MAX], have[PATH_MAX];
    if (!realpath(path, want))
        return NULL;
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name)
            continue;
        if (strcmp(name, want) == 0 || (realpath(name, have) && strcmp(have, want) == 0)) {
            const struct mach_header *mh = _dyld_get_image_header(i);
            if (mh) {
                memcpy(resolved_out, want, PATH_MAX);
                return mh;
            }
        }
    }
    return NULL;
}

typedef void (*init_fn)(int, char **, char **);

static henge_image *link_locked(const char *path, char *err, size_t errlen)
{
    char resolved[PATH_MAX];
    const struct mach_header *mh = find_header(path, resolved);
    if (!mh) {
        fail(err, errlen, "%s: image not found in dyld image list (dlopen it first)", path);
        return NULL;
    }
    for (henge_image *im = g_images; im; im = im->next)
        if (strcmp(im->path, resolved) == 0)
            return im; /* already linked */

    uintptr_t base = (uintptr_t)mh + HENGE_SHIFT;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (eh->e_ident[0] != ELFMAG0 || memcmp(eh->e_ident + 1, "ELF", 3) != 0 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_AARCH64 || eh->e_type != ET_DYN) {
        fail(err, errlen, "%s: no arm64 ELF header at Mach-O header + 0x%x", path, HENGE_SHIFT);
        return NULL;
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) {
        fail(err, errlen, "%s: unexpected e_phentsize %u", path, eh->e_phentsize);
        return NULL;
    }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
    const Elf64_Phdr *dyn_ph = NULL;
    uint64_t w_lo = UINT64_MAX, w_hi = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC)
            dyn_ph = &ph[i];
        else if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_W)) {
            if (ph[i].p_vaddr < w_lo)
                w_lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > w_hi)
                w_hi = ph[i].p_vaddr + ph[i].p_memsz;
        } else if (ph[i].p_type == PT_TLS) {
            fail(err, errlen, "%s: PT_TLS not supported until M2", path);
            return NULL;
        }
    }
    if (!dyn_ph) {
        fail(err, errlen, "%s: no PT_DYNAMIC", path);
        return NULL;
    }

    henge_image *im = calloc(1, sizeof *im);
    if (!im) {
        fail(err, errlen, "out of memory");
        return NULL;
    }
    memcpy(im->path, resolved, PATH_MAX);
    im->base = base;
    im->dynamic = (const Elf64_Dyn *)(base + dyn_ph->p_vaddr);
    if (w_hi > w_lo) {
        im->w_lo = base + w_lo;
        im->w_hi = base + w_hi;
    }

    uint64_t rela = 0, relasz = 0, relaent = sizeof(Elf64_Rela), jmprel = 0, pltrelsz = 0;
    uint64_t pltrel = DT_RELA, relr = 0, relrsz = 0, relrent = 8, init = 0, init_array = 0;
    uint64_t init_arraysz = 0, soname = 0, flags = 0;
    int textrel = 0;
    for (const Elf64_Dyn *d = im->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: im->strtab = (const char *)(base + d->d_val); break;
        case DT_STRSZ: im->strsz = d->d_val; break;
        case DT_SYMTAB: im->symtab = (const Elf64_Sym *)(base + d->d_val); break;
        case DT_GNU_HASH: im->gnu_hash = (const uint32_t *)(base + d->d_val); break;
        case DT_HASH: im->hash = (const uint32_t *)(base + d->d_val); break;
        case DT_RELA: rela = d->d_val; break;
        case DT_RELASZ: relasz = d->d_val; break;
        case DT_RELAENT: relaent = d->d_val; break;
        case DT_JMPREL: jmprel = d->d_val; break;
        case DT_PLTRELSZ: pltrelsz = d->d_val; break;
        case DT_PLTREL: pltrel = d->d_val; break;
        case DT_RELR: case DT_ANDROID_RELR: relr = d->d_val; break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: relrsz = d->d_val; break;
        case DT_RELRENT: case DT_ANDROID_RELRENT: relrent = d->d_val; break;
        case DT_INIT: init = d->d_val; break;
        case DT_INIT_ARRAY: init_array = d->d_val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_val; break;
        case DT_SONAME: soname = d->d_val; break;
        case DT_FLAGS: flags = d->d_val; break;
        case DT_TEXTREL: textrel = 1; break;
        case DT_ANDROID_REL: case DT_ANDROID_RELA:
            fail(err, errlen, "%s: Android packed relocations (APS2) not supported until M2", path);
            goto bad;
        default: break;
        }
    }
    if (textrel || (flags & DF_TEXTREL)) {
        fail(err, errlen, "%s: DT_TEXTREL not supported", path);
        goto bad;
    }
    if (!im->strtab || !im->symtab || (!im->gnu_hash && !im->hash)) {
        fail(err, errlen, "%s: missing DT_STRTAB/DT_SYMTAB/hash table", path);
        goto bad;
    }
    if (soname && soname < im->strsz)
        im->soname = im->strtab + soname;
    if ((relasz && relaent != sizeof(Elf64_Rela)) || (relrsz && relrent != 8)) {
        fail(err, errlen, "%s: unexpected relocation entry size", path);
        goto bad;
    }
    if (jmprel && pltrelsz && pltrel != DT_RELA) {
        fail(err, errlen, "%s: DT_PLTREL is not DT_RELA", path);
        goto bad;
    }

    /* Symbol count from DT_HASH if present (used only for bounds checks). */
    uint64_t nsyms = im->hash ? im->hash[1] : 0;

    /* 1. DT_NEEDED */
    for (const Elf64_Dyn *d = im->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED)
            continue;
        if (d->d_val >= im->strsz) {
            fail(err, errlen, "%s: bad DT_NEEDED", path);
            goto bad;
        }
        const char *needed = im->strtab + d->d_val;
        if (!needed_satisfied(needed)) {
            fail(err, errlen, "%s: needed library %s is not linked (link it first)", path, needed);
            goto bad;
        }
    }

    /* 2. Relocations. RELR first (RELATIVE only), then RELA, then JMPREL. */
    if (relr && apply_relr(im, (const uint64_t *)(base + relr), relrsz / 8, err, errlen))
        goto bad;
    if (rela && apply_rela(im, (const Elf64_Rela *)(base + rela), relasz / sizeof(Elf64_Rela),
                           nsyms, err, errlen))
        goto bad;
    if (jmprel && apply_rela(im, (const Elf64_Rela *)(base + jmprel),
                             pltrelsz / sizeof(Elf64_Rela), nsyms, err, errlen))
        goto bad;

    /* Register before running initializers so later images can resolve against it. */
    henge_image **tail = &g_images;
    while (*tail)
        tail = &(*tail)->next;
    *tail = im;

    /* 3. Initializers, with bionic's (argc, argv, envp) = (0, NULL, NULL). */
    if (init)
        ((init_fn)(base + init))(0, NULL, NULL);
    if (init_array) {
        const uintptr_t *arr = (const uintptr_t *)(base + init_array);
        for (size_t i = 0; i < init_arraysz / sizeof(uintptr_t); i++) {
            if (arr[i] == 0 || arr[i] == (uintptr_t)-1)
                continue;
            ((init_fn)arr[i])(0, NULL, NULL);
        }
    }
    return im;

bad:
    free(im);
    return NULL;
}

henge_image *henge_link(const char *path, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    if (!path) {
        fail(err, errlen, "path is NULL");
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    henge_image *im = link_locked(path, err, errlen);
    pthread_mutex_unlock(&g_lock);
    return im;
}
