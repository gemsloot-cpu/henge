/* M2 patch sample: code the converter must rewrite (docs/m2-patching.md).
 * Built with -fstack-protector-all for API 29+, so it has mrs tpidr_el0 reads (stack guard,
 * TLSDESC), raw svc #0, ELF TLS and an IFUNC. */
#include <stddef.h>
#include <string.h>

/* ---- stack protector: every function reads the guard at tpidr_el0 + 0x28 ---- */

int checksum(const char *s) {
    char buf[64];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    int sum = 0;
    for (size_t i = 0; i < n; i++) sum = sum * 31 + buf[i];
    return sum;
}

/* The raw thread pointer and its stack-guard slot as Android code sees them. */
void *read_tp(void) {
    void *tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

unsigned long stack_guard(void) { return ((unsigned long *)read_tp())[5]; }

/* ---- raw Linux syscalls ---- */

static long raw_syscall3(long nr, long a, long b, long c) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

long svc_getpid(void) { return raw_syscall3(172 /* getpid */, 0, 0, 0); }
long svc_write(int fd, const void *p, long n) { return raw_syscall3(64 /* write */, fd, (long)p, n); }
long svc_close_bad(void) { return raw_syscall3(57 /* close */, -1, 0, 0); } /* -EBADF */
long svc_unknown(void) { return raw_syscall3(4095, 0, 0, 0); }               /* -ENOSYS */

/* ---- ELF TLS (TLSDESC) ---- */

__thread int tls_counter = 5;
__thread char tls_buf[256];

int tls_inc(void) { return ++tls_counter; }
int *tls_counter_addr(void) { return &tls_counter; }
int tls_buf_sum(void) {
    int s = 0;
    for (size_t i = 0; i < sizeof tls_buf; i++) s += tls_buf[i];
    tls_buf[0]++;
    return s;
}

/* ---- IFUNC (R_AARCH64_IRELATIVE) ---- */

static int impl_generic(void) { return 1; }
static int impl_atomics(void) { return 2; }
static int (*resolve_pick(unsigned long hwcap))(void) {
    return (hwcap & (1ul << 8)) /* HWCAP_ATOMICS */ ? impl_atomics : impl_generic;
}
int pick(void) __attribute__((ifunc("resolve_pick")));
/* A local call goes through an IRELATIVE-relocated PLT slot. */
int call_pick(void) { return pick(); }
