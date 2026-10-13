/* M1 layout test: checks a converted dylib against its source ELF.
 *   test_layout in.so out.dylib [ios|macos]
 * Checks (docs/m1-elf-to-macho.md §3): __TEXT at vmaddr 0 / fileoff 0, r-x; every ELF
 * PT_LOAD's bytes appear at vmaddr p_vaddr + H; __DATA is rw- and starts at
 * rounddown(min writable vaddr) + H; LC_BUILD_VERSION platform; LC_ID_DYLIB present;
 * the embedded ad-hoc signature is well formed and its page hashes match the file. */
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/loader.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "henge/elf64.h"
#include "henge/henge.h"

static int failures;

static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s  ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) failures++;
}

static unsigned char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: read failed\n", path);
        exit(2);
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint32_t rd_be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Checks the SuperBlob at [off, off + size): CodeDirectory present, SHA-256, every code
 * page hash matches the file, codeLimit == signature offset. */
static void check_signature(const unsigned char *mo, size_t mo_size, uint32_t off, uint32_t size) {
    if ((uint64_t)off + size > mo_size || size < 12) {
        check(0, "code signature inside the file (dataoff 0x%x datasize 0x%x)", off, size);
        return;
    }
    const unsigned char *sb = mo + off;
    check(rd_be32(sb) == 0xfade0cc0u, "SuperBlob magic 0xfade0cc0 (0x%x)", rd_be32(sb));
    uint32_t count = rd_be32(sb + 8);
    const unsigned char *cd = NULL;
    for (uint32_t i = 0; i < count && 12 + 8 * (i + 1) <= size; i++) {
        uint32_t type = rd_be32(sb + 12 + 8 * i), at = rd_be32(sb + 16 + 8 * i);
        if (type == 0 && at + 88 <= size) cd = sb + at;
    }
    check(cd != NULL && rd_be32(cd) == 0xfade0c02u, "CodeDirectory present");
    if (!cd) return;
    uint32_t hash_off = rd_be32(cd + 16), ident_off = rd_be32(cd + 20), ncode = rd_be32(cd + 28);
    uint32_t limit = rd_be32(cd + 32);
    unsigned hash_size = cd[36], hash_type = cd[37], page_log2 = cd[39];
    printf("info  CodeDirectory v0x%x flags 0x%x ident \"%s\" ncode %u codeLimit 0x%x\n",
           rd_be32(cd + 8), rd_be32(cd + 12), (const char *)cd + ident_off, ncode, limit);
    check(hash_type == 2 && hash_size == 32, "CodeDirectory hash type SHA-256");
    check(limit == off, "codeLimit 0x%x == signature offset 0x%x", limit, off);
    uint32_t page = 1u << page_log2, bad = 0;
    for (uint32_t i = 0; i < ncode; i++) {
        uint64_t start = (uint64_t)i * page;
        uint64_t n = limit - start < page ? limit - start : page;
        unsigned char h[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(mo + start, (CC_LONG)n, h);
        if (memcmp(h, cd + hash_off + 32 * i, 32) != 0) bad++;
    }
    check(ncode == (limit + page - 1) / page && bad == 0, "%u code page hashes match the file (%u bad)",
          ncode, bad);
}

static const char *prot_str(vm_prot_t p) {
    static char s[4][4];
    static int i;
    char *o = s[i++ & 3];
    o[0] = (p & VM_PROT_READ) ? 'r' : '-';
    o[1] = (p & VM_PROT_WRITE) ? 'w' : '-';
    o[2] = (p & VM_PROT_EXECUTE) ? 'x' : '-';
    o[3] = 0;
    return o;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.so out.dylib [ios|macos]\n", argv[0]);
        return 2;
    }
    uint32_t want_platform = HENGE_PLATFORM_MACOS;
    if (argc > 3 && strcmp(argv[3], "ios") == 0) want_platform = HENGE_PLATFORM_IOS;

    size_t elf_size, mo_size;
    unsigned char *elf = slurp(argv[1], &elf_size);
    unsigned char *mo = slurp(argv[2], &mo_size);
    const uint64_t H = HENGE_SHIFT;

    /* ---- ELF side ---- */
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);
    uint64_t min_w = UINT64_MAX, max_w_end = 0, max_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end > max_end) max_end = end;
        if (ph[i].p_flags & PF_W) {
            if (ph[i].p_vaddr < min_w) min_w = ph[i].p_vaddr;
            if (end > max_w_end) max_w_end = end;
        }
    }
    if (min_w == UINT64_MAX) {
        fprintf(stderr, "ELF has no writable PT_LOAD\n");
        return 2;
    }
    uint64_t want_data_vmaddr = (min_w & ~(uint64_t)(HENGE_PAGE - 1)) + H;

    /* ---- Mach-O side ---- */
    const struct mach_header_64 *mh = (const struct mach_header_64 *)mo;
    check(mh->magic == MH_MAGIC_64, "Mach-O magic MH_MAGIC_64 (0x%x)", mh->magic);
    check(mh->cputype == CPU_TYPE_ARM64, "cputype ARM64 (0x%x)", mh->cputype);
    check(mh->filetype == MH_DYLIB, "filetype MH_DYLIB (%u)", mh->filetype);
    if (failures) goto done;

    const struct segment_command_64 *text = NULL, *data = NULL, *linkedit = NULL;
    const struct build_version_command *bv = NULL;
    const struct linkedit_data_command *codesig = NULL;
    int have_id = 0, have_chained = 0, have_libsystem = 0, have_codesig = 0;
    const unsigned char *lc = mo + sizeof *mh;
    uint64_t cmds_total = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *c = (const struct load_command *)lc;
        cmds_total += c->cmdsize;
        if (c->cmdsize < sizeof *c || cmds_total > mh->sizeofcmds) break;
        switch (c->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *s = (const void *)c;
            printf("info  segment %-12s vmaddr 0x%06llx vmsize 0x%06llx fileoff 0x%06llx "
                   "filesize 0x%06llx %s/%s\n",
                   s->segname, s->vmaddr, s->vmsize, s->fileoff, s->filesize,
                   prot_str(s->initprot), prot_str(s->maxprot));
            if (!strcmp(s->segname, "__TEXT")) text = s;
            if (!strcmp(s->segname, "__DATA")) data = s;
            if (!strcmp(s->segname, "__LINKEDIT")) linkedit = s;
            break;
        }
        case LC_BUILD_VERSION: bv = (const void *)c; break;
        case LC_ID_DYLIB: have_id = 1; break;
        case LC_DYLD_CHAINED_FIXUPS: have_chained = 1; break;
        case LC_CODE_SIGNATURE: have_codesig = 1; codesig = (const void *)c; break;
        case LC_LOAD_DYLIB: {
            const struct dylib_command *d = (const void *)c;
            const char *name = (const char *)c + d->dylib.name.offset;
            if (strstr(name, "libSystem")) have_libsystem = 1;
            printf("info  LC_LOAD_DYLIB %s\n", name);
            break;
        }
        }
        lc += c->cmdsize;
    }
    printf("info  LC_DYLD_CHAINED_FIXUPS: %s, libSystem dependency: %s, LC_CODE_SIGNATURE: %s\n",
           have_chained ? "yes" : "no", have_libsystem ? "yes" : "no", have_codesig ? "yes" : "no");

    check(cmds_total == mh->sizeofcmds, "sum of cmdsize over ncmds=%u == sizeofcmds (%llu vs %u)",
          mh->ncmds, (unsigned long long)cmds_total, mh->sizeofcmds);
    check(have_codesig, "LC_CODE_SIGNATURE present (dylib was signed)");
    if (codesig) check_signature(mo, mo_size, codesig->dataoff, codesig->datasize);
    check(text != NULL, "__TEXT segment present");
    check(data != NULL, "__DATA segment present");
    check(linkedit != NULL, "__LINKEDIT segment present");
    check(have_id, "LC_ID_DYLIB present");
    check(bv != NULL, "LC_BUILD_VERSION present");
    if (bv)
        check(bv->platform == want_platform, "LC_BUILD_VERSION platform = %u (expected %u)",
              bv->platform, want_platform);
    if (!text || !data) goto done;

    check(text->vmaddr == 0 && text->fileoff == 0, "__TEXT at vmaddr 0 / fileoff 0");
    check(text->initprot == (VM_PROT_READ | VM_PROT_EXECUTE), "__TEXT initprot r-x (got %s)",
          prot_str(text->initprot));
    check(data->initprot == (VM_PROT_READ | VM_PROT_WRITE), "__DATA initprot rw- (got %s)",
          prot_str(data->initprot));
    check(data->vmaddr == want_data_vmaddr,
          "__DATA vmaddr 0x%llx == rounddown(min writable vaddr 0x%llx) + H = 0x%llx",
          data->vmaddr, min_w, want_data_vmaddr);
    check(text->vmaddr + text->vmsize <= data->vmaddr, "__TEXT ends (0x%llx) at or before __DATA",
          text->vmaddr + text->vmsize);
    check(data->vmaddr + data->vmsize >= max_w_end + H,
          "__DATA covers ELF RW end + H (0x%llx >= 0x%llx)", data->vmaddr + data->vmsize,
          max_w_end + H);
    if (linkedit) {
        check(linkedit->vmaddr >= data->vmaddr + data->vmsize, "__LINKEDIT after __DATA");
        check(linkedit->fileoff + linkedit->filesize == mo_size, "__LINKEDIT ends at end of file");
        if (codesig)
            check(codesig->dataoff >= linkedit->fileoff &&
                      codesig->dataoff + codesig->datasize <= linkedit->fileoff + linkedit->filesize,
                  "code signature inside __LINKEDIT");
    }
    check(H + 4 <= mo_size && memcmp(mo + H, elf, 4) == 0, "ELF header bytes at file offset H");

    /* Every PT_LOAD's file bytes must sit at vmaddr p_vaddr + H; bss must be zero. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        const struct segment_command_64 *s = (ph[i].p_flags & PF_W) ? data : text;
        uint64_t va = ph[i].p_vaddr + H;
        int ok = va >= s->vmaddr && va + ph[i].p_memsz <= s->vmaddr + s->vmsize;
        uint64_t off = s->fileoff + (va - s->vmaddr);
        if (ok) ok = off + ph[i].p_filesz <= mo_size &&
                     memcmp(mo + off, elf + ph[i].p_offset, ph[i].p_filesz) == 0;
        /* bss part: zero in the file if it lies within filesize, else implicitly zero */
        for (uint64_t k = ph[i].p_filesz; ok && k < ph[i].p_memsz; k++) {
            uint64_t o = off + k;
            if (o < s->fileoff + s->filesize && mo[o] != 0) ok = 0;
        }
        check(ok, "PT_LOAD[%d] %c%c%c vaddr 0x%llx (filesz 0x%llx memsz 0x%llx) -> %s vmaddr 0x%llx",
              i, (ph[i].p_flags & PF_R) ? 'R' : '-', (ph[i].p_flags & PF_W) ? 'W' : '-',
              (ph[i].p_flags & PF_X) ? 'X' : '-', (unsigned long long)ph[i].p_vaddr,
              (unsigned long long)ph[i].p_filesz, (unsigned long long)ph[i].p_memsz, s->segname,
              (unsigned long long)va);
    }

done:
    printf("%s: test_layout (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
