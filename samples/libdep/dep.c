/* libdep.so: DT_NEEDED libbase.so plus Android-side libdl use. */
#include <dlfcn.h>
#include <link.h>
#include <string.h>

int base_value(void);
const char *base_name(int i);
extern int base_counter;

int dep_value(void) { return base_value() + base_counter; }

int dep_name_len(int i) { return (int)strlen(base_name(i)); }

/* dlopen by soname returns the already linked libbase. */
int dep_dlsym(void) {
    void *h = dlopen("libbase.so", RTLD_NOW);
    if (!h) return -1;
    int (*f)(void) = (int (*)(void))dlsym(h, "base_value");
    return f ? f() : -2;
}

/* dlsym through RTLD_DEFAULT, and a missing symbol sets dlerror. */
int dep_dlsym_default(void) {
    int (*f)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "dep_value");
    return f ? f() : -1;
}

int dep_dlsym_missing(void) {
    void *h = dlopen("libbase.so", RTLD_NOW);
    dlerror();
    return dlsym(h, "no_such_symbol") == NULL && dlerror() != NULL && dlerror() == NULL;
}

int dep_dlopen_missing(void) { return dlopen("libnope.so", RTLD_NOW) == NULL && dlerror() != NULL; }

int dep_dladdr(void) {
    Dl_info info;
    if (!dladdr((const char *)dep_value + 4, &info)) return 0;
    return info.dli_sname && strcmp(info.dli_sname, "dep_value") == 0 && info.dli_fname != NULL;
}

static int count_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    int *n = data;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_GNU_EH_FRAME) n[1]++;
    n[0]++;
    return 0;
}

/* Number of images seen * 100 + images with PT_GNU_EH_FRAME. */
int dep_phdr(void) {
    int n[2] = {0, 0};
    dl_iterate_phdr(count_cb, n);
    return n[0] * 100 + n[1];
}
