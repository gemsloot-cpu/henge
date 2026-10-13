/* Mach-O emission: wrap the ELF image in a minimal MH_DYLIB (docs/m1-elf-to-macho.md §4). */
#include <stdlib.h>
#include <string.h>

#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "internal.h"

#define H HC_SHIFT
#define PAGE HC_PAGE
#define MARKER_SYM "_henge_image_marker"
#define LIBSYSTEM_PATH "/usr/lib/libSystem.B.dylib"

static void put(uint8_t *out, size_t *off, const void *src, size_t n) {
    memcpy(out + *off, src, n);
    *off += n;
}

static uint32_t lc_str_size(size_t base, const char *s) { return (uint32_t)hc_align_up(base + strlen(s) + 1, 8); }

int hc_macho_write(const struct elf_info *e, const struct henge_convert_opts *opts, uint8_t **out,
                   size_t *out_size, char *err, size_t errlen) {
    /* ---- layout ---- */
    uint64_t text_size = H + e->data_start;
    uint64_t data_addr = H + e->data_start;
    uint64_t data_filesize = hc_align_up(e->w_file_end, PAGE) - e->data_start;
    uint64_t data_vmsize = hc_align_up(e->w_mem_end, PAGE) - e->data_start;
    uint64_t le_off = data_addr + data_filesize;
    uint64_t le_addr = data_addr + data_vmsize;

    const size_t strtab_raw = 1 + sizeof(MARKER_SYM); /* leading NUL + name + NUL */
    const uint32_t le_strsize = (uint32_t)hc_align_up(strtab_raw, 8);
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
        return hc_fail(err, errlen, "load commands do not fit in the %u-byte header area (install name too long?)",
                       (unsigned)H);

    size_t total = (size_t)(le_off + le_size);
    uint8_t *mo = calloc(1, total);
    if (!mo) return hc_fail(err, errlen, "out of memory");

    /* ELF image bytes at H + vaddr. */
    for (unsigned i = 0; i < e->nloads; i++)
        memcpy(mo + H + e->loads[i].p_vaddr, e->data + e->loads[i].p_offset, e->loads[i].p_filesz);

    /* ---- header and load commands ---- */
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
        .size = e->text_end,
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
        .vmsize = hc_align_up(le_size, PAGE),
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
    hc_elf_uuid(e, uc.uuid);
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
        return hc_fail(err, errlen, "internal error: load command size mismatch (%zu vs %zu)", o,
                       sizeof mh + (size_t)sizeofcmds);
    }

    /* ---- __LINKEDIT: symbol table + string table ---- */
    struct nlist_64 nl = {.n_un.n_strx = 1, .n_type = N_SECT | N_EXT, .n_sect = 1, .n_desc = 0, .n_value = H};
    o = (size_t)le_off;
    put(mo, &o, &nl, sizeof nl);
    o++; /* leading NUL */
    put(mo, &o, MARKER_SYM, sizeof(MARKER_SYM));

    *out = mo;
    *out_size = total;
    return 0;
}
