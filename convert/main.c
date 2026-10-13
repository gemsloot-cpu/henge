/* henge-convert CLI. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "henge/henge.h"

static void usage(void) {
    fprintf(stderr,
            "usage: henge-convert [--platform ios|iossim|macos] [--install-name NAME] [--libsystem]\n"
            "                     [--sign] [-m manifest.json] -o out.dylib in.so\n"
            "       henge-convert --check in.so...   (compatibility report; writes nothing)\n");
    exit(2);
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 1 << 16;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return NULL; }
    *size = len;
    return buf;
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, size, f);
    int rc = (n == size && fclose(f) == 0) ? 0 : -1;
    if (n != size) fclose(f);
    return rc;
}

static void print_warnings(const char *path, const struct henge_convert_result *r) {
    if (r->x18_count)
        fprintf(stderr, "henge-convert: %s: warning: %u instruction(s) use x18, which Darwin may clobber%s\n",
                path, r->x18_count, r->shadow_call_stack ? " (Shadow Call Stack; unsupported)" : "");
}

/* --check: convert every input in memory and report what blocks or limits running it.
 * Imports count as satisfied if the shim or any other input exports them. */
static int has_export(const struct henge_convert_result *r, int n, const char *name) {
    for (int i = 0; i < n; i++)
        for (size_t k = 0; k < r[i].nexports; k++)
            if (!strcmp(r[i].exports[k].name, name)) return 1;
    return 0;
}

static int check(int n, char **paths) {
    struct henge_convert_result *r = calloc((size_t)n, sizeof *r);
    char (*errs)[512] = calloc((size_t)n, sizeof *errs);
    int *ok = calloc((size_t)n, sizeof *ok), rc = 0;
    if (!r || !errs || !ok) { fprintf(stderr, "henge-convert: out of memory\n"); return 1; }
    for (int i = 0; i < n; i++) {
        size_t size;
        uint8_t *elf = read_file(paths[i], &size);
        if (!elf) { snprintf(errs[i], sizeof errs[i], "%s", strerror(errno)); continue; }
        const char *base = strrchr(paths[i], '/');
        struct henge_convert_opts opts = {.platform = HENGE_PLATFORM_IOS, .install_name = base ? base + 1 : paths[i]};
        ok[i] = henge_convert(elf, size, &opts, &r[i], errs[i], sizeof errs[i]) == 0;
        free(elf);
    }
    for (int i = 0; i < n; i++) {
        if (!ok[i]) {
            printf("%s: NOT CONVERTIBLE: %s\n", paths[i], errs[i]);
            rc = 1;
            continue;
        }
        size_t missing = 0;
        for (size_t k = 0; k < r[i].nimports; k++)
            if (!henge_shim_provides(r[i].imports[k].name) && !has_export(r, n, r[i].imports[k].name)) missing++;
        printf("%s: convertible; svc %u, tpidr_el0 %u, x18 %u%s, TLS %s; imports %zu, unresolved %zu\n", paths[i],
               r[i].svc_count, r[i].tpidr_count, r[i].x18_count, r[i].shadow_call_stack ? " (SCS)" : "",
               r[i].has_tls ? "yes" : "no", r[i].nimports, missing);
        for (size_t k = 0; k < r[i].nimports; k++)
            if (!henge_shim_provides(r[i].imports[k].name) && !has_export(r, n, r[i].imports[k].name))
                printf("  unresolved %s%s\n", r[i].imports[k].name, r[i].imports[k].weak ? " (weak)" : "");
        print_warnings(paths[i], &r[i]);
    }
    for (int i = 0; i < n; i++) henge_convert_result_free(&r[i]);
    free(r);
    free(errs);
    free(ok);
    return rc;
}

