// Register-preservation check around patched instructions: loads known values into
// x1-x17 and v0-v7, sets NZCV, runs mrs tpidr_el0 into x0 and x16 and an svc, and checks
// everything else is unchanged. Returns 0 on success, else the number of the failed check.
    .text
    .globl regcheck
    .type regcheck, %function
    .p2align 2
regcheck:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    .irp r, 1,2,3,4,5,6,7,9,10,11,12,13,14,15,16,17
    mov x\r, #(\r * 239)
    .endr
    .irp r, 0,1,2,3,4,5,6,7
    mov x8, #(\r + 0x40)
    dup v\r\().2d, x8
    .endr
    mov x8, #0
    cmp x8, #0              // Z=1
    mrs x0, tpidr_el0       // patched (x0)
    b.ne fail_flags
    cbz x0, fail_tp
    bl verify
    cbnz x8, done
    mrs x16, tpidr_el0      // patched (x16: thunk uses x17 as scratch)
    cmp x16, x0
    b.ne fail_x16
    mov x16, #(16 * 239)
    bl verify
    cbnz x8, done
    mov x0, #1
    cmn x0, #1              // N=0 Z=0 C=0 V=0 ... set C via cmp below
    cmp x0, #0              // C=1, Z=0
    mov x8, #172            // getpid
    svc #0                  // patched
    b.cc fail_flags
    b.eq fail_flags
    cmp x8, #172
    b.ne fail_x8
    mov x8, #0
    bl verify
    b done
fail_flags:
    mov x8, #100
    b done
fail_tp:
    mov x8, #101
    b done
fail_x16:
    mov x8, #102
    b done
fail_x8:
    mov x8, #103
done:
    mov x0, x8
    ldp x29, x30, [sp], #16
    ret

// x8 = 0 if x1-x7, x9-x17 and v0-v7 hold their values, else the register number + 1.
verify:
    mov x8, #0
    .irp r, 1,2,3,4,5,6,7,9,10,11,12,13,14,15,16,17
    cmp x\r, #(\r * 239)
    b.eq 1f
    mov x8, #(\r + 1)
    ret
1:
    .endr
    .irp r, 0,1,2,3,4,5,6,7
    mov x8, v\r\().d[1]
    cmp x8, #(\r + 0x40)
    b.eq 2f
    mov x8, #(\r + 33)
    ret
2:
    .endr
    mov x8, #0
    ret
