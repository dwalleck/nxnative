// Headless NVN2 program initialization, identity and failure atomicity.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    sub sp, sp, #320
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    adrp x21, #0x1000
    ldr w21, [x21, #0x780]
    add x0, x20, #16
    call 0                       // DeviceBuilderSetDefaults
    mov x0, x20
    add x1, x20, #16
    call 1                       // DeviceInitialize
    cbz w0, fail
    add x0, x20, #32
    call 2                       // ProgramBuilderSetDefaults
    add x0, x20, #32
    mov x1, x20
    call 3                       // ProgramBuilderSetDevice

    adrp x9, #0x1000
    add x10, x9, #0x580          // Vertex code address
    add x11, x9, #0x500          // Vertex control address
    cmp w21, #2
    csel x12, xzr, x10, eq       // Negative: invalid code address
    stp x12, x11, [x20, #96]
    add x12, x9, #0x680          // Pixel code address
    add x11, x9, #0x600          // Pixel control address
    cmp w21, #3
    csel x12, x10, x12, eq       // Negative: two vertex stages
    stp x12, x11, [x20, #112]
    cmp w21, #5
    b.lo from_builder
    add x0, x20, #128
    mov x1, x20
    call 7                       // ProgramInitialize, without a builder
    cbz w0, fail
    add x0, x20, #128
    mov w1, #2
    add x2, x20, #96
    cmp w21, #5
    b.ne extended
    call 8                       // SetShaders: two 16-byte descriptions
    b valid
extended:
    add x2, x20, #224
    ldp x9, x10, [x20, #96]
    stp x9, x10, [x2]
    str xzr, [x2, #16]
    ldp x9, x10, [x20, #112]
    stp x9, x10, [x2, #24]
    str xzr, [x2, #40]
    call 9                       // SetShadersExt: extension word after each pair
    b valid
from_builder:
    add x0, x20, #32
    mov w1, #2
    add x2, x20, #96
    call 4                       // ProgramBuilderSetShaders

    mov w9, #0x1234
    str w9, [x20, #128]
    add x0, x20, #128
    add x1, x20, #32
    call 5                       // ProgramInitializeFromBuilder
    cbz w21, valid
    cbnz w0, fail
    ldr w9, [x20, #128]
    mov w10, #0x1234
    cmp w9, w10                 // Rejected input didn't publish a program
    b.ne fail
    b done
valid:
    cbz w0, fail
    add x0, x20, #32
    call 2
    stp xzr, xzr, [x20, #96]
    stp xzr, xzr, [x20, #112]
    ldr w9, [x20, #132]          // Retained vertex and pixel stage mask
    cmp w9, #3
    b.ne fail
    adrp x9, #0x1000
    add x9, x9, #0x700
    ldp x10, x11, [x9]          // Independently computed SHA-256 prefixes
    ldp x12, x13, [x20, #144]
    cmp x10, x12
    b.ne fail
    cmp x11, x13
    b.ne fail
    add x0, x20, #128
    call 6                       // ProgramFinalize
    ldr x9, [x20, #128]
    cbnz x9, fail
    ldp x9, x10, [x20, #144]
    orr x9, x9, x10
    cbnz x9, fail
    b done
fail:
    mov w0, #1
    b leave
done:
    mov w0, wzr
leave:
    add sp, sp, #320
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
