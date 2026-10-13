/* bionic compatibility shim (M1). See docs/architecture.md §6. */
#ifndef HENGE_SHIM_H
#define HENGE_SHIM_H

/* Returns the shim implementation of the bionic symbol `name`, or NULL. */
void *henge_shim_lookup(const char *name);

#endif
