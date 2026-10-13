/* ELF input: header/segment/dynamic validation and UUID derivation.
 * Rules: docs/m1-elf-to-macho.md §3. */
#include <string.h>

#include "internal.h"

/* Translate an ELF vaddr range to a file offset via the PT_LOADs. Returns 0 on success. */
static int vaddr_to_off(const struct elf_info *e, uint64_t v, uint64_t len, uint64_t *off) {
    for (unsigned i = 0; i < e->nloads; i++) {
        const Elf64_Phdr *p = &e->loads[i];
        if (v >= p->p_vaddr && v - p->p_vaddr < p->p_filesz && len <= p->p_filesz - (v - p->p_vaddr)) {
            *off = p->p_offset + (v - p->p_vaddr);
            return 0;
        }
    }
    return -1;
}

int hc_elf_parse(struct elf_info *e, const uint8_t *elf, size_t elf_size, char *err, size_t errlen) {
    memset(e, 0, sizeof *e);
    e->data = elf;
    e->size = elf_size;

    /* ---- ELF header ---- */
    if (elf_size < sizeof(Elf64_Ehdr)) return hc_fail(err, errlen, "file too small for an ELF header");
    e->eh = (const Elf64_Ehdr *)elf;
    if (memcmp(e->eh->e_ident, "\x7f" "ELF", 4) != 0) return hc_fail(err, errlen, "not an ELF file (bad magic)");
    if (e->eh->e_ident[4] != ELFCLASS64) return hc_fail(err, errlen, "not ELFCLASS64 (32-bit ELF is not supported)");
    if (e->eh->e_ident[5] != ELFDATA2LSB) return hc_fail(err, errlen, "not little-endian");
    if (e->eh->e_machine != EM_AARCH64)
        return hc_fail(err, errlen, "e_machine is %u, expected EM_AARCH64 (183)", e->eh->e_machine);
    if (e->eh->e_type != ET_DYN) return hc_fail(err, errlen, "e_type is %u, expected ET_DYN (shared object)", e->eh->e_type);
    if (e->eh->e_phentsize != sizeof(Elf64_Phdr)) return hc_fail(err, errlen, "unexpected e_phentsize %u", e->eh->e_phentsize);
    if (e->eh->e_phoff > elf_size || (uint64_t)e->eh->e_phnum * sizeof(Elf64_Phdr) > elf_size - e->eh->e_phoff)
        return hc_fail(err, errlen, "program headers extend past end of file");
    if (e->eh->e_phoff % 8) return hc_fail(err, errlen, "misaligned program headers");
    e->ph = (const Elf64_Phdr *)(elf + e->eh->e_phoff);

    /* ---- PT_LOADs and validation ---- */
    for (unsigned i = 0; i < e->eh->e_phnum; i++) {
        const Elf64_Phdr *p = &e->ph[i];
        if (p->p_type == PT_DYNAMIC) e->dyn_ph = p;
        if (p->p_type != PT_LOAD) continue;
        if (e->nloads == HC_MAX_LOADS) return hc_fail(err, errlen, "too many PT_LOAD segments (max %d)", HC_MAX_LOADS);
        if (p->p_offset > elf_size || p->p_filesz > elf_size - p->p_offset)
            return hc_fail(err, errlen, "PT_LOAD #%u file range extends past end of file", e->nloads);
        if (p->p_filesz > p->p_memsz) return hc_fail(err, errlen, "PT_LOAD #%u has p_filesz > p_memsz", e->nloads);
        if (p->p_vaddr > (1ull << 32) || p->p_memsz > (1ull << 32))
            return hc_fail(err, errlen, "PT_LOAD #%u is unreasonably large", e->nloads);
        if (e->nloads > 0) {
            const Elf64_Phdr *q = &e->loads[e->nloads - 1];
            if (p->p_vaddr < q->p_vaddr + q->p_memsz)
                return hc_fail(err, errlen, "PT_LOAD #%u overlaps or is not sorted after the previous one", e->nloads);
        }
        e->loads[e->nloads++] = *p;
    }
    if (e->nloads == 0) return hc_fail(err, errlen, "no PT_LOAD segments");
    if (e->loads[0].p_vaddr != 0 || e->loads[0].p_offset != 0)
        return hc_fail(err, errlen,
                       "first PT_LOAD must have p_vaddr 0 and p_offset 0 (got vaddr 0x%llx offset 0x%llx); "
                       "the ELF header must be mapped at the image base",
                       (unsigned long long)e->loads[0].p_vaddr, (unsigned long long)e->loads[0].p_offset);

    unsigned first_w = e->nloads;
    for (unsigned i = 0; i < e->nloads; i++) {
        int w = (e->loads[i].p_flags & PF_W) != 0;
        if (w && first_w == e->nloads) first_w = i;
        if (!w && first_w != e->nloads)
            return hc_fail(err, errlen, "non-writable PT_LOAD #%u follows a writable one; expected R/RX segments before RW", i);
    }
    if (first_w == 0) return hc_fail(err, errlen, "first PT_LOAD is writable; the ELF header must be in a read-only segment");
    if (first_w == e->nloads) return hc_fail(err, errlen, "no writable PT_LOAD (expected .dynamic/.got in an RW segment)");
    e->first_w = first_w;

    for (unsigned i = 0; i < first_w; i++) {
        uint64_t end = e->loads[i].p_vaddr + e->loads[i].p_memsz;
        if (end > e->text_end) e->text_end = end;
    }
    uint64_t w_min = e->loads[first_w].p_vaddr;
    for (unsigned i = first_w; i < e->nloads; i++) {
        const Elf64_Phdr *p = &e->loads[i];
        if (p->p_vaddr + p->p_filesz > e->w_file_end) e->w_file_end = p->p_vaddr + p->p_filesz;
        if (p->p_vaddr + p->p_memsz > e->w_mem_end) e->w_mem_end = p->p_vaddr + p->p_memsz;
    }
    e->data_start = hc_align_down(w_min, HC_PAGE);
    if (hc_align_up(e->text_end, HC_PAGE) > e->data_start)
        return hc_fail(err, errlen,
                       "code/read-only data (ends at 0x%llx) and writable data (starts at 0x%llx) share a 16 KiB page; "
                       "relink with -Wl,-z,max-page-size=16384",
                       (unsigned long long)e->text_end, (unsigned long long)w_min);

    /* ---- dynamic section: TEXTREL, STRTAB ---- */
    uint64_t strtab = 0;
    int have_strtab = 0;
    const Elf64_Phdr *dyn_ph = e->dyn_ph;
    if (!dyn_ph) return hc_fail(err, errlen, "no PT_DYNAMIC (not a dynamically linked shared object)");
    if (dyn_ph->p_offset > elf_size || dyn_ph->p_filesz > elf_size - dyn_ph->p_offset || dyn_ph->p_offset % 8)
        return hc_fail(err, errlen, "PT_DYNAMIC file range is invalid");
    e->dyn = (const Elf64_Dyn *)(elf + dyn_ph->p_offset);
    e->ndyn = dyn_ph->p_filesz / sizeof(Elf64_Dyn);
    for (size_t i = 0; i < e->ndyn && e->dyn[i].d_tag != DT_NULL; i++) {
        switch (e->dyn[i].d_tag) {
        case DT_TEXTREL:
            return hc_fail(err, errlen, "DT_TEXTREL present: text relocations are not supported");
        case DT_FLAGS:
            if (e->dyn[i].d_val & DF_TEXTREL) return hc_fail(err, errlen, "DF_TEXTREL set: text relocations are not supported");
            break;
        case DT_STRTAB: strtab = e->dyn[i].d_val; have_strtab = 1; break;
        case DT_STRSZ: e->strsz = e->dyn[i].d_val; break;
        default: break;
        }
    }
    if (have_strtab) {
        uint64_t off;
        if (e->strsz == 0 || vaddr_to_off(e, strtab, e->strsz, &off) != 0)
            return hc_fail(err, errlen, "DT_STRTAB/DT_STRSZ does not map to file data");
        e->dynstr = (const char *)elf + off;
    }
    return 0;
}

