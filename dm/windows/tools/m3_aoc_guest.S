// No-add-on fixture. The test populates the two SDK imports at +0x3800.
    .text
    stp x19, x30, [sp, #-16]!
    sub sp, sp, #16
    adrp x19, #0x3000
    add x19, x19, #0x800
    ldr x16, [x19]
    blr x16                   // CountAddOnContent()
    cbnz w0, fail
    mov w9, #0xa0c
    str w9, [sp]
    mov x0, sp
    mov w1, #0
    mov w2, #1
    ldr x16, [x19, #8]
    blr x16                   // ListAddOnContent(&index, 0, 1)
    cbnz w0, fail
    ldr w9, [sp]
    cmp w9, #0xa0c            // No entries written, even with space available.
    cset w0, ne
    b done
fail:
    mov w0, #1
done:
    add sp, sp, #16
    ldp x19, x30, [sp], #16
    ret
