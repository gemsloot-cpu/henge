/* Linux / bionic (arm64) ABI constants and Darwin translations.
 * Values are taken from the NDK r29 sysroot headers (aarch64-linux-android). */
#ifndef HENGE_LINUX_H
#define HENGE_LINUX_H

#include <stdint.h>

/* errno: 1..34 match Darwin except 11 (EAGAIN) */
#define L_EPERM 1
#define L_ENOENT 2
#define L_EINTR 4
#define L_EBADF 9
#define L_EAGAIN 11
#define L_ENOMEM 12
#define L_EBUSY 16
#define L_EINVAL 22
#define L_ENOTTY 25
#define L_ERANGE 34
#define L_EDEADLK 35
#define L_ENOSYS 38
#define L_ETIMEDOUT 110

int hr_errno_to_linux(int darwin_errno);
int hr_errno_from_linux(int linux_errno);

/* open(2) flags */
#define L_O_ACCMODE 3
#define L_O_CREAT 0x40
#define L_O_EXCL 0x80
#define L_O_NOCTTY 0x100
#define L_O_TRUNC 0x200
#define L_O_APPEND 0x400
#define L_O_NONBLOCK 0x800
#define L_O_DSYNC 0x1000
#define L_O_ASYNC 0x2000
#define L_O_DIRECTORY 0x4000
#define L_O_NOFOLLOW 0x8000
#define L_O_DIRECT 0x10000
#define L_O_LARGEFILE 0x20000
#define L_O_NOATIME 0x40000
#define L_O_CLOEXEC 0x80000
#define L_O_SYNC 0x101000
#define L_O_PATH 0x200000
#define L_AT_FDCWD (-100)
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_REMOVEDIR 0x200

int hr_oflags_from_linux(int flags);
int hr_oflags_to_linux(int flags);

/* clocks */
#define L_CLOCK_REALTIME 0
#define L_CLOCK_MONOTONIC 1
#define L_CLOCK_PROCESS_CPUTIME_ID 2
#define L_CLOCK_THREAD_CPUTIME_ID 3
#define L_CLOCK_MONOTONIC_RAW 4
#define L_CLOCK_REALTIME_COARSE 5
#define L_CLOCK_MONOTONIC_COARSE 6
#define L_CLOCK_BOOTTIME 7

/* Returns the Darwin clockid, or -1 if unknown. */
int hr_clock_from_linux(int clk);

/* signals */
int hr_signal_from_linux(int sig); /* 0 if no Darwin equivalent */
int hr_signal_to_linux(int sig);

/* Linux syscall numbers (asm-generic, arm64) */
#define L_NR_getcwd 17
#define L_NR_dup 23
#define L_NR_dup3 24
#define L_NR_fcntl 25
#define L_NR_ioctl 29
#define L_NR_mkdirat 34
#define L_NR_unlinkat 35
#define L_NR_faccessat 48
#define L_NR_openat 56
#define L_NR_close 57
#define L_NR_pipe2 59
#define L_NR_lseek 62
#define L_NR_read 63
#define L_NR_write 64
#define L_NR_readv 65
#define L_NR_writev 66
#define L_NR_pread64 67
#define L_NR_pwrite64 68
#define L_NR_fsync 82
#define L_NR_exit 93
#define L_NR_exit_group 94
#define L_NR_set_tid_address 96
#define L_NR_futex 98
#define L_NR_nanosleep 101
#define L_NR_clock_gettime 113
#define L_NR_clock_getres 114
#define L_NR_sched_yield 124
#define L_NR_kill 129
#define L_NR_tgkill 131
#define L_NR_rt_sigprocmask 135
#define L_NR_uname 160
#define L_NR_prctl 167
#define L_NR_gettimeofday 169
#define L_NR_getpid 172
#define L_NR_getppid 173
#define L_NR_getuid 174
#define L_NR_geteuid 175
#define L_NR_getgid 176
#define L_NR_getegid 177
#define L_NR_gettid 178
#define L_NR_munmap 215
#define L_NR_mmap 222
#define L_NR_mprotect 226
#define L_NR_madvise 233
#define L_NR_getrandom 278
#define L_NR_membarrier 283

/* Linux thread id for the calling thread (bionic gettid()). */
int hr_gettid(void);

#endif
