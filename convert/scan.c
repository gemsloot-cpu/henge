/* Instruction scan over the executable code (docs/m2-patching.md §2).
 *
 * Finds the instructions the converter must rewrite (svc #0, mrs xN, tpidr_el0) or reject
 * (msr tpidr_el0, xN), and counts x18 uses. Code ranges come from SHF_EXECINSTR sections
 * when section headers are present, else from the PF_X segments.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define INSN_SVC0 0xd4000001u
#define INSN_MRS_TPIDR 0xd53bd040u /* | Rt */
#define INSN_MSR_TPIDR 0xd51bd040u /* | Rt */
#define INSN_SCS_PUSH 0xf800865eu  /* str x30, [x18], #8 */
#define INSN_SCS_POP 0xf85f8e5eu   /* ldr x30, [x18, #-8]! */

#define RD(w) ((w) & 31)
#define RN(w) (((w) >> 5) & 31)
#define RM(w) (((w) >> 16) & 31)
#define RA(w) (((w) >> 10) & 31)

/* Nonzero if the instruction names x18 / w18 as a general-purpose register. Covers the
 * integer data-processing, branch, system and load/store classes; SIMD/FP is ignored.
 * Clang reserves x18 on Android, so any hit means hand-written asm or Shadow Call Stack. */
static int uses_x18(uint32_t w) {
    uint32_t op0 = (w >> 25) & 15;
    if ((op0 & 14) == 8) { /* 100x: data processing, immediate */
        switch ((w >> 23) & 7) {
        case 0: case 1: case 5: return RD(w) == 18;              /* adr/adrp, movz/movk */
        case 7: return RD(w) == 18 || RN(w) == 18 || RM(w) == 18; /* extr */
        default: return RD(w) == 18 || RN(w) == 18;
        }
    }
    if ((op0 & 14) == 10) { /* 101x: branches, exception, system */
        if ((w & 0xfe000000u) == 0xd6000000u) return RN(w) == 18;          /* br/blr/ret */
        if ((w & 0x7e000000u) == 0x34000000u) return RD(w) == 18;          /* cbz/cbnz */
        if ((w & 0x7e000000u) == 0x36000000u) return RD(w) == 18;          /* tbz/tbnz */
        if ((w & 0xffc00000u) == 0xd5000000u) return RD(w) == 18;          /* sys/mrs/msr */
        return 0;
    }
    if ((op0 & 5) == 4) { /* x1x0: loads and stores */
        int v = (w >> 26) & 1;
        if ((w & 0xbe000000u) == 0x0c000000u) /* SIMD structure; post-index takes Rm */
            return RN(w) == 18 || (((w >> 23) & 1) && RM(w) == 18);
        if ((w & 0x3f000000u) == 0x08000000u) /* exclusive / ordered: Rs, Rt2, Rn, Rt */
            return RD(w) == 18 || RN(w) == 18 || RM(w) == 18 || RA(w) == 18;
        uint32_t op = (w >> 27) & 7;
        if (op == 3) return !v && RD(w) == 18; /* literal */
        int hit = RN(w) == 18 || (!v && RD(w) == 18);
        if (op == 5) hit |= !v && RA(w) == 18; /* pair: Rt2 */
        if (op == 7 && !((w >> 24) & 1) && ((w >> 21) & 1)) hit |= RM(w) == 18; /* reg offset, atomics */
        return hit;
    }
    if ((op0 & 7) == 5) { /* x101: data processing, register */
        int hit = RD(w) == 18 || RN(w) == 18;
        int ccmp_imm = ((w >> 21) & 0xff) == 0xd2 && (w & 0x800);
        if (!ccmp_imm) hit |= RM(w) == 18;
        if (((w >> 24) & 31) == 0x1b) hit |= RA(w) == 18; /* madd/msub/... */
        return hit;
    }
    return 0;
}

struct range {
    uint64_t vaddr, off, size;
};

