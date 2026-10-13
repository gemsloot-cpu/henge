/* libdl for Android code: dlopen / dlsym / dladdr / dl_iterate_phdr over henge images.
 *
 * Handles are henge_image pointers, or g_system for shim-backed system libraries
 * ("libm.so", "liblog.so", ...). bionic LP64 uses RTLD_DEFAULT = 0 and RTLD_NEXT = -1. */
#include "internal.h"
#include "shim.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define B_RTLD_DEFAULT ((void *)0)
#define B_RTLD_NEXT ((void *)-1L)

static char g_system; /* handle for shim-backed libraries */
static _Thread_local char t_dlerror[512];
static _Thread_local int t_dlerror_set;

static void set_error(const char *msg)
{
    snprintf(t_dlerror, sizeof t_dlerror, "%s", msg);
    t_dlerror_set = 1;
}

static int is_image(const void *h)
{
    for (henge_image *im = hr_images; im; im = im->next)
        if (im == h)
            return 1;
    return 0;
}

static void *shim_dlopen(const char *name, int flags)
{
    (void)flags; /* RTLD_LAZY/NOW/LOCAL/GLOBAL: henge binds everything immediately */
    if (!name)
        return &g_system; /* dlopen(NULL): the "main program" */
    if (hr_is_system_lib(name))
        return &g_system;
    char err[512];
    hr_lock();
    henge_image *im = hr_load_locked(name, err, sizeof err);
    hr_unlock();
    if (!im)
        set_error(err);
    return im;
}

static void *android_dlopen_ext(const char *name, int flags, const void *extinfo)
{
    (void)extinfo;
    return shim_dlopen(name, flags);
}

/* Breadth-first over an image and its dependencies, as bionic's dlsym(handle) does. */
static void *lookup_from(henge_image *root, const char *name)
{
    henge_image *queue[256];
    size_t head = 0, tail = 0;
    queue[tail++] = root;
    while (head < tail) {
        henge_image *im = queue[head++];
        const Elf64_Sym *s = hr_lookup_sym(im, name);
        if (s)
            return (void *)hr_sym_addr(im, s);
        for (size_t i = 0; i < im->ndeps && tail < 256; i++) {
            int seen = 0;
            for (size_t k = 0; k < tail; k++)
                seen |= queue[k] == im->deps[i];
            if (!seen)
                queue[tail++] = im->deps[i];
        }
    }
    return henge_shim_lookup(name);
}

static void *shim_dlsym(void *handle, const char *name)
{
    void *p = NULL;
    hr_lock();
    if (handle == B_RTLD_DEFAULT || handle == B_RTLD_NEXT) {
        uintptr_t a;
        if (hr_resolve_global(name, &a))
            p = (void *)a;
    } else if (handle == &g_system) {
        p = henge_shim_lookup(name);
    } else if (is_image(handle)) {
        p = lookup_from(handle, name);
    } else {
        hr_unlock();
        set_error("dlsym: invalid handle");
        return NULL;
    }
    hr_unlock();
    if (!p) {
        char msg[512];
        snprintf(msg, sizeof msg, "undefined symbol: %s", name);
        set_error(msg);
    }
    return p;
}

static int shim_dlclose(void *handle)
{
    (void)handle; /* images stay loaded */
    return 0;
}

static char *shim_dlerror(void)
{
    if (!t_dlerror_set)
        return NULL;
    t_dlerror_set = 0;
    return t_dlerror;
}

/* Dl_info has the same layout on bionic and Darwin. */
static int shim_dladdr(const void *addr, Dl_info *info)
{
    hr_lock();
    henge_image *im = hr_image_for_addr((uintptr_t)addr);
    if (!im) {
        hr_unlock();
        return dladdr(addr, info);
    }
    info->dli_fname = im->path;
    info->dli_fbase = (void *)im->base;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    uint64_t n = hr_symbol_count(im);
    uintptr_t best = 0;
    for (uint64_t i = 1; i < n; i++) {
        const Elf64_Sym *s = &im->symtab[i];
        if (s->st_shndx == SHN_UNDEF || s->st_value == 0 || s->st_name >= im->strsz)
            continue;
        uintptr_t a = im->base + s->st_value;
        if (a <= (uintptr_t)addr && (uintptr_t)addr < a + (s->st_size ? s->st_size : 1) && a >= best) {
            best = a;
            info->dli_sname = im->strtab + s->st_name;
            info->dli_saddr = (void *)a;
        }
    }
    hr_unlock();
    return 1;
}

/* bionic struct dl_phdr_info (LP64). */
struct b_dl_phdr_info {
    uint64_t dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

static unsigned long long g_adds;

/* Used by libunwind / libc++abi to find PT_GNU_EH_FRAME for C++ exceptions. */
static int shim_dl_iterate_phdr(int (*cb)(struct b_dl_phdr_info *, size_t, void *), void *data)
{
    hr_lock();
    size_t n = 0;
    for (henge_image *im = hr_images; im; im = im->next)
        n++;
    g_adds = n;
    int rc = 0;
    for (henge_image *im = hr_images; im && rc == 0; im = im->next) {
        struct b_dl_phdr_info info = {
            .dlpi_addr = im->base,
            .dlpi_name = im->path,
            .dlpi_phdr = im->phdr,
            .dlpi_phnum = im->phnum,
            .dlpi_adds = g_adds,
            .dlpi_subs = 0,
        };
        rc = cb(&info, sizeof info, data);
    }
    hr_unlock();
    return rc;
}

#define E HR_SHIM
const struct hr_shim_entry hr_shim_dl[] = {
    E("dlopen", shim_dlopen),
    E("android_dlopen_ext", android_dlopen_ext),
    E("dlsym", shim_dlsym),
    E("dlclose", shim_dlclose),
    E("dlerror", shim_dlerror),
    E("dladdr", shim_dladdr),
    E("dl_iterate_phdr", shim_dl_iterate_phdr),
};
#undef E
const size_t hr_shim_dl_count = sizeof hr_shim_dl / sizeof hr_shim_dl[0];
