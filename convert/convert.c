/* henge-convert library entry point: repack an Android arm64 ELF shared object as a
 * Mach-O dylib. Layout and rules: docs/m1-elf-to-macho.md §3–4.
 *
 *   elf_input.c     parse + validate the ELF (§3)
 *   scan.c          instruction scan (svc / tpidr_el0 / x18), docs/m2-patching.md
 *   dynsym.c        imports from the dynamic symbol table
 *   macho_writer.c  emit the Mach-O (§4) and the patch island
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
    if (hc_scan_code(&e, &scan, err, errlen) != 0) return -1;
    if (hc_elf_symbols(&e, 0, &out->imports, &out->nimports, err, errlen) != 0 ||
        hc_elf_symbols(&e, 1, &out->exports, &out->nexports, err, errlen) != 0) {
        hc_scan_free(&scan);
        henge_convert_result_free(out);
        return -1;
    }
    out->svc_count = scan.svc_count;
    out->tpidr_count = scan.tpidr_count;
    out->x18_count = scan.x18_count;
    out->shadow_call_stack = scan.scs;
    out->has_tls = e.tls_ph != NULL;

    int rc = hc_macho_write(&e, opts, &scan, &out->macho, &out->macho_size, err, errlen);
    if (rc == 0) {
        out->manifest_json = hc_manifest_build(&e, opts, &scan, out->imports, out->nimports);
        if (!out->manifest_json) rc = hc_fail(err, errlen, "out of memory");
    }
    hc_scan_free(&scan);
    if (rc != 0) henge_convert_result_free(out);
    return rc;
}

void henge_convert_result_free(struct henge_convert_result *res) {
    free(res->macho);
    free(res->manifest_json);
    hc_symbols_free(res->imports, res->nimports);
    hc_symbols_free(res->exports, res->nexports);
    memset(res, 0, sizeof *res);
}
