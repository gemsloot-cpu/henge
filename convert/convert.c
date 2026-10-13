/* henge-convert library entry point: repack an Android arm64 ELF shared object as a
 * Mach-O dylib. Layout and rules: docs/m1-elf-to-macho.md §3–4.
 *
 *   elf_input.c     parse + validate the ELF (§3)
 *   scan.c          instruction scan (svc / tpidr_el0)
 *   macho_writer.c  emit the Mach-O (§4)
 *   manifest.c      emit the manifest JSON
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

int hc_fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int henge_convert(const uint8_t *elf, size_t elf_size, const struct henge_convert_opts *opts,
                  struct henge_convert_result *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);

    if (!opts || !opts->install_name || !*opts->install_name)
        return hc_fail(err, errlen, "install name is required");
    if (opts->platform != HENGE_PLATFORM_IOS && opts->platform != HENGE_PLATFORM_MACOS &&
        opts->platform != HENGE_PLATFORM_IOSSIM)
        return hc_fail(err, errlen, "unknown platform %d", (int)opts->platform);

    struct elf_info e;
    if (hc_elf_parse(&e, elf, elf_size, err, errlen) != 0) return -1;

    struct scan_result scan;
    hc_scan_code(&e, &scan);
    if (scan.svc_count > 0)
        return hc_fail(err, errlen, "found %u svc #0 instruction(s): svc patching not implemented yet (M2)",
                       scan.svc_count);

    uint8_t *mo;
    size_t mo_size;
    if (hc_macho_write(&e, opts, &mo, &mo_size, err, errlen) != 0) return -1;

    char *manifest = hc_manifest_build(&e, opts, &scan);
    if (!manifest) {
        free(mo);
        return hc_fail(err, errlen, "out of memory");
    }

    out->macho = mo;
    out->macho_size = mo_size;
    out->manifest_json = manifest;
    out->svc_count = scan.svc_count;
    out->tpidr_count = scan.tpidr_count;
    return 0;
}
