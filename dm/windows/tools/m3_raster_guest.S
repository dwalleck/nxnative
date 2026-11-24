// Check the NVN2 value ABI across the guest import boundary, including inline loads/stores.
    .text
    .macro proc name
    adr x1, .Lname\@
    bl lookup
    .subsection 1
.Lname\@: .asciz "nvn\name"
    .subsection 0
    .endm

    .macro call name, a, b, c, d, e
    proc \name
    .ifnb \a
    mov w1, #\a
    .endif
    .ifnb \b
    mov w2, #\b
    .endif
    .ifnb \c
    mov w3, #\c
    .endif
    .ifnb \d
    mov w4, #\d
    .endif
    .ifnb \e
    mov w5, #\e
    .endif
    blr x16
    .endm

    .macro word reg, value
    movz \reg, #(\value & 0xffff)
    movk \reg, #((\value >> 16) & 0xffff), lsl #16
    .endm

    .macro expect_word offset, value
    ldr w9, [x20, #\offset]
    word w10, \value
    cmp w9, w10
    b.ne fail
    .endm

    .macro expect_return value
    cmp w0, #\value
    b.ne fail
    .endm

    .macro fill number
    mov w21, #\number
    mov x9, #0xcccccccccccccccc
    stp x9, x9, [x20]
    stp x9, x9, [x20, #16]
    stp x9, x9, [x20, #32]
    .endm

    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    sub sp, sp, #64
    mov x20, sp
    adrp x19, #0x3000
    ldr x19, [x19, #0x800]

    fill 1
    call ColorStateSetDefaults
    expect_word 0, 0x08030000
    word w9, 0xa8030405            // Inline-written unrelated fields and target 2
    str w9, [x20]
    call ColorStateSetBlendEnable, 7, 0x102
    call ColorStateSetBlendEnable, 2, 0x100
    call ColorStateSetLogicOp, 6
    call ColorStateSetAlphaTest, 6
    expect_word 0, 0xa6068005
    expect_word 4, 0xcccccccc
    call ColorStateGetBlendEnable, 7
    expect_return 1
    call ColorStateGetBlendEnable, 2
    expect_return 0
    call ColorStateGetLogicOp
    expect_return 6
    call ColorStateGetAlphaTest
    expect_return 6

    fill 2
    call BlendStateSetDefaults
    expect_word 0, 0x110c0000
    expect_word 4, 0x01020102
    word w9, 0x998cc0f8
    word w10, 0x81828182           // Padding bits of the four factor bytes
    stp w9, w10, [x20]
    call BlendStateSetBlendTarget, 7
    call BlendStateSetBlendFunc, 5, 6, 0x12, 0x64
    call BlendStateSetBlendEquation, 2, 5
    call BlendStateSetAdvancedMode, 0x15
    call BlendStateSetAdvancedOverlap, 2
    call BlendStateSetAdvancedPremultipliedSrc, 0
    expect_word 0, 0xda8ad5ff
    expect_word 4, 0xe4928685
    expect_word 8, 0xcccccccc
    proc BlendStateGetBlendFunc
    add x1, x20, #32
    add x2, x20, #36
    add x3, x20, #40
    add x4, x20, #44
    blr x16
    expect_word 32, 5
    expect_word 36, 6
    expect_word 40, 0x12
    expect_word 44, 0x64

    fill 3
    call ChannelMaskStateSetDefaults
    expect_word 0, 0xffffffff
    call ChannelMaskStateSetChannelMask, 7, 2, 0x100, 0, 0xff
    expect_word 0, 0x9fffffff
    expect_word 4, 0xcccccccc
    proc ChannelMaskStateGetChannelMask
    mov w1, #7
    add x2, x20, #32
    add x3, x20, #33
    add x4, x20, #34
    add x5, x20, #35
    blr x16
    expect_word 32, 0x01000001
    expect_word 36, 0xcccccccc

    fill 4
    call DepthStencilStateSetDefaults
    expect_word 0, 0x22
    expect_word 4, 0x11181118
    call DepthStencilStateSetDepthTestEnable, 1
    call DepthStencilStateSetDepthWriteEnable, 0
    call DepthStencilStateSetDepthFunc, 7
    call DepthStencilStateSetStencilTestEnable, 1
    call DepthStencilStateSetStencilFunc, 3, 4  // Applies to both faces
    call DepthStencilStateSetStencilFunc, 2, 7
    call DepthStencilStateSetStencilOp, 1, 2, 3, 4
    call DepthStencilStateSetStencilOp, 2, 5, 6, 7
    expect_word 0, 0x75
    expect_word 4, 0x67573424
    expect_word 8, 0xcccccccc
    call DepthStencilStateGetStencilFunc, 1
    expect_return 4
    call DepthStencilStateGetStencilFunc, 2
    expect_return 7
    proc DepthStencilStateGetStencilOp
    mov w1, #2
    add x2, x20, #32
    add x3, x20, #36
    add x4, x20, #40
    blr x16
    expect_word 32, 5
    expect_word 36, 6
    expect_word 40, 7

    fill 5
    call PolygonStateSetDefaults
    expect_word 0, 0x14
    word w9, 0x80000814
    str w9, [x20]
    call PolygonStateSetCullFace, 2
    call PolygonStateSetFrontFace, 0
    call PolygonStateSetPolygonMode, 1
    call PolygonStateSetPolygonOffsetEnables, 3
    expect_word 0, 0x8001880a
    expect_word 4, 0xcccccccc
    call PolygonStateGetPolygonOffsetEnables
    expect_return 3

    fill 6
    call MultisampleStateSetDefaults
    expect_word 0, 5
    expect_word 4, 0x88888888
    expect_word 8, 0x88888888
    expect_word 12, 0x88888888
    expect_word 16, 0x88888888
    expect_word 20, 0
    call MultisampleStateSetSamples, 4
    call MultisampleStateSetRasterSamples, 8
    call MultisampleStateSetSampleLocationsEnable, 1
    expect_word 0, 0x0004040d
    proc MultisampleStateGetSampleLocationsGrid
    add x1, x20, #32
    add x2, x20, #36
    blr x16
    expect_word 32, 1
    expect_word 36, 2
    proc MultisampleStateSetSampleLocations
    mov w1, #14
    mov w2, #2
    adr x3, locations
    blr x16
    expect_word 16, 0x90f48888
    expect_word 20, 0
    expect_word 24, 0xcccccccc
    proc MultisampleStateGetSampleLocations
    mov w1, #14
    mov w2, #2
    add x3, x20, #32
    blr x16
    expect_word 32, 0x3e800000     // 0.25
    expect_word 36, 0x3f700000     // 0.9375
    expect_word 40, 0
    expect_word 44, 0x3f100000     // 0.5625

    mov w21, wzr
fail:
    mov w0, w21
    add sp, sp, #64
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
lookup:
    str x30, [sp, #-16]!
    mov x0, xzr
    blr x19                        // nvnDeviceGetProcAddress(NULL, name)
    ldr x30, [sp], #16
    cbz x0, fail
    mov x16, x0
    mov x0, x20
    ret
locations:
    .float 0.21875, 1.0, -0.25, 0.53125
    .subsection 1
    .p2align 2