static int find_build_id(const struct elf_info *e, uint8_t uuid[16]) {
    for (unsigned i = 0; i < e->eh->e_phnum; i++) {
        const Elf64_Phdr *p = &e->ph[i];
        if (p->p_type != PT_NOTE) continue;
        if (p->p_offset > e->size || p->p_filesz > e->size - p->p_offset) continue;
        const uint8_t *n = e->data + p->p_offset, *end = n + p->p_filesz;
        while ((size_t)(end - n) >= sizeof(Elf64_Nhdr)) {
            Elf64_Nhdr nh;
            memcpy(&nh, n, sizeof nh);
            uint64_t namesz = hc_align_up(nh.n_namesz, 4), descsz = hc_align_up(nh.n_descsz, 4);
            if (sizeof nh + namesz + descsz > (uint64_t)(end - n)) break;
            const uint8_t *name = n + sizeof nh, *desc = name + namesz;
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && memcmp(name, "GNU", 4) == 0 &&
                nh.n_descsz > 0) {
                memset(uuid, 0, 16);
                memcpy(uuid, desc, nh.n_descsz < 16 ? nh.n_descsz : 16);
                return 0;
            }
            n += sizeof nh + namesz + descsz;
        }
    }
    return -1;
}

/* No build-id: derive a stable UUID from the input bytes (two FNV-1a 64 lanes). */
static void hash_uuid(const uint8_t *d, size_t n, uint8_t uuid[16]) {
    uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (size_t i = 0; i < n; i++) {
        a = (a ^ d[i]) * 0x100000001b3ull;
        b = (b ^ d[n - 1 - i]) * 0x100000001b3ull;
    }
    memcpy(uuid, &a, 8);
    memcpy(uuid + 8, &b, 8);
    uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40); /* RFC 4122 v4-ish shape */
    uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);
}

void hc_elf_uuid(const struct elf_info *e, uint8_t uuid[16]) {
    if (find_build_id(e, uuid) != 0) hash_uuid(e->data, e->size, uuid);
}
