/* henge-convert CLI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "henge/henge.h"

static void usage(void) {
    fprintf(stderr,
            "usage: henge-convert [--platform ios|iossim|macos] [--install-name NAME] [--libsystem]\n"
            "                     [-m manifest.json] -o out.dylib in.so\n"
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

/* --check: convert in memory and report what blocks or limits running the library. */
static int check(const char *path) {
    size_t size;
    uint8_t *elf = read_file(path, &size);
    if (!elf) { perror(path); return 1; }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    struct henge_convert_opts opts = {.platform = HENGE_PLATFORM_IOS, .install_name = base};
    struct henge_convert_result r;
    char err[512];
    int rc = henge_convert(elf, size, &opts, &r, err, sizeof err);
    free(elf);
    if (rc != 0) {
        printf("%s: NOT CONVERTIBLE: %s\n", path, err);
        return 1;
    }
    size_t missing = 0;
    for (size_t i = 0; i < r.nimports; i++)
        if (!henge_shim_provides(r.imports[i].name)) missing++;
    printf("%s: convertible; svc %u, tpidr_el0 %u, x18 %u%s, TLS %s; imports %zu, not in shim %zu\n", path,
           r.svc_count, r.tpidr_count, r.x18_count, r.shadow_call_stack ? " (SCS)" : "", r.has_tls ? "yes" : "no",
           r.nimports, missing);
    for (size_t i = 0; i < r.nimports; i++)
        if (!henge_shim_provides(r.imports[i].name))
            printf("  missing %s%s\n", r.imports[i].name, r.imports[i].weak ? " (weak)" : "");
    print_warnings(path, &r);
    henge_convert_result_free(&r);
    return 0;
}

int main(int argc, char **argv) {
    struct henge_convert_opts opts = {.platform = HENGE_PLATFORM_IOS};
    const char *out_path = NULL, *manifest_path = NULL, *in_path = NULL, *install_name = NULL;

    if (argc > 1 && !strcmp(argv[1], "--check")) {
        if (argc < 3) usage();
        int rc = 0;
        for (int i = 2; i < argc; i++) rc |= check(argv[i]);
        return rc;
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
