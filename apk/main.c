/* henge-apk CLI: inspect an APK and extract its arm64-v8a native libraries. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "henge/apk.h"

#define ABI "arm64-v8a"

static void usage(void) {
    fprintf(stderr,
            "usage: henge-apk info app.apk\n"
            "       henge-apk extract app.apk outdir   (writes lib/" ABI "/*.so)\n");
    exit(2);
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, size, f);
    int rc = (n == size && fclose(f) == 0) ? 0 : -1;
    if (n != size) fclose(f);
    return rc;
}

/* Prints each distinct <abi> that has lib/<abi>/... entries. */
static void print_abis(const henge_apk *apk) {
    printf("abis:");
    int any = 0;
    for (size_t i = 0; i < henge_apk_count(apk); i++) {
        const char *n = henge_apk_name(apk, i);
        if (strncmp(n, "lib/", 4) != 0) continue;
        const char *slash = strchr(n + 4, '/');
        if (!slash || slash == n + 4) continue;
        size_t len = (size_t)(slash - (n + 4));
        int dup = 0;
        for (size_t j = 0; j < i && !dup; j++) {
            const char *m = henge_apk_name(apk, j);
            dup = strncmp(m, "lib/", 4) == 0 && strncmp(m + 4, n + 4, len) == 0 && m[4 + len] == '/';
        }
        if (dup) continue;
        printf(" %.*s", (int)len, n + 4);
        any = 1;
    }
    printf("%s\n", any ? "" : " (none)");
}

static int cmd_info(const henge_apk *apk) {
    char err[512];
    struct henge_apk_manifest m;
    if (henge_apk_read_manifest(apk, &m, err, sizeof err) != 0) {
        fprintf(stderr, "henge-apk: %s\n", err);
        return 1;
    }
    printf("package: %s\n", m.package);
    printf("version: %s (%lld)\n", m.version_name[0] ? m.version_name : "-", (long long)m.version_code);
    printf("label: %s\n", m.label[0] ? m.label : "-");
    printf("sdk: min %d, target %d\n", m.min_sdk, m.target_sdk);
    printf("has_code: %d\n", m.has_code);
    if (m.native_activity) printf("native_activity: lib%s.so\n", m.lib_name);
    else printf("native_activity: -\n");
    print_abis(apk);

    size_t *idx, n;
    if (henge_apk_native_libs(apk, ABI, &idx, &n) != 0) {
        fprintf(stderr, "henge-apk: out of memory\n");
        return 1;
    }
    printf(ABI " libs: %zu\n", n);
    for (size_t i = 0; i < n; i++) printf("  %s\n", henge_apk_name(apk, idx[i]));
    free(idx);
    return 0;
}

static int cmd_extract(const henge_apk *apk, const char *outdir) {
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        perror(outdir);
        return 1;
    }
    size_t *idx, n;
    if (henge_apk_native_libs(apk, ABI, &idx, &n) != 0) {
        fprintf(stderr, "henge-apk: out of memory\n");
        return 1;
    }
    if (n == 0) fprintf(stderr, "henge-apk: no lib/" ABI "/*.so entries\n");
    int rc = n == 0;
    for (size_t i = 0; i < n && !rc; i++) {
        const char *name = henge_apk_name(apk, idx[i]);
        const char *base = name + strlen("lib/" ABI "/"); /* no '/' (henge_apk_native_libs) */
        char path[4096], err[512];
        if (snprintf(path, sizeof path, "%s/%s", outdir, base) >= (int)sizeof path) {
            fprintf(stderr, "henge-apk: output path too long\n");
            rc = 1;
            break;
        }
        uint8_t *data;
        size_t size;
        if (henge_apk_extract(apk, idx[i], &data, &size, err, sizeof err) != 0) {
            fprintf(stderr, "henge-apk: %s\n", err);
            rc = 1;
            break;
        }
        if (write_file(path, data, size) != 0) {
            perror(path);
            rc = 1;
        } else {
            printf("%s -> %s (%zu bytes)\n", name, path, size);
        }
        free(data);
    }
    free(idx);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 3) usage();
    const char *cmd = argv[1];
    if (!((!strcmp(cmd, "info") && argc == 3) || (!strcmp(cmd, "extract") && argc == 4))) usage();

    char err[512];
    henge_apk *apk = henge_apk_open(argv[2], err, sizeof err);
    if (!apk) {
        fprintf(stderr, "henge-apk: %s\n", err);
        return 1;
    }
    int rc = !strcmp(cmd, "info") ? cmd_info(apk) : cmd_extract(apk, argv[3]);
    henge_apk_close(apk);
    return rc;
}
