/* Dynamic symbol table: imports (undefined symbols) and exports for the manifest and
 * compatibility reports. The symbol count comes from DT_HASH, else from walking DT_GNU_HASH. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* Number of .dynsym entries, or 0 if it cannot be determined. */
static uint64_t symbol_count(const struct elf_info *e, uint64_t hash, uint64_t gnu_hash) {
    uint64_t off;
    if (hash && hc_vaddr_to_off(e, hash, 8, &off) == 0) {
        uint32_t nchain;
        memcpy(&nchain, e->data + off + 4, 4);
        return nchain;
    }
    if (!gnu_hash || hc_vaddr_to_off(e, gnu_hash, 16, &off) != 0) return 0;
    uint32_t h[4];
    memcpy(h, e->data + off, 16);
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    uint64_t buckets_va = gnu_hash + 16 + (uint64_t)bloom_size * 8;
    uint64_t boff;
    if (nbuckets == 0 || hc_vaddr_to_off(e, buckets_va, (uint64_t)nbuckets * 4, &boff) != 0) return 0;
    uint32_t max = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        uint32_t b;
        memcpy(&b, e->data + boff + 4ull * i, 4);
        if (b > max) max = b;
    }
    if (max < symoffset) return symoffset;
    /* Follow the last chain to its terminator (low bit set). */
    uint64_t chain_va = buckets_va + (uint64_t)nbuckets * 4;
    for (;; max++) {
        uint64_t coff;
        if (hc_vaddr_to_off(e, chain_va + 4ull * (max - symoffset), 4, &coff) != 0) return 0;
        uint32_t c;
        memcpy(&c, e->data + coff, 4);
        if (c & 1) return (uint64_t)max + 1;
    }
}

int hc_elf_symbols(const struct elf_info *e, int defined, struct henge_symbol **out, size_t *n, char *err,
                   size_t errlen) {
    *out = NULL;
    *n = 0;
    uint64_t symtab = 0, syment = sizeof(Elf64_Sym), hash = 0, gnu_hash = 0;
    for (size_t i = 0; i < e->ndyn && e->dyn[i].d_tag != DT_NULL; i++) {
        switch (e->dyn[i].d_tag) {
        case DT_SYMTAB: symtab = e->dyn[i].d_val; break;
        case DT_SYMENT: syment = e->dyn[i].d_val; break;
        case DT_HASH: hash = e->dyn[i].d_val; break;
        case DT_GNU_HASH: gnu_hash = e->dyn[i].d_val; break;
        default: break;
        }
    }
    if (!symtab || !e->dynstr) return 0;
    if (syment != sizeof(Elf64_Sym)) return hc_fail(err, errlen, "unexpected DT_SYMENT %llu", (unsigned long long)syment);
    uint64_t nsyms = symbol_count(e, hash, gnu_hash), off;
    if (nsyms == 0) return 0;
    if (nsyms > (1u << 24) || hc_vaddr_to_off(e, symtab, nsyms * sizeof(Elf64_Sym), &off) != 0)
        return hc_fail(err, errlen, "DT_SYMTAB does not map to file data");

    struct henge_symbol *imp = NULL;
    size_t count = 0, cap = 0;
    for (uint64_t i = 1; i < nsyms; i++) {
        Elf64_Sym s;
        memcpy(&s, e->data + off + i * sizeof s, sizeof s);
        unsigned bind = ELF64_ST_BIND(s.st_info);
        if ((s.st_shndx != SHN_UNDEF) != (defined != 0) || (bind != STB_GLOBAL && bind != STB_WEAK) ||
            s.st_name == 0 || s.st_name >= e->strsz)
            continue;
        const char *name = e->dynstr + s.st_name;
        size_t len = strnlen(name, (size_t)(e->strsz - s.st_name));
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            struct henge_symbol *ni = realloc(imp, cap * sizeof *ni);
            if (!ni) goto oom;
            imp = ni;
        }
        char *copy = malloc(len + 1);
        if (!copy) goto oom;
        memcpy(copy, name, len);
        copy[len] = 0;
        imp[count++] = (struct henge_symbol){copy, bind == STB_WEAK};
    }
    *out = imp;
    *n = count;
    return 0;
oom:
    hc_symbols_free(imp, count);
    return hc_fail(err, errlen, "out of memory");
}

void hc_symbols_free(struct henge_symbol *imports, size_t n) {
    for (size_t i = 0; i < n; i++) free(imports[i].name);
    free(imports);
}
