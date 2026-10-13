/* Linux <-> Darwin constant translation (errno, open flags, clocks, signals). */
#include "linux.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>

/* Darwin errno -> Linux errno for every value that differs. */
static const struct { int darwin, linux_; } k_errno[] = {
    {EAGAIN, 11}, {EDEADLK, 35}, {ENAMETOOLONG, 36}, {ENOLCK, 37}, {ENOSYS, 38}, {ENOTEMPTY, 39},
    {ELOOP, 40}, {ENOMSG, 42}, {EIDRM, 43}, {ENOSTR, 60}, {ENODATA, 61}, {ETIME, 62}, {ENOSR, 63},
    {ENOLINK, 67}, {EPROTO, 71}, {EMULTIHOP, 72}, {EBADMSG, 74}, {EOVERFLOW, 75}, {EILSEQ, 84},
    {EUSERS, 87}, {ENOTSOCK, 88}, {EDESTADDRREQ, 89}, {EMSGSIZE, 90}, {EPROTOTYPE, 91},
    {ENOPROTOOPT, 92}, {EPROTONOSUPPORT, 93}, {ESOCKTNOSUPPORT, 94}, {EOPNOTSUPP, 95}, {ENOTSUP, 95},
    {EPFNOSUPPORT, 96}, {EAFNOSUPPORT, 97}, {EADDRINUSE, 98}, {EADDRNOTAVAIL, 99}, {ENETDOWN, 100},
    {ENETUNREACH, 101}, {ENETRESET, 102}, {ECONNABORTED, 103}, {ECONNRESET, 104}, {ENOBUFS, 105},
    {EISCONN, 106}, {ENOTCONN, 107}, {ESHUTDOWN, 108}, {ETOOMANYREFS, 109}, {ETIMEDOUT, 110},
    {ECONNREFUSED, 111}, {EHOSTDOWN, 112}, {EHOSTUNREACH, 113}, {EALREADY, 114}, {EINPROGRESS, 115},
    {ESTALE, 116}, {EDQUOT, 122}, {ECANCELED, 125}, {EOWNERDEAD, 130}, {ENOTRECOVERABLE, 131},
};
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

int hr_errno_to_linux(int e)
{
    if (e > 0 && e <= 34 && e != EAGAIN && e != EDEADLK)
        return e;
    for (size_t i = 0; i < NELEM(k_errno); i++)
        if (k_errno[i].darwin == e)
            return k_errno[i].linux_;
    return e <= 34 ? e : L_EINVAL;
}

int hr_errno_from_linux(int e)
{
    if (e > 0 && e <= 34 && e != L_EAGAIN)
        return e;
    for (size_t i = 0; i < NELEM(k_errno); i++)
        if (k_errno[i].linux_ == e)
            return k_errno[i].darwin;
    return e <= 34 ? e : EINVAL;
}

static const struct { int linux_, darwin; } k_oflags[] = {
    {L_O_CREAT, O_CREAT},         {L_O_EXCL, O_EXCL},         {L_O_NOCTTY, O_NOCTTY},
    {L_O_TRUNC, O_TRUNC},         {L_O_APPEND, O_APPEND},     {L_O_NONBLOCK, O_NONBLOCK},
    {L_O_DSYNC, O_DSYNC},         {L_O_ASYNC, O_ASYNC},       {L_O_DIRECTORY, O_DIRECTORY},
    {L_O_NOFOLLOW, O_NOFOLLOW},   {L_O_CLOEXEC, O_CLOEXEC},
};

int hr_oflags_from_linux(int f)
{
    int d = f & L_O_ACCMODE; /* O_RDONLY/O_WRONLY/O_RDWR are 0/1/2 on both */
    if ((f & L_O_SYNC) == L_O_SYNC)
        d |= O_SYNC;
    for (size_t i = 0; i < NELEM(k_oflags); i++)
        if (f & k_oflags[i].linux_)
            d |= k_oflags[i].darwin;
    /* O_DIRECT, O_LARGEFILE, O_NOATIME, O_PATH: no Darwin equivalent; dropped. */
    return d;
}

int hr_oflags_to_linux(int d)
{
    int f = d & O_ACCMODE;
    if (d & O_SYNC)
        f |= L_O_SYNC;
    for (size_t i = 0; i < NELEM(k_oflags); i++)
        if (d & k_oflags[i].darwin)
            f |= k_oflags[i].linux_;
    return f | L_O_LARGEFILE; /* bionic LP64 reports O_LARGEFILE in F_GETFL */
}

int hr_clock_from_linux(int clk)
{
    switch (clk) {
    case L_CLOCK_REALTIME:
    case L_CLOCK_REALTIME_COARSE: return CLOCK_REALTIME;
    case L_CLOCK_MONOTONIC:
    case L_CLOCK_MONOTONIC_COARSE: return CLOCK_UPTIME_RAW; /* Linux MONOTONIC stops in suspend */
    case L_CLOCK_MONOTONIC_RAW: return CLOCK_UPTIME_RAW;
    case L_CLOCK_BOOTTIME: return CLOCK_MONOTONIC; /* Darwin MONOTONIC counts sleep */
    case L_CLOCK_PROCESS_CPUTIME_ID: return CLOCK_PROCESS_CPUTIME_ID;
    case L_CLOCK_THREAD_CPUTIME_ID: return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}

/* Index = Linux signal number; value = Darwin number (0: none). */
static const int k_sig_from_linux[32] = {
    0,       SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGTRAP, SIGABRT,   SIGBUS,
    SIGFPE,  SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM,   SIGTERM,
    0 /* STKFLT */, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,
    SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, 0 /* PWR */, SIGSYS,
};

int hr_signal_from_linux(int sig)
{
    return sig > 0 && sig < 32 ? k_sig_from_linux[sig] : 0;
}

int hr_signal_to_linux(int sig)
{
    for (int i = 1; i < 32; i++)
        if (k_sig_from_linux[i] == sig)
            return i;
    return 0;
}

int hr_gettid(void)
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (int)(tid & 0x7fffffff);
}
