/* Manifest JSON (henge-manifest-1) describing the converted image. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ---- growable string buffer ---- */

struct sbuf {
    char *p;
    size_t len, cap;
    int oom;
};

__attribute__((format(printf, 2, 3)))
static void sb_printf(struct sbuf *b, const char *fmt, ...) {
    if (b->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) { b->oom = 1; return; }
        if (b->p && b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        size_t ncap = (b->cap ? b->cap * 2 : 256) + (size_t)n;
        char *np = realloc(b->p, ncap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = ncap;
    }
}

static void sb_json_str(struct sbuf *b, const char *s, size_t maxlen) {
    sb_printf(b, "\"");
    for (size_t i = 0; i < maxlen && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", c);
        else if (c < 0x20 || c >= 0x7f) sb_printf(b, "\\u%04x", c);
        else sb_printf(b, "%c", c);
    }
    sb_printf(b, "\"");
}

char *hc_manifest_build(const struct elf_info *e, const struct henge_convert_opts *opts,
                        const struct scan_result *scan, const struct henge_import *imports,
                        size_t nimports) {
    const Elf64_Dyn *dyn = e->dyn;
    const char *dynstr = e->dynstr;
    uint64_t strsz = e->strsz;

    struct sbuf b = {0};
    sb_printf(&b, "{\n  \"format\": \"henge-manifest-1\",\n");
    const char *soname = NULL;
    for (size_t i = 0; dynstr && i < e->ndyn && dyn[i].d_tag != DT_NULL; i++)
        if (dyn[i].d_tag == DT_SONAME && dyn[i].d_val < strsz) soname = dynstr + dyn[i].d_val;
    sb_printf(&b, "  \"soname\": ");
    if (soname) sb_json_str(&b, soname, (size_t)(strsz - (uint64_t)(soname - dynstr)));
    else sb_printf(&b, "null");
    sb_printf(&b, ",\n  \"install_name\": ");
    sb_json_str(&b, opts->install_name, strlen(opts->install_name));
    sb_printf(&b, ",\n  \"shift\": %llu,\n  \"page_size\": %llu,\n", (unsigned long long)HC_SHIFT,
              (unsigned long long)HC_PAGE);
    sb_printf(&b, "  \"dynamic_vaddr\": %llu,\n", (unsigned long long)e->dyn_ph->p_vaddr);
    sb_printf(&b, "  \"segments\": [\n");
    for (unsigned i = 0; i < e->nloads; i++) {
        const Elf64_Phdr *p = &e->loads[i];
        sb_printf(&b, "    {\"vaddr\": %llu, \"memsz\": %llu, \"filesz\": %llu, \"flags\": \"%c%c%c\"}%s\n",
                  (unsigned long long)p->p_vaddr, (unsigned long long)p->p_memsz, (unsigned long long)p->p_filesz,
                  (p->p_flags & PF_R) ? 'r' : '-', (p->p_flags & PF_W) ? 'w' : '-', (p->p_flags & PF_X) ? 'x' : '-',
                  i + 1 < e->nloads ? "," : "");
    }
    sb_printf(&b, "  ],\n  \"needed\": [");
    int first = 1;
    for (size_t i = 0; dynstr && i < e->ndyn && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag != DT_NEEDED || dyn[i].d_val >= strsz) continue;
        sb_printf(&b, "%s", first ? "" : ", ");
        sb_json_str(&b, dynstr + dyn[i].d_val, (size_t)(strsz - dyn[i].d_val));
        first = 0;
    }
    sb_printf(&b, "],\n  \"imports\": [");
    for (size_t i = 0; i < nimports; i++) {
        sb_printf(&b, "%s", i ? ", " : "");
        sb_json_str(&b, imports[i].name, strlen(imports[i].name));
    }
    sb_printf(&b, "],\n  \"tls\": %s,\n", e->tls_ph ? "true" : "false");
    sb_printf(&b, "  \"svc_count\": %u,\n  \"tpidr_count\": %u,\n", scan->svc_count, scan->tpidr_count);
    sb_printf(&b, "  \"x18_count\": %u,\n  \"shadow_call_stack\": %s\n}\n", scan->x18_count,
              scan->scs ? "true" : "false");
    if (b.oom) {
        free(b.p);
        return NULL;
    }
    return b.p;
}
