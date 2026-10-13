/* Mach-O emission: wrap the ELF image in a minimal MH_DYLIB (docs/m1-elf-to-macho.md §4). */
#include <stdlib.h>
#include <string.h>

#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "internal.h"

#define PAGE HC_PAGE
#define MARKER_SYM "_henge_image_marker"
#define LIBSYSTEM_PATH "/usr/lib/libSystem.B.dylib"
#define CODESIG_ROOM 256 /* left after the load commands for the signer's LC_CODE_SIGNATURE */

static void put(uint8_t *out, size_t *off, const void *src, size_t n) {
    memcpy(out + *off, src, n);
    *off += n;
}

static uint32_t lc_str_size(size_t base, const char *s) { return (uint32_t)hc_align_up(base + strlen(s) + 1, 8); }

/* ---- AArch64 encodings used by the island ---- */

#define X_SP 31
#define A64_RET 0xd65f03c0u
#define A64_NOP 0xd503201fu
#define A64_STR_LR_PRE 0xf81f0ffeu  /* str x30, [sp, #-16]! */
#define A64_LDR_LR_POST 0xf84107feu /* ldr x30, [sp], #16 */

static int branch(uint32_t op, uint64_t from, uint64_t to, uint32_t *w) {
    int64_t d = (int64_t)(to - from);
    if (d < -(1ll << 27) || d >= (1ll << 27)) return -1;
    *w = op | (uint32_t)((d >> 2) & 0x3ffffff);
    return 0;
}
static uint32_t a64_stp_pre16(unsigned t, unsigned t2) { return 0xa9bf0000u | t2 << 10 | X_SP << 5 | t; }
static uint32_t a64_ldp_post16(unsigned t, unsigned t2) { return 0xa8c10000u | t2 << 10 | X_SP << 5 | t; }
static uint32_t a64_adrp(unsigned d, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | (imm & 3) << 29 | (imm >> 2) << 5 | d;
}
static uint32_t a64_ldr_uoff(unsigned t, unsigned n, uint64_t addr) { return 0xf9400000u | (uint32_t)((addr & 0xfff) >> 3) << 10 | n << 5 | t; }
static uint32_t a64_ldr_reg(unsigned t, unsigned n, unsigned m) { return 0xf8606800u | m << 16 | n << 5 | t; }
static uint32_t a64_mrs_tpidrro(unsigned t) { return 0xd53bd060u | t; }
static uint32_t a64_and_not7(unsigned d, unsigned n) { return 0x927df000u | n << 5 | d; }
static uint32_t a64_cbnz(unsigned t, int insns) { return 0xb5000000u | ((uint32_t)insns & 0x7ffff) << 5 | t; }
static uint32_t a64_blr(unsigned n) { return 0xd63f0000u | n << 5; }
static uint32_t a64_mov(unsigned d, unsigned m) { return 0xaa0003e0u | m << 16 | d; }

/* ---- patch island (docs/m2-patching.md §3) ----
 *
 *   henge_island_hdr
 *   svc thunk            calls *slot[SVC] (preserves everything but x0)
 *   tpidr thunk per Xn   xN = Darwin TSD[slot[TP_KEY]], allocating via *slot[TP_SLOW] if NULL
 *   trampoline per site  str x30, [sp,#-16]! ; bl thunk ; ldr x30, [sp],#16 ; b site+4
 */

#define SVC_THUNK_INSNS 6
#define TP_THUNK_INSNS 13
#define TRAMP_INSNS 4

struct island {
    uint64_t addr, size; /* Mach-O vmaddr (== file offset) and size */
    uint64_t svc_thunk;
    uint64_t tp_thunk[31];
    uint64_t tramps;
    uint64_t slots;      /* Mach-O vmaddr of the slot words */
};

static uint64_t island_plan(const struct scan_result *scan, uint64_t start, struct island *is) {
    memset(is, 0, sizeof *is);
    is->addr = start;
    uint64_t o = start + sizeof(struct henge_island_hdr);
    int have_svc = 0, regs[31] = {0};
    for (size_t i = 0; i < scan->npatches; i++) {
        if (scan->patches[i].kind == HC_PATCH_SVC) have_svc = 1;
        else if (scan->patches[i].reg < 31) regs[scan->patches[i].reg] = 1;
    }
    if (have_svc) { is->svc_thunk = o; o += 4 * SVC_THUNK_INSNS; }
    for (int r = 0; r < 31; r++)
        if (regs[r]) { is->tp_thunk[r] = o; o += 4 * TP_THUNK_INSNS; }
    is->tramps = o;
    o += 4ull * TRAMP_INSNS * scan->npatches;
    is->size = o - start;
    return o;
}

