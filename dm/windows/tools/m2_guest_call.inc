    // Called only by dm_guest_call after validating the target and guest SP.
    // Windows ARM64 and AAPCS64 both preserve x19-x29 and the low halves of
    // v8-v15; x19 and x29 are the only ones this trampoline repurposes. The
    // rewritten guest uses TLS slots, not the physical TEB register x18.
    // (entry, uint64_t args[8], &slots.native_sp, guest_sp) -> {x0, x1}
    .globl m2_guest_call_asm
    .seh_proc m2_guest_call_asm
m2_guest_call_asm:
    stp x29, x30, [sp, #-16]!
    .seh_save_fplr_x 16
    sub sp, sp, #48
    .seh_stackalloc 48
    str x19, [sp, #16]
    .seh_save_reg x19, 16
    .seh_endprologue
    mov x19, x2
    ldr x9, [x19]
    stp x9, xzr, [sp]          // Previous native SP; new gate's guest SP slot.
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #32]
    mov x9, sp
    str x9, [x19]              // Reentry lands below all suspended native frames.

    mov x16, x0
    mov x29, x3                // Link the guest frame chain to the suspended thunk.
    ldp x6, x7, [x1, #48]
    ldp x4, x5, [x1, #32]
    ldp x2, x3, [x1, #16]
    ldp x0, x1, [x1]
    mov sp, x29
    blr x16

    ldr x9, [x19]              // AAPCS64 returns x19 unchanged; keep both result registers.
    mov sp, x9
    ldr x9, [sp]
    str x9, [x19]
    ldp x9, x10, [sp, #32]
    msr fpcr, x9
    msr fpsr, x10
    .seh_startepilogue
    ldr x19, [sp, #16]
    .seh_save_reg x19, 16
    add sp, sp, #48
    .seh_stackalloc 48
    ldp x29, x30, [sp], #16
    .seh_save_fplr_x 16
    .seh_endepilogue
    ret
    .seh_endproc
