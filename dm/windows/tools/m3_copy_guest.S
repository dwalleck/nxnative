// Record texture transfers without touching their GPU-time storage.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    stp x19, x20, [sp, #-16]!
    stp x21, x30, [sp, #-16]!
    sub sp, sp, #1024
    mov x20, sp
    adrp x21, #0x1000
    ldr w21, [x21, #0x780]
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    str x1, [x20, #640]
    mov w9, #0x9abc
    str wzr, [x1]                // TextureInitialize must copy the separate packaged bytes
    add x10, x1, #4, lsl #12
    str w9, [x10]

    mov x0, x20
    call 0                       // DeviceBuilderSetDefaults
    add x0, x20, #16
    mov x1, x20
    call 1                       // DeviceInitialize
    cbz w0, fail
    add x0, x20, #32
    call 2                       // MemoryPoolBuilderSetDefaults
    add x0, x20, #32
    add x1, x20, #16
    call 3                       // MemoryPoolBuilderSetDevice
    add x0, x20, #32
    ldr x1, [x20, #640]
    mov x2, #0x4000
    call 4                       // MemoryPoolBuilderSetStorage
    add x0, x20, #64
    add x1, x20, #32
    call 5                       // MemoryPoolInitialize
    cbz w0, fail
    cmp w21, #5
    b.hs texture_copy
    // Create a suballocated upload buffer. Reset the builder before asking
    // the initialized buffer for its backing, as the game's resource code does.
    add x0, x20, #832
    call 30                      // BufferBuilderSetDefaults
    add x0, x20, #832
    add x1, x20, #16
    call 31                      // BufferBuilderSetDevice
    add x0, x20, #832
    add x1, x20, #64
    mov x2, #0x3000
    mov x3, #256
    call 32                      // BufferBuilderSetStorage
    add x0, x20, #896
    add x1, x20, #832
    call 33                      // BufferInitialize
    cbz w0, fail
    add x0, x20, #832
    call 30
    add x0, x20, #896
    call 34                      // BufferGetMemoryOffset
    str x0, [x20, #824]
    cmp x0, #3, lsl #12
    b.ne fail
    add x0, x20, #896
    call 35                      // BufferGetMemoryPool
    add x9, x20, #64
    cmp x0, x9
    b.ne fail
    call 36                      // Pool base + saved buffer offset is the upload source
    ldr x9, [x20, #824]
    add x9, x0, x9
    str x9, [x20, #824]

    add x0, x20, #256
    call 6                       // TextureBuilderSetDefaults
    add x0, x20, #256
    add x1, x20, #16
    call 7                       // TextureBuilderSetDevice
    add x0, x20, #256
    mov w1, #16
    mov w2, #16
    call 8                       // TextureBuilderSetSize2D
    add x0, x20, #256
    mov w1, #0x25                // RGBA8
    call 9                       // TextureBuilderSetFormat
    add x0, x20, #256
    add x1, x20, #64
    mov x2, xzr
    call 10                      // TextureBuilderSetStorage
    add x0, x20, #256
    ldr x1, [x20, #640]
    add x1, x1, #4, lsl #12
    call 37                      // SetPackagedTextureData (already block-linear)
    add x0, x20, #384
    add x1, x20, #256
    call 11                      // TextureInitialize
    cbz w0, fail
    add x0, x20, #576
    call 12                      // TextureViewSetDefaults
    add x0, x20, #576
    mov w1, wzr
    mov w2, #1
    call 13                      // TextureViewSetLevels

    add x0, x20, #128
    add x1, x20, #16
    call 14                      // CommandBufferInitialize
    cbz w0, fail
    add x0, x20, #128
    add x1, x20, #64
    mov x2, #0x3f38              // Last 200 bytes hold this measured list; the 256-byte grant is rounded up
    mov x3, #0x100
    call 15                      // AddCommandMemory
    add x0, x20, #128
    ldr x1, [x20, #640]
    add x1, x1, #2, lsl #12
    mov x2, #0x100
    call 16                      // AddControlMemory
    add x0, x20, #128
    mov x1, #64
    call 17                      // SetCopyRowStride
    add x0, x20, #128
    mov x1, #256
    call 18                      // SetCopyImageStride
    add x0, x20, #128
    call 19                      // BeginRecording

    add x4, x20, #608           // NVNcopyRegion: (0,0,0), 4x4x1
    cmp w21, #1
    cset w9, eq
    lsl w9, w9, #4              // Negative fixture mode: x = 16
    stp w9, wzr, [x4]
    mov w9, #4
    stp wzr, w9, [x4, #8]
    mov w10, #1
    stp w9, w10, [x4, #16]
    add x0, x20, #128
    ldr x1, [x20, #824]
    mov w9, #0x1111
    str w9, [x1]
    cmp w21, #2
    mov x9, #1                  // Unreadable only when execution tries to read it
    csel x1, x9, x1, eq
    str x1, [x20, #656]
    add x2, x20, #384
    add x3, x20, #576
    mov w5, #2                  // ENGINE_2D_BIT
    call 20                      // CopyBufferToTexture
    add x0, x20, #128
    mov w1, #0x10
    call 27                      // Barrier: invalidate texture cache (none in headless)

    add x0, x20, #128
    mov x1, xzr
    call 17
    add x0, x20, #128
    mov x1, xzr
    call 18
    add x0, x20, #576
    call 12                     // Reuse the original view and region
    add x9, x20, #608
    stp xzr, xzr, [x9]
    str xzr, [x9, #16]
    add x0, x20, #384
    call 22                     // Texture description survives in the copy record
    add x0, x20, #128
    call 21                     // EndRecording
    str x0, [x20, #648]

    ldr x9, [x20, #640]
    ldr w10, [x9]
    mov w11, #0x9abc
    cmp w10, w11
    b.ne fail
    add x10, x9, #2, lsl #12
    cmp x0, x10
    b.ne fail
    ldp x10, x11, [x0, #16]      // Command span and size
    mov x12, #0x3f38
    add x9, x9, x12
    cmp x10, x9
    b.ne fail
    cmp x11, #200
    b.ne fail
    ldp w10, w11, [x9]
    cmp w10, #3                 // Buffer-to-texture opcode
    b.ne fail
    cmp w11, #184
    b.ne fail
    ldr x10, [x9, #8]
    ldr x11, [x20, #656]
    cmp x10, x11
    b.ne fail
    ldp w10, w11, [x9, #16]
    cmp w10, #2
    b.ne fail
    cmp w11, #64
    b.ne fail
    ldr w10, [x9, #24]
    cmp w10, #256
    b.ne fail
    ldp w10, w11, [x9, #40]     // Width and height in retained region
    cmp w10, #4
    b.ne fail
    cmp w11, #4
    b.ne fail
    ldr w10, [x9, #60]          // Retained texture target
    cmp w10, #1
    b.ne fail
    ldr w10, [x9, #168]         // Retained view: explicit level count of one
    mov w11, #1
    movk w11, #0x100, lsl #16
    cmp w10, w11
    b.ne fail
    cmp w21, #3
    b.ne source_data
    str wzr, [x9, #4]           // Corrupt record length after recording
source_data:
    ldr x9, [x20, #640]
    add x9, x9, #3, lsl #12
    mov w10, #0x1234           // Source values change after the list is closed
    str w10, [x9]
    mov w10, #0x5678
    str w10, [x9, #64]          // Second row uses the captured stride, not 16 bytes
    add x0, x20, #672
    call 23                      // QueueBuilderSetDefaults
    add x0, x20, #672
    add x1, x20, #16
    call 24                      // QueueBuilderSetDevice
    add x0, x20, #736
    add x1, x20, #672
    call 25                      // QueueInitialize
    cbz w0, fail
    cmp w21, #4
    b.ne submit
    mov x0, xzr
    call 28                      // Finish must reject an invalid queue
submit:
    add x0, x20, #736
    mov w1, #1
    add x2, x20, #648
    call 26                      // QueueSubmitCommands
    add x0, x20, #736
    call 28                      // QueueFinish: uploaded texels must now be visible
    add x0, x20, #16
    ldr x1, [x20, #648]
    call 29                      // Retire the handle, not the texture's backing
    ldr x9, [x20, #640]
    ldr w10, [x9]
    mov w11, #0x1234
    cmp w10, w11
    b.ne fail
    ldr w10, [x9, #16]         // Switch 2 GOB row 1
    mov w11, #0x5678
    cmp w10, w11
    b.ne fail
    mov w0, wzr
    b done
fail:
    mov w0, #1
done:
    add sp, sp, #1024
    ldp x21, x30, [sp], #16
    ldp x19, x20, [sp], #16
    ret

texture_copy:
    // Three block-aligned array layers, each with a 16x16 and an 8x8 mip.
    // Layer 2's mip 1 starts at 2*2048+1024.
    add x0, x20, #256
    call 6                       // TextureBuilderSetDefaults
    add x0, x20, #256
    add x1, x20, #16
    call 7                       // TextureBuilderSetDevice
    add x0, x20, #256
    mov w1, #4
    call 39                      // TextureBuilderSetTarget: 2D_ARRAY
    add x0, x20, #256
    mov w1, #16
    mov w2, #16
    mov w3, #3
    call 40                      // TextureBuilderSetSize3D
    add x0, x20, #256
    mov w1, #2
    call 41                      // TextureBuilderSetLevels
    add x0, x20, #256
    mov w1, #0x25
    call 9                       // RGBA8
    add x0, x20, #256
    add x1, x20, #64
    mov x2, xzr
    call 10                      // TextureBuilderSetStorage
    add x0, x20, #384
    add x1, x20, #256
    call 11                      // TextureInitialize
    cbz w0, fail
    add x0, x20, #656
    call 12                      // TextureViewSetDefaults
    add x0, x20, #656
    mov w1, #1
    mov w2, #1
    call 13                      // TextureViewSetLevels
    add x0, x20, #656
    mov w1, #1
    mov w2, #2
    call 42                      // TextureViewSetLayers
    cmp w21, #6
    b.ne 1f
    add x0, x20, #656
    mov w1, #0x38
    call 45                      // TextureViewSetFormat: read RGBA8 storage as sRGB
1:

    add x0, x20, #256
    call 6
    add x0, x20, #256
    add x1, x20, #16
    call 7
    add x0, x20, #256
    mov w1, #8
    mov w2, #6
    call 8                       // TextureBuilderSetSize2D
    add x0, x20, #256
    mov w9, #0x25
    mov w10, #0x9a
    cmp w21, #6
    csel w1, w10, w9, eq
    call 9                       // RGBA8 or BGRA8 for Vulkan conversion
    add x0, x20, #256
    mov w1, #0x10
    call 43                      // TextureBuilderSetFlags: LINEAR
    add x0, x20, #256
    mov x1, #64
    call 44                      // TextureBuilderSetStride
    add x0, x20, #256
    add x1, x20, #64
    mov x2, #0x2000
    call 10
    add x0, x20, #512
    add x1, x20, #256
    call 11
    cbz w0, fail
    cmp w21, #6
    b.ne 1f
    // Use the real display's native Vulkan replay queue with no shader stages.
    add x0, x20, #872
    call 46                      // WindowBuilderSetDefaults
    add x0, x20, #872
    add x1, x20, #16
    call 47                      // WindowBuilderSetDevice
    add x9, x20, #512
    str x9, [x20, #944]
    add x0, x20, #872
    mov w1, #1
    add x2, x20, #944
    call 48                      // WindowBuilderSetTextures
    ldr x0, [x20, #640]
    add x0, x0, #5, lsl #12
    add x1, x20, #872
    call 49                      // WindowInitialize
    cbz w0, fail
1:

    ldr x9, [x20, #640]
    add x9, x9, #2, lsl #12
    sub x9, x9, #4
    mov w10, #0xa5a5
    movk w10, #0xa5a5, lsl #16
    mov w11, #98                 // six 64-byte rows plus two outside sentinels
1:  str w10, [x9], #4
    subs w11, w11, #1
    b.ne 1b

    add x0, x20, #128
    add x1, x20, #16
    call 14                      // CommandBufferInitialize
    cbz w0, fail
    add x0, x20, #128
    add x1, x20, #64
    mov x2, #0x3000
    mov x3, #0x400
    call 15
    add x0, x20, #128
    ldr x1, [x20, #640]
    add x1, x1, #2, lsl #12
    add x1, x1, #0x800
    mov x2, #0x100
    call 16
    add x0, x20, #128
    call 19                      // BeginRecording

    add x3, x20, #816
    mov w9, #1
    mov w10, #2
    mov w11, #3
    stp w9, w9, [x3]             // src (1, 1, 1): mip 1, layer base 1 + z 1
    stp w9, w11, [x3, #8]
    stp w10, w9, [x3, #16]        // 3x2x1
    add x6, x20, #840
    mov w11, #6
    stp w10, w10, [x6]           // dst (2, 2, 0)
    stp wzr, w11, [x6, #8]
    stp w10, w9, [x6, #16]        // 6x2x1
    add x0, x20, #128
    add x1, x20, #384
    add x2, x20, #656
    add x4, x20, #512
    mov x5, xzr
    mov w9, #0xc                 // Mirror X and Y
    mov w10, #0xd                // Add LINEAR_FILTER_BIT
    cmp w21, #6
    csel w7, w10, w9, eq
    call 38                      // CopyTextureToTexture
    add x0, x20, #128
    call 21                      // EndRecording
    str x0, [x20, #648]
    add x0, x20, #384
    call 22                      // Descriptions, views and regions are retained
    cmp w21, #6                  // The Vulkan window still references the destination.
    b.eq 1f
    add x0, x20, #512
    call 22
1:
    add x0, x20, #656
    call 12
    add x9, x20, #816
    stp xzr, xzr, [x9]
    stp xzr, xzr, [x9, #16]
    stp xzr, xzr, [x9, #32]

    ldr x9, [x20, #640]
    add x9, x9, #1, lsl #12      // Final array layer's mip 1
    add x9, x9, #0x400
    cmp w21, #6
    b.ne 1f
    mov w10, #0xff000000         // red channel is 0, 128, 255 in sRGB storage
    orr w11, w10, #128
    orr w12, w10, #255
    stp w10, w11, [x9, #20]
    str w12, [x9, #28]
    stp w12, w11, [x9, #36]
    str w10, [x9, #44]
    b 2f
1:
    mov w10, #1
    mov w11, #2
    mov w12, #3
    stp w10, w11, [x9, #20]      // GOB y=1, x=1..3
    str w12, [x9, #28]
    mov w10, #4
    mov w11, #5
    mov w12, #6
    stp w10, w11, [x9, #36]      // GOB y=2, x=1..3 (Switch 2)
    str w12, [x9, #44]
2:
    ldr w10, [x9, #0xc88]        // Destination unchanged before submission
    mov w11, #0xa5a5
    movk w11, #0xa5a5, lsl #16
    cmp w10, w11
    b.ne fail

    add x0, x20, #672
    call 23                      // QueueBuilderSetDefaults
    add x0, x20, #672
    add x1, x20, #16
    call 24
    add x0, x20, #736
    add x1, x20, #672
    call 25                      // QueueInitialize
    cbz w0, fail
    add x0, x20, #736
    mov w1, #1
    add x2, x20, #648
    call 26                      // QueueSubmitCommands
    add x0, x20, #736
    call 28                      // QueueFinish

    ldr x9, [x20, #640]
    add x9, x9, #2, lsl #12      // Destination
    mov w10, #0xa5a5
    movk w10, #0xa5a5, lsl #16
    ldur w11, [x9, #-4]
    cmp w11, w10
    b.ne fail
    ldr w11, [x9, #384]
    cmp w11, w10
    b.ne fail
    mov w13, #0                  // Destination byte offset
2:  ldr w11, [x9, x13]
    mov w12, w10
    lsr w14, w13, #6             // Row index
    sub w15, w14, #2
    cmp w15, #1
    b.hi 3f
    and w16, w13, #63
    sub w16, w16, #8
    cmp w16, #24
    b.hs 3f
    cmp w21, #6
    b.eq check_blit_color
    // Rows of expected texels: 6 6 5 5 4 4 / 3 3 2 2 1 1.
    mov w12, #3
    msub w12, w15, w12, w12
    add w12, w12, #3
    sub w12, w12, w16, lsr #3
3:  cmp w11, w12
    b.ne fail
check_next_pixel:
    add w13, w13, #4
    cmp w13, #384
    b.lo 2b
    cmp w21, #6
    b.ne 1f
    ldr x0, [x20, #640]
    add x0, x0, #5, lsl #12
    call 50                      // WindowFinalize
1:
    mov w0, wzr
    b done

check_blit_color:
    // Linear-light red after sampling sRGB texels: [0, 14, 41, 105, 205, 255].
    // The second mirrored row reverses those values. Allow one UNORM step
    // for device filtering precision, but require exact other BGRA bytes.
    lsl w14, w16, #1
    cbz w15, 1f
    mov w12, #40
    sub w14, w12, w14
1:  mov x12, #0x0e00
    movk x12, #0x6929, lsl #16
    movk x12, #0xffcd, lsl #32
    lsr x12, x12, x14
    and w12, w12, #0xff
    and w14, w11, #0xff00ffff
    mov w17, #0xff000000
    cmp w14, w17
    b.ne fail
    ubfx w14, w11, #16, #8
    sub w14, w14, w12
    add w14, w14, #1
    cmp w14, #2
    b.hi fail
    b check_next_pixel
