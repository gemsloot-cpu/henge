/* Entry points called from patched Android code (docs/m2-patching.md §3).
 *
 * Each one preserves every register the Android caller can observe except its result:
 * x0-x17, q0-q31, NZCV and FPSR are saved around the C call. x18 is left alone (Darwin's).
 *
 *   hr_svc_entry      Linux syscall: x8 = number, x0-x5 = args; returns x0 (-errno on error)
 *   hr_tp_slow_entry  allocates this thread's bionic slot array; returns it in x16
 *   hr_tlsdesc_entry  TLSDESC resolver: x0 = descriptor; returns x0 = block + offset - tp
 */
    .text
    .p2align 2

#define FRAME 672 /* x0-x17 (144) + nzcv/fpsr (16) + q0-q31 (512) */

.macro SAVE_ALL
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #FRAME
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp q0, q1, [sp, #160]
    stp q2, q3, [sp, #192]
    stp q4, q5, [sp, #224]
    stp q6, q7, [sp, #256]
    stp q8, q9, [sp, #288]
    stp q10, q11, [sp, #320]
    stp q12, q13, [sp, #352]
    stp q14, q15, [sp, #384]
    stp q16, q17, [sp, #416]
    stp q18, q19, [sp, #448]
    stp q20, q21, [sp, #480]
    stp q22, q23, [sp, #512]
    stp q24, q25, [sp, #544]
    stp q26, q27, [sp, #576]
    stp q28, q29, [sp, #608]
    stp q30, q31, [sp, #640]
    mrs x9, nzcv
    mrs x10, fpsr
    stp x9, x10, [sp, #144]
.endm

.macro RESTORE_ALL
    ldp x9, x10, [sp, #144]
    msr nzcv, x9
    msr fpsr, x10
    ldp q0, q1, [sp, #160]
    ldp q2, q3, [sp, #192]
    ldp q4, q5, [sp, #224]
    ldp q6, q7, [sp, #256]
    ldp q8, q9, [sp, #288]
    ldp q10, q11, [sp, #320]
    ldp q12, q13, [sp, #352]
    ldp q14, q15, [sp, #384]
    ldp q16, q17, [sp, #416]
    ldp q18, q19, [sp, #448]
    ldp q20, q21, [sp, #480]
    ldp q22, q23, [sp, #512]
    ldp q24, q25, [sp, #544]
    ldp q26, q27, [sp, #576]
    ldp q28, q29, [sp, #608]
    ldp q30, q31, [sp, #640]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    mov sp, x29
    ldp x29, x30, [sp], #16
.endm

    .globl _hr_svc_entry
    .private_extern _hr_svc_entry
_hr_svc_entry:
    SAVE_ALL
    mov x6, x8
    bl _hr_syscall
    str x0, [sp, #0]
    RESTORE_ALL
    ret

    .globl _hr_tp_slow_entry
    .private_extern _hr_tp_slow_entry
_hr_tp_slow_entry:
    SAVE_ALL
    bl _hr_tp_get
    str x0, [sp, #128]
    RESTORE_ALL
    ret

    .globl _hr_tlsdesc_entry
    .private_extern _hr_tlsdesc_entry
_hr_tlsdesc_entry:
    SAVE_ALL
    bl _hr_tlsdesc_resolve
    str x0, [sp, #0]
    RESTORE_ALL
    ret
