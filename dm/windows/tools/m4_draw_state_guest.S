// The native import fixture checks command-memory use, record-time snapshots,
// stage retention across submissions, and clearing state on queue finalization.
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
    .equ lists, 512
    .equ data, 544
    .equ program_builder, 576
    .equ program, 640
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x30, [sp, #-16]!
    sub sp, sp, #768
    mov x20, sp
    adrp x19, #0x3000
    add x19, x19, #0x800
    adrp x9, #0x1000
    ldr w22, [x9, #0x780]
    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    mov x21, x1

    mov x0, x20
    call 0                       // nvnDeviceBuilderSetDefaults
    add x0, x20, #device
    mov x1, x20
    call 1                       // nvnDeviceInitialize
    cbz w0, fail
    add x0, x20, #pool_builder
    call 2                       // nvnMemoryPoolBuilderSetDefaults
    add x0, x20, #pool_builder
    add x1, x20, #device
    call 3                       // nvnMemoryPoolBuilderSetDevice
    add x0, x20, #pool_builder
    mov x1, x21
    mov x2, #0x4000
    call 4                       // nvnMemoryPoolBuilderSetStorage
    add x0, x20, #pool
    add x1, x20, #pool_builder
    call 5                       // nvnMemoryPoolInitialize
    cbz w0, fail
    add x0, x20, #command
    add x1, x20, #device
    call 6                       // nvnCommandBufferInitialize
    cbz w0, fail
    add x0, x20, #command
    add x1, x20, #pool
    mov x2, xzr
    mov x3, #0x100
    call 7                       // nvnCommandBufferAddCommandMemory
    add x0, x20, #command
    add x1, x21, #1, lsl #12
    mov x2, #0x100
    call 8                       // nvnCommandBufferAddControlMemory
    add x23, x20, #data
    cmp w22, #11
    b.hs record_rate_state
    cmp w22, #8
    b.hs record_deferred_textures

    add x0, x20, #program_builder
    call 23                      // nvnProgramBuilderSetDefaults
    add x0, x20, #program_builder
    add x1, x20, #device
    call 24                      // nvnProgramBuilderSetDevice
    adrp x9, #0x1000
    add x10, x9, #0x580
    add x11, x9, #0x500
    stp x10, x11, [x23]         // Vertex code and control
    add x10, x9, #0x680
    add x11, x9, #0x600
    stp x10, x11, [x23, #16]    // Fragment code and control
    add x0, x20, #program_builder
    mov w1, #2
    mov x2, x23
    call 25                      // nvnProgramBuilderSetShaders
    add x0, x20, #program
    add x1, x20, #program_builder
    call 26                      // nvnProgramInitializeFromBuilder
    cbz w0, fail

    add x0, x20, #command
    call 9                       // nvnCommandBufferBeginRecording

    add x9, x21, #2, lsl #12
    mov x10, #0x40
    stp x9, x10, [x23]
    add x9, x9, #0x100
    stp x9, x10, [x23, #16]
    add x0, x20, #command
    mov w1, wzr                  // Vertex-stage uniform buffers
    mov w2, #2
    mov w9, #15
    cmp w22, #2
    csel w2, w9, w2, eq         // Negative case: range extends past slot 15
    mov w3, #2
    mov x4, x23
    call 10                      // nvnCommandBufferBindUniformBuffers
    mov w9, #0x3f800000          // Both depth ranges are [0, 1]; reuse the array
    stp wzr, w9, [x23]
    stp wzr, w9, [x23, #8]
    add x0, x20, #command
    mov w1, #3
    mov w2, #2
    mov x3, x23
    call 11                      // nvnCommandBufferSetDepthRanges
    stp xzr, xzr, [x23]          // Buffers and depth ranges are already recorded
    mov w9, #0x40000000
    stp w9, w9, [x23]            // Invalid depth range if read late: [2, 2]
    add x0, x20, #command
    add x1, x20, #program
    mov w2, #3                   // Bind both stages in a different submission
    call 12                      // nvnCommandBufferBindProgram
    ldr w9, [x21, #48]
    lsr w9, w9, #24
    cmp w9, #2                  // SPH VPRS export is recorded for the vertex stage only.
    b.ne fail
    fmov s0, #3.5
    add x0, x20, #command
    call 28                      // nvnCommandBufferSetLineWidth
    fmov s0, #6.25
    add x0, x20, #command
    call 29                      // nvnCommandBufferSetPointSize
    ldr s0, [x21, #80]           // The native float ABI writes the record payload, not x1.
    fmov s1, #3.5
    fcmp s0, s1
    b.ne fail
    ldr s0, [x21, #96]
    fmov s1, #6.25
    fcmp s0, s1
    b.ne fail
    sub w1, w22, #4
    cmp w22, #4
    csel w1, w1, wzr, gt
    add x0, x20, #command
    call 30                      // nvnCommandBufferSetGeometryProcessingMode
    add x0, x20, #command
    call 13                      // nvnCommandBufferEndRecording
    str x0, [x20, #lists]
    add x0, x20, #program
    call 27                      // nvnProgramFinalize: shader keys are recorded
    add x0, x20, #command
    call 14                      // nvnCommandBufferGetCommandMemoryUsed
    cmp x0, #112                 // 24-byte uniform, depths, program; two 16-byte sizes; 8-byte mode
    b.ne fail

    add x0, x20, #command
    call 9
    add x0, x20, #command
    mov x1, xzr
    mov w2, #2
    call 12                      // Unbind only fragment
    add x0, x20, #command
    mov w1, #1
    mov w2, #3
    mov w3, #0x11
    movk w3, #0x20, lsl #16
    call 15                      // nvnCommandBufferBindTexture
    add x0, x20, #command
    mov w1, wzr
    mov w2, wzr
    mov w3, #640
    mov w4, #480
    call 16                      // nvnCommandBufferSetViewport
    add x0, x20, #command
    mov w1, #4                   // Triangles, unsigned-short index
    mov w2, #1
    cmp w22, #3
    cset w3, ge                  // Negative cases issue a real, nonzero draw
    add x4, x21, #3, lsl #12
    strh wzr, [x4]
    call 17                      // nvnCommandBufferDrawElements
    add x0, x20, #command
    call 13
    str x0, [x20, #lists + 8]
    add x0, x20, #command
    call 14
    cmp x0, #184                 // +8-byte program +16-byte handle +24-byte viewport +24-byte draw
    b.ne fail

    cmp w22, #1
    b.ne init_queue
    mov w9, #8
    str w9, [x21, #4]            // Forge a short UBO record, still 8-byte aligned
init_queue:
    add x0, x20, #queue_builder
    call 18                      // nvnQueueBuilderSetDefaults
    add x0, x20, #queue_builder
    add x1, x20, #device
    call 19                      // nvnQueueBuilderSetDevice
    add x0, x20, #queue
    add x1, x20, #queue_builder
    call 20                      // nvnQueueInitialize
    cbz w0, fail
    cmp w22, #11
    b.hs submit_rate_state
    cmp w22, #8
    b.hs submit_deferred_textures
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #lists
    call 21                      // nvnQueueSubmitCommands
    cmp w22, #4
    b.ne second_list
    add x0, x20, #queue
    call 22                      // Reuse the queue object at the identical address
    add x0, x20, #queue
    add x1, x20, #queue_builder
    call 20
    cbz w0, fail
second_list:
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #lists + 8
    call 21
finish_queue:
    add x0, x20, #queue
    call 22                      // nvnQueueFinalize
    mov w0, wzr
    b done

record_deferred_textures:
    mov x1, #0xf0000
    svc #1
    cbnz x0, fail
    add x23, x21, #0x100, lsl #12
    sub x23, x23, #8             // The whole handle list is unmapped during recording.
    add x0, x20, #command
    call 9
    add x0, x20, #command
    mov w1, #1
    mov w2, #128
    mov w3, wzr
    mov x4, xzr
    call 31                      // Empty binding may have a null list.
    cmp w22, #10
    csel x4, xzr, x23, eq        // Non-empty binding with a null list.
    add x0, x20, #command
    mov w1, #1
    mov w2, #126
    mov w3, #2
    call 31                      // nvnCommandBufferBindTexturesDeferred
    add x0, x20, #command
    call 13
    str x0, [x20, #lists]
    b init_queue

submit_deferred_textures:
    mov x1, #0x200000
    svc #1
    cbnz x0, fail
    mov w9, #0x11
    mov w10, #0x22
    movk w10, #0x20, lsl #16
    mov w11, #0xabcd
    stp w9, w11, [x23]
    stp w10, w11, [x23, #8]
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #lists
    call 21
    str wzr, [x23]               // Unbind one slot and replace the other.
    mov w9, #0x99
    str w9, [x23, #8]
    cmp w22, #9
    b.ne 1f
    mov x1, #0x100000
    svc #1                       // Only one handle remains readable for the second submit.
    cbnz x0, fail
1:
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #lists
    call 21                      // Submit the very same command handle again.
    b finish_queue

record_rate_state:
    add x0, x20, #command
    call 9
    mov w9, #11
    cmp w22, #15
    cinc w9, w9, eq             // Raw shading rates 12..15 are reserved.
    stp w9, wzr, [x23]
    add x0, x20, #command
    mov w1, #15                 // Last palette table is valid.
    cmp w22, #12
    cinc w1, w1, eq
    mov w2, #2
    mov w9, #17
    cmp w22, #13
    csel w2, w9, w2, eq
    add x9, x21, #0x200, lsl #12
    sub x9, x9, #4
    cmp w22, #14
    csel x3, x9, x23, eq         // Count is valid, but only one entry is mapped.
    call 32                      // nvnCommandBufferShadingRateImagePalette
    str xzr, [x23]               // The data is reusable immediately.
    ldr x9, [x21, #8]
    mov x10, #0x5555555555555555
    movk x10, #0x550b
    cmp x9, x10                  // 11, 0, then fourteen defaults (5).
    b.ne fail
    add x0, x20, #command
    mov w1, #1
    call 34                      // nvnCommandBufferSetShadingRatePaletteMode

    add x0, x20, #program_builder
    call 35                      // nvnTextureBuilderSetDefaults
    add x0, x20, #program_builder
    add x1, x20, #device
    call 36                      // nvnTextureBuilderSetDevice
    add x0, x20, #program_builder
    mov w1, #3                  // NVN_FORMAT_R8UI
    call 37                      // nvnTextureBuilderSetFormat
    add x0, x20, #program_builder
    mov w1, #16
    mov w2, #16
    call 38                      // nvnTextureBuilderSetSize2D
    add x0, x20, #program_builder
    mov w1, #2
    call 39                      // nvnTextureBuilderSetLevels
    add x0, x20, #program_builder
    add x1, x20, #pool
    mov x2, #0x3000
    call 40                      // nvnTextureBuilderSetStorage
    add x0, x20, #program
    add x1, x20, #program_builder
    call 41                      // nvnTextureInitialize
    cbz w0, fail
    mov x0, x23
    call 42                      // nvnTextureViewSetDefaults
    mov x0, x23
    mov w1, #1
    mov w2, #1
    call 43                      // nvnTextureViewSetLevels
    add x0, x20, #command
    add x1, x20, #program
    mov x2, x23
    call 33                      // nvnCommandBufferBindShadingRateImage
    mov x0, x23
    call 42                      // A subsequent edit to the view must not affect the bind.
    ldr x9, [x21, #88]           // Snapshot's view, after palette and mode records.
    movz w10, #0x0101, lsl #16
    orr w10, w10, #1
    cmp x9, x10
    b.ne fail
    add x0, x20, #command
    mov x1, xzr
    mov x2, xzr
    call 33                      // Unbind the snapshotted image.
    add x0, x20, #command
    mov w1, #15
    mov w2, wzr
    mov x3, xzr
    call 32                      // Restore the inactive palette.
    add x0, x20, #command
    mov w1, wzr
    call 34
    add x0, x20, #command
    call 13
    str x0, [x20, #lists]
    cmp w22, #16
    b.ne init_queue
    mov w9, #12
    strb w9, [x21, #8]           // Corrupt the recorded palette after validation.
    b init_queue

submit_rate_state:
    add x0, x20, #queue
    mov w1, #1
    add x2, x20, #lists
    call 21
    b finish_queue

fail:
    mov w0, #1
done:
    add sp, sp, #768
    ldp x23, x30, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret
