# ABI differences between Android and Darwin arm64

A list of differences that matter at the boundary between Android code (AAPCS64 + bionic) and henge's shims (Darwin arm64 + libSystem).
The shims are compiled for Darwin, so **every function called from the Android side must receive its arguments according to the Android ABI**.

## 1. Calling convention

| Item | Android (AAPCS64) | Darwin arm64 | How henge handles it |
| --- | --- | --- | --- |
| Variadic arguments | Passed in registers (x0-x7, v0-v7), same as named arguments | **All passed on the stack** | Variadic functions (`printf`, `open`, `ioctl`, `fcntl`, etc.) save registers in an asm entry stub and build an AAPCS-style `va_list` |
| `va_list` | A struct (`__stack`, `__gr_top`, `__vr_top`, `__gr_offs`, `__vr_offs`) | `char *` | The `vprintf` family gets a custom implementation that reads AAPCS-style `va_list` |
| `long double` | 128-bit (IEEE quad) | 64-bit (same as `double`) | `strtold`, `%Lf`, etc. use compiler-rt soft float (`__addtf3`, etc.) |
| Integer arguments narrower than 32 bits | Callee extends | Caller extends to 32 bits | Shim functions taking `bool`, `char`, or `short` explicitly mask or extend them on entry |
| Placement of stack arguments | 8-byte slots | Packed by natural type alignment | Matters for functions that take 9 or more arguments. List those functions and handle each one individually |
| Signedness of `char` | Unsigned | Signed | A `char` crossing the boundary is treated like "integers narrower than 32 bits" above |
| x18 | Not used by ordinary code; used by code built with Shadow Call Stack | Platform-reserved; the OS may overwrite it | The converter detects x18 use and warns. `.so` files using SCS are out of scope |

The same differences apply in reverse when a shim calls back into Android code (`qsort` comparators, `pthread_create` start routines, `atexit`, etc.).
Fixed-arity functions taking only pointers and `int` are unaffected, so list the others first.

## 2. TLS and stack protection

- Android's Clang reads the stack-protector value from `TPIDR_EL0 + 0x28` (a bionic TLS slot).
- Darwin keeps its own thread pointer in `TPIDRRO_EL0`. If `TPIDR_EL0` is free and preserved across context switches and signals, henge can place a bionic-style TLS area there.
- If it is not preserved, the converter rewrites `mrs xN, tpidr_el0` into a call to a shim function. This is possible because the rewrite happens before signing.

→ Verification item V1 in [roadmap.md](../docs/roadmap.md).

ELF TLS (`__thread` variables, `R_AARCH64_TLSDESC`) is handled by henge managing its own static TLS block with the same layout as bionic (M2).

## 3. Structs and constants

| Target | Difference | Handling |
| --- | --- | --- |
| `pthread_mutex_t` | 40 bytes in bionic, 64 bytes in Darwin | Put an `os_unfair_lock` (4 bytes) and state directly inside the bionic-sized area. bionic's static initializers are all zero, so treat zero as uninitialized |
| `pthread_cond_t`, `pthread_rwlock_t` | Same as above | Implement on top of `__ulock_wait` / `__ulock_wake` equivalents, or store a pointer to a separately allocated object |
| `struct stat` | Field order and sizes | Write conversion functions |
| `struct dirent` | Same as above | Same as above |
| `struct sigaction` | Same as above; `sa_flags` values also differ | Same as above |
| `errno` | Values differ (`EAGAIN` is 11 on Linux / 35 on Darwin, etc.) | Convert via a table. The shim provides `__errno()` |
| `open` / `mmap` / `fcntl` flags | Values differ | Convert via a table |
| Signal numbers | Some differ (`SIGUSR1` is 10 on Linux / 30 on Darwin, etc.) | Convert via a table |
| `ucontext_t` / `mcontext_t` | Completely different contents | Convert to Linux layout when passing to signal handlers. Used by crash reporters (M3 and later) |
| `__sF` (`stdin` / `stdout` / `stderr`) | Legacy bionic ABI | The shim exports an array of `FILE` |

## 4. Threads and memory

| Target | Difference | Handling |
| --- | --- | --- |
| Thread stack size | 1 MiB on Android; 512 KiB for secondary threads on iOS | The `pthread_create` shim defaults to 1 MiB when no size is given |
| Page size | Some Android code assumes 4 KiB; iOS uses 16 KiB | `getpagesize()` / `sysconf(_SC_PAGESIZE)` return 16 KiB. Code that hard-codes 4 KiB is handled case by case |
| Linux-specific APIs such as `gettid`, `futex` | Not on Darwin | Implement with `pthread_threadid_np`, `__ulock_*`, etc. |
| `/proc/self/*` | Not on Darwin | Return plausible contents only for commonly read files such as `/proc/self/maps` |

## 5. How to verify

- For each function that crosses the boundary, call it from a test `.so` built with the NDK and compare the results.
- Always write tests for variadic arguments, `long double`, 9+ arguments, and `char` / `short` arguments.
