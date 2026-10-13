/* bionic compatibility shim. See docs/architecture.md §6 and research/abi-differences.md.
 *
 * Each shim_*.c / dl.c file exports a table of {bionic name, implementation}; shim.c merges
 * them into one sorted index. A name must appear in exactly one table. */
#ifndef HENGE_SHIM_H
#define HENGE_SHIM_H

#include <stddef.h>

struct hr_shim_entry {
    const char *name;
    void *addr;
};

#define HR_SHIM(n, f) {n, (void *)(f)}
#define HR_SHIM_TABLE(id) \
    extern const struct hr_shim_entry hr_shim_##id[]; \
    extern const size_t hr_shim_##id##_count

HR_SHIM_TABLE(core);
HR_SHIM_TABLE(dl);

/* Returns the shim implementation of the bionic symbol `name`, or NULL. */
void *henge_shim_lookup(const char *name);

#endif
