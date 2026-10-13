/* ELF dynamic relocations: RELA/JMPREL and RELR (docs/m1-elf-to-macho.md §5). */
#include "internal.h"

static int check_target(const henge_image *im, uintptr_t p, char *err, size_t errlen)
{
    if (p < im->w_lo || p + 8 > im->w_hi || (p & 7) != 0)
        return hr_fail(err, errlen, "relocation target 0x%llx (vaddr 0x%llx) outside writable segment",
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
        return hr_fail(err, errlen, "symbol index %u out of range", symidx);
    const Elf64_Sym *s = &im->symtab[symidx];
    if (s->st_name >= im->strsz)
        return hr_fail(err, errlen, "bad symbol name offset");
    const char *name = im->strtab + s->st_name;
    if (ELF64_ST_TYPE(s->st_info) == 6 /* STT_TLS */)
        return hr_fail(err, errlen, "TLS symbol %s: not supported until M2", name);
    if (s->st_shndx != SHN_UNDEF) {
        *out = im->base + s->st_value;
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
            return hr_fail(err, errlen, "relocation type %u at 0x%llx: not supported until M2", type,
                           (unsigned long long)r->r_offset);
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
