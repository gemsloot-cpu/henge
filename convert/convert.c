/* henge-convert: repack an Android arm64 ELF shared object as a Mach-O dylib.
 * Layout and rules: docs/m1-elf-to-macho.md §3–4. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "henge/elf64.h"
#include "henge/henge.h"

#define H ((uint64_t)HENGE_SHIFT)
#define PAGE ((uint64_t)HENGE_PAGE)
#define MAX_LOADS 16
#define MARKER_SYM "_henge_image_marker"
#define LIBSYSTEM_PATH "/usr/lib/libSystem.B.dylib"

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- growable string buffer for the manifest ---- */

struct sbuf {
    char *p;
    size_t len, cap;
    int oom;
};

static void sb_printf(struct sbuf *b, const char *fmt, ...) {
    if (b->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) { b->oom = 1; return; }
        if (b->p && b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        size_t ncap = (b->cap ? b->cap * 2 : 256) + (size_t)n;
        char *np = realloc(b->p, ncap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = ncap;
    }
}

static void sb_json_str(struct sbuf *b, const char *s, size_t maxlen) {
    sb_printf(b, "\"");
    for (size_t i = 0; i < maxlen && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", c);
        else if (c < 0x20 || c >= 0x7f) sb_printf(b, "\\u%04x", c);
        else sb_printf(b, "%c", c);
    }
    sb_printf(b, "\"");
}

/* ---- ELF helpers ---- */

struct elf_info {
    const uint8_t *data;
    size_t size;
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *ph;
    Elf64_Phdr loads[MAX_LOADS];
    unsigned nloads;
};

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

static int find_build_id(const struct elf_info *e, uint8_t uuid[16]) {
    for (unsigned i = 0; i < e->eh->e_phnum; i++) {
        const Elf64_Phdr *p = &e->ph[i];
        if (p->p_type != PT_NOTE) continue;
        if (p->p_offset > e->size || p->p_filesz > e->size - p->p_offset) continue;
        const uint8_t *n = e->data + p->p_offset, *end = n + p->p_filesz;
        while ((size_t)(end - n) >= sizeof(Elf64_Nhdr)) {
            Elf64_Nhdr nh;
            memcpy(&nh, n, sizeof nh);
            uint64_t namesz = align_up(nh.n_namesz, 4), descsz = align_up(nh.n_descsz, 4);
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

/* ---- Mach-O emission helpers ---- */

static void put(uint8_t *out, size_t *off, const void *src, size_t n) {
    memcpy(out + *off, src, n);
    *off += n;
}

static uint32_t lc_str_size(size_t base, const char *s) { return (uint32_t)align_up(base + strlen(s) + 1, 8); }

int henge_convert(const uint8_t *elf, size_t elf_size, const struct henge_convert_opts *opts,
                  struct henge_convert_result *out, char *err, size_t errlen) {
    struct elf_info e;
    memset(&e, 0, sizeof e);
    memset(out, 0, sizeof *out);
    e.data = elf;
    e.size = elf_size;

    if (!opts || !opts->install_name || !*opts->install_name)
        return fail(err, errlen, "install name is required");
    if (opts->platform != HENGE_PLATFORM_IOS && opts->platform != HENGE_PLATFORM_MACOS &&
        opts->platform != HENGE_PLATFORM_IOSSIM)
        return fail(err, errlen, "unknown platform %d", (int)opts->platform);

    /* ---- 1. ELF header ---- */
    if (elf_size < sizeof(Elf64_Ehdr)) return fail(err, errlen, "file too small for an ELF header");
    e.eh = (const Elf64_Ehdr *)elf;
    if (memcmp(e.eh->e_ident, "\x7f" "ELF", 4) != 0) return fail(err, errlen, "not an ELF file (bad magic)");
    if (e.eh->e_ident[4] != ELFCLASS64) return fail(err, errlen, "not ELFCLASS64 (32-bit ELF is not supported)");
    if (e.eh->e_ident[5] != ELFDATA2LSB) return fail(err, errlen, "not little-endian");
    if (e.eh->e_machine != EM_AARCH64)
        return fail(err, errlen, "e_machine is %u, expected EM_AARCH64 (183)", e.eh->e_machine);
    if (e.eh->e_type != ET_DYN) return fail(err, errlen, "e_type is %u, expected ET_DYN (shared object)", e.eh->e_type);
    if (e.eh->e_phentsize != sizeof(Elf64_Phdr)) return fail(err, errlen, "unexpected e_phentsize %u", e.eh->e_phentsize);
    if (e.eh->e_phoff > elf_size || (uint64_t)e.eh->e_phnum * sizeof(Elf64_Phdr) > elf_size - e.eh->e_phoff)
        return fail(err, errlen, "program headers extend past end of file");
    if (e.eh->e_phoff % 8) return fail(err, errlen, "misaligned program headers");
    e.ph = (const Elf64_Phdr *)(elf + e.eh->e_phoff);

    /* ---- 2. PT_LOADs and validation ---- */
    const Elf64_Phdr *dyn_ph = NULL;
    for (unsigned i = 0; i < e.eh->e_phnum; i++) {
        const Elf64_Phdr *p = &e.ph[i];
        if (p->p_type == PT_DYNAMIC) dyn_ph = p;
        if (p->p_type != PT_LOAD) continue;
        if (e.nloads == MAX_LOADS) return fail(err, errlen, "too many PT_LOAD segments (max %d)", MAX_LOADS);
        if (p->p_offset > elf_size || p->p_filesz > elf_size - p->p_offset)
            return fail(err, errlen, "PT_LOAD #%u file range extends past end of file", e.nloads);
        if (p->p_filesz > p->p_memsz) return fail(err, errlen, "PT_LOAD #%u has p_filesz > p_memsz", e.nloads);
        if (p->p_vaddr > (1ull << 32) || p->p_memsz > (1ull << 32))
            return fail(err, errlen, "PT_LOAD #%u is unreasonably large", e.nloads);
        if (e.nloads > 0) {
            const Elf64_Phdr *q = &e.loads[e.nloads - 1];
            if (p->p_vaddr < q->p_vaddr + q->p_memsz)
                return fail(err, errlen, "PT_LOAD #%u overlaps or is not sorted after the previous one", e.nloads);
        }
        e.loads[e.nloads++] = *p;
    }
    if (e.nloads == 0) return fail(err, errlen, "no PT_LOAD segments");
    if (e.loads[0].p_vaddr != 0 || e.loads[0].p_offset != 0)
        return fail(err, errlen,
                    "first PT_LOAD must have p_vaddr 0 and p_offset 0 (got vaddr 0x%llx offset 0x%llx); "
                    "the ELF header must be mapped at the image base",
                    (unsigned long long)e.loads[0].p_vaddr, (unsigned long long)e.loads[0].p_offset);

    unsigned first_w = e.nloads;
    for (unsigned i = 0; i < e.nloads; i++) {
        int w = (e.loads[i].p_flags & PF_W) != 0;
        if (w && first_w == e.nloads) first_w = i;
        if (!w && first_w != e.nloads)
            return fail(err, errlen, "non-writable PT_LOAD #%u follows a writable one; expected R/RX segments before RW", i);
    }
    if (first_w == 0) return fail(err, errlen, "first PT_LOAD is writable; the ELF header must be in a read-only segment");
    if (first_w == e.nloads) return fail(err, errlen, "no writable PT_LOAD (expected .dynamic/.got in an RW segment)");

    uint64_t text_end = 0; /* end of non-writable memory image */
    for (unsigned i = 0; i < first_w; i++) {
        uint64_t end = e.loads[i].p_vaddr + e.loads[i].p_memsz;
        if (end > text_end) text_end = end;
    }
    uint64_t w_min = e.loads[first_w].p_vaddr, w_file_end = 0, w_mem_end = 0;
    for (unsigned i = first_w; i < e.nloads; i++) {
        const Elf64_Phdr *p = &e.loads[i];
        if (p->p_vaddr + p->p_filesz > w_file_end) w_file_end = p->p_vaddr + p->p_filesz;
        if (p->p_vaddr + p->p_memsz > w_mem_end) w_mem_end = p->p_vaddr + p->p_memsz;
    }
    uint64_t data_start = align_down(w_min, PAGE);
    if (align_up(text_end, PAGE) > data_start)
        return fail(err, errlen,
                    "code/read-only data (ends at 0x%llx) and writable data (starts at 0x%llx) share a 16 KiB page; "
                    "relink with -Wl,-z,max-page-size=16384",
                    (unsigned long long)text_end, (unsigned long long)w_min);

    /* ---- 3. dynamic section: TEXTREL, NEEDED, SONAME ---- */
    const Elf64_Dyn *dyn = NULL;
    size_t ndyn = 0;
    uint64_t strtab = 0, strsz = 0;
    int have_strtab = 0;
    if (!dyn_ph) return fail(err, errlen, "no PT_DYNAMIC (not a dynamically linked shared object)");
    if (dyn_ph->p_offset > elf_size || dyn_ph->p_filesz > elf_size - dyn_ph->p_offset || dyn_ph->p_offset % 8)
        return fail(err, errlen, "PT_DYNAMIC file range is invalid");
    dyn = (const Elf64_Dyn *)(elf + dyn_ph->p_offset);
    ndyn = dyn_ph->p_filesz / sizeof(Elf64_Dyn);
    for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
        case DT_TEXTREL:
            return fail(err, errlen, "DT_TEXTREL present: text relocations are not supported");
        case DT_FLAGS:
            if (dyn[i].d_val & DF_TEXTREL) return fail(err, errlen, "DF_TEXTREL set: text relocations are not supported");
            break;
        case DT_STRTAB: strtab = dyn[i].d_val; have_strtab = 1; break;
        case DT_STRSZ: strsz = dyn[i].d_val; break;
        default: break;
        }
    }
    const char *dynstr = NULL;
    if (have_strtab) {
        uint64_t off;
        if (strsz == 0 || vaddr_to_off(&e, strtab, strsz, &off) != 0)
            return fail(err, errlen, "DT_STRTAB/DT_STRSZ does not map to file data");
        dynstr = (const char *)elf + off;
    }

    /* ---- 4. code scan over executable PT_LOADs ---- */
    unsigned svc = 0, tpidr = 0;
    for (unsigned i = 0; i < first_w; i++) {
        const Elf64_Phdr *p = &e.loads[i];
        if (!(p->p_flags & PF_X)) continue;
        const uint8_t *c = elf + p->p_offset;
        for (uint64_t k = 0; k + 4 <= p->p_filesz; k += 4) {
            uint32_t w;
            memcpy(&w, c + k, 4);
            if (w == 0xd4000001u) svc++;                          /* svc #0 */
            else if ((w & 0xffffffe0u) == 0xd53bd040u) tpidr++;   /* mrs xN, tpidr_el0 */
        }
    }
    /* TODO(M2): detect x18 usage (platform register on Darwin) and warn. */
    if (svc > 0) return fail(err, errlen, "found %u svc #0 instruction(s): svc patching not implemented yet (M2)", svc);

    /* ---- 5. Mach-O layout ---- */
    uint64_t text_size = H + data_start;
    uint64_t data_addr = H + data_start;
    uint64_t data_filesize = align_up(w_file_end, PAGE) - data_start;
    uint64_t data_vmsize = align_up(w_mem_end, PAGE) - data_start;
    uint64_t le_off = data_addr + data_filesize;
    uint64_t le_addr = data_addr + data_vmsize;

    const size_t strtab_raw = 1 + sizeof(MARKER_SYM); /* leading NUL + name + NUL */
    const uint32_t le_strsize = (uint32_t)align_up(strtab_raw, 8);
    const uint64_t le_size = sizeof(struct nlist_64) + le_strsize;

    uint32_t id_size = lc_str_size(sizeof(struct dylib_command), opts->install_name);
    uint32_t ls_size = lc_str_size(sizeof(struct dylib_command), LIBSYSTEM_PATH);
    uint32_t ncmds = 8, sizeofcmds = 0; /* 3 segments, id, build, uuid, symtab, dysymtab */
    sizeofcmds += 3 * sizeof(struct segment_command_64) + 2 * sizeof(struct section_64);
    sizeofcmds += id_size + sizeof(struct build_version_command) + sizeof(struct uuid_command) +
                  sizeof(struct symtab_command) + sizeof(struct dysymtab_command);
    if (opts->link_libsystem) { ncmds++; sizeofcmds += ls_size; }
    /* Leave room for codesign's LC_CODE_SIGNATURE (and then some). */
    if (sizeof(struct mach_header_64) + sizeofcmds + 256 > H)
        return fail(err, errlen, "load commands do not fit in the %u-byte header area (install name too long?)",
                    (unsigned)H);

    size_t total = (size_t)(le_off + le_size);
    uint8_t *mo = calloc(1, total);
    if (!mo) return fail(err, errlen, "out of memory");

    /* ELF image bytes at H + vaddr. */
    for (unsigned i = 0; i < e.nloads; i++)
        memcpy(mo + H + e.loads[i].p_vaddr, elf + e.loads[i].p_offset, e.loads[i].p_filesz);

    size_t o = 0;
    struct mach_header_64 mh = {
        .magic = MH_MAGIC_64,
        .cputype = CPU_TYPE_ARM64,
        .cpusubtype = CPU_SUBTYPE_ARM64_ALL,
        .filetype = MH_DYLIB,
        .ncmds = ncmds,
        .sizeofcmds = sizeofcmds,
        .flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_NO_REEXPORTED_DYLIBS,
    };
    put(mo, &o, &mh, sizeof mh);

    struct segment_command_64 seg = {
        .cmd = LC_SEGMENT_64,
        .cmdsize = sizeof(struct segment_command_64) + sizeof(struct section_64),
        .segname = SEG_TEXT,
        .vmaddr = 0,
        .vmsize = text_size,
        .fileoff = 0,
        .filesize = text_size,
        .maxprot = VM_PROT_READ | VM_PROT_EXECUTE,
        .initprot = VM_PROT_READ | VM_PROT_EXECUTE,
        .nsects = 1,
    };
    put(mo, &o, &seg, sizeof seg);
    struct section_64 sect = {
        .sectname = "__elf",
        .segname = SEG_TEXT,
        .addr = H,
        .size = text_end,
        .offset = (uint32_t)H,
        .align = 14,
        .flags = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
    };
    put(mo, &o, &sect, sizeof sect);

    seg = (struct segment_command_64){
        .cmd = LC_SEGMENT_64,
        .cmdsize = sizeof(struct segment_command_64) + sizeof(struct section_64),
        .segname = SEG_DATA,
        .vmaddr = data_addr,
        .vmsize = data_vmsize,
        .fileoff = data_addr,
        .filesize = data_filesize,
        .maxprot = VM_PROT_READ | VM_PROT_WRITE,
        .initprot = VM_PROT_READ | VM_PROT_WRITE,
        .nsects = 1,
    };
    put(mo, &o, &seg, sizeof seg);
    sect = (struct section_64){
        .sectname = "__elf_data",
        .segname = SEG_DATA,
        .addr = data_addr,
        .size = data_filesize,
        .offset = (uint32_t)data_addr,
        .align = 14,
        .flags = S_REGULAR,
    };
    put(mo, &o, &sect, sizeof sect);

    seg = (struct segment_command_64){
        .cmd = LC_SEGMENT_64,
        .cmdsize = sizeof(struct segment_command_64),
        .segname = SEG_LINKEDIT,
        .vmaddr = le_addr,
        .vmsize = align_up(le_size, PAGE),
        .fileoff = le_off,
        .filesize = le_size,
        .maxprot = VM_PROT_READ,
        .initprot = VM_PROT_READ,
    };
    put(mo, &o, &seg, sizeof seg);

    struct dylib_command dc = {
        .cmd = LC_ID_DYLIB,
        .cmdsize = id_size,
        .dylib = {.name.offset = sizeof(struct dylib_command), .timestamp = 1,
                  .current_version = 0x10000, .compatibility_version = 0x10000},
    };
    size_t start = o;
    put(mo, &o, &dc, sizeof dc);
    put(mo, &o, opts->install_name, strlen(opts->install_name));
    o = start + id_size; /* NUL + padding already zero */

    uint32_t ver = opts->platform == HENGE_PLATFORM_MACOS ? (13u << 16) : (16u << 16); /* macOS 13.0, else iOS 16.0 */
    struct build_version_command bv = {
        .cmd = LC_BUILD_VERSION,
        .cmdsize = sizeof(struct build_version_command),
        .platform = (uint32_t)opts->platform,
        .minos = ver,
        .sdk = ver,
        .ntools = 0,
    };
    put(mo, &o, &bv, sizeof bv);

    struct uuid_command uc = {.cmd = LC_UUID, .cmdsize = sizeof(struct uuid_command)};
    if (find_build_id(&e, uc.uuid) != 0) hash_uuid(elf, elf_size, uc.uuid);
    put(mo, &o, &uc, sizeof uc);

    struct symtab_command st = {
        .cmd = LC_SYMTAB,
        .cmdsize = sizeof(struct symtab_command),
        .symoff = (uint32_t)le_off,
        .nsyms = 1,
        .stroff = (uint32_t)(le_off + sizeof(struct nlist_64)),
        .strsize = le_strsize,
    };
    put(mo, &o, &st, sizeof st);

    struct dysymtab_command dst = {
        .cmd = LC_DYSYMTAB,
        .cmdsize = sizeof(struct dysymtab_command),
        .ilocalsym = 0, .nlocalsym = 0,
        .iextdefsym = 0, .nextdefsym = 1,
        .iundefsym = 1, .nundefsym = 0,
    };
    put(mo, &o, &dst, sizeof dst);

    if (opts->link_libsystem) {
        dc = (struct dylib_command){
            .cmd = LC_LOAD_DYLIB,
            .cmdsize = ls_size,
            .dylib = {.name.offset = sizeof(struct dylib_command), .timestamp = 2,
                      .current_version = (1351u << 16), .compatibility_version = 0x10000},
        };
        start = o;
        put(mo, &o, &dc, sizeof dc);
        put(mo, &o, LIBSYSTEM_PATH, strlen(LIBSYSTEM_PATH));
        o = start + ls_size;
    }
    if (o != sizeof mh + sizeofcmds) {
        free(mo);
        return fail(err, errlen, "internal error: load command size mismatch (%zu vs %zu)", o, sizeof mh + (size_t)sizeofcmds);
    }

    /* __LINKEDIT: symbol table + string table. */
    struct nlist_64 nl = {.n_un.n_strx = 1, .n_type = N_SECT | N_EXT, .n_sect = 1, .n_desc = 0, .n_value = H};
    o = (size_t)le_off;
    put(mo, &o, &nl, sizeof nl);
    o++; /* leading NUL */
    put(mo, &o, MARKER_SYM, sizeof(MARKER_SYM));

    /* ---- 6. manifest ---- */
    struct sbuf b = {0};
    sb_printf(&b, "{\n  \"format\": \"henge-manifest-1\",\n");
    const char *soname = NULL;
    for (size_t i = 0; dynstr && i < ndyn && dyn[i].d_tag != DT_NULL; i++)
        if (dyn[i].d_tag == DT_SONAME && dyn[i].d_val < strsz) soname = dynstr + dyn[i].d_val;
    sb_printf(&b, "  \"soname\": ");
    if (soname) sb_json_str(&b, soname, (size_t)(strsz - (uint64_t)(soname - dynstr)));
    else sb_printf(&b, "null");
    sb_printf(&b, ",\n  \"install_name\": ");
    sb_json_str(&b, opts->install_name, strlen(opts->install_name));
    sb_printf(&b, ",\n  \"shift\": %llu,\n  \"page_size\": %llu,\n", (unsigned long long)H, (unsigned long long)PAGE);
    sb_printf(&b, "  \"dynamic_vaddr\": %llu,\n", (unsigned long long)dyn_ph->p_vaddr);
    sb_printf(&b, "  \"segments\": [\n");
    for (unsigned i = 0; i < e.nloads; i++) {
        const Elf64_Phdr *p = &e.loads[i];
        sb_printf(&b, "    {\"vaddr\": %llu, \"memsz\": %llu, \"filesz\": %llu, \"flags\": \"%c%c%c\"}%s\n",
                  (unsigned long long)p->p_vaddr, (unsigned long long)p->p_memsz, (unsigned long long)p->p_filesz,
                  (p->p_flags & PF_R) ? 'r' : '-', (p->p_flags & PF_W) ? 'w' : '-', (p->p_flags & PF_X) ? 'x' : '-',
                  i + 1 < e.nloads ? "," : "");
    }
    sb_printf(&b, "  ],\n  \"needed\": [");
    int first = 1;
    for (size_t i = 0; dynstr && i < ndyn && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag != DT_NEEDED || dyn[i].d_val >= strsz) continue;
        sb_printf(&b, first ? "" : ", ");
        sb_json_str(&b, dynstr + dyn[i].d_val, (size_t)(strsz - dyn[i].d_val));
        first = 0;
    }
    sb_printf(&b, "],\n  \"svc_count\": %u,\n  \"tpidr_count\": %u\n}\n", svc, tpidr);
    if (b.oom) {
        free(b.p);
        free(mo);
        return fail(err, errlen, "out of memory");
    }

    out->macho = mo;
    out->macho_size = total;
    out->manifest_json = b.p;
    out->svc_count = svc;
    out->tpidr_count = tpidr;
    return 0;
}
