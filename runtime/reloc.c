/* ELF dynamic relocations: RELA/JMPREL and RELR (docs/m1-elf-to-macho.md §5). */
#include "internal.h"

#include <stdlib.h>

static int check_target_n(const henge_image *im, uintptr_t p, size_t n, char *err, size_t errlen)
{
    if (p < im->w_lo || p + n > im->w_hi || (p & 7) != 0)
        return hr_fail(err, errlen, "relocation target 0x%llx (vaddr 0x%llx) outside writable segment",
                       (unsigned long long)p, (unsigned long long)(p - im->base));
    return 0;
}

static int check_target(const henge_image *im, uintptr_t p, char *err, size_t errlen)
{
    return check_target_n(im, p, 8, err, errlen);
}

/* TLSDESC: finds the module and offset of a TLS symbol (symidx 0: this module). */
static int tls_symbol(henge_image *im, uint32_t symidx, uint64_t nsyms, struct hr_tls_module **mod,
                      uint64_t *off, char *err, size_t errlen)
{
    *mod = im->tls;
    *off = 0;
    if (symidx != 0) {
        if (nsyms && symidx >= nsyms)
            return hr_fail(err, errlen, "symbol index %u out of range", symidx);
        const Elf64_Sym *s = &im->symtab[symidx];
        if (s->st_name >= im->strsz)
            return hr_fail(err, errlen, "bad symbol name offset");
        const char *name = im->strtab + s->st_name;
        if (s->st_shndx != SHN_UNDEF) {
            *off = s->st_value;
        } else {
            *mod = NULL;
            for (henge_image *o = hr_images; o; o = o->next) {
                const Elf64_Sym *d = hr_lookup_sym(o, name);
                if (d && ELF64_ST_TYPE(d->st_info) == STT_TLS) {
                    *mod = o->tls;
                    *off = d->st_value;
                    break;
                }
            }
            if (!*mod)
                return hr_fail(err, errlen, "undefined TLS symbol: %s", name);
        }
    }
    if (!*mod)
        return hr_fail(err, errlen, "TLS relocation in an image without PT_TLS");
    return 0;
}

/* glibc/bionic ifunc resolver signature on arm64: (hwcap | _IFUNC_ARG_HWCAP, &arg). */
struct ifunc_arg {
    uint64_t size, hwcap, hwcap2;
};
typedef uintptr_t (*ifunc_resolver)(uint64_t, const struct ifunc_arg *);

uintptr_t hr_call_ifunc(uintptr_t resolver)
{
    struct ifunc_arg arg = {sizeof arg, 0, 0};
    hr_hwcap(&arg.hwcap, &arg.hwcap2);
    return ((ifunc_resolver)resolver)(arg.hwcap | (1ull << 62) /* _IFUNC_ARG_HWCAP */, &arg);
}

static int symbol_value(henge_image *im, uint32_t symidx, uint64_t nsyms, uintptr_t *out,
                        char *err, size_t errlen)
{
    if (symidx == 0) {
        *out = 0;
        return 0;
    }
    if (nsyms && symidx >= nsyms)
        return hr_fail(err, errlen, "symbol index %u out of range", symidx);
    const Elf64_Sym *s = &im->symtab[symidx];
    if (s->st_name >= im->strsz)
        return hr_fail(err, errlen, "bad symbol name offset");
    const char *name = im->strtab + s->st_name;
    if (ELF64_ST_TYPE(s->st_info) == 6 /* STT_TLS */)
        return hr_fail(err, errlen, "TLS symbol %s: not supported until M2", name);
    if (s->st_shndx != SHN_UNDEF) {
        *out = hr_sym_addr(im, s);
        return 0;
    }
    if (hr_resolve_global(name, out))
        return 0;
    if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        *out = 0;
        return 0;
    }
    return hr_fail(err, errlen, "undefined symbol: %s", name);
}

