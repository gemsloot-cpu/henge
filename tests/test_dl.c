/* Linker test: henge_load of libdep.so pulls in libbase.so from the library path (DT_NEEDED),
 * both use APS2 packed relocations, and libdep calls dlopen / dlsym / dladdr /
 * dl_iterate_phdr from the Android side.
 *   test_dl <dir with libdep.dylib and libbase.dylib> */
#include <stdio.h>
#include <string.h>

#include "henge/henge.h"

static int failures;

#define CHECK_EQ(desc, got, want)                                             \
    do {                                                                      \
        long g_ = (long)(got), w_ = (long)(want);                             \
        if (g_ == w_) printf("PASS  %-34s = %ld\n", desc, g_);                \
        else { printf("FAIL  %-34s = %ld (expected %ld)\n", desc, g_, w_); failures++; } \
    } while (0)

typedef int (*fn_i)(void);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s dir\n", argv[0]);
        return 2;
    }
    char err[512];
    henge_image *missing = henge_load("libdep.so", err, sizeof err);
    CHECK_EQ("henge_load without library path", missing == NULL, 1);
    printf("info  error: %s\n", err);

    henge_set_library_path(argv[1]);
    henge_image *dep = henge_load("libdep.so", err, sizeof err);
    if (!dep) {
        printf("FAIL  henge_load(\"libdep.so\"): %s\n", err);
        return 1;
    }
    printf("PASS  henge_load(\"libdep.so\") (loads libbase.so via DT_NEEDED)\n");
    henge_image *base = henge_load("libbase.so", err, sizeof err);
    CHECK_EQ("libbase.so already linked", base != NULL && base != dep, 1);

    const char *names[] = {"dep_value", "dep_name_len", "dep_dlsym", "dep_dlsym_default", "dep_dlsym_missing",
                           "dep_dlopen_missing", "dep_dladdr", "dep_phdr"};
    void *f[8];
    for (int i = 0; i < 8; i++) {
        f[i] = henge_dlsym(dep, names[i]);
        if (!f[i]) {
            printf("FAIL  henge_dlsym(\"%s\") returned NULL\n", names[i]);
            return 1;
        }
    }
    CHECK_EQ("dep_value() [cross-image call + data]", ((fn_i)f[0])(), 1234 + 7);
    int (*name_len)(int) = (int (*)(int))f[1];
    CHECK_EQ("dep_name_len(13) [APS2 RELATIVE]", name_len(13), (long)strlen("thirteen"));
    CHECK_EQ("dep_name_len(0)", name_len(0), 4);
    CHECK_EQ("dlsym(dlopen(\"libbase.so\"))", ((fn_i)f[2])(), 1234);
    CHECK_EQ("dlsym(RTLD_DEFAULT, \"dep_value\")", ((fn_i)f[3])(), 1241);
    CHECK_EQ("dlsym missing -> NULL + dlerror", ((fn_i)f[4])(), 1);
    CHECK_EQ("dlopen(\"libnope.so\") -> NULL", ((fn_i)f[5])(), 1);
    CHECK_EQ("dladdr(dep_value + 4)", ((fn_i)f[6])(), 1);
    /* libbase, libdep (plus libpatch / libadd if the process linked them: not here). */
    CHECK_EQ("dl_iterate_phdr: 2 images, 2 EH frames", ((fn_i)f[7])(), 202);

    printf("%s: test_dl (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
