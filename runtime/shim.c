/* bionic compatibility shim: core table (C runtime support, memory) and the merged index.
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
#include "henge/henge.h"

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

#define E HR_SHIM
const struct hr_shim_entry hr_shim_core[] = {
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
const size_t hr_shim_core_count = sizeof hr_shim_core / sizeof hr_shim_core[0];

/* ---- merged, sorted index ---- */

static const struct {
    const struct hr_shim_entry *entries;
    const size_t *count;
} k_tables[] = {
    {hr_shim_core, &hr_shim_core_count},
    {hr_shim_dl, &hr_shim_dl_count},
};

static pthread_once_t g_index_once = PTHREAD_ONCE_INIT;
static const struct hr_shim_entry **g_index;
static size_t g_nindex;

static int cmp_entry(const void *a, const void *b)
{
    return strcmp((*(const struct hr_shim_entry *const *)a)->name,
                  (*(const struct hr_shim_entry *const *)b)->name);
}

static void build_index(void)
{
    size_t n = 0;
    for (size_t t = 0; t < sizeof k_tables / sizeof k_tables[0]; t++)
        n += *k_tables[t].count;
    g_index = malloc(n * sizeof *g_index);
    if (!g_index)
        abort();
    for (size_t t = 0; t < sizeof k_tables / sizeof k_tables[0]; t++)
        for (size_t i = 0; i < *k_tables[t].count; i++)
            g_index[g_nindex++] = &k_tables[t].entries[i];
    qsort(g_index, g_nindex, sizeof *g_index, cmp_entry);
}

void *henge_shim_lookup(const char *name)
{
    pthread_once(&g_index_once, build_index);
    size_t lo = 0, hi = g_nindex;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, g_index[mid]->name);
        if (c == 0)
            return g_index[mid]->addr;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

int henge_shim_provides(const char *name)
{
    return henge_shim_lookup(name) != NULL;
}
