// Synthetic queue, texture, event and headless swapchain lifecycle.
    .text
    .macro call index
    ldr x16, [x19, #8 * \index]
    blr x16
    .endm
    .macro proc slot
    mov w1, #\slot
    bl lookup_proc
    .endm
    .macro acquire
    add x0, x20, #640
    add x1, x20, #800
    add x2, x20, #848
    call 24
    .endm
    .macro present index
    add x0, x20, #128
    add x1, x20, #640
    mov w2, #\index
    call 25
    .endm
    .macro queue_init expect
    add x0, x20, #128
    add x1, x20, #48
    call 6
    cmp w0, #\expect
    b.ne fail
    .endm
    .macro check_swizzle index, object, r, g, b, a
    add x0, x20, #\object
    add x1, x20, #992
    add x2, x20, #996
    add x3, x20, #1000
    add x4, x20, #1004
    call \index
    add x11, x20, #992
    ldp w9, w10, [x11]
    cmp w9, #\r
    b.ne fail
    cmp w10, #\g
    b.ne fail
    ldp w9, w10, [x11, #8]
    cmp w9, #\b
    b.ne fail
    cmp w10, #\a
    b.ne fail
    .endm
    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    sub sp, sp, #1792
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov w21, #5
    mov x9, #0xabc
    str x9, [x20, #944]
    add x0, x20, #944
    mov w1, #2
    call 31                       // No preselected-user launch parameter
    cbnz w0, fail
    ldr x9, [x20, #944]
    cmp x9, #0xabc                 // Empty channel leaves storage unchanged
    b.ne fail

    mov w21, #1
    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    str x1, [x20, #920]
    mov x0, x20
    call 0                        // DeviceBuilderSetDefaults
    add x0, x20, #16
    mov x1, x20
    call 1                        // DeviceInitialize
    cbz w0, fail
    mov w21, #15
    proc 0xc40                   // DeviceGetTextureHandle, before any pool exists
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #14
    add x0, x20, #16
    csel x0, xzr, x0, eq
    mov w1, #0x8765
    movk w1, #9, lsl #16
    mov w2, #0xabc
    blr x16
    mov w9, #0x8765
    movk w9, #0xabc9, lsl #16
    cmp x0, x9
    b.ne fail
    proc 0xc40
    add x0, x20, #16
    mov w1, wzr
    mov w2, #3
    blr x16
    cbnz x0, fail                // Texture zero nulls the whole handle
    proc 0xc40
    add x0, x20, #16
    mov w1, #7
    mov w2, wzr
    blr x16
    cmp x0, #7                   // Sampler zero does not null the handle
    b.ne fail
    mov w21, #6
    proc 0x700                   // RegisterFastClearColor
    add x0, x20, #16
    add x1, x20, #960
    stp xzr, xzr, [x1]            // Four float channels.
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #2
    csel x1, xzr, x1, eq         // Negative control: no readable color array.
    mov w2, #0x25                // RGBA8
    blr x16                      // RegisterFastClearColor: optimization declined.
    cbnz w0, fail
    mov w21, #1
    add x0, x20, #48
    call 2                        // QueueBuilderSetDefaults
    add x0, x20, #48
    add x1, x20, #16
    call 3                        // QueueBuilderSetDevice
    add x0, x20, #48
    call 4                        // Default command + compute + control
    mov w9, #0x4000
    movk w9, #5, lsl #16
    cmp x0, x9
    b.ne fail
    add x0, x20, #48
    ldr x1, [x20, #920]
    mov w2, #0x3000
    movk w2, #5, lsl #16
    call 5                        // QueueBuilderSetQueueMemory
    queue_init 0                  // Storage one page too short
    add x0, x20, #48
    ldr x1, [x20, #920]
    add x1, x1, #8
    mov w2, #0x4000
    movk w2, #5, lsl #16
    call 5
    queue_init 0                  // Misaligned storage
    add x0, x20, #48
    ldr x1, [x20, #920]
    mov w2, #0x4000
    movk w2, #5, lsl #16
    call 5
    queue_init 1
    mov w21, #9
    add x0, x20, #48
    call 2                        // Clear explicit memory for the second queue
    add x0, x20, #48
    add x1, x20, #128
    call 42                       // Associate the secondary builder with the primary
    add x0, x20, #48
    call 43
    add x9, x20, #128
    cmp x0, x9
    b.ne fail
    queue_init 0                  // Reject making a queue its own primary
    add x0, x20, #1280
    add x1, x20, #48
    call 6                        // Initialize the secondary queue
    cbz w0, fail
    add x0, x20, #48
    call 2
    add x0, x20, #48
    call 43
    cbnz x0, fail                 // Reset clears the association

    mov w21, #2
    add x0, x20, #224
    call 7                        // MemoryPoolBuilderSetDefaults
    add x0, x20, #224
    add x1, x20, #16
    call 8                        // MemoryPoolBuilderSetDevice
    add x0, x20, #224
    ldr x1, [x20, #920]
    add x1, x1, #0x60, lsl #12
    add x10, x1, #1, lsl #12
    mov w9, #0x5a
    strb w9, [x10, #0xfff]       // Flush should preserve the backing bytes
    mov x2, #0x2000
    call 9                        // MemoryPoolBuilderSetStorage
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    mov w10, #0x21                // Default CPU_NO_ACCESS | GPU_CACHED
    str w10, [x20, #984]
    cmp w9, #6
    b.eq pool_init                // Flushing does not require a CPU mapping
    add x0, x20, #224
    mov w1, #0x24                 // CPU_CACHED | GPU_CACHED
    mov w10, #0x22                // CPU_UNCACHED | GPU_CACHED
    cmp w9, #7
    csel w1, w10, w1, eq
    str w1, [x20, #984]
    call 33                       // MemoryPoolBuilderSetFlags
pool_init:
    add x0, x20, #256
    add x1, x20, #224
    call 10                       // MemoryPoolInitialize
    cbz w0, fail

    mov w21, #8
    add x0, x20, #224
    call 7                        // Reset the builder; pool flags should stay put.
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #8
    b.ne get_pool_flags
    add x0, x20, #256
    call 34                       // Negative: getter on a finalized pool
get_pool_flags:
    proc 0x780                   // MemoryPoolGetFlags
    add x0, x20, #256
    blr x16
    ldr w9, [x20, #984]
    cmp w0, w9
    b.ne fail

    mov w21, #7
    proc 0x740                   // MemoryPoolFlushMappedRange
    str x16, [x20, #976]
    add x0, x20, #256
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    mov x1, #0x1fff
    mov x2, #1                    // Last byte, without a cache-line restriction
    cmp w9, #3
    csinv x1, x1, xzr, ne          // Negative offset
    cmp w9, #4
    cinc x2, x2, eq               // Cross the end by one byte
    cmp w9, #5
    csinv x2, x2, xzr, ne          // SIZE_MAX length must not wrap
    ldr x16, [x20, #976]
    blr x16
    add x0, x20, #256
    mov x1, #0x2000
    mov x2, xzr                   // Empty range at the end
    ldr x16, [x20, #976]
    blr x16
    ldr x9, [x20, #920]
    add x9, x9, #0x61, lsl #12
    ldrb w9, [x9, #0xfff]
    cmp w9, #0x5a
    b.ne fail
    mov w21, #2
    add x0, x20, #1024
    call 11                       // TextureBuilderSetDefaults
    check_swizzle 36, 1024, 2, 3, 4, 5
    add x0, x20, #1024
    add x1, x20, #16
    call 12                       // TextureBuilderSetDevice
    add x0, x20, #1024
    mov w1, #16
    mov w2, #16
    call 13                       // TextureBuilderSetSize2D
    add x0, x20, #1024
    mov w1, #0x25                 // RGBA8
    call 14                       // TextureBuilderSetFormat
    add x0, x20, #1024
    mov w1, #4                    // NVN_TEXTURE_FLAGS_IMAGE
    call 48
    add x0, x20, #1024
    mov w1, #4                   // B, 0, R, 1
    mov w2, wzr
    mov w3, #2
    mov w4, #1
    call 35                       // TextureBuilderSetSwizzle
    check_swizzle 36, 1024, 4, 0, 2, 1
    add x0, x20, #1024
    call 15                       // TextureBuilderGetStorageSize
    cmp x0, #0x400
    b.ne fail
    add x0, x20, #1024
    call 41                       // No GPU page class on the headless device
    cbnz w0, fail
    add x0, x20, #1024
    add x1, x20, #256
    mov x2, #0x1e00
    call 16                       // TextureBuilderSetStorage
    add x0, x20, #352
    add x1, x20, #1024
    call 17                       // TextureInitialize rejects short backing
    cbnz w0, fail
    add x0, x20, #1024
    add x1, x20, #256
    mov x2, xzr
    call 16
    add x0, x20, #352
    add x1, x20, #1024
    call 17
    cbz w0, fail
    add x0, x20, #1024
    add x1, x20, #256
    mov x2, #0x1000
    call 16
    add x0, x20, #464
    add x1, x20, #1024
    call 17
    cbz w0, fail
    add x0, x20, #1024
    call 39                       // Builder defaults to depth sampling
    cbnz w0, fail
    add x0, x20, #1024
    mov w1, #0x35                 // DEPTH24_STENCIL8
    call 14
    proc 0xd80                   // TextureBuilderSetTarget
    add x0, x20, #1024
    mov w1, #4                   // 2D_ARRAY (one layer)
    blr x16
    add x0, x20, #1024
    mov w1, wzr
    call 48
    add x0, x20, #1024
    mov w1, #1                    // Stencil sampling mode
    call 38
    add x0, x20, #1024
    call 39
    cmp w0, #1
    b.ne fail
    add x0, x20, #1152
    add x1, x20, #1024
    call 17
    cbz w0, fail
    add x0, x20, #1024
    call 11                       // Reset builder without altering textures
    proc 0xdc0                   // TextureGetTarget
    add x0, x20, #1152
    blr x16
    cmp w0, #4
    b.ne fail
    proc 0xdc0
    add x0, x20, #352
    blr x16
    cmp w0, #1                   // Default 2D texture
    b.ne fail
    check_swizzle 37, 352, 4, 0, 2, 1
    add x0, x20, #352
    call 47                       // Read back flags after resetting builder.
    cmp w0, #4
    b.ne fail
    add x0, x20, #352
    call 40
    cbnz w0, fail
    add x0, x20, #1152
    call 40
    cmp w0, #1
    b.ne fail

    mov w21, #12
    proc 0x7c0                   // TextureViewSetDefaults
    add x0, x20, #1536
    blr x16
    proc 0x800                   // TextureViewSetLevels
    add x0, x20, #1536
    mov w1, wzr
    mov w2, #1
    blr x16
    add x2, x20, #1552           // NVNcopyRegion: (5,1,0), 1x1x1
    mov w9, #5
    mov w10, #1
    adrp x11, #0x2000
    ldr w11, [x11, #0xfe0]
    cmp w11, #12
    mov w12, #16
    csel w9, w12, w9, eq
    stp w9, w10, [x2]
    stp wzr, w10, [x2, #8]
    stp w10, w10, [x2, #16]
    mov w9, #0x1234
    str w9, [x20, #1584]
    proc 0x840                   // TextureWriteTexels
    add x0, x20, #352
    add x1, x20, #1536
    add x2, x20, #1552
    add x3, x20, #1584
    blr x16
    ldr x9, [x20, #920]
    add x9, x9, #0x60, lsl #12
    ldr w9, [x9, #84]            // Switch 2 GOB: xByte=20, y=1
    mov w10, #0x1234
    cmp w9, w10
    b.ne fail
    proc 0x880                   // TextureFlushTexels
    add x0, x20, #352
    add x1, x20, #1536
    add x2, x20, #1552
    blr x16

    mov w21, #11
    add x0, x20, #304            // Descriptor pool in unused space after pool
    add x1, x20, #256
    mov x2, #0x1800
    mov w3, #4
    call 49                      // TexturePoolInitialize (SDK fixture: 32-byte slots)
    cbz w0, fail
    ldr x9, [x20, #920]
    add x9, x9, #0x61, lsl #12
    add x9, x9, #0x820
    mov x10, #0x5eed
    str x10, [x9]
    add x0, x20, #304
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #11
    cset w1, ne                  // Slot 0 is reserved; normally write slot 1
    add x2, x20, #352
    mov x3, xzr
    call 50
    add x0, x20, #304
    mov w1, #1
    add x2, x20, #464
    add x3, x20, #1536
    call 50                      // Replace the slot with an explicit view
    ldr x9, [x20, #920]
    add x9, x9, #0x61, lsl #12
    ldr x9, [x9, #0x820]
    mov x10, #0x5eed
    cmp x9, x10
    b.ne fail                    // Registration does not emit GPU bytes
    add x0, x20, #304
    call 51                      // Drop host-owned descriptors

    mov w21, #10
    add x0, x20, #1424
    stp xzr, xzr, [x0]
    stp xzr, xzr, [x0, #16]
    stp xzr, xzr, [x0, #32]
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #15
    cset w9, eq
    str x9, [x0, #16]             // Negative: not an all-zero inline pool
    call 45                       // GetBufferAddress before initialization
    cbnz x0, fail
    add x0, x20, #224
    mov w1, #0x10c                // PHYSICAL | CPU_CACHED | GPU_NO_ACCESS
    call 33
    add x0, x20, #224
    ldr x1, [x20, #920]
    add x1, x1, #0x80, lsl #12
    mov x2, #0x4000
    call 9
    add x0, x20, #1376
    add x1, x20, #224
    call 10
    cbz w0, fail
    add x0, x20, #224
    mov w1, #0x221                // VIRTUAL | CPU_NO_ACCESS | GPU_CACHED
    call 33
    add x0, x20, #224
    mov x1, xzr                   // Virtual pool has no bytes of its own
    mov x2, #0x2000
    call 9
    add x0, x20, #1424
    add x1, x20, #224
    call 10
    cbz w0, fail
    proc 0xcc0                   // MemoryPoolSetDebugLabel is diagnostic-only
    add x0, x20, #1424
    mov x1, #1                    // Production NVN does not dereference the label
    blr x16
    proc 0xcc0
    add x0, x20, #1424
    adrp x1, #0x1000
    add x1, x1, #0xcc0           // Read-only label string
    blr x16
    proc 0xc80                   // MemoryPoolGetSize before virtual mapping
    add x0, x20, #1424
    blr x16
    cmp x0, #0x2000
    b.ne fail
    proc 0xc80
    add x0, x20, #1376
    blr x16
    cmp x0, #0x4000
    b.ne fail
    add x0, x20, #1472            // 0x28-byte NVN mapping request
    add x9, x20, #1376
    mov x10, #0x1000
    stp x9, x10, [x0]
    mov x10, #0x2000
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #9
    mov x11, #0x1000
    csel x10, x11, x10, eq        // Negative: partial virtual mapping
    stp xzr, x10, [x0, #16]
    str xzr, [x0, #32]
    mov x2, x0
    add x0, x20, #1424
    mov w1, #1
    call 44
    cbz w0, fail
    add x0, x20, #1424
    call 45
    ldr x9, [x20, #920]
    add x9, x9, #0x81, lsl #12
    cmp x0, x9                   // Alias honors the physical offset
    b.ne fail
    add x0, x20, #1424
    call 46
    cbnz x0, fail                 // Alias is still GPU-only
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #10
    b.ne close_alias
    add x0, x20, #1376
    call 34                       // Negative: backing pool still in use
    b fail
close_alias:
    add x0, x20, #1424
    call 34
    add x0, x20, #1376
    call 34

    mov w21, #13
    mov w9, #-1
    str w9, [x20, #1604]         // Adjacent attribute must survive defaults
    proc 0x8c0
    add x0, x20, #1600
    blr x16
    proc 0x900
    add x0, x20, #1600
    mov w1, #3
    blr x16
    proc 0x940
    add x0, x20, #1600
    mov w1, #0x25
    mov x2, #36
    blr x16
    ldr x9, [x20, #1600]
    mov w10, #0x2425
    movk w10, #0x1800, lsl #16
    cmp w9, w10
    b.ne fail
    lsr x9, x9, #32
    cmn w9, #1
    b.ne fail
    proc 0x980
    add x0, x20, #1608
    blr x16
    ldr x9, [x20, #1608]
    cbnz x9, fail
    proc 0x9c0
    add x0, x20, #1608
    mov x1, #48
    blr x16
    proc 0xa00
    add x0, x20, #1608
    mov w1, #2
    blr x16
    ldr x9, [x20, #1608]
    mov x10, #48
    movk x10, #2, lsl #32
    cmp x9, x10
    b.ne fail

    mov w21, #14
    proc 0xa40                   // CommandBufferInitialize
    add x0, x20, #1632
    add x1, x20, #16
    blr x16
    cbz w0, fail
    proc 0xa80                   // AddCommandMemory
    add x0, x20, #1632
    add x1, x20, #256
    mov x2, #0x1400
    mov x3, #0x100
    blr x16
    proc 0xac0                   // AddControlMemory
    add x0, x20, #1632
    ldr x1, [x20, #920]
    add x1, x1, #0x90, lsl #12
    mov x2, #0x100
    blr x16
    proc 0xd00                   // SetRecordingMode
    add x0, x20, #1632
    mov w1, #2
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    sub w10, w9, #16
    cmp w10, #1
    add w10, w10, #3
    csel w1, w10, w1, ls         // Negative: modes 3 and 4
    blr x16
    proc 0xe00                   // SetCopyRowStride outside recording
    add x0, x20, #1632
    mov x1, #0x100
    movk x1, #0x8000, lsl #16
    movk x1, #7, lsl #32         // The native setter stores only w1
    blr x16
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #18
    b.ne begin_record
    proc 0xd40                   // Negative: barrier outside recording
    add x0, x20, #1632
    mov w1, #2
    blr x16
    b fail
begin_record:
    proc 0xb00                   // BeginRecording uses no command memory
    add x0, x20, #1632
    blr x16
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #13
    b.ne record_draw
    proc 0xb00                   // Negative: nested BeginRecording
    add x0, x20, #1632
    blr x16
    b fail
record_draw:
    proc 0xe40                   // GetCopyRowStride inside recording
    add x0, x20, #1632
    blr x16
    mov w9, #0x100
    movk w9, #0x8000, lsl #16
    cmp x0, x9                   // The native getter zero-extends w0
    b.ne fail
    proc 0xec0                   // GetCopyImageStride still returns zero
    add x0, x20, #1632
    blr x16
    cbnz x0, fail
    proc 0xb40                   // GetCommandMemoryUsed
    add x0, x20, #1632
    blr x16
    cbnz x0, fail
    proc 0xb80                   // DrawElements, no execution at record time
    add x0, x20, #1632
    mov w1, #4                    // TRIANGLES
    mov w2, #1                    // UNSIGNED_SHORT
    mov w3, #3
    mov x4, xzr                   // An address token is not dereferenced here
    blr x16
    proc 0xd40                   // Barrier follows the draw
    add x0, x20, #1632
    mov w1, #0x52                // Order fragments; invalidate textures and descriptors
    movk w1, #0x8000, lsl #16    // Preserve all 32 bits of the NVN2 mask
    blr x16
    proc 0xbc0                   // EndRecording returns the next control entry
    add x0, x20, #1632
    blr x16
    cbz x0, fail
    str x0, [x20, #944]          // Keep the opaque handle across the next recording
    proc 0xe80                   // SetCopyImageStride after recording
    add x0, x20, #1632
    mov x1, #0x4000
    blr x16
    proc 0xec0
    add x0, x20, #1632
    blr x16
    cmp x0, #0x4000
    b.ne fail
    proc 0xd00                   // Reset to mode 0 for a second, empty list
    add x0, x20, #1632
    mov w1, wzr
    blr x16
    proc 0xb00
    add x0, x20, #1632
    blr x16
    proc 0xbc0
    add x0, x20, #1632
    blr x16
    cbz x0, fail
    ldr x9, [x20, #944]
    cmp x0, x9
    b.eq fail                    // The empty list must not overwrite the first
    proc 0xc00                   // IsRecording
    add x0, x20, #1632
    blr x16
    cbnz w0, fail

    mov w21, #3
    add x0, x20, #864
    call 26                       // EventBuilderSetDefaults
    add x0, x20, #864
    add x1, x20, #256
    mov x2, #0x1ffc
    call 27                       // EventBuilderSetStorage
    add x0, x20, #896
    add x1, x20, #864
    call 28                       // EventInitialize
    cbz w0, fail
    add x0, x20, #896
    mov w1, #0
    mov w2, #-1
    call 29                       // EventSignal WRITE
    add x0, x20, #896
    mov w1, #1
    mov w2, #42
    call 29                       // EventSignal ADD, wrapping at 32 bits
    add x0, x20, #896
    call 30                       // EventGetValue
    cmp w0, #41
    b.ne fail
    ldr x9, [x20, #920]
    add x9, x9, #0x61, lsl #12
    ldr w9, [x9, #0xffc]          // The event writes its backing, not a private copy
    cmp w9, #41
    b.ne fail

    mov w21, #4
    add x0, x20, #576
    call 18                       // WindowBuilderSetDefaults
    add x0, x20, #576
    add x1, x20, #16
    call 19                       // WindowBuilderSetDevice
    add x9, x20, #352
    add x10, x20, #464
    str x9, [x20, #832]
    str x10, [x20, #840]
    add x0, x20, #576
    mov w1, #2
    add x2, x20, #832
    call 20                       // WindowBuilderSetTextures
    add x0, x20, #576
    mov w1, wzr
    call 21                       // No timing dependency for functional test
    add x0, x20, #640
    add x1, x20, #576
    call 22                       // WindowInitialize
    cbz w0, fail
    add x0, x20, #800
    add x1, x20, #16
    call 23                       // SyncInitialize
    cbz w0, fail
    adrp x9, #0x2000
    ldr w9, [x9, #0xfe0]
    cmp w9, #1
    b.ne lifecycle
    present 0                     // Negative: present without owning a texture
    b fail
lifecycle:
    acquire
    cbnz w0, fail
    ldr w9, [x20, #848]
    cbnz w9, fail
    acquire
    cbnz w0, fail
    ldr w9, [x20, #848]
    cmp w9, #1
    b.ne fail
    acquire                       // No drawable slot remains
    cmp w0, #1
    b.ne fail
    present 0
    acquire
    cbnz w0, fail
    ldr w9, [x20, #848]
    cbnz w9, fail
    present 1
    present 0
    mov w0, wzr
    b done
fail:
    mov w0, w21
done:
    add sp, sp, #1792
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret

lookup_proc:
    str x30, [sp, #-16]!
    add x0, x20, #16
    adrp x9, #0x1000
    add x1, x9, x1
    call 32                       // DeviceGetProcAddress
    ldr x30, [sp], #16
    cbz x0, fail
    mov x16, x0
    ret

// A second standalone NRO payload; the first occupies one text page with
// its entry at +0x100. Keep the two payloads independently relocatable.
    .org 0xf00
    .equ cmd, 128
    .equ qb, 288
    .equ queue, 352
    .equ tb, 480
    .equ tex, 608
    .equ region, 800
    .equ state, 832
    .equ leaf, 880
    .equ middle, 888
    .equ called, 896
    .equ copied, 904
    .macro cmd_call index
    add x0, x20, #cmd
    call \index
    .endm
    .macro grant_command offset, size
    add x1, x20, #64
    mov x2, #\offset
    mov x3, #\size
    cmd_call 14
    .endm
    .macro grant_control offset, size
    mov x1, #\offset
    add x1, x22, x1
    mov x2, #\size
    cmd_call 15
    .endm
    .macro upload input, pixel
    mov w9, #\pixel
    str w9, [x20, #region]
    add x1, x22, #0x600
    add x1, x1, #4 * \input
    add x2, x20, #tex
    mov x3, xzr
    add x4, x20, #region
    mov w5, wzr
    cmd_call 23
    .endm
    .macro submit_list handle, pixel
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #\handle
    call 12
    ldp w9, w10, [x22]
    cmp w9, #\pixel
    b.ne command_fail
    cmp w10, #0x44
    b.ne command_fail
    .endm
    stp x19, x20, [sp, #-32]!
    stp x22, x30, [sp, #16]
    sub sp, sp, #928
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    mov x1, #0x200000
    svc #1
    cbnz x0, command_fail
    mov x22, x1
    mov x0, x20
    call 0                       // DeviceBuilderSetDefaults
    add x0, x20, #16
    mov x1, x20
    call 1                       // DeviceInitialize
    cbz w0, command_fail
    add x0, x20, #32
    call 2                       // MemoryPoolBuilderSetDefaults
    add x0, x20, #32
    add x1, x20, #16
    call 3                       // MemoryPoolBuilderSetDevice
    add x0, x20, #32
    mov x1, x22
    mov x2, #0x8000
    call 4                       // MemoryPoolBuilderSetStorage
    add x0, x20, #64
    add x1, x20, #32
    call 5                       // MemoryPoolInitialize
    cbz w0, command_fail
    add x0, x20, #qb
    call 6                       // QueueBuilderSetDefaults
    add x0, x20, #qb
    add x1, x20, #16
    call 7                       // QueueBuilderSetDevice
    add x0, x20, #queue
    add x1, x20, #qb
    call 8                       // QueueInitialize
    cbz w0, command_fail
    add x0, x20, #tb
    call 9                       // TextureBuilderSetDefaults
    add x0, x20, #tb
    add x1, x20, #16
    call 10                      // TextureBuilderSetDevice
    add x0, x20, #tb
    mov w1, #0x25                // RGBA8
    call 11                      // TextureBuilderSetFormat
    add x0, x20, #tb
    mov w1, #16
    mov w2, #16
    call 24                      // TextureBuilderSetSize2D
    add x0, x20, #tb
    add x1, x20, #64
    mov x2, xzr
    call 25                      // TextureBuilderSetStorage
    add x0, x20, #tex
    add x1, x20, #tb
    call 26                      // TextureInitialize
    cbz w0, command_fail
    add x9, x22, #0x600
    mov w10, #0x11
    mov w11, #0x22
    stp w10, w11, [x9]
    mov w10, #0x33
    mov w11, #0x44
    stp w10, w11, [x9, #8]
    mov w10, #0x55
    str w10, [x9, #16]
    add x9, x20, #region
    mov w10, #1
    stp wzr, wzr, [x9]
    stp wzr, w10, [x9, #8]
    stp w10, w10, [x9, #16]
    add x1, x20, #16
    cmd_call 13                  // CommandBufferInitialize
    cbz w0, command_fail
    adr x1, command_callback
    cmd_call 16                  // SetMemoryCallback
    add x1, x20, #state
    add x9, x20, #cmd
    add x10, x20, #64
    stp x9, x10, [x1]           // Callback's expected command buffer and pool
    stp x19, x22, [x1, #16]     // Import table and heap
    mov x9, #0x2000
    mov x10, #0x8100
    stp w9, w10, [x1, #32]      // Next command and control grant offsets
    stp wzr, wzr, [x1, #40]     // Callback counts
    cmd_call 17                  // SetMemoryCallbackData

    grant_command 0x1000, 184
    grant_control 0x8000, 32
    cmd_call 19                  // BeginRecording
    upload 0, 0
    adrp x9, #0x1000
    ldr w9, [x9, #0x780]
    cbz w9, command_growth
    mov w1, wzr
    cmd_call 18                  // Negative: command callback disabled
command_growth:
    upload 1, 0                 // Grow command memory with a live piece
    mov w1, wzr
    cmd_call 18                  // Does not disable the control callback
    cmd_call 20                  // EndRecording crosses control grants
    str x0, [x20, #leaf]
    add x9, x20, #state
    ldp w10, w11, [x9, #40]
    cmp w10, #1
    b.ne command_fail
    cmp w11, #2
    b.ne command_fail
    cmd_call 27                  // GetCommandMemoryUsed counts the latest grant
    cmp x0, #184
    b.ne command_fail

    grant_command 0x3000, 0x400
    grant_control 0x8800, 0x100
    cmd_call 19
    upload 2, 0
    mov w1, #1
    add x2, x20, #leaf
    cmd_call 21                  // Call the two-piece leaf
    grant_control 0x8900, 0x100  // Explicit replacement with a live control chain
    upload 3, 1
    cmd_call 20
    str x0, [x20, #middle]

    grant_command 0x4000, 0x200
    grant_control 0x9000, 0x100
    cmd_call 19
    mov w1, #1
    add x2, x20, #middle
    cmd_call 21                  // Nested call flattens both source lists
    cmd_call 27
    cbnz x0, command_fail
    cmd_call 20
    str x0, [x20, #called]

    mov w1, #1
    cmd_call 18
    grant_command 0x5000, 184
    grant_control 0x9100, 0x100
    cmd_call 19
    mov w1, #1
    add x2, x20, #middle
    cmd_call 22                  // Copy each source command across new grants
    cmd_call 20
    str x0, [x20, #copied]
    ldr w9, [x20, #state + 40]
    cmp w9, #4
    b.ne command_fail

    add x9, x22, #8, lsl #12
    add x10, x9, #1, lsl #12
recycle_control:
    stp xzr, xzr, [x9], #16      // Both source lists' control grants are reusable
    cmp x9, x10
    b.ne recycle_control
    grant_command 0x2000, 184    // Re-record the leaf's last command span
    grant_control 0x8000, 0x100
    cmd_call 19
    upload 4, 0
    cmd_call 20

    submit_list called, 0x55     // Calls use live command bytes, but not source control
    stp wzr, wzr, [x22]
    submit_list copied, 0x22     // Copies retain the original commands and order
    mov w0, wzr
    b command_done
command_fail:
    mov w0, #1
command_done:
    add sp, sp, #928
    ldp x22, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret

command_callback:
    ldr x9, [x3]
    cmp x0, x9
    b.ne callback_done
    cbz x2, callback_done
    cmp x2, #0x100
    b.hi callback_done
    ldp x9, x10, [x3, #16]
    cbz w1, grow_command
    cmp w1, #1
    b.ne callback_done
    ldr w11, [x3, #36]
    add x1, x10, x11
    add w11, w11, #0x100
    str w11, [x3, #36]
    ldr w11, [x3, #44]
    add w11, w11, #1
    str w11, [x3, #44]
    ldr x16, [x9, #8 * 15]      // AddControlMemory, passing the requested size
    br x16
grow_command:
    ldr x1, [x3, #8]
    ldr w10, [x3, #32]
    ldr w11, [x3, #40]
    add w11, w11, #1
    str w11, [x3, #40]
    add w11, w10, #0x100
    str w11, [x3, #32]
    mov x3, x2
    mov x2, x10
    ldr x16, [x9, #8 * 14]      // AddCommandMemory, passing the requested size
    br x16
callback_done:
    ret
