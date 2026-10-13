/* henge mini linker (M1). See docs/m1-elf-to-macho.md §5 and docs/architecture.md §5.
 *
 * dyld maps the converted dylib but performs no fixups. This file finds the image,
 * locates the original ELF headers inside it, resolves DT_NEEDED, applies the ELF
 * dynamic relocations and runs the initializers.
 */
#include "internal.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER; /* initializers may call henge_link */
henge_image *hr_images; /* load order (head = first loaded) */

int hr_fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
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
    for (henge_image *im = hr_images; im; im = im->next) {
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

/* Reads the converter's layout from the Mach-O load commands: H is the __TEXT,__elf address,
 * the island (if any) is __TEXT,__henge. */
static int image_layout(const struct mach_header *mh, uint64_t *shift,
                        const struct henge_island_hdr **island)
{
    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    if (h->magic != MH_MAGIC_64)
        return -1;
    *shift = 0;
    *island = NULL;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)c;
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);
            for (uint32_t k = 0; k < seg->nsects; k++) {
                if (strcmp(sect[k].segname, "__TEXT") != 0)
                    continue;
                if (strncmp(sect[k].sectname, "__elf", 16) == 0)
                    *shift = sect[k].addr;
                else if (strncmp(sect[k].sectname, "__henge", 16) == 0)
                    *island = (const struct henge_island_hdr *)((uintptr_t)mh + sect[k].addr);
            }
        }
        lc += c->cmdsize;
    }
    return *shift ? 0 : -1;
}

/* Fills the island's slots so patched svc / tpidr_el0 sites reach the runtime. */
static int setup_island(const struct mach_header *mh, const struct henge_island_hdr *is,
                        const char *path, char *err, size_t errlen)
{
    if (is->magic != HENGE_ISLAND_MAGIC || is->version != HENGE_ISLAND_VERSION)
        return hr_fail(err, errlen, "%s: unknown patch island version (reconvert it)", path);
    if (is->tpidr_sites && hr_thread_init(err, errlen) != 0)
        return -1;
    uintptr_t *slots = (uintptr_t *)((uintptr_t)mh + is->slots_vmaddr);
    slots[HENGE_SLOT_SVC] = (uintptr_t)hr_svc_entry;
    slots[HENGE_SLOT_TP_KEY] = hr_tp_key_offset();
    slots[HENGE_SLOT_TP_SLOW] = (uintptr_t)hr_tp_slow_entry;
    return 0;
}

static henge_image *link_locked(const char *path, char *err, size_t errlen)
{
    char resolved[PATH_MAX];
    const struct mach_header *mh = find_header(path, resolved);
    if (!mh) {
        hr_fail(err, errlen, "%s: image not found in dyld image list (dlopen it first)", path);
        return NULL;
    }
    for (henge_image *im = hr_images; im; im = im->next)
        if (strcmp(im->path, resolved) == 0)
            return im; /* already linked */

    uint64_t shift;
    const struct henge_island_hdr *island;
    if (image_layout(mh, &shift, &island) != 0) {
        hr_fail(err, errlen, "%s: not a henge image (no __TEXT,__elf section)", path);
        return NULL;
    }
    uintptr_t base = (uintptr_t)mh + shift;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (eh->e_ident[0] != ELFMAG0 || memcmp(eh->e_ident + 1, "ELF", 3) != 0 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_AARCH64 || eh->e_type != ET_DYN) {
        hr_fail(err, errlen, "%s: no arm64 ELF header at Mach-O header + 0x%llx", path,
                (unsigned long long)shift);
        return NULL;
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) {
        hr_fail(err, errlen, "%s: unexpected e_phentsize %u", path, eh->e_phentsize);
        return NULL;
    }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
    const Elf64_Phdr *dyn_ph = NULL, *tls_ph = NULL;
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
            tls_ph = &ph[i];
        }
    }
    if (!dyn_ph) {
        hr_fail(err, errlen, "%s: no PT_DYNAMIC", path);
        return NULL;
    }

    henge_image *im = calloc(1, sizeof *im);
    if (!im) {
        hr_fail(err, errlen, "out of memory");
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
            hr_fail(err, errlen, "%s: Android packed relocations (APS2) not supported until M2", path);
            goto bad;
        default: break;
        }
    }
    if (textrel || (flags & DF_TEXTREL)) {
        hr_fail(err, errlen, "%s: DT_TEXTREL not supported", path);
        goto bad;
    }
    if (!im->strtab || !im->symtab || (!im->gnu_hash && !im->hash)) {
        hr_fail(err, errlen, "%s: missing DT_STRTAB/DT_SYMTAB/hash table", path);
        goto bad;
    }
    if (soname && soname < im->strsz)
        im->soname = im->strtab + soname;
    if ((relasz && relaent != sizeof(Elf64_Rela)) || (relrsz && relrent != 8)) {
        hr_fail(err, errlen, "%s: unexpected relocation entry size", path);
        goto bad;
    }
    if (jmprel && pltrelsz && pltrel != DT_RELA) {
        hr_fail(err, errlen, "%s: DT_PLTREL is not DT_RELA", path);
        goto bad;
    }

    /* Symbol count from DT_HASH if present (used only for bounds checks). */
    uint64_t nsyms = im->hash ? im->hash[1] : 0;
    im->nsyms = nsyms;
    if (island && setup_island(mh, island, path, err, errlen) != 0)
        goto bad;
    if (tls_ph && hr_tls_register(im, tls_ph, err, errlen) != 0)
        goto bad;

    /* 1. DT_NEEDED */
    for (const Elf64_Dyn *d = im->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED)
            continue;
        if (d->d_val >= im->strsz) {
            hr_fail(err, errlen, "%s: bad DT_NEEDED", path);
            goto bad;
        }
        const char *needed = im->strtab + d->d_val;
        if (!needed_satisfied(needed)) {
            hr_fail(err, errlen, "%s: needed library %s is not linked (link it first)", path, needed);
            goto bad;
        }
    }

    /* 2. Relocations. RELR first (RELATIVE only), then RELA, then JMPREL; IRELATIVE last. */
    if (relr && hr_apply_relr(im, (const uint64_t *)(base + relr), relrsz / 8, err, errlen))
        goto bad;
    for (int pass = 0; pass < 2; pass++) {
        if (rela && hr_apply_rela(im, (const Elf64_Rela *)(base + rela), relasz / sizeof(Elf64_Rela),
                                  nsyms, pass, err, errlen))
            goto bad;
        if (jmprel && hr_apply_rela(im, (const Elf64_Rela *)(base + jmprel),
                                    pltrelsz / sizeof(Elf64_Rela), nsyms, pass, err, errlen))
            goto bad;
    }

    /* Register before running initializers so later images can resolve against it. */
    henge_image **tail = &hr_images;
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
        hr_fail(err, errlen, "path is NULL");
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    henge_image *im = link_locked(path, err, errlen);
    pthread_mutex_unlock(&g_lock);
    return im;
}
