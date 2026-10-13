/* M2 patch test: runs libpatch.so (stack protector, raw svc, TLSDESC, IFUNC) converted with
 * the patch island, on macOS. See docs/m2-patching.md. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "henge/henge.h"

static int failures;

#define CHECK(desc, cond)                                                     \
    do {                                                                      \
        int ok_ = (cond);                                                     \
        printf("%s  %s\n", ok_ ? "PASS" : "FAIL", desc);                      \
        if (!ok_) failures++;                                                 \
    } while (0)

#define CHECK_EQ(desc, got, want)                                             \
    do {                                                                      \
        long g_ = (long)(got), w_ = (long)(want);                             \
        if (g_ == w_) printf("PASS  %-34s = %ld\n", desc, g_);                \
        else { printf("FAIL  %-34s = %ld (expected %ld)\n", desc, g_, w_); failures++; } \
    } while (0)

static henge_image *img;

static void *sym(const char *name) {
    void *p = henge_dlsym(img, name);
    if (!p) { printf("FAIL  henge_dlsym(\"%s\") returned NULL\n", name); failures++; }
    return p;
}

static int host_checksum(const char *s) {
    int sum = 0;
    for (size_t i = 0; i < strlen(s) && i < 63; i++) sum = sum * 31 + s[i];
    return sum;
}

/* ---- per-thread TLS and thread pointer ---- */

struct thread_result {
    int counter_ok, buf_ok;
    void *tp;
    unsigned long guard;
    int *counter_addr;
};

static int (*tls_inc)(void);
static int *(*tls_counter_addr)(void);
static int (*tls_buf_sum)(void);
static void *(*read_tp)(void);
static unsigned long (*stack_guard)(void);

static void *thread_main(void *arg) {
    struct thread_result *r = arg;
    r->counter_ok = 1;
    for (int i = 0; i < 1000; i++)
        if (tls_inc() != 6 + i) r->counter_ok = 0;
    r->buf_ok = tls_buf_sum() == 0 && tls_buf_sum() == 1;
    r->tp = read_tp();
    r->guard = stack_guard();
    r->counter_addr = tls_counter_addr();
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/libpatch.dylib\n", argv[0]);
        return 2;
    }
    if (!dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) {
        printf("FAIL  dlopen: %s\n", dlerror());
        return 1;
    }
    char err[512] = {0};
    img = henge_link(argv[1], err, sizeof err);
    if (!img) {
        printf("FAIL  henge_link: %s\n", err);
        return 1;
    }
    printf("PASS  dlopen + henge_link\n");

    int (*checksum)(const char *) = (int (*)(const char *))sym("checksum");
    long (*svc_getpid)(void) = (long (*)(void))sym("svc_getpid");
    long (*svc_write)(int, const void *, long) = (long (*)(int, const void *, long))sym("svc_write");
    long (*svc_close_bad)(void) = (long (*)(void))sym("svc_close_bad");
    long (*svc_unknown)(void) = (long (*)(void))sym("svc_unknown");
    int (*regcheck)(void) = (int (*)(void))sym("regcheck");
    int (*pick)(void) = (int (*)(void))sym("pick");
    int (*call_pick)(void) = (int (*)(void))sym("call_pick");
    tls_inc = (int (*)(void))sym("tls_inc");
    tls_counter_addr = (int *(*)(void))sym("tls_counter_addr");
    tls_buf_sum = (int (*)(void))sym("tls_buf_sum");
    read_tp = (void *(*)(void))sym("read_tp");
    stack_guard = (unsigned long (*)(void))sym("stack_guard");
    if (failures) goto done;

    /* Stack protector: prologue/epilogue read tp + 0x28 through the patched mrs. */
    CHECK_EQ("checksum(\"henge\") [stack protector]", checksum("henge"), host_checksum("henge"));
    void *tp = read_tp();
    CHECK("thread pointer is non-NULL", tp != NULL);
    CHECK("thread pointer is stable", read_tp() == tp);
    CHECK("stack guard slot (tp + 0x28) is set", stack_guard() != 0);

    /* Raw syscalls. */
    CHECK_EQ("svc getpid()", svc_getpid(), getpid());
    CHECK_EQ("svc write(1, \"ok\\n\", 3)", svc_write(1, "ok\n", 3), 3);
    CHECK_EQ("svc close(-1) = -EBADF", svc_close_bad(), -9);
    CHECK_EQ("svc #4095 = -ENOSYS", svc_unknown(), -38);

    /* Registers and flags across patched mrs / svc. */
    CHECK_EQ("regcheck() [registers preserved]", regcheck(), 0);

    /* IFUNC: exported symbol and IRELATIVE PLT slot both pick the LSE variant on Apple CPUs. */
    CHECK_EQ("pick() [STT_GNU_IFUNC]", pick(), 2);
    CHECK_EQ("call_pick() [IRELATIVE]", call_pick(), 2);

    /* TLS on the main thread. */
    CHECK_EQ("tls_inc() #1", tls_inc(), 6);
    CHECK_EQ("tls_inc() #2", tls_inc(), 7);
    CHECK_EQ("*tls_counter_addr()", *tls_counter_addr(), 7);
    CHECK_EQ("tls_buf_sum() (bss zeroed)", tls_buf_sum(), 0);

    /* TLS and thread pointers per thread. */
    enum { N = 4 };
    pthread_t th[N];
    struct thread_result res[N];
    memset(res, 0, sizeof res);
    for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, thread_main, &res[i]);
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    int counters = 1, bufs = 1, distinct = 1, guards = 1;
    for (int i = 0; i < N; i++) {
        counters &= res[i].counter_ok;
        bufs &= res[i].buf_ok;
        guards &= res[i].guard == stack_guard();
        distinct &= res[i].tp != tp && res[i].counter_addr != tls_counter_addr();
        for (int k = 0; k < i; k++)
            distinct &= res[i].tp != res[k].tp && res[i].counter_addr != res[k].counter_addr;
    }
    CHECK("each thread starts its own tls_counter at 5", counters);
    CHECK("each thread has its own zeroed tls_buf", bufs);
    CHECK("each thread has a distinct tp and TLS block", distinct);
    CHECK("all threads share the stack guard value", guards);
    CHECK_EQ("main thread tls_counter unaffected", *tls_counter_addr(), 7);

done:
    printf("%s: test_patch (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