int main(int argc, char **argv) {
    struct henge_convert_opts opts = {.platform = HENGE_PLATFORM_IOS};
    const char *out_path = NULL, *manifest_path = NULL, *in_path = NULL, *install_name = NULL;
    int sign = 0;

    if (argc > 1 && !strcmp(argv[1], "--check")) {
        if (argc < 3) usage();
        return check(argc - 2, argv + 2);
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--platform") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "ios")) opts.platform = HENGE_PLATFORM_IOS;
            else if (!strcmp(p, "macos")) opts.platform = HENGE_PLATFORM_MACOS;
            else if (!strcmp(p, "iossim")) opts.platform = HENGE_PLATFORM_IOSSIM;
            else { fprintf(stderr, "henge-convert: unknown platform '%s'\n", p); usage(); }
        } else if (!strcmp(a, "--install-name") && i + 1 < argc) {
            install_name = argv[++i];
        } else if (!strcmp(a, "--libsystem")) {
            opts.link_libsystem = 1;
        } else if (!strcmp(a, "--sign")) {
            sign = 1;
        } else if (!strcmp(a, "-m") && i + 1 < argc) {
            manifest_path = argv[++i];
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a[0] == '-' ) {
            usage();
        } else if (!in_path) {
            in_path = a;
        } else {
            usage();
        }
    }
    if (!in_path || !out_path) usage();

    char default_name[1024];
    if (!install_name) {
        const char *base = strrchr(in_path, '/');
        base = base ? base + 1 : in_path;
        size_t n = strlen(base);
        if (n > 3 && !strcmp(base + n - 3, ".so")) n -= 3;
        if (n > 900) { fprintf(stderr, "henge-convert: input name too long\n"); return 1; }
        snprintf(default_name, sizeof default_name, "@rpath/%.*s.dylib", (int)n, base);
        install_name = default_name;
    }
    opts.install_name = install_name;

    size_t size;
    uint8_t *elf = read_file(in_path, &size);
    if (!elf) { perror(in_path); return 1; }

    struct henge_convert_result res;
    char err[512];
    if (henge_convert(elf, size, &opts, &res, err, sizeof err) != 0) {
        fprintf(stderr, "henge-convert: %s: %s\n", in_path, err);
        free(elf);
        return 1;
    }
    free(elf);

    if (sign) {
        /* Identifier: install name basename without extension (e.g. "libadd"). */
        const char *b = strrchr(install_name, '/');
        b = b ? b + 1 : install_name;
        char ident[256];
        snprintf(ident, sizeof ident, "%.*s", (int)strcspn(b, "."), b);
        uint8_t *signed_mo;
        size_t signed_size;
        if (henge_sign_adhoc(res.macho, res.macho_size, ident, &signed_mo, &signed_size, err, sizeof err) != 0) {
            fprintf(stderr, "henge-convert: %s: signing failed: %s\n", in_path, err);
            henge_convert_result_free(&res);
            return 1;
        }
        free(res.macho);
        res.macho = signed_mo;
        res.macho_size = signed_size;
    }

    int rc = 0;
    if (write_file(out_path, res.macho, res.macho_size) != 0) { perror(out_path); rc = 1; }
    if (!rc && manifest_path) {
        /* henge_convert doesn't know the file name; splice "input" in after the opening brace. */
        const char *base = strrchr(in_path, '/');
        base = base ? base + 1 : in_path;
        FILE *f = fopen(manifest_path, "wb");
        if (f) {
            fputs("{\n  \"input\": \"", f);
            for (const char *p = base; *p; p++) {
                if (*p == '"' || *p == '\\') fputc('\\', f);
                if ((unsigned char)*p < 0x20) fprintf(f, "\\u%04x", (unsigned char)*p);
                else fputc(*p, f);
            }
            fputs("\",", f);
            fputs(res.manifest_json + 1, f); /* skip '{' */
            if (fclose(f) != 0) f = NULL;
        }
        if (!f) { perror(manifest_path); rc = 1; }
    }
    if (!rc) printf("%s -> %s (%zu bytes): svc %u, tpidr_el0 %u patched\n", in_path, out_path, res.macho_size,
                    res.svc_count, res.tpidr_count);
    print_warnings(in_path, &res);
    henge_convert_result_free(&res);
    return rc;
}
