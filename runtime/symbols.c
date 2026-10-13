/* Symbol lookup: DT_GNU_HASH / DT_HASH tables and global resolution. */
#include "internal.h"
#include "shim.h"

#include <string.h>

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

const Elf64_Sym *hr_lookup_sym(const henge_image *im, const char *name)
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

uintptr_t hr_sym_addr(const henge_image *im, const Elf64_Sym *s)
{
    uintptr_t a = im->base + s->st_value;
    if (ELF64_ST_TYPE(s->st_info) == STT_GNU_IFUNC)
        a = hr_call_ifunc(a);
    return a;
}

uint64_t hr_symbol_count(const henge_image *im)
{
    if (im->hash)
        return im->hash[1];
    if (!im->gnu_hash)
        return 0;
    const uint32_t *h = im->gnu_hash;
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint32_t *buckets = (const uint32_t *)((const uint64_t *)(h + 4) + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t max = 0;
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > max)
            max = buckets[i];
    if (max < symoffset)
        return symoffset;
    while (!(chain[max - symoffset] & 1))
        max++;
    return (uint64_t)max + 1;
}

void *henge_dlsym(henge_image *image, const char *name)
{
    if (!image || !name)
        return NULL;
    const Elf64_Sym *s = hr_lookup_sym(image, name);
    return s ? (void *)hr_sym_addr(image, s) : NULL;
}

/* Global lookup for an undefined symbol: shim first, then linked images in load order. */
int hr_resolve_global(const char *name, uintptr_t *out)
{
    void *p = henge_shim_lookup(name);
    if (p) {
        *out = (uintptr_t)p;
        return 1;
    }
    for (henge_image *im = hr_images; im; im = im->next) {
        const Elf64_Sym *s = hr_lookup_sym(im, name);
        if (s) {
            *out = hr_sym_addr(im, s);
            return 1;
        }
    }
    return 0;
}
