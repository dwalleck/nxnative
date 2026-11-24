// Requires the externally supplied SDK; no codec implementation in the fixture.
// 20 ms of an original 440 Hz sine wave, mono 8 kHz, encoded with libopus.
    .text
    .macro call n
    ldr x16, [x19, #(8 * \n)]
    blr x16
    .endm
    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov x1, #0x200000
    svc #1
    cbnz w0, fail
    mov x20, x1
    mov w0, #8000
    mov w1, #1
    mov w2, wzr
    call 0
    cbz x0, fail
    cmp x0, #0x100000
    b.hi fail
    mov x4, x0
    mov x0, x20
    mov w1, #8000
    mov w2, #1
    add x3, x20, #0x1000
    mov w5, wzr
    call 1
    cbnz w0, fail
    ldr x8, [x20]                // Same inlined IsInitialized test as the game
    cbz x8, fail
    mov x0, x20
    call 2
    mov w8, #8000
    cmp w0, w8
    b.ne fail
    mov x0, x20
    call 3
    cmp w0, #1
    b.ne fail
    mov x0, x20
    add x1, x20, #0x100
    add x2, x20, #0x108
    add x3, x20, #0x200
    mov x4, #320
    adr x5, packet
    mov x6, #40
    call 4
    cbnz w0, fail
    ldr x8, [x20, #0x100]
    cmp x8, #40
    b.ne fail
    ldr w8, [x20, #0x108]
    cmp w8, #160
    b.ne fail
    // Demand decoded signal, not just a successful call or silence.
    add x9, x20, #0x200
    mov w10, wzr
1:  ldrsh w11, [x9], #2
    cmp w11, #0
    cneg w11, w11, mi
    add w10, w10, w11
    subs w8, w8, #1
    b.ne 1b
    cmp w10, #0x1000
    b.lo fail
    mov x0, x20
    call 5
    mov x0, x20
    call 6
    ldr x8, [x20]
    cbnz x8, fail
    mov w0, wzr
    b done
fail:
    mov w0, #1
done:
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
packet:
    .byte 0,0,0,32,0x18,0x68,0xfa,0x28,0x08,0x83,0xca,0x24,0xaa,0x5b,0xea,0x7d
    .byte 0x35,0x75,0xd5,0xf6,0xbd,0x6f,0x4b,0x4a,0xef,0x5e,0x44,0xd0,0x3e,0x55,0xb7,0x63
    .byte 0x8d,0x03,0xe5,0xb5,0x69,0xa3,0x31,0x80
