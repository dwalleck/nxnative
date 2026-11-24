// Synthetic Horizon leaf function, never run until patch_nso.py rewrites it.
// x0 is a per-thread seed; x1 is the expected guest read-only TLS slot value.
    .text
    msr tpidr_el0, x0
    mrs x18, tpidr_el0
    add x18, x18, #3
    msr tpidr_el0, x18
    mrs x10, tpidr_el0
    cmp x10, x18
    b.ne mismatch
    mrs x18, tpidrro_el0
    cmp x18, x1
    b.ne mismatch
    mov x14, x29
    mov x15, x30
    mov x9, #0x123
    movi v0.16b, #0xa5
    mrs x11, fpcr
    mrs x12, fpsr
    mov x13, #0x400000
    msr fpcr, x13
    mov x8, #1
    msr fpsr, x8
    cmp x9, x9
    svc #0x42
    b.ne mismatch_fp
    cmp x9, #0x123
    b.ne mismatch_fp
    cmp x14, x29
    b.ne mismatch_fp
    cmp x15, x30
    b.ne mismatch_fp
    umov w7, v0.b[15]
    cmp w7, #0xa5
    b.ne mismatch_fp
    mrs x7, fpcr
    cmp x7, x13
    b.ne mismatch_fp
    mrs x7, fpsr
    cmp x7, x8
    b.ne mismatch_fp
    svc #0x43
    msr fpcr, x11
    msr fpsr, x12
    ret
mismatch_fp:
    msr fpcr, x11
    msr fpsr, x12
mismatch:
    mov x0, #-1
    ret
