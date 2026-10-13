/* Linux syscalls reached through patched `svc #0` (docs/m2-patching.md §3). Arguments and
 * results use Linux conventions: numbers from linux.h, -errno on failure. Unknown calls
 * return -ENOSYS, which well-behaved callers treat as "not supported". */
#include "internal.h"
#include "linux.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static long ret_errno(long r)
{
    return r < 0 ? -(long)hr_errno_to_linux(errno) : r;
}

long hr_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr)
{
    (void)a4;
    (void)a5;
    switch (nr) {
    case L_NR_read: return ret_errno(read((int)a0, (void *)a1, (size_t)a2));
    case L_NR_write: return ret_errno(write((int)a0, (const void *)a1, (size_t)a2));
    case L_NR_readv: return ret_errno(readv((int)a0, (const struct iovec *)a1, (int)a2));
    case L_NR_writev: return ret_errno(writev((int)a0, (const struct iovec *)a1, (int)a2));
    case L_NR_pread64: return ret_errno(pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3));
    case L_NR_pwrite64: return ret_errno(pwrite((int)a0, (const void *)a1, (size_t)a2, (off_t)a3));
    case L_NR_openat: {
        int fd = (int)a0 == L_AT_FDCWD ? AT_FDCWD : (int)a0;
        return ret_errno(openat(fd, (const char *)a1, hr_oflags_from_linux((int)a2), (int)a3));
    }
    case L_NR_close: return ret_errno(close((int)a0));
    case L_NR_lseek: return ret_errno(lseek((int)a0, (off_t)a1, (int)a2));
    case L_NR_fsync: return ret_errno(fsync((int)a0));
    case L_NR_dup: return ret_errno(dup((int)a0));
    case L_NR_getcwd: return getcwd((char *)a0, (size_t)a1) ? (long)strlen((char *)a0) + 1 : ret_errno(-1);
    case L_NR_exit:
        pthread_exit(NULL);
    case L_NR_exit_group:
        _exit((int)a0);
    case L_NR_getpid: return getpid();
    case L_NR_getppid: return getppid();
    case L_NR_getuid: return getuid();
    case L_NR_geteuid: return geteuid();
    case L_NR_getgid: return getgid();
    case L_NR_getegid: return getegid();
    case L_NR_gettid: return hr_gettid();
    case L_NR_sched_yield: return sched_yield();
    case L_NR_clock_gettime: {
        int clk = hr_clock_from_linux((int)a0);
        if (clk < 0) return -L_EINVAL;
        return ret_errno(clock_gettime((clockid_t)clk, (struct timespec *)a1));
    }
    case L_NR_clock_getres: {
        int clk = hr_clock_from_linux((int)a0);
        if (clk < 0) return -L_EINVAL;
        return ret_errno(clock_getres((clockid_t)clk, (struct timespec *)a1));
    }
    case L_NR_nanosleep:
        return ret_errno(nanosleep((const struct timespec *)a0, (struct timespec *)a1));
    case L_NR_kill: {
        int sig = a1 ? hr_signal_from_linux((int)a1) : 0;
        if (a1 && !sig) return -L_EINVAL;
        return ret_errno(kill((pid_t)a0, sig));
    }
    case L_NR_getrandom:
        arc4random_buf((void *)a0, (size_t)a1);
        return a1;
    case L_NR_munmap: return ret_errno(munmap((void *)a0, (size_t)a1));
    case L_NR_mprotect: return ret_errno(mprotect((void *)a0, (size_t)a1, (int)a2));
    case L_NR_membarrier: return 0;
    default: return -L_ENOSYS;
    }
}