static void emit(uint8_t *mo, uint64_t *pc, uint32_t w) {
    memcpy(mo + *pc, &w, 4);
    *pc += 4;
}

static void emit_svc_thunk(uint8_t *mo, const struct island *is) {
    uint64_t pc = is->svc_thunk, slot = is->slots + 8 * HENGE_SLOT_SVC;
    emit(mo, &pc, a64_stp_pre16(16, 30));
    emit(mo, &pc, a64_adrp(16, pc, slot));
    emit(mo, &pc, a64_ldr_uoff(16, 16, slot));
    emit(mo, &pc, a64_blr(16));
    emit(mo, &pc, a64_ldp_post16(16, 30));
    emit(mo, &pc, A64_RET);
}

static void emit_tp_thunk(uint8_t *mo, const struct island *is, unsigned n) {
    unsigned s = n == 16 ? 17 : 16;
    uint64_t pc = is->tp_thunk[n];
    uint64_t key = is->slots + 8 * HENGE_SLOT_TP_KEY, slow = is->slots + 8 * HENGE_SLOT_TP_SLOW;
    emit(mo, &pc, a64_stp_pre16(s, 30));
    emit(mo, &pc, a64_adrp(s, pc, key));
    emit(mo, &pc, a64_ldr_uoff(s, s, key));
    emit(mo, &pc, a64_mrs_tpidrro(n));
    emit(mo, &pc, a64_and_not7(n, n));
    emit(mo, &pc, a64_ldr_reg(n, n, s));
    emit(mo, &pc, a64_cbnz(n, 5));
    emit(mo, &pc, a64_adrp(s, pc, slow));
    emit(mo, &pc, a64_ldr_uoff(s, s, slow));
    emit(mo, &pc, a64_blr(s)); /* returns the thread pointer in x16 */
    emit(mo, &pc, n == 16 ? A64_NOP : a64_mov(n, 16));
    emit(mo, &pc, a64_ldp_post16(s, 30));
    emit(mo, &pc, A64_RET);
}

static int apply_patches(uint8_t *mo, uint64_t H, const struct scan_result *scan, const struct island *is,
                         char *err, size_t errlen) {
    struct henge_island_hdr hdr = {
        .magic = HENGE_ISLAND_MAGIC,
        .version = HENGE_ISLAND_VERSION,
        .slots_vmaddr = is->slots,
    };
    if (is->svc_thunk) emit_svc_thunk(mo, is);
    for (unsigned r = 0; r < 31; r++)
        if (is->tp_thunk[r]) emit_tp_thunk(mo, is, r);
    uint64_t t = is->tramps;
    for (size_t i = 0; i < scan->npatches; i++) {
        const struct hc_patch *p = &scan->patches[i];
        uint64_t site = H + p->vaddr;
        uint32_t w;
        if (p->kind == HC_PATCH_TPIDR && p->reg == 31) { /* mrs xzr: no effect */
            memcpy(mo + site, &(uint32_t){A64_NOP}, 4);
            t += 4 * TRAMP_INSNS;
            continue;
        }
        uint64_t thunk = p->kind == HC_PATCH_SVC ? is->svc_thunk : is->tp_thunk[p->reg];
        if (p->kind == HC_PATCH_SVC) hdr.svc_sites++;
        else hdr.tpidr_sites++;
        uint64_t pc = t;
        emit(mo, &pc, A64_STR_LR_PRE);
        if (branch(0x94000000u, pc, thunk, &w)) goto range;
        emit(mo, &pc, w);
        emit(mo, &pc, A64_LDR_LR_POST);
        if (branch(0x14000000u, pc, site + 4, &w)) goto range;
        emit(mo, &pc, w);
        if (branch(0x14000000u, site, t, &w)) goto range;
        memcpy(mo + site, &w, 4);
        t = pc;
        continue;
    range:
        return hc_fail(err, errlen, "patch site 0x%llx is out of branch range of the island (code > 128 MiB?)",
                       (unsigned long long)p->vaddr);
    }
    memcpy(mo + is->addr, &hdr, sizeof hdr);
    return 0;
}

