/* M1 end-to-end test: dlopen a converted, signed libadd.dylib, link it with the
 * henge mini linker, and call into the original Android code. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "henge/henge.h"

static int failures;

#define CHECK_EQ(desc, got, want)                                                     \
    do {                                                                              \
        long g_ = (long)(got), w_ = (long)(want);                                     \
        if (g_ == w_) {                                                               \
            printf("PASS  %-28s = %ld\n", desc, g_);                                  \
        } else {                                                                      \
            printf("FAIL  %-28s = %ld (expected %ld)\n", desc, g_, w_);               \
            failures++;                                                               \
        }                                                                             \
    } while (0)

static void *must_sym(henge_image *img, const char *name) {
    void *p = henge_dlsym(img, name);
    if (!p) {
        printf("FAIL  henge_dlsym(\"%s\") returned NULL\n", name);
        failures++;
    } else {
        printf("PASS  henge_dlsym(\"%s\") = %p\n", name, p);
    }
    return p;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/libadd.dylib\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("FAIL  dlopen(%s): %s\n", path, dlerror());
        return 1;
    }
    printf("PASS  dlopen(%s)\n", path);

    char err[512] = {0};
    henge_image *img = henge_link(path, err, sizeof err);
    if (!img) {
        printf("FAIL  henge_link: %s\n", err[0] ? err : "(no message)");
        return 1;
    }
    printf("PASS  henge_link\n");

    int (*add)(int, int) = (int (*)(int, int))must_sym(img, "add");
    int (*bump)(void) = (int (*)(void))must_sym(img, "bump");
    int (*apply)(int, int, int) = (int (*)(int, int, int))must_sym(img, "apply");
    int (*was_initialized)(void) = (int (*)(void))must_sym(img, "was_initialized");
    int *counter = (int *)must_sym(img, "counter");
    if (failures) goto done;

    CHECK_EQ("add(2, 3)", add(2, 3), 5);
    CHECK_EQ("was_initialized()", was_initialized(), 1);
    CHECK_EQ("*counter (initial)", *counter, 40);
    CHECK_EQ("bump() #1", bump(), 41);
    CHECK_EQ("bump() #2", bump(), 42);
    CHECK_EQ("*counter (after bumps)", *counter, 42);
    CHECK_EQ("apply(0, 7, 5) [add]", apply(0, 7, 5), 12);
    CHECK_EQ("apply(1, 7, 5) [sub]", apply(1, 7, 5), 2);

done:
    printf("%s: test_m1 (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