int hr_apply_rela(henge_image *im, const Elf64_Rela *rel, size_t count, uint64_t nsyms,
                  int irelative_pass, char *err, size_t errlen)
{
    for (size_t i = 0; i < count; i++) {
        const Elf64_Rela *r = &rel[i];
        uint32_t type = ELF64_R_TYPE(r->r_info);
        /* IFUNC resolvers run last, once everything else is relocated (as bionic does). */
        if ((type == R_AARCH64_IRELATIVE) != (irelative_pass != 0))
            continue;
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
        case R_AARCH64_TLSDESC: {
            struct hr_tls_module *mod;
            uint64_t off;
            if (check_target_n(im, p, 16, err, errlen))
                return -1;
            if (tls_symbol(im, ELF64_R_SYM(r->r_info), nsyms, &mod, &off, err, errlen))
                return -1;
            void *arg = hr_tlsdesc_arg(mod, off + (uint64_t)r->r_addend);
            if (!arg)
                return hr_fail(err, errlen, "out of memory");
            ((uintptr_t *)p)[0] = (uintptr_t)hr_tlsdesc_entry;
            ((uintptr_t *)p)[1] = (uintptr_t)arg;
            /* The access sequence reads tpidr_el0 afterwards: needs the patched thread pointer. */
            if (hr_thread_init(err, errlen))
                return -1;
            break;
        }
        case R_AARCH64_IRELATIVE: {
            if (check_target(im, p, err, errlen))
                return -1;
            *(uint64_t *)p = hr_call_ifunc(im->base + (uint64_t)r->r_addend);
            break;
        }
        case R_AARCH64_TLS_TPREL64:
            return hr_fail(err, errlen, "static TLS (R_AARCH64_TLS_TPREL64 at 0x%llx) is not supported; "
                           "only TLSDESC (-fPIC general dynamic) is", (unsigned long long)r->r_offset);
        default:
            return hr_fail(err, errlen, "unknown relocation type %u at 0x%llx", type,
                           (unsigned long long)r->r_offset);
        }
    }
    return 0;
}

/* Standard RELR: an even word is an address (relocate it, then continue after it); an odd
 * word is a bitmap of the next 63 words. Each relocated word gets base added. */
int hr_apply_relr(henge_image *im, const uint64_t *relr, size_t count, char *err,
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
                return hr_fail(err, errlen, "RELR bitmap without preceding address");
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

/* ---- Android packed relocations (APS2, DT_ANDROID_RELA) ----
 * "APS2" then SLEB128s: count, initial offset, then groups of
 * {size, flags, [offset delta], [info], [addend delta]} each followed by per-relocation
 * fields not covered by the group. Same decoder as bionic's packed_reloc_iterator. */

#define APS2_BY_INFO 1
#define APS2_BY_OFFSET_DELTA 2
#define APS2_BY_ADDEND 4
#define APS2_HAS_ADDEND 8

struct sleb {
    const uint8_t *p, *end;
    int bad;
};

static int64_t sleb_next(struct sleb *d)
{
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        if (d->p >= d->end || shift >= 64) {
            d->bad = 1;
            return 0;
        }
        b = *d->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= ~0ull << shift;
    return (int64_t)v;
}

int hr_decode_aps2(const uint8_t *data, size_t size, Elf64_Rela **out, size_t *count, char *err,
                   size_t errlen)
{
    *out = NULL;
    *count = 0;
    if (size < 4 || data[0] != 'A' || data[1] != 'P' || data[2] != 'S' || data[3] != '2')
        return hr_fail(err, errlen, "DT_ANDROID_RELA without APS2 header");
    struct sleb d = {data + 4, data + size, 0};
    int64_t n = sleb_next(&d);
    /* Each relocation takes at least one byte of input. */
    if (d.bad || n < 0 || (uint64_t)n > size)
        return hr_fail(err, errlen, "bad APS2 relocation count");
    Elf64_Rela *rel = calloc((size_t)n ? (size_t)n : 1, sizeof *rel);
    if (!rel)
        return hr_fail(err, errlen, "out of memory");
    Elf64_Rela cur = {(uint64_t)sleb_next(&d), 0, 0};
    size_t i = 0;
    while (i < (size_t)n && !d.bad) {
        int64_t gsize = sleb_next(&d), flags = sleb_next(&d), odelta = 0;
        if (gsize <= 0 || (uint64_t)gsize > (uint64_t)n - i) {
            d.bad = 1;
            break;
        }
        if (flags & APS2_BY_OFFSET_DELTA)
            odelta = sleb_next(&d);
        if (flags & APS2_BY_INFO)
            cur.r_info = (uint64_t)sleb_next(&d);
        int64_t af = flags & (APS2_HAS_ADDEND | APS2_BY_ADDEND);
        if (af == (APS2_HAS_ADDEND | APS2_BY_ADDEND))
            cur.r_addend += sleb_next(&d);
        else if (af != APS2_HAS_ADDEND)
            cur.r_addend = 0;
        for (int64_t k = 0; k < gsize && !d.bad; k++) {
            cur.r_offset += (flags & APS2_BY_OFFSET_DELTA) ? (uint64_t)odelta : (uint64_t)sleb_next(&d);
            if (!(flags & APS2_BY_INFO))
                cur.r_info = (uint64_t)sleb_next(&d);
            if (af == APS2_HAS_ADDEND)
                cur.r_addend += sleb_next(&d);
            rel[i++] = cur;
        }
    }
    if (d.bad) {
        free(rel);
        return hr_fail(err, errlen, "malformed APS2 relocations");
    }
    *out = rel;
    *count = i;
    return 0;
}
