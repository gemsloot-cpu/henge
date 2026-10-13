/* bionic compatibility shim, M1 subset.
 *
 * Only functions whose ABI is identical between Android (AAPCS64 + bionic) and
 * Darwin arm64 are exported here: fixed arity, pointer / size_t / int arguments,
 * no structs whose layout differs, no variadics.
 *
 * TODO(M2): variadic functions (printf, open, ioctl, fcntl, ...), va_list users,
 * long double, char/short arguments, struct-layout and constant conversions all
 * need real ABI shims. See research/abi-differences.md before adding anything
 * to this table.
 */
#define _DARWIN_C_SOURCE
#include "shim.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Darwin's own __cxa_atexit (libSystem). */
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso);

static void shim_cxa_finalize(void *dso)
{
    /* No-op in M1: images are never unloaded. */
    (void)dso;
}

static int shim_cxa_atexit(void (*func)(void *), void *arg, void *dso)
{
    /* dso is &__dso_handle inside the converted image, i.e. an address in the
     * mapped dylib, so Darwin's per-image finalization still matches it. */
    return __cxa_atexit(func, arg, dso);
}

static int shim_register_atfork(void (*prepare)(void), void (*parent)(void),
                                void (*child)(void), void *dso)
{
    /* bionic's pthread_atfork() is a wrapper passing &__dso_handle. */
    (void)dso;
    return pthread_atfork(prepare, parent, child);
}

static void shim_stack_chk_fail(void)
{
    abort();
}

struct shim_entry {
    const char *name;
    void *addr;
};

#define E(n, f) { n, (void *)(f) }
static const struct shim_entry shim_table[] = {
    E("__cxa_finalize", shim_cxa_finalize),
    E("__cxa_atexit", shim_cxa_atexit),
    E("__register_atfork", shim_register_atfork),
    E("__stack_chk_fail", shim_stack_chk_fail),
    E("abort", abort),
    E("malloc", malloc),
    E("free", free),
    E("calloc", calloc),
    E("realloc", realloc),
    E("memcpy", memcpy),
    E("memmove", memmove),
    E("memset", memset),
    E("memcmp", memcmp),
    E("strlen", strlen),
    E("strcmp", strcmp),
};
#undef E

void *henge_shim_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof shim_table / sizeof shim_table[0]; i++) {
        if (strcmp(shim_table[i].name, name) == 0)
            return shim_table[i].addr;
    }
    return NULL;
}