int hc_macho_write(const struct elf_info *e, const struct henge_convert_opts *opts,
                   const struct scan_result *scan, uint8_t **out, size_t *out_size, char *err,
                   size_t errlen) {
    int patched = scan->npatches > 0;

    const size_t strtab_raw = 1 + sizeof(MARKER_SYM); /* leading NUL + name + NUL */
    const uint32_t le_strsize = (uint32_t)hc_align_up(strtab_raw, 8);
    const uint64_t le_size = sizeof(struct nlist_64) + le_strsize;

    uint32_t id_size = lc_str_size(sizeof(struct dylib_command), opts->install_name);
    uint32_t ls_size = lc_str_size(sizeof(struct dylib_command), LIBSYSTEM_PATH);
    uint32_t ncmds = 8, sizeofcmds = 0; /* 3 segments, id, build, uuid, symtab, dysymtab */
    sizeofcmds += 3 * sizeof(struct segment_command_64) + (2 + patched) * sizeof(struct section_64);
    sizeofcmds += id_size + sizeof(struct build_version_command) + sizeof(struct uuid_command) +
                  sizeof(struct symtab_command) + sizeof(struct dysymtab_command);
    if (opts->link_libsystem) { ncmds++; sizeofcmds += ls_size; }

    /* ---- layout ---- */
    uint64_t lc_end = sizeof(struct mach_header_64) + sizeofcmds + CODESIG_ROOM;
    struct island is = {0};
    uint64_t H = HC_SHIFT;
    if (patched) {
        uint64_t end = island_plan(scan, hc_align_up(lc_end, 16), &is);
        if (hc_align_up(end, PAGE) > H) H = hc_align_up(end, PAGE);
    } else if (lc_end > H) {
        return hc_fail(err, errlen, "load commands do not fit in the %u-byte header area (install name too long?)",
                       (unsigned)H);
    }
    uint64_t slots_vaddr = hc_align_up(e->w_mem_end, 16); /* ELF vaddr just past the image */
    uint64_t mem_end = patched ? slots_vaddr + 8 * HENGE_NSLOTS : e->w_mem_end;
    if (patched) is.slots = H + slots_vaddr;

    uint64_t text_size = H + e->data_start;
    uint64_t data_addr = H + e->data_start;
    uint64_t data_filesize = hc_align_up(e->w_file_end, PAGE) - e->data_start;
    uint64_t data_vmsize = hc_align_up(mem_end, PAGE) - e->data_start;
    uint64_t le_off = data_addr + data_filesize;
    uint64_t le_addr = data_addr + data_vmsize;

    size_t total = (size_t)(le_off + le_size);
    uint8_t *mo = calloc(1, total);
    if (!mo) return hc_fail(err, errlen, "out of memory");

    /* ELF image bytes at H + vaddr, then the code patches. */
    for (unsigned i = 0; i < e->nloads; i++)
        memcpy(mo + H + e->loads[i].p_vaddr, e->data + e->loads[i].p_offset, e->loads[i].p_filesz);
    if (patched && apply_patches(mo, H, scan, &is, err, errlen) != 0) {
        free(mo);
        return -1;
    }

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
        .cmdsize = sizeof(struct segment_command_64) + (1 + patched) * sizeof(struct section_64),
        .segname = SEG_TEXT,
        .vmaddr = 0,
        .vmsize = text_size,
        .fileoff = 0,
        .filesize = text_size,
        .maxprot = VM_PROT_READ | VM_PROT_EXECUTE,
        .initprot = VM_PROT_READ | VM_PROT_EXECUTE,
        .nsects = 1 + patched,
    };
    put(mo, &o, &seg, sizeof seg);
    struct section_64 sect;
    if (patched) {
        sect = (struct section_64){
            .sectname = "__henge",
            .segname = SEG_TEXT,
            .addr = is.addr,
            .size = is.size,
            .offset = (uint32_t)is.addr,
            .align = 4,
            .flags = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
        };
        put(mo, &o, &sect, sizeof sect);
    }
    sect = (struct section_64){
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
    struct nlist_64 nl = {.n_un.n_strx = 1, .n_type = N_SECT | N_EXT, .n_sect = (uint8_t)(1 + patched),
                          .n_desc = 0, .n_value = H};
    o = (size_t)le_off;
    put(mo, &o, &nl, sizeof nl);
    o++; /* leading NUL */
    put(mo, &o, MARKER_SYM, sizeof(MARKER_SYM));

    *out = mo;
    *out_size = total;
    return 0;
}
