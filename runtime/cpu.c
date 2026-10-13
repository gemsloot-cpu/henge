/* Linux AT_HWCAP / AT_HWCAP2 bits for this CPU, from Darwin's hw.optional sysctls. Used for
 * IFUNC resolvers and getauxval(). Only features Darwin lets user code use are reported
 * (no CPUID: MIDR reads trap; no SVE/SME/PAC). */
#include "internal.h"

#include <pthread.h>
#include <sys/sysctl.h>

static const struct {
    const char *name;
    int hwcap2;
    int bit;
} k_feats[] = {
    {"hw.optional.arm.FEAT_AES", 0, 3},       {"hw.optional.arm.FEAT_PMULL", 0, 4},
    {"hw.optional.arm.FEAT_SHA1", 0, 5},      {"hw.optional.arm.FEAT_SHA256", 0, 6},
    {"hw.optional.arm.FEAT_CRC32", 0, 7},     {"hw.optional.arm.FEAT_LSE", 0, 8},
    {"hw.optional.arm.FEAT_FP16", 0, 9},      {"hw.optional.arm.FEAT_FP16", 0, 10},
    {"hw.optional.arm.FEAT_RDM", 0, 12},      {"hw.optional.arm.FEAT_JSCVT", 0, 13},
    {"hw.optional.arm.FEAT_FCMA", 0, 14},     {"hw.optional.arm.FEAT_LRCPC", 0, 15},
    {"hw.optional.arm.FEAT_DPB", 0, 16},      {"hw.optional.arm.FEAT_SHA3", 0, 17},
    {"hw.optional.arm.FEAT_DotProd", 0, 20},  {"hw.optional.arm.FEAT_SHA512", 0, 21},
    {"hw.optional.arm.FEAT_FHM", 0, 23},      {"hw.optional.arm.FEAT_DIT", 0, 24},
    {"hw.optional.arm.FEAT_LSE2", 0, 25},     {"hw.optional.arm.FEAT_LRCPC2", 0, 26},
    {"hw.optional.arm.FEAT_FlagM", 0, 27},    {"hw.optional.arm.FEAT_SSBS", 0, 28},
    {"hw.optional.arm.FEAT_SB", 0, 29},
    {"hw.optional.arm.FEAT_DPB2", 1, 0},      {"hw.optional.arm.FEAT_FlagM2", 1, 7},
    {"hw.optional.arm.FEAT_FRINTTS", 1, 8},   {"hw.optional.arm.FEAT_I8MM", 1, 13},
    {"hw.optional.arm.FEAT_BF16", 1, 14},     {"hw.optional.arm.FEAT_BTI", 1, 17},
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static uint64_t g_hwcap, g_hwcap2;

static void probe(void)
{
    g_hwcap = 1u << 0 | 1u << 1; /* FP, ASIMD: always present on arm64 */
    for (size_t i = 0; i < sizeof k_feats / sizeof k_feats[0]; i++) {
        int v = 0;
        size_t len = sizeof v;
        if (sysctlbyname(k_feats[i].name, &v, &len, NULL, 0) == 0 && v)
            *(k_feats[i].hwcap2 ? &g_hwcap2 : &g_hwcap) |= 1ull << k_feats[i].bit;
    }
}

void hr_hwcap(uint64_t *hwcap, uint64_t *hwcap2)
{
    pthread_once(&g_once, probe);
    *hwcap = g_hwcap;
    *hwcap2 = g_hwcap2;
}
