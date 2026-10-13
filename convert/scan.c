/* Instruction scan over the executable PT_LOADs (docs/m1-elf-to-macho.md §3). */
#include <string.h>

#include "internal.h"

void hc_scan_code(const struct elf_info *e, struct scan_result *out) {
    unsigned svc = 0, tpidr = 0;
    for (unsigned i = 0; i < e->first_w; i++) {
        const Elf64_Phdr *p = &e->loads[i];
        if (!(p->p_flags & PF_X)) continue;
        const uint8_t *c = e->data + p->p_offset;
        for (uint64_t k = 0; k + 4 <= p->p_filesz; k += 4) {
            uint32_t w;
            memcpy(&w, c + k, 4);
            if (w == 0xd4000001u) svc++;                          /* svc #0 */
            else if ((w & 0xffffffe0u) == 0xd53bd040u) tpidr++;   /* mrs xN, tpidr_el0 */
        }
    }
    /* TODO(M2): detect x18 usage (platform register on Darwin) and warn. */
    out->svc_count = svc;
    out->tpidr_count = tpidr;
}
