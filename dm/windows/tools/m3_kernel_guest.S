// Read a normalized monotonic timespec, use the event's two handle rights,
// then (negative mode) validate the same output address after heap shrink.
    .text
    stp x19, x20, [sp, #-64]!
    stp x21, x30, [sp, #16]
    adrp x19, #0x3000
    ldr x19, [x19, #0x800]
    mov x1, #0x200000
    svc #1
    cbnz w0, fail
    mov x20, x1
    mov x0, x20
    blr x19
    cbnz w0, fail
    ldp x9, x10, [x20]
    stp x9, x10, [sp, #32]
    mov x0, #0x200000
    svc #0xb
    mov x0, x20
    blr x19
    cbnz w0, fail
    ldp x9, x10, [x20]
    mov w11, #0xca00
    movk w11, #0x3b9a, lsl #16
    cmp x10, x11
    b.hs fail
    ldp x11, x12, [sp, #32]
    cmp x9, x11
    b.hi events
    b.lo fail
    cmp x10, x12
    b.ls fail
 events:
    adrp x9, #0x3000
    ldr x9, [x9, #0x808]
    mov x0, x20
    blr x9
    cbnz w0, fail
    ldp x9, x10, [x20]
    cmp x9, #0                   // Positive seconds relative to Unix epoch
    b.le fail
    mov w11, #0xca00
    movk w11, #0x3b9a, lsl #16
    cmp x10, x11
    b.hs fail
    // Snapshot arithmetic on saves: a shared steady source wins over wall
    // time; snapshots imported from another machine use their network times.
    adrp x21, #0x3000
    add x0, x20, #0x100
    ldr x9, [x21, #0x810]
    blr x9
    cbnz w0, fail
    add x0, x20, #0x200
    ldr x9, [x21, #0x810]
    blr x9
    cbnz w0, fail
    ldr x9, [x20, #0x190]
    add x9, x9, #2
    str x9, [x20, #0x290]
    ldr x9, [x20, #0x148]
    add x9, x9, #3
    str x9, [x20, #0x248]
    mov x0, x20
    add x1, x20, #0x100
    add x2, x20, #0x200
    ldr x9, [x21, #0x818]
    blr x9
    tbz w0, #0, fail
    ldr x9, [x20]
    mov w10, #0x9400
    movk w10, #0x7735, lsl #16  // 2,000,000,000 ns
    cmp x9, x10
    b.ne fail
    ldrb w9, [x20, #0x298]
    eor w9, w9, #1
    strb w9, [x20, #0x298]
    mov x0, x20
    add x1, x20, #0x100
    add x2, x20, #0x200
    ldr x9, [x21, #0x818]
    blr x9
    tbz w0, #0, fail
    ldr x9, [x20]
    mov w10, #0x5e00
    movk w10, #0xb2d0, lsl #16  // 3,000,000,000 ns
    cmp x9, x10
    b.ne fail
    svc #0x45
    cbnz w0, fail
    mov w21, w1
    str w2, [sp, #48]
    mov w0, w2                 // Cannot signal a read handle.
    svc #0x11
    mov w9, #0xe401
    cmp w0, w9
    b.ne fail
    mov w0, w21
    svc #0x11
    cbnz w0, fail
    mov w0, w21
    svc #0x16                  // Read end keeps the signaled event alive.
    cbnz w0, fail
    mov w0, #0xffff8000          // Cancel this thread's next blocking wait.
    svc #0x19
    cbnz w0, fail
    add x1, sp, #48
    mov w2, #1
    mov x3, xzr
    svc #0x18
    cbnz w0, fail
    cbnz w1, fail
    ldr w0, [sp, #48]
    svc #0x17
    cbnz w0, fail
    add x1, sp, #48
    mov w2, #1
    mov x3, xzr
    svc #0x18
    mov w9, #0xea01
    cmp w0, w9
    b.ne fail
    mov x1, xzr                 // Neither the signaled wait nor zero-time
    mov w2, wzr                 // poll consumed the queued cancellation.
    mov x3, #-1
    svc #0x18
    mov w9, #0xec01
    cmp w0, w9
    b.ne fail
    ldr w0, [sp, #48]
    svc #0x16
    cbnz w0, fail
    adrp x9, #0x1000
    ldr w9, [x9, #0x780]
    cbz w9, good
    mov x1, xzr
    svc #1
    cbnz w0, fail
    mov x0, x20                // Cached heap write span is no longer valid.
    blr x19
fail:
    mov w0, #1
    b done
good:
    mov w0, wzr
done:
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #64
    ret
