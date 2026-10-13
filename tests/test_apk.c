/* APK reader test: opens the sample APK built by `make samples-apk`, checks the entry list,
 * extraction (deflate and stored), manifest parsing, and rejection of damaged input.
 *   test_apk sample.apk libadd.so hello.txt */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "henge/apk.h"
#include "internal.h" /* apk/: to check which compression method each entry uses */

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
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: read failed\n", path);
        exit(2);
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* Extracts name from apk and compares it with want. */
static void check_entry(const henge_apk *apk, const char *name, const unsigned char *want, size_t want_size,
                        uint16_t method) {
    long i = henge_apk_find(apk, name);
    check(i >= 0, "find %s", name);
    if (i < 0) return;
    check(apk->entries[i].method == method, "%s uses method %u (got %u)", name, method, apk->entries[i].method);
    char err[512] = "";
    uint8_t *data;
    size_t size;
    int rc = henge_apk_extract(apk, (size_t)i, &data, &size, err, sizeof err);
    check(rc == 0, "extract %s%s%s", name, rc ? ": " : "", err);
    if (rc) return;
    check(size == want_size && memcmp(data, want, size) == 0, "%s matches the source (%zu bytes)", name, size);
    free(data);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s sample.apk libadd.so hello.txt\n", argv[0]);
        return 2;
    }
    size_t so_size, txt_size, apk_size;
    unsigned char *so = slurp(argv[2], &so_size), *txt = slurp(argv[3], &txt_size);
    unsigned char *raw = slurp(argv[1], &apk_size);
    char err[512] = "";

    henge_apk *apk = henge_apk_open(argv[1], err, sizeof err);
    check(apk != NULL, "henge_apk_open%s%s", apk ? "" : ": ", err);
    if (!apk) return 1;

    check(henge_apk_count(apk) >= 3, "%zu entries", henge_apk_count(apk));
    check(henge_apk_find(apk, "AndroidManifest.xml") >= 0, "find AndroidManifest.xml");
    check(henge_apk_name(apk, henge_apk_count(apk)) == NULL, "henge_apk_name out of range is NULL");
    check(henge_apk_find(apk, "lib/arm64-v8a/missing.so") == -1, "find missing entry is -1");

    check_entry(apk, "lib/arm64-v8a/libadd.so", so, so_size, 8);
    check_entry(apk, "assets/hello.txt", txt, txt_size, 0);

    size_t *idx, n;
    check(henge_apk_native_libs(apk, "arm64-v8a", &idx, &n) == 0 && n == 1 &&
              strcmp(henge_apk_name(apk, idx[0]), "lib/arm64-v8a/libadd.so") == 0,
          "native libs for arm64-v8a = [libadd.so]");
    free(idx);
    check(henge_apk_native_libs(apk, "x86_64", &idx, &n) == 0 && n == 0 && idx == NULL, "no x86_64 libs");

    struct henge_apk_manifest m;
    int rc = henge_apk_read_manifest(apk, &m, err, sizeof err);
    check(rc == 0, "henge_apk_read_manifest%s%s", rc ? ": " : "", err);
    if (rc == 0) {
        check(strcmp(m.package, "dev.henge.sample") == 0, "package = %s", m.package);
        check(m.version_code == 7, "versionCode = %lld", (long long)m.version_code);
        check(strcmp(m.version_name, "1.2.3") == 0, "versionName = %s", m.version_name);
        check(m.min_sdk == 28 && m.target_sdk == 35, "sdk min %d target %d", m.min_sdk, m.target_sdk);
        check(m.has_code == 0, "hasCode = %d", m.has_code);
        check(m.native_activity == 1, "NativeActivity declared");
        check(strcmp(m.lib_name, "add") == 0, "lib_name = %s", m.lib_name);
        check(strcmp(m.label, "henge sample") == 0, "label = %s", m.label);
    }
    henge_apk_close(apk);

    /* ---- damaged input ---- */
    henge_apk *bad = henge_apk_open(argv[2], err, sizeof err);
    check(bad == NULL, "an ELF file is rejected (%s)", err);
    henge_apk_close(bad);

    bad = henge_apk_open_mem(raw, apk_size / 2, err, sizeof err);
    check(bad == NULL, "a truncated APK is rejected (%s)", err);
    henge_apk_close(bad);

    /* Flip a byte in the middle of the compressed libadd.so: inflate or the CRC must catch it. */
    err[0] = '\0';
    henge_apk *mem = henge_apk_open_mem(raw, apk_size, err, sizeof err);
    check(mem != NULL, "henge_apk_open_mem%s%s", mem ? "" : ": ", err);
    if (mem) {
        long i = henge_apk_find(mem, "lib/arm64-v8a/libadd.so");
        const struct apk_entry *e = &mem->entries[i];
        size_t data_off = (size_t)e->lho + 30 + (raw[e->lho + 26] | raw[e->lho + 27] << 8) +
                          (raw[e->lho + 28] | raw[e->lho + 29] << 8);
        raw[data_off + (size_t)e->csize / 2] ^= 0x5a;
        uint8_t *data = NULL;
        size_t size;
        err[0] = '\0';
        rc = henge_apk_extract(mem, (size_t)i, &data, &size, err, sizeof err);
        check(rc != 0 && data == NULL, "corrupt deflate data is rejected (%s)", err);
        henge_apk_close(mem);
    }

    printf("%s: test_apk (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    free(so);
    free(txt);
    free(raw);
    return failures ? 1 : 0;
}
