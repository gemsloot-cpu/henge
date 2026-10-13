# M2: code patching, thread pointer and TLS

How `henge-convert` rewrites the instructions that cannot run as-is on Darwin, and what the runtime does at the other end.

## 1. Why patch

| Instruction | On Android | On Darwin | What henge does |
| --- | --- | --- | --- |
| `svc #0` | Linux syscall (`x8` = number) | Traps into XNU with the wrong ABI | Calls the runtime's Linux syscall emulation |
| `mrs xN, tpidr_el0` | bionic thread pointer; the stack guard is at `tp + 0x28` | **Holds the CPU number** (macOS 26, Apple Silicon: reads `0x2009`; writes do not survive a context switch) | Loads a henge-managed per-thread pointer instead |
| `msr tpidr_el0, xN` | Only bionic itself | Same problem | Rejected at conversion |
| Uses of `x18` | Reserved on Android (Shadow Call Stack only) | Platform register, may be clobbered | Counted and warned about; not patched |

The `TPIDR_EL0` behavior answers V1 on macOS: the register is not free. Clang's stack protector reads `[tpidr_el0, #0x28]` in every protected function, so without the patch nearly every real `.so` would crash.

## 2. Scan

`convert/scan.c` walks the `SHF_EXECINSTR` sections (or the `PF_X` segments if there are no section headers) four bytes at a time and records:

- `svc #0` (`0xd4000001`)
- `mrs xN, tpidr_el0` (`0xd53bd040 | N`); `N = 30` is rejected, `N = 31` (xzr) becomes a `nop`
- `msr tpidr_el0, xN`: conversion fails
- any instruction naming `x18` / `w18` as a general register (integer, branch, system and load/store classes). The Shadow Call Stack push/pop pair is reported separately.

## 3. Patch island

When there is anything to patch, the Mach-O header area grows to hold an island (`__TEXT,__henge`) in front of the ELF image. `H` (the shift from Mach-O vmaddr to ELF vaddr) becomes the next 16 KiB boundary after the island instead of the fixed `0x4000`; the runtime reads it from the `__TEXT,__elf` section address. A shift that is a multiple of 16 KiB keeps every `adrp` result valid.

```
__TEXT  0x0000      Mach-O header + load commands (+256 bytes for LC_CODE_SIGNATURE)
        __henge     henge_island_hdr | svc thunk | tpidr thunk per Xn | trampoline per site
        H           ELF image (R, RX)
__DATA              ELF RW image ... bss | slots[HENGE_NSLOTS]
```

Each patched instruction becomes `b trampoline`. A trampoline is four instructions, so `x30` survives even in leaf functions that never saved it:

```
str  x30, [sp, #-16]!
bl   thunk
ldr  x30, [sp], #16
b    site + 4
```

Thunks reach the runtime through the slots in `__DATA`, which `henge_link` fills before running any code from the image:

| Slot | Contents |
| --- | --- |
| `HENGE_SLOT_SVC` | `hr_svc_entry` |
| `HENGE_SLOT_TP_KEY` | byte offset of henge's pthread key in Darwin's TSD array |
| `HENGE_SLOT_TP_SLOW` | `hr_tp_slow_entry` |

The svc thunk saves `x16` and calls `*slot[SVC]`. `hr_svc_entry` (runtime/entry.S) saves x0–x17, q0–q31, NZCV and FPSR, calls `hr_syscall` (runtime/syscall.c), and returns only `x0` changed, as the kernel would. Linux numbers, flags and errno values are translated in `runtime/linux.c`. Unknown syscalls return `-ENOSYS`.

The tpidr thunk for `xN` uses `x16` as scratch (`x17` when `N = 16`):

```
stp  xS, x30, [sp, #-16]!
adrp xS, slots ; ldr xS, [xS, #TP_KEY]
mrs  xN, tpidrro_el0          ; Darwin TSD base
and  xN, xN, #~7
ldr  xN, [xN, xS]             ; TSD[key] == pthread_getspecific(key)
cbnz xN, 1f
adrp xS, slots ; ldr xS, [xS, #TP_SLOW] ; blr xS   ; allocates, returns tp in x16
mov  xN, x16
1: ldp xS, x30, [sp], #16 ; ret
```

The fast path is seven instructions with no call. `hr_thread_init` checks once that `TSD[key]` matches `pthread_getspecific`, and fails `henge_link` if Darwin's layout ever changes.

## 4. Thread pointer and TLS

The thread pointer points at a bionic-style slot array (`tp[-1]` to `tp[14]`), allocated on first use per thread and freed by the key's destructor. `tp[5]` (`TLS_SLOT_STACK_GUARD`) holds a random guard value shared by all threads, as on Android.

ELF TLS is supported through TLSDESC, which NDK Clang emits for `-fPIC` code at API 29 and later:

- Each image with `PT_TLS` gets its own pthread key. The block for the current thread is allocated on first access, copied from `.tdata` and zero-filled for `.tbss`.
- `R_AARCH64_TLSDESC` gets `hr_tlsdesc_entry` and a `{module, offset}` descriptor. The resolver returns `block + offset - tp`. The code then adds the value of the patched `mrs tpidr_el0`, which gives the variable's address.
- Static TLS (`R_AARCH64_TLS_TPREL64`, initial-exec) is rejected. Shared libraries do not normally use it.
- Code for API 28 and below uses emutls (`__emutls_get_address` in compiler-rt), which needs only `pthread_key_*` from the shim.

## 5. IFUNC

`STT_GNU_IFUNC` symbols and `R_AARCH64_IRELATIVE` relocations call the resolver with `(AT_HWCAP | _IFUNC_ARG_HWCAP, &{size, hwcap, hwcap2})`. The HWCAP bits come from Darwin's `hw.optional.arm.FEAT_*` sysctls (runtime/cpu.c). IRELATIVE runs after all other relocations, as in bionic.

## 6. Limits

- Branches between a site and the island must be within ±128 MiB, so code above roughly 128 MiB fails to convert.
- Every patched `tpidr_el0` read costs a branch out and back plus seven instructions. Functions with a stack protector pay this twice per call.
- Signal handlers interrupting a trampoline see a slightly deeper stack; nothing else is visible.
