/* Private interfaces shared by the converter's translation units (not installed).
 * The library is everything in convert/ except main.c (the CLI). */
#ifndef HENGE_CONVERT_INTERNAL_H
#define HENGE_CONVERT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "henge/elf64.h"
#include "henge/henge.h"

#define HC_SHIFT ((uint64_t)HENGE_SHIFT)
#define HC_PAGE ((uint64_t)HENGE_PAGE)
#define HC_MAX_LOADS 16

static inline uint64_t hc_align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static inline uint64_t hc_align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* Formats a message into err (if non-NULL) and returns -1. */
int hc_fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- elf_input.c: parsed and validated input ---- */

struct elf_info {
    const uint8_t *data;
    size_t size;
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *ph;
    Elf64_Phdr loads[HC_MAX_LOADS]; /* PT_LOADs, sorted by vaddr */
    unsigned nloads;
    unsigned first_w;        /* index of the first writable PT_LOAD */
    uint64_t text_end;       /* end of the non-writable memory image */
    uint64_t data_start;     /* page-aligned start of the writable image */
    uint64_t w_file_end;     /* end of writable file-backed bytes (vaddr) */
    uint64_t w_mem_end;      /* end of writable memory (vaddr) */
    const Elf64_Phdr *dyn_ph;
    const Elf64_Dyn *dyn;
    size_t ndyn;
    const char *dynstr;      /* DT_STRTAB, or NULL */
    uint64_t strsz;
};

/* Validates the ELF against the M1 input rules (docs/m1-elf-to-macho.md §3) and fills e. */
int hc_elf_parse(struct elf_info *e, const uint8_t *elf, size_t elf_size, char *err, size_t errlen);

/* LC_UUID value: the GNU build-id if present, else a stable hash of the input bytes. */
void hc_elf_uuid(const struct elf_info *e, uint8_t uuid[16]);

/* ---- scan.c: instruction scan over executable segments ---- */

struct scan_result {
    unsigned svc_count;   /* svc #0 */
    unsigned tpidr_count; /* mrs xN, tpidr_el0 */
};

void hc_scan_code(const struct elf_info *e, struct scan_result *out);

/* ---- macho_writer.c ---- */

/* Builds the Mach-O dylib (docs/m1-elf-to-macho.md §4). *out is malloc'd on success. */
int hc_macho_write(const struct elf_info *e, const struct henge_convert_opts *opts, uint8_t **out,
                   size_t *out_size, char *err, size_t errlen);

/* ---- manifest.c ---- */

/* Returns the malloc'd manifest JSON, or NULL when out of memory. */
char *hc_manifest_build(const struct elf_info *e, const struct henge_convert_opts *opts,
                        const struct scan_result *scan);

#endif
