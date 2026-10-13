/* henge-convert CLI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "henge/henge.h"

static void usage(void) {
    fprintf(stderr,
            "usage: henge-convert [--platform ios|macos] [--install-name NAME] [--libsystem]\n"
            "                     [-m manifest.json] -o out.dylib in.so\n");
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

int main(int argc, char **argv) {
    struct henge_convert_opts opts = {.platform = HENGE_PLATFORM_IOS};
    const char *out_path = NULL, *manifest_path = NULL, *in_path = NULL, *install_name = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--platform") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "ios")) opts.platform = HENGE_PLATFORM_IOS;
            else if (!strcmp(p, "macos")) opts.platform = HENGE_PLATFORM_MACOS;
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
    if (!rc) printf("%s -> %s (%zu bytes): svc %u, tpidr_el0 %u\n", in_path, out_path, res.macho_size,
                    res.svc_count, res.tpidr_count);
    free(res.macho);
    free(res.manifest_json);
    return rc;
}
