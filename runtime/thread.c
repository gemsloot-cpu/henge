/* Thread pointer and ELF TLS for converted code (docs/m2-patching.md §4).
 *
 * Android code reads its thread pointer with `mrs xN, tpidr_el0`. On Darwin that register
 * holds the CPU number, so the converter rewrites each read to load a per-thread pointer
 * from Darwin's TSD array: tp = TSD[g_tp_key]. tp points at a bionic-style slot array, so
 * the stack guard is at tp + 0x28 (TLS_SLOT_STACK_GUARD) as Clang expects.
 *
 * ELF TLS uses TLSDESC only: each module gets a pthread key holding this thread's block, and
 * the resolver returns block + offset - tp. Static TLS (TPREL64) is rejected by reloc.c.
 */
#include "internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BIONIC_SLOTS 16          /* slots -1 .. 14 */
#define SLOT_STACK_GUARD 5

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_tp_key;
static int g_init_err; /* 0 ok, else a message index */
uintptr_t hr_stack_guard;

static void tp_free(void *tp)
{
    free((uintptr_t *)tp - 1);
}

static void *tsd_base(void)
{
    uintptr_t v;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
    return (void *)(v & ~(uintptr_t)7);
}

static void thread_init(void)
{
    arc4random_buf(&hr_stack_guard, sizeof hr_stack_guard);
    hr_stack_guard &= ~(uintptr_t)0xff; /* bionic-style: low byte zero stops string overreads */
    if (pthread_key_create(&g_tp_key, tp_free) != 0) {
        g_init_err = 1;
        return;
    }
    /* The island reads TSD[key] directly; check that matches pthread_getspecific. */
    static int probe;
    pthread_setspecific(g_tp_key, &probe);
    void *direct = ((void **)tsd_base())[g_tp_key];
    pthread_setspecific(g_tp_key, NULL);
    if (direct != &probe)
        g_init_err = 2;
}

int hr_thread_init(char *err, size_t errlen)
{
    pthread_once(&g_once, thread_init);
    if (g_init_err == 1)
        return hr_fail(err, errlen, "pthread_key_create failed for the thread-pointer key");
    if (g_init_err == 2)
        return hr_fail(err, errlen, "Darwin TSD layout differs from what the patch island expects");
    return 0;
}

uintptr_t hr_tp_key_offset(void)
{
    return (uintptr_t)g_tp_key * sizeof(void *);
}

void *hr_tp_get(void)
{
    void *tp = pthread_getspecific(g_tp_key);
    if (tp)
        return tp;
    uintptr_t *slots = calloc(BIONIC_SLOTS, sizeof *slots);
    if (!slots)
        abort();
    uintptr_t *t = slots + 1; /* slot -1 (TLS_SLOT_BIONIC_TLS) stays NULL */
    t[0] = (uintptr_t)t;      /* TLS_SLOT_SELF-ish; nothing outside bionic reads it */
    t[SLOT_STACK_GUARD] = hr_stack_guard;
    pthread_setspecific(g_tp_key, t);
    return t;
}

/* ---- ELF TLS (TLSDESC) ---- */

struct hr_tls_module {
    pthread_key_t key;
    const void *init; /* .tdata image */
    size_t init_size, size, align;
};

struct hr_tlsdesc {
    struct hr_tls_module *mod;
    uint64_t offset;
};

int hr_tls_register(henge_image *im, const Elf64_Phdr *tls, char *err, size_t errlen)
{
    struct hr_tls_module *m = calloc(1, sizeof *m);
    if (!m)
        return hr_fail(err, errlen, "out of memory");
    if (pthread_key_create(&m->key, free) != 0) {
        free(m);
        return hr_fail(err, errlen, "pthread_key_create failed for a TLS module");
    }
    m->init = (const void *)(im->base + tls->p_vaddr);
    m->init_size = tls->p_filesz;
    m->size = tls->p_memsz ? tls->p_memsz : 1;
    m->align = tls->p_align < sizeof(void *) ? sizeof(void *) : tls->p_align;
    im->tls = m;
    return 0;
}

void *hr_tlsdesc_arg(struct hr_tls_module *mod, uint64_t offset)
{
    struct hr_tlsdesc *d = malloc(sizeof *d);
    if (d) {
        d->mod = mod;
        d->offset = offset;
    }
    return d;
}

/* Called by hr_tlsdesc_entry with x0 = the descriptor (GOT pair). Returns block + offset - tp. */
intptr_t hr_tlsdesc_resolve(const uintptr_t *desc)
{
    const struct hr_tlsdesc *d = (const struct hr_tlsdesc *)desc[1];
    struct hr_tls_module *m = d->mod;
    uint8_t *block = pthread_getspecific(m->key);
    if (!block) {
        void *p = NULL;
        if (posix_memalign(&p, m->align, m->size) != 0)
            abort();
        block = p;
        memcpy(block, m->init, m->init_size);
        memset(block + m->init_size, 0, m->size - m->init_size);
        pthread_setspecific(m->key, block);
    }
    return (intptr_t)(block + d->offset) - (intptr_t)hr_tp_get();
}
