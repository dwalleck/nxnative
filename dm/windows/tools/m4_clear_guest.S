// Exercise the actual NVN clear imports, command recording and queue replay.
// test_windows_rewrite.py supplies texture configurations and expected backing words.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    .equ device, 16
    .equ pool_builder, 32
    .equ pool, 64
    .equ command, 128
    .equ queue_builder, 288
    .equ queue, 384
    .equ tex_builder, 512
    .equ color, 640
    .equ depth, 768
    .equ color_view, 896
    .equ depth_view, 928
    .equ targets, 960
    .equ views, 968
    .equ handles, 976
    .equ rgba, 992
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x30, [sp, #-16]!
    sub sp, sp, #1024
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    adrp x22, #0x1000
    add x22, x22, #0x400
    mov w23, #1
    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    mov x21, x1

    mov x0, x20
    call 0                       // DeviceBuilderSetDefaults
    mov x0, x20
    ldr w1, [x22]
    call 1                       // DeviceBuilderSetFlags: GOB order
    add x0, x20, #device
    mov x1, x20
    call 2                       // DeviceInitialize
    cbz w0, fail
    add x0, x20, #pool_builder
    call 3                       // MemoryPoolBuilderSetDefaults
    add x0, x20, #pool_builder
    add x1, x20, #device
    call 4                       // MemoryPoolBuilderSetDevice
    add x0, x20, #pool_builder
    mov x1, x21
    mov x2, #0xc000
    call 5                       // MemoryPoolBuilderSetStorage
    add x0, x20, #pool
    add x1, x20, #pool_builder
    call 6                       // MemoryPoolInitialize
    cbz w0, fail

    bl texture_defaults
    add x0, x20, #tex_builder
    ldr w1, [x22, #4]
    call 9                       // TextureBuilderSetTarget
    add x0, x20, #tex_builder
    ldr w1, [x22, #8]
    call 10                      // TextureBuilderSetFormat
    add x0, x20, #tex_builder
    ldr w1, [x22, #12]
    call 11                      // TextureBuilderSetFlags (128-byte default pitch if linear RT)
    add x0, x20, #tex_builder
    ldp w1, w2, [x22, #16]
    ldr w3, [x22, #24]
    call 12                      // TextureBuilderSetSize3D
    add x0, x20, #tex_builder
    ldr w1, [x22, #28]
    call 13                      // TextureBuilderSetLevels
    add x0, x20, #tex_builder
    add x1, x20, #pool
    mov x2, xzr
    call 14                      // TextureBuilderSetStorage
    add x0, x20, #color
    add x1, x20, #tex_builder
    call 15                      // TextureInitialize
    cbz w0, fail

    ldr w9, [x22, #32]
    cbz w9, no_depth_texture
    bl texture_defaults
    add x0, x20, #tex_builder
    ldr w1, [x22, #32]
    call 10
    add x0, x20, #tex_builder
    ldp w1, w2, [x22, #36]
    mov w3, #1
    call 12
    add x0, x20, #tex_builder
    add x1, x20, #pool
    mov x2, #0x7000
    call 14
    add x0, x20, #depth
    add x1, x20, #tex_builder
    call 15
    cbz w0, fail
    add x0, x20, #depth
    call 42                      // Depth starts at a nonzero pool offset, not at the pool or texture object.
    add x9, x21, #7, lsl #12
    cmp x0, x9
    b.ne fail

no_depth_texture:
    add x0, x20, #color_view
    call 16                      // TextureViewSetDefaults
    add x0, x20, #color_view
    ldp w1, w2, [x22, #44]
    call 17                      // TextureViewSetLevels
    add x0, x20, #color_view
    ldp w1, w2, [x22, #52]
    call 18                      // TextureViewSetLayers
    add x0, x20, #color_view
    ldr w1, [x22, #60]
    call 19                      // TextureViewSetFormat
    add x0, x20, #color_view
    mov w1, #4
    mov w2, #3
    mov w3, #2
    mov w4, #5
    call 20                      // TextureViewSetSwizzle: sampling only
    add x0, x20, #depth_view
    call 16
    add x0, x20, #depth_view
    mov w1, #1
    call 21                      // TextureViewSetDepthStencilMode: sampling only

    add x0, x20, #command
    add x1, x20, #device
    call 22                      // CommandBufferInitialize
    cbz w0, fail
    add x0, x20, #command
    add x1, x20, #pool
    mov x2, #0x9000
    mov x3, #0x1000
    call 23                      // CommandBufferAddCommandMemory
    add x0, x20, #command
    add x1, x21, #10, lsl #12
    mov x2, #0x1000
    call 24                      // CommandBufferAddControlMemory

    mov x9, xzr
    mov w10, #0xc7d8
    movk w10, #0xa5b6, lsl #16
1:  str w10, [x21, x9]
    add x9, x9, #4
    cmp x9, #9, lsl #12
    b.lo 1b

    add x0, x20, #command
    call 25                      // BeginRecording
    // Indirect X/Y/Z are all nonzero while recording. In queue order two
    // fills change them to 0,3,3: no dispatch, without a bound compute shader.
    add x9, x21, #11, lsl #12
    mov w10, #1
    stp w10, w10, [x9, #64]
    stp w10, w10, [x9, #72]
    add x0, x20, #command
    add x1, x9, #68
    mov x2, #8
    mov w3, #3
    call 40
    add x0, x20, #command
    add x1, x21, #11, lsl #12
    add x1, x1, #64
    mov x2, #4
    mov w3, wzr
    call 40
    add x0, x20, #command
    add x1, x21, #11, lsl #12
    add x1, x1, #64
    call 41
    add x9, x20, #color
    str x9, [x20, #targets]
    add x9, x20, #color_view
    str x9, [x20, #views]
    add x0, x20, #command
    mov w1, #1
    add x2, x20, #targets
    add x3, x20, #views
    add x4, x20, #depth
    add x5, x20, #depth_view
    ldr w9, [x22, #32]
    cmp w9, #0
    csel x4, x4, xzr, ne
    csel x5, x5, xzr, ne
    call 26                      // SetRenderTargets
    add x0, x20, #command
    ldp w1, w2, [x22, #64]
    ldp w3, w4, [x22, #72]
    call 27                      // SetScissor
    // Equal while recording. The CPU changes the second counter before submit.
    add x0, x20, #command
    mov w1, #1                  // Render if not equal
    add x2, x21, #11, lsl #12
    call 38
    mov w9, #0x3e800000          // RGBA = (.25, .75, .5, .5)
    mov w10, #0x3f400000
    mov w11, #0x3f000000
    add x2, x20, #rgba
    stp w9, w10, [x2]
    stp w11, w11, [x2, #8]
    add x0, x20, #command
    mov w1, wzr
    mov w3, #0xd                 // R | B | A
    call 28                      // ClearColor
    add x9, x20, #rgba
    stp xzr, xzr, [x9]          // Overwrite source values after recording
    add x0, x20, #command
    fmov s0, #0.75
    mov w1, wzr
    mov w2, #0xa3
    mov w3, #0x0f
    call 29                      // ClearDepthStencil: stencil low nibble only
    add x0, x20, #command
    mov w1, wzr
    call 39                      // Disable rendering across command lists
    add x0, x20, #command
    call 30                      // EndRecording
    str x0, [x20, #handles]

    add x0, x20, #command
    call 25                      // BeginRecording again, retaining targets via queue state
    add x0, x20, #command
    mov w1, wzr
    add x2, x20, #rgba
    mov w3, #0xf
    call 28                      // Suppressed: must not zero the first region
    add x0, x20, #command
    mov w1, #1
    call 39
    add x0, x20, #command
    ldp w1, w2, [x22, #80]
    ldp w3, w4, [x22, #88]
    call 27
    add x0, x20, #command
    mov w1, wzr
    add x2, x20, #rgba
    mov w3, #0xf
    call 28                      // ClearColor: zero
    add x0, x20, #command
    fmov s0, #0.5
    mov w1, #1
    mov w2, #0xff
    mov w3, wzr
    call 29                      // ClearDepthStencil: depth only
    add x0, x20, #command
    mov w1, wzr                 // False: the counter high halves differ
    add x2, x21, #11, lsl #12
    call 38
    add x0, x20, #command
    fmov s0, #1.0
    mov w1, #1
    mov w2, wzr
    mov w3, #0xff
    call 29                      // Must not overwrite depth/stencil
    // An explicit texture clear ignores the framebuffer scissor. Its region
    // selects the images to update and only the chosen channel becomes 1.
    add x0, x20, #command
    add x1, x20, #color
    add x2, x20, #color_view
    add x3, x22, #96
    add x4, x20, #rgba
    ldr w9, [x22, #120]
    mov w10, #0x3f800000
    str w10, [x4, w9, uxtw #2]
    mov w5, #1
    lsl w5, w5, w9
    call 37
    add x9, x20, #rgba
    stp xzr, xzr, [x9]
    add x0, x20, #command
    call 30
    str x0, [x20, #(handles + 8)]

    mov w23, #2
    mov x9, xzr
    mov w10, #0xc7d8
    movk w10, #0xa5b6, lsl #16
2:  ldr w11, [x21, x9]          // Recording must not alter any texture backing
    cmp w11, w10
    b.ne bad_word
    add x9, x9, #4
    cmp x9, #9, lsl #12
    b.lo 2b
    add x9, x21, #11, lsl #12
    ldp w10, w11, [x9, #64]
    cmp w10, #1
    ccmp w11, #1, #0, eq
    b.ne fail
    mov w23, #3
    add x0, x20, #queue_builder
    call 31                      // QueueBuilderSetDefaults
    add x0, x20, #queue_builder
    add x1, x20, #device
    call 32                      // QueueBuilderSetDevice
    add x0, x20, #queue
    add x1, x20, #queue_builder
    call 33                      // QueueInitialize
    cbz w0, fail
    add x9, x21, #11, lsl #12
    mov x10, #1
    lsl x10, x10, #32
    str x10, [x9, #16]
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #handles
    call 34                      // QueueSubmitCommands
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #(handles + 8)
    call 34
    add x0, x20, #queue
    call 35                      // QueueFinish
    add x9, x21, #11, lsl #12
    ldp w10, w11, [x9, #64]
    ldp w12, w13, [x9, #72]
    cbnz w10, fail
    cmp w11, #3
    ccmp w12, #3, #0, eq
    ccmp w13, #1, #0, eq       // Guard word after the argument block.
    b.ne fail
    add x0, x20, #device
    ldr x1, [x20, #handles]
    call 36                      // DeviceFinalizeCommandHandle
    add x0, x20, #device
    ldr x1, [x20, #(handles + 8)]
    call 36

    mov w23, #4
    add x12, x22, #128           // Sorted (offset, expected word) pairs terminated by UINT32_MAX
    ldp w13, w14, [x12], #8
    mov x9, xzr
    mov w10, #0xc7d8
    movk w10, #0xa5b6, lsl #16
3:  ldr w11, [x21, x9]
    cmp w9, w13
    csel w15, w14, w10, eq      // Everything not listed (including padding) stays poisoned
    b.ne 4f
    ldp w13, w14, [x12], #8
4:  cmp w11, w15
    b.ne bad_word
    add x9, x9, #4
    cmp x9, #9, lsl #12
    b.lo 3b
    mov w0, wzr
    b done
texture_defaults:
    str x30, [sp, #-16]!
    add x0, x20, #tex_builder
    call 7                       // TextureBuilderSetDefaults
    add x0, x20, #tex_builder
    add x1, x20, #device
    call 8                       // TextureBuilderSetDevice
    ldr x30, [sp], #16
    ret
bad_word:
    orr w23, w23, w9, lsl #8    // Report the offending backing word offset
fail:
    mov w0, w23
 done:
    add sp, sp, #1024
    ldp x23, x30, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret
