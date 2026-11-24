// Fixture entry at module +0x100. The host import at +0x3800 calls guest code.
    .text

    // Eight distinct GPR arguments, including the caller's live frame address.
    .macro make_args depth
    mov x9, #\depth
    mov x10, sp
    stp x9, x10, [sp, #16]
    mrs x9, tpidr_el0
    mrs x10, tpidrro_el0
    stp x9, x10, [sp, #32]
    mov x9, x18
    ldr w10, [x10, #0x110]      // Horizon thread ID in the thread-local region.
    stp x9, x10, [sp, #48]
    mov x9, #0x6616
    mov x10, #0x7717
    stp x9, x10, [sp, #64]
    .endm

    .macro check_tls target
    ldp x9, x10, [sp, #32]
    mrs x11, tpidr_el0
    cmp x9, x11
    b.ne \target
    mrs x11, tpidrro_el0
    cmp x10, x11
    b.ne \target
    ldr x9, [sp, #48]
    cmp x18, x9
    b.ne \target
    .endm

    .macro check_fp fpcr_bits, fpsr_bits, target
    mrs x9, fpcr
    cmp x9, #\fpcr_bits
    b.ne \target
    mrs x9, fpsr
    cmp x9, #\fpsr_bits
    b.ne \target
    .endm

    stp x29, x30, [sp, #-128]!
    mov x29, sp
    stp x19, x20, [sp, #80]
    str d8, [sp, #96]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #104]
    mov x9, x18
    str x9, [sp, #120]

    mov x19, #0x1979
    mov x20, #0x20a0
    fmov d8, x20
    mov x18, #0x1858
    mov x9, #0x400000
    msr fpcr, x9
    mov x9, #1
    msr fpsr, x9
    make_args 1
    adr x0, callback
    add x1, sp, #16
    bl import_call
    cbnz x0, done             // Callback's diagnostic comes back through host C.
    check_tls bad_outer
    check_fp 0x400000, 1, bad_outer
    mov x9, #0x1979
    cmp x19, x9
    b.ne bad_outer
    fmov x9, d8
    cmp x9, x20
    b.ne bad_outer
    ldp x9, x10, [sp, #64]
    mov x11, #0x6616
    cmp x9, x11
    b.ne bad_outer
    mov x11, #0x7717
    cmp x10, x11
    b.ne bad_outer

    adr x0, callback
    add x0, x0, #2             // Misaligned text address.
    add x1, sp, #16
    bl import_call
    cmn x0, #1
    b.ne bad_validation
    mov x0, sp                 // Writable guest address, not executable text.
    add x1, sp, #16
    bl import_call
    cmn x0, #1
    b.ne bad_validation
    adrp x9, #0x3000
    ldr x0, [x9, #0x800]       // Executable native import address, not guest text.
    add x1, sp, #16
    bl import_call
    cmn x0, #1
    b.ne bad_validation

    mov x0, xzr                // Fixture import selects the original SDK symbol.
    add x1, sp, #16
    bl import_call
    cbnz x0, bad_validation
    mov w0, #0
    b done
bad_outer:
    mov w0, #1
    b done
bad_validation:
    mov w0, #2
done:
    ldr x9, [sp, #120]
    mov x18, x9
    ldp x9, x10, [sp, #104]
    msr fpcr, x9
    msr fpsr, x10
    ldr d8, [sp, #96]
    ldp x19, x20, [sp, #80]
    ldp x29, x30, [sp], #128
    ret

import_call:
    adrp x16, #0x3000
    ldr x16, [x16, #0x800]
    br x16

callback:
    cmp x0, #1
    b.hi bad_args
    mov x9, sp
    sub x9, x1, x9
    cbz x9, bad_args
    cmp x9, #128              // Must be just below this caller, not the stack top.
    b.hi bad_args
    mrs x9, tpidr_el0
    cmp x2, x9
    b.ne bad_args
    mrs x9, tpidrro_el0
    cmp x3, x9
    b.ne bad_args
    cmp x4, x18
    b.ne bad_args
    mov x9, #0x6616
    cmp x6, x9
    b.ne bad_args
    mov x9, #0x7717
    cmp x7, x9
    b.ne bad_args
    mov x10, x0
    mov w1, #0xffff8000        // GetThreadId on the same registered guest thread.
    svc #0x25
    cbnz w0, bad_svc
    cmp w1, w5
    b.ne bad_svc
    cbz x10, nested

    check_fp 0x400000, 1, bad_svc
    stp x29, x30, [sp, #-32]!
    str x19, [sp, #16]
    mov x19, sp
    // Like the SDK's fiber dispatcher, switch SP without creating a kernel
    // thread. A nested import calls guest code, including an SVC, on this stack.
    adrp x9, #0x3000
    add x9, x9, #0xf00
    sub x9, x9, #80
    mov sp, x9
    mov x29, x19
    add x2, x2, #16
    msr tpidr_el0, x2
    mov x18, #0x1878
    mov x9, #0x800000
    msr fpcr, x9
    mov x9, #2
    msr fpsr, x9
    make_args 0
    adr x0, callback
    add x1, sp, #16
    bl import_call
    cbnz x0, callback_done
    check_tls bad_nested
    check_fp 0x800000, 2, bad_nested
    b success
bad_nested:
    mov w0, #3
callback_done:
    mov sp, x19
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
success:
    mov sp, x19
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    b return_pair
nested:
    check_fp 0x800000, 2, bad_svc
    add x2, x2, #16
    msr tpidr_el0, x2
    mov x18, #0x1898
    mov x9, #0xc00000
    msr fpcr, x9
    mov x9, #4
    msr fpsr, x9
return_pair:
    mov x0, #0x1357
    mov x1, #0x2468
    ret
bad_args:
    mov w0, #4
    ret
bad_svc:
    mov w0, #5
    ret