/* Executable byte ranges. Returns the count (at most max). */
static unsigned code_ranges(const struct elf_info *e, struct range *r, unsigned max) {
    unsigned n = 0;
    const Elf64_Ehdr *eh = e->eh;
    if (eh->e_shoff && eh->e_shnum && eh->e_shentsize == sizeof(Elf64_Shdr) && eh->e_shoff % 8 == 0 &&
        eh->e_shoff <= e->size && (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) <= e->size - eh->e_shoff) {
        const Elf64_Shdr *sh = (const Elf64_Shdr *)(e->data + eh->e_shoff);
        for (unsigned i = 0; i < eh->e_shnum && n < max; i++) {
            if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR) || !sh[i].sh_size) continue;
            /* Must sit inside a non-writable PF_X segment's file bytes. */
            for (unsigned k = 0; k < e->first_w; k++) {
                const Elf64_Phdr *p = &e->loads[k];
                if ((p->p_flags & PF_X) && sh[i].sh_addr >= p->p_vaddr &&
                    sh[i].sh_addr + sh[i].sh_size <= p->p_vaddr + p->p_filesz) {
                    r[n++] = (struct range){sh[i].sh_addr, p->p_offset + (sh[i].sh_addr - p->p_vaddr), sh[i].sh_size};
                    break;
                }
            }
        }
        if (n) return n;
    }
    for (unsigned i = 0; i < e->first_w && n < max; i++) {
        const Elf64_Phdr *p = &e->loads[i];
        if (p->p_flags & PF_X) r[n++] = (struct range){p->p_vaddr, p->p_offset, p->p_filesz};
    }
    return n;
}

static int add_patch(struct scan_result *s, size_t *cap, uint64_t vaddr, uint8_t kind, uint8_t reg) {
    if (s->npatches == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        struct hc_patch *np = realloc(s->patches, nc * sizeof *np);
        if (!np) return -1;
        s->patches = np;
        *cap = nc;
    }
    s->patches[s->npatches++] = (struct hc_patch){vaddr, kind, reg};
    return 0;
}

int hc_scan_code(const struct elf_info *e, struct scan_result *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    struct range r[64];
    unsigned nr = code_ranges(e, r, 64);
    size_t cap = 0;
    for (unsigned i = 0; i < nr; i++) {
        const uint8_t *c = e->data + r[i].off;
        for (uint64_t k = 0; k + 4 <= r[i].size; k += 4) {
            uint32_t w;
            memcpy(&w, c + k, 4);
            uint64_t va = r[i].vaddr + k;
            uint8_t kind = 0, reg = 0;
            if (w == INSN_SVC0) {
                out->svc_count++;
                kind = HC_PATCH_SVC;
            } else if ((w & ~31u) == INSN_MRS_TPIDR) {
                out->tpidr_count++;
                kind = HC_PATCH_TPIDR;
                reg = (uint8_t)RD(w);
                if (reg == 30) {
                    hc_scan_free(out);
                    return hc_fail(err, errlen, "mrs x30, tpidr_el0 at 0x%llx cannot be patched",
                                   (unsigned long long)va);
                }
            } else if ((w & ~31u) == INSN_MSR_TPIDR) {
                hc_scan_free(out);
                return hc_fail(err, errlen, "msr tpidr_el0 at 0x%llx: code that sets the thread pointer "
                               "(a libc?) cannot run under henge", (unsigned long long)va);
            } else if (uses_x18(w)) {
                if (!out->x18_count) out->first_x18 = va;
                out->x18_count++;
                if (w == INSN_SCS_PUSH || w == INSN_SCS_POP) out->scs = 1;
            }
            if (kind && add_patch(out, &cap, va, kind, reg) != 0) {
                hc_scan_free(out);
                return hc_fail(err, errlen, "out of memory");
            }
        }
    }
    return 0;
}

void hc_scan_free(struct scan_result *s) {
    free(s->patches);
    s->patches = NULL;
    s->npatches = 0;
}
